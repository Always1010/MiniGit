#include <catch2/catch_test_macros.hpp>
#include "minigit/merge.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "minigit/worktree.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
#include <sys/stat.h>
#include <utility>
using namespace minigit;
namespace {
using MergeFiles = std::map<std::string, std::pair<std::string, std::string>>;
ObjectId merge_commit(const Repository& repo, const MergeFiles& files, std::string_view message,
                      const std::vector<ObjectId>& parents = {}) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Snapshot snapshot;
    for (const auto& [path, file] : files)
        snapshot.emplace_hint(snapshot.end(), path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    return store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, snapshot), parents,
        "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
void merge_file(const Repository& repo, std::string_view path, std::string_view content, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(content), permissions);
}
void merge_install(const Repository& repo, ObjectId id, bool detached = false) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Index index;
    for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, id).tree)) {
        merge_file(repo, path, to_string(store.read(state.id).payload), state.mode == "100755" ? 0755U : 0644U);
        index.push_back({path, state.mode, state.id});
    }
    save_index(repo, index);
    if (detached) atomic_write(repo.metadata() / "HEAD", to_bytes(id.hex() + "\n"));
    else update_ref(repo, "refs/heads/main", std::nullopt, id);
    const auto log = encode_reflog({{std::nullopt, id, "fixture"}});
    detail::ensure_directory(repo.metadata() / "logs");
    atomic_write(repo.metadata() / "logs/HEAD", log);
    if (!detached) {
        detail::ensure_directory(repo.metadata() / "logs/refs/heads");
        atomic_write(repo.metadata() / "logs/refs/heads/main", log);
    }
    atomic_write(repo.metadata() / "identity", to_bytes("version=1\nuser.name=Test\nuser.email=a@b\n"));
}
std::map<std::string, Bytes> merge_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && std::filesystem::is_regular_file(entry.symlink_status()))
            result.emplace(path, read_file(entry.path()));
    }
    return result;
}
MergeOptions merge_start(ObjectId source, std::optional<std::string> message = std::nullopt) {
    return {MergeAction::Start, source.hex(), std::move(message)};
}
MergeOptions merge_action(MergeAction action) { return {action, {}, std::nullopt}; }
struct MergeFork { ObjectId base; ObjectId ours; ObjectId theirs; };
MergeFork merge_text_fork(const Repository& repo) {
    const auto base = merge_commit(repo, {{"file", {"base\n", "100644"}}, {"outside", {"stable", "100644"}}}, "base");
    const auto ours = merge_commit(repo, {{"file", {"ours\n", "100644"}}, {"outside", {"stable", "100644"}}}, "ours", {base});
    const auto theirs = merge_commit(repo, {{"file", {"theirs\n", "100644"}}, {"outside", {"stable", "100644"}}}, "theirs", {base});
    merge_install(repo, ours);
    return {base, ours, theirs};
}
std::uint32_t merge_permissions(const Repository& repo, std::string_view path) {
    return static_cast<std::uint32_t>(std::filesystem::symlink_status(repo.root() / path).permissions()) & 07777;
}
OperationState merge_state(const Repository& repo) {
    RepoLock lock(repo.metadata());
    const auto state = read_operation(repo);
    REQUIRE(state.has_value());
    return *state;
}
const WorkChange& merge_saved(const OperationState& state, std::string_view path) {
    const auto it = std::find_if(state.work.begin(), state.work.end(), [&](const auto& change) { return change.path == path; });
    REQUIRE(it != state.work.end());
    return *it;
}
}
TEST_CASE("Merge handles unborn fast-forward detached and up-to-date only after tracked validation", "[merge]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto base = merge_commit(repo, {}, "base");
    const auto tip = merge_commit(repo, {{"file", {"tip", "100755"}}, {"empty", {"", "100644"}}}, "tip", {base});
    ObjectId source = tip;
    Head expected{"refs/heads/main", tip};
    MergeOutcome outcome = MergeOutcome::FastForward;
    SECTION("unborn fast-forward keeps unknown work and creates first Index") {}
    SECTION("incoming exact same untracked files need no overwrite and become tracked") {
        merge_file(repo, "file", "tip", 0755);
        merge_file(repo, "empty", "", 0644);
    }
    SECTION("attached fast-forward advances branch and both logs") { merge_install(repo, base); }
    SECTION("detached fast-forward retains detached HEAD") { merge_install(repo, base, true); expected.symbolic.reset(); }
    SECTION("up-to-date still validates work and keeps every metadata byte") {
        merge_install(repo, tip); source = base; outcome = MergeOutcome::UpToDate;
    }
    merge_file(repo, "unknown", "keep", 0600);
    const auto before = merge_metadata(repo);
    const auto result = merge_repository(repo, merge_start(source));
    REQUIRE(result.outcome == outcome);
    REQUIRE(result.head == expected);
    REQUIRE(result.conflicts.empty());
    REQUIRE(read_file(repo.root() / "unknown") == to_bytes("keep"));
    REQUIRE(merge_permissions(repo, "unknown") == 0600);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    if (outcome == MergeOutcome::UpToDate) REQUIRE(merge_metadata(repo) == before);
    else {
        RepoLock lock(repo.metadata());
        REQUIRE(index_snapshot(repo) == read_snapshot(repo, read_commit(repo, tip).tree));
        REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{before.contains("logs/HEAD") ? std::optional(base) : std::nullopt, tip, "merge:fast-forward"});
        REQUIRE(merge_permissions(repo, "file") == 0755);
        REQUIRE(read_file(repo.root() / "empty").empty());
    }
}
TEST_CASE("Merge divergent clean content creates ordered two-parent commit and preserves untouched permissions", "[merge]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto base = merge_commit(repo, {{"left", {"0", "100644"}}, {"right", {"0", "100644"}}, {"stay", {"same", "100644"}}}, "base");
    const auto ours = merge_commit(repo, {{"left", {"ours", "100644"}}, {"right", {"0", "100644"}}, {"stay", {"same", "100644"}}}, "ours", {base});
    const auto theirs = merge_commit(repo, {{"left", {"0", "100644"}}, {"right", {"theirs", "100755"}}, {"stay", {"same", "100644"}}}, "theirs", {base});
    merge_install(repo, ours);
    merge_file(repo, "stay", "same", 0640);
    const auto identity = read_file(repo.metadata() / "identity");
    const auto result = merge_repository(repo, merge_start(theirs, "custom merge\nbody"));
    REQUIRE(result.outcome == MergeOutcome::Merged);
    REQUIRE(result.head.symbolic == "refs/heads/main");
    REQUIRE(result.head.id != ours);
    REQUIRE(result.head.id != theirs);
    REQUIRE(result.conflicts.empty());
    const auto commit = read_commit(repo, *result.head.id);
    REQUIRE(commit.parents == std::vector<ObjectId>{ours, theirs});
    REQUIRE(commit.message == "custom merge\nbody");
    REQUIRE(read_file(repo.root() / "left") == to_bytes("ours"));
    REQUIRE(read_file(repo.root() / "right") == to_bytes("theirs"));
    REQUIRE(merge_permissions(repo, "right") == 0755);
    REQUIRE(merge_permissions(repo, "stay") == 0640);
    REQUIRE(read_file(repo.metadata() / "identity") == identity);
    { RepoLock lock(repo.metadata());
      REQUIRE(index_snapshot(repo) == read_snapshot(repo, commit.tree));
      REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{ours, result.head.id, "merge"});
      REQUIRE(read_reflog(repo, "refs/heads/main").back() == ReflogEntry{ours, result.head.id, "merge"}); }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
}
TEST_CASE("Merge conflict continue requires resolved typed Index and preserves all unstaged work", "[merge]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto fork = merge_text_fork(repo);
    const auto original_head = read_file(repo.metadata() / "HEAD");
    const auto original_log = read_file(repo.metadata() / "logs/HEAD");
    const auto start = merge_repository(repo, merge_start(fork.theirs, "saved merge message"));
    REQUIRE(start.outcome == MergeOutcome::Conflicted);
    REQUIRE(start.head == Head{"refs/heads/main", fork.ours});
    REQUIRE(start.conflicts.size() == 1);
    REQUIRE(start.conflicts.front().kind == MergeConflictKind::Text);
    REQUIRE(read_file(repo.root() / "file") == to_bytes("<<<<<<< ours\nours\n=======\ntheirs\n>>>>>>> theirs\n"));
    REQUIRE(read_file(repo.metadata() / "HEAD") == original_head);
    REQUIRE(read_file(repo.metadata() / "logs/HEAD") == original_log);
    const auto state = merge_state(repo);
    REQUIRE(state.message == "saved merge message");
    REQUIRE(state.source_commit == fork.theirs);
    const auto unresolved = merge_metadata(repo);
    REQUIRE_THROWS_AS(merge_repository(repo, merge_action(MergeAction::Continue)), Error);
    REQUIRE(merge_metadata(repo) == unresolved);
    merge_file(repo, "file", "resolved\n");
    merge_file(repo, "added", "staged extra");
    add_paths(repo, {repo.root() / "file", repo.root() / "added"});
    merge_file(repo, "file", "unstaged resolution\n", 0600);
    merge_file(repo, "outside", "unstaged outside", 0640);
    const auto working = read_file(repo.root() / "file");
    const auto index_before = read_file(repo.metadata() / "index");
    const auto result = merge_repository(repo, merge_action(MergeAction::Continue));
    REQUIRE(result.outcome == MergeOutcome::Merged);
    REQUIRE(read_file(repo.metadata() / "index") == index_before);
    REQUIRE(read_file(repo.root() / "file") == working);
    REQUIRE(merge_permissions(repo, "file") == 0600);
    REQUIRE(read_file(repo.root() / "outside") == to_bytes("unstaged outside"));
    const auto commit = read_commit(repo, *result.head.id);
    REQUIRE(commit.parents == std::vector<ObjectId>{fork.ours, fork.theirs});
    REQUIRE(commit.message == state.message);
    { RepoLock lock(repo.metadata()); REQUIRE(read_snapshot(repo, commit.tree) == index_snapshot(repo)); }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
}
TEST_CASE("Merge abort restores whole raw Index exact HEAD and actual affected permissions while retaining outside work", "[merge]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    ObjectId ours, theirs;
    bool missing_index = false;
    SECTION("text conflict restores saved full permissions and whole original Index") {
        const auto fork = merge_text_fork(repo); ours = fork.ours; theirs = fork.theirs;
        merge_file(repo, "file", "ours\n", 0640);
    }
    SECTION("missing original Index differs from a valid empty file representation") {
        const auto base = merge_commit(repo, {{"file", {"base", "100644"}}}, "base");
        ours = merge_commit(repo, {}, "ours deletion", {base});
        theirs = merge_commit(repo, {{"file", {"theirs", "100644"}}}, "theirs", {base});
        merge_install(repo, ours);
        std::filesystem::remove(repo.metadata() / "index"); missing_index = true;
    }
    SECTION("detached raw uppercase HEAD is restored without invented reflog movement") {
        const auto fork = merge_text_fork(repo); ours = fork.ours; theirs = fork.theirs;
        auto upper = ours.hex();
        std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
        atomic_write(repo.metadata() / "HEAD", to_bytes(upper + "\n"));
    }
    const auto raw_head = read_file(repo.metadata() / "HEAD");
    const auto raw_index = missing_index ? std::optional<Bytes>{} : std::optional(read_file(repo.metadata() / "index"));
    const auto log = read_file(repo.metadata() / "logs/HEAD");
    REQUIRE(merge_repository(repo, merge_start(theirs)).outcome == MergeOutcome::Conflicted);
    merge_file(repo, "file", "user resolution", 0751);
    merge_file(repo, "added", "outside new");
    add_paths(repo, {repo.root() / "file", repo.root() / "added"});
    merge_file(repo, "outside", "outside edit");
    if (!read_head(repo).symbolic) atomic_write(repo.metadata() / "HEAD", to_bytes(ours.hex() + "\n"));
    REQUIRE(merge_repository(repo, merge_action(MergeAction::Abort)).outcome == MergeOutcome::Aborted);
    REQUIRE(read_file(repo.metadata() / "HEAD") == raw_head);
    REQUIRE(read_file(repo.metadata() / "logs/HEAD") == log);
    REQUIRE(read_file(repo.root() / "added") == to_bytes("outside new"));
    REQUIRE(read_file(repo.root() / "outside") == to_bytes("outside edit"));
    if (raw_index) REQUIRE(read_file(repo.metadata() / "index") == *raw_index);
    else REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "index"));
    if (missing_index) REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
    else {
        REQUIRE(read_file(repo.root() / "file") == to_bytes("ours\n"));
        REQUIRE(merge_permissions(repo, "file") == (raw_head == to_bytes("ref: refs/heads/main\n") ? 0640 : 0644));
    }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
}
TEST_CASE("Merge saves unchanged Mode Binary and DF conflict regions and restores resolved structure with original directory permissions", "[merge]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    ObjectId ours, theirs;
    bool directory = false;
    bool mode = false;
    SECTION("Mode same-content add-add noop remains affected and restores full permissions") {
        const auto base = merge_commit(repo, {}, "base");
        ours = merge_commit(repo, {{"file", {"same", "100644"}}}, "ours", {base});
        theirs = merge_commit(repo, {{"file", {"same", "100755"}}}, "theirs", {base});
        merge_install(repo, ours); mode = true;
        merge_file(repo, "file", "same", 0640);
    }
    SECTION("Binary ours layout noop remains affected and is restored") {
        const auto base = merge_commit(repo, {{"file", {std::string("b\0", 2), "100644"}}}, "base");
        ours = merge_commit(repo, {{"file", {std::string("o\0", 2), "100644"}}}, "ours", {base});
        theirs = merge_commit(repo, {{"file", {std::string("t\0", 2), "100644"}}}, "theirs", {base});
        merge_install(repo, ours);
        merge_file(repo, "file", std::string("o\0", 2), 0640);
    }
    SECTION("DF ours directory saves every real ancestor permission") {
        const auto base = merge_commit(repo, {}, "base");
        ours = merge_commit(repo, {{"r/deep/file", {"ours", "100644"}}}, "ours", {base});
        theirs = merge_commit(repo, {{"r", {"theirs", "100644"}}}, "theirs", {base});
        merge_install(repo, ours); directory = true;
        REQUIRE(::chmod((repo.root() / "r").c_str(), 0750) == 0);
        REQUIRE(::chmod((repo.root() / "r/deep").c_str(), 0710) == 0);
        merge_file(repo, "r/deep/file", "ours", 0640);
    }
    SECTION("DF original File restores over newly tracked resolved directory children") {
        const auto base = merge_commit(repo, {}, "base");
        ours = merge_commit(repo, {{"r", {"ours", "100644"}}}, "ours", {base});
        theirs = merge_commit(repo, {{"r/child", {"theirs", "100644"}}}, "theirs", {base});
        merge_install(repo, ours);
        merge_file(repo, "r", "ours", 0640);
    }
    const auto result = merge_repository(repo, merge_start(theirs));
    REQUIRE(result.outcome == MergeOutcome::Conflicted);
    const auto state = merge_state(repo);
    if (mode || result.conflicts.front().kind == MergeConflictKind::Binary) {
        REQUIRE(result.conflicts.front().kind == (mode ? MergeConflictKind::Mode : MergeConflictKind::Binary));
        const auto& saved = merge_saved(state, "file");
        REQUIRE(saved.before == saved.after);
        REQUIRE(saved.before.permissions == 0640);
        merge_file(repo, "file", "resolved"); add_paths(repo, {repo.root() / "file"});
    } else if (directory) {
        REQUIRE(merge_saved(state, "r").before == merge_saved(state, "r").after);
        REQUIRE(merge_saved(state, "r/deep").before.permissions == 0710);
        REQUIRE(merge_saved(state, "r/deep/file").before.permissions == 0640);
        std::filesystem::remove(repo.root() / "r/deep/file");
        std::filesystem::remove(repo.root() / "r/deep");
        std::filesystem::remove(repo.root() / "r");
        merge_file(repo, "r", "resolved file");
        add_paths(repo, {repo.root() / "r"}, true);
    } else {
        REQUIRE(merge_saved(state, "r").before == merge_saved(state, "r").after);
        std::filesystem::remove(repo.root() / "r");
        merge_file(repo, "r/new/deeper", "new tracked resolution");
        add_paths(repo, {repo.root() / "r"}, true);
    }
    REQUIRE(merge_repository(repo, merge_action(MergeAction::Abort)).outcome == MergeOutcome::Aborted);
    if (mode || result.conflicts.front().kind == MergeConflictKind::Binary) {
        REQUIRE(read_file(repo.root() / "file") == to_bytes(mode ? std::string("same") : std::string("o\0", 2)));
        REQUIRE(merge_permissions(repo, "file") == 0640);
    } else if (directory) {
        REQUIRE(read_file(repo.root() / "r/deep/file") == to_bytes("ours"));
        REQUIRE(merge_permissions(repo, "r") == 0750);
        REQUIRE(merge_permissions(repo, "r/deep") == 0710);
        REQUIRE(merge_permissions(repo, "r/deep/file") == 0640);
    } else {
        REQUIRE(read_file(repo.root() / "r") == to_bytes("ours"));
        REQUIRE(merge_permissions(repo, "r") == 0640);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "r/new"));
    }
    { RepoLock lock(repo.metadata()); REQUIRE(index_snapshot(repo) == read_snapshot(repo, read_commit(repo, ours).tree)); }
}
TEST_CASE("Merge rejects dirty input unsafe work collisions and damaged shortcut histories without mutable publication", "[merge]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto base = merge_commit(repo, {{"file", {"base", "100644"}}}, "base");
    auto source = merge_commit(repo, {{"file", {"next", "100644"}}, {"new", {"incoming", "100644"}}}, "source", {base});
    merge_install(repo, base);
    SECTION("staged content differs") { merge_file(repo, "file", "staged"); add_paths(repo, {repo.root() / "file"}); }
    SECTION("unstaged content differs") { merge_file(repo, "file", "dirty"); }
    SECTION("tracked deletion cannot be concealed") { std::filesystem::remove(repo.root() / "file"); }
    SECTION("mode differs even on up-to-date source") { source = base; REQUIRE(::chmod((repo.root() / "file").c_str(), 0755) == 0); }
    SECTION("unknown destination cannot be overwritten") { merge_file(repo, "new", "unknown"); }
    SECTION("ignored unknown destination cannot be overwritten") { merge_file(repo, ".minigitignore", "new\n"); merge_file(repo, "new", "ignored"); }
    SECTION("tracked symlink is rejected") { std::filesystem::remove(repo.root() / "file"); std::filesystem::create_symlink("new", repo.root() / "file"); }
    SECTION("tracked FIFO is rejected") { std::filesystem::remove(repo.root() / "file"); REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0); }
    SECTION("non-Blob Index is checked before work") {
        RepoLock lock(repo.metadata()); save_index(repo, {{"file", "100644", base}});
    }
    SECTION("source tree is missing even for same-ID shortcut") {
        RepoLock lock(repo.metadata());
        ObjectStore store(repo.metadata() / "objects");
        source = store.write(ObjectType::Commit, encode_commit({ObjectId{}, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "bad tree"}));
        atomic_write(repo.metadata() / "refs/heads/main", to_bytes(source.hex() + "\n"));
    }
    SECTION("same-ID cannot hide missing ancestor") {
        RepoLock lock(repo.metadata());
        ObjectStore store(repo.metadata() / "objects");
        source = store.write(ObjectType::Commit, encode_commit({read_commit(repo, base).tree, {ObjectId{}},
            "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "bad ancestor"}));
        atomic_write(repo.metadata() / "refs/heads/main", to_bytes(source.hex() + "\n"));
    }
    SECTION("unrelated commits with the same tree are not a merge base") {
        source = merge_commit(repo, {{"file", {"base", "100644"}}}, "unrelated");
    }
    SECTION("crisscross histories with two lowest bases are rejected") {
        const MergeFiles files{{"file", {"base", "100644"}}};
        const auto x = merge_commit(repo, files, "x", {base});
        const auto y = merge_commit(repo, files, "y", {base});
        const auto left = merge_commit(repo, files, "left", {x, y});
        source = merge_commit(repo, files, "right", {y, x});
        atomic_write(repo.metadata() / "refs/heads/main", to_bytes(left.hex() + "\n"));
    }
    const auto before = merge_metadata(repo);
    REQUIRE_THROWS_AS(merge_repository(repo, merge_start(source)), Error);
    REQUIRE(merge_metadata(repo) == before);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
TEST_CASE("Merge lifecycle rejects invalid state CAS unresolved Index and unsafe abort inventory while retaining operation", "[merge]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto fork = merge_text_fork(repo);
    REQUIRE(merge_repository(repo, merge_start(fork.theirs)).outcome == MergeOutcome::Conflicted);
    auto options = merge_action(MergeAction::Abort);
    SECTION("another start is guarded") { options = merge_start(fork.theirs); }
    SECTION("continue rejects unresolved stages") { options = merge_action(MergeAction::Continue); }
    SECTION("continue without identity cannot publish a commit") {
        merge_file(repo, "file", "resolved"); add_paths(repo, {repo.root() / "file"});
        std::filesystem::remove(repo.metadata() / "identity"); options = merge_action(MergeAction::Continue);
    }
    SECTION("logical HEAD moving while active is rejected") {
        atomic_write(repo.metadata() / "refs/heads/main", to_bytes(fork.theirs.hex() + "\n"));
    }
    SECTION("abort rejects corrupt current Index even though it has a saved backup") { atomic_write(repo.metadata() / "index", to_bytes("bad")); }
    SECTION("continue and abort reject extraneous options") { options.message = "replacement"; }
    SECTION("new unknown directory child blocks restoration of saved File") {
        std::filesystem::remove(repo.root() / "file");
        merge_file(repo, "file/tracked", "tracked resolution"); add_paths(repo, {repo.root() / "file"}, true);
        merge_file(repo, "file/unknown", "keep me");
    }
    SECTION("new unknown empty directory blocks saved File restoration") {
        std::filesystem::remove(repo.root() / "file");
        merge_file(repo, "file/tracked", "tracked resolution"); add_paths(repo, {repo.root() / "file"}, true);
        std::filesystem::create_directory(repo.root() / "file/unknown-empty");
    }
    const auto before = merge_metadata(repo);
    REQUIRE_THROWS_AS(merge_repository(repo, options), Error);
    REQUIRE(merge_metadata(repo) == before);
    REQUIRE(std::filesystem::exists(repo.metadata() / "operation"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
TEST_CASE("Merge conflict preparation and committed continuation recover exactly without duplicate records", "[merge]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto fork = merge_text_fork(repo);
    SECTION("Prepared conflict creation recovers original Index Work and missing operation") {
        merge_file(repo, "file", "ours\n", 0640);
        const auto before = merge_metadata(repo);
        bool reached = false;
        REQUIRE_THROWS_AS(merge_repository(repo, merge_start(fork.theirs), [&](std::size_t, std::string_view point) {
            if (point == "journal.prepared") { reached = true; throw Error("fixture stop"); }
        }), Error);
        REQUIRE(reached);
        { RepoLock lock(repo.metadata()); REQUIRE_FALSE(transaction_roots(repo).empty()); recover_transaction(repo); recover_transaction(repo); }
        REQUIRE(merge_metadata(repo) == before);
        REQUIRE(read_file(repo.root() / "file") == to_bytes("ours\n"));
        REQUIRE(merge_permissions(repo, "file") == 0640);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    }
    SECTION("Committed continue recovery keeps one commit and one reflog append") {
        REQUIRE(merge_repository(repo, merge_start(fork.theirs)).outcome == MergeOutcome::Conflicted);
        merge_file(repo, "file", "resolved"); add_paths(repo, {repo.root() / "file"});
        merge_file(repo, "file", "unstaged", 0600);
        const auto resolved_index = read_file(repo.metadata() / "index");
        bool reached = false;
        REQUIRE_THROWS_AS(merge_repository(repo, merge_action(MergeAction::Continue), [&](std::size_t, std::string_view point) {
            if (point == "journal.committed") { reached = true; throw Error("fixture stop"); }
        }), Error);
        REQUIRE(reached);
        Head desired;
        { RepoLock lock(repo.metadata());
          REQUIRE_FALSE(transaction_roots(repo).empty()); recover_transaction(repo);
          desired = read_head(repo);
          const auto log = read_reflog(repo, "HEAD");
          REQUIRE(log.size() == 2); REQUIRE(log.back() == ReflogEntry{fork.ours, desired.id, "merge:continue"});
          recover_transaction(repo); REQUIRE(read_head(repo) == desired); REQUIRE(read_reflog(repo, "HEAD") == log); }
        REQUIRE(read_commit(repo, *desired.id).parents == std::vector<ObjectId>{fork.ours, fork.theirs});
        REQUIRE(read_file(repo.metadata() / "index") == resolved_index);
        REQUIRE(read_file(repo.root() / "file") == to_bytes("unstaged"));
        REQUIRE(merge_permissions(repo, "file") == 0600);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    }
}
