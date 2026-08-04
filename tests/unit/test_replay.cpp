#include <catch2/catch_test_macros.hpp>
#include "minigit/replay.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/merge.hpp"
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
using ReplayFiles = std::map<std::string, std::pair<std::string, std::string>>;
constexpr std::string_view source_actor = "Source Person <source@example.test> 0 +0000";
ObjectId replay_commit(const Repository& repo, const ReplayFiles& files, std::string_view message,
                       const std::vector<ObjectId>& parents = {}, std::string_view author = source_actor) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Snapshot snapshot;
    for (const auto& [path, file] : files)
        snapshot.emplace_hint(snapshot.end(), path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    return store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, snapshot), parents,
        std::string(author), "Fixture <fixture@example.test> 0 +0000", std::string(message)}));
}
void replay_file(const Repository& repo, std::string_view path, std::string_view content, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(content), permissions);
}
void replay_install(const Repository& repo, ObjectId id, bool detached = false) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Index index;
    for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, id).tree)) {
        replay_file(repo, path, to_string(store.read(state.id).payload), state.mode == "100755" ? 0755U : 0644U);
        index.push_back({path, state.mode, state.id});
    }
    save_index(repo, index);
    if (detached) atomic_write(repo.metadata() / "HEAD", to_bytes(id.hex() + "\n"));
    else update_ref(repo, "refs/heads/main", std::nullopt, id);
    const auto log = encode_reflog({{std::nullopt, id, "fixture"}});
    detail::ensure_directory(repo.metadata() / "logs/refs/heads");
    atomic_write(repo.metadata() / "logs/HEAD", log);
    if (!detached) atomic_write(repo.metadata() / "logs/refs/heads/main", log);
    atomic_write(repo.metadata() / "identity", to_bytes("version=1\nuser.name=Current\nuser.email=current@example.test\n"));
}
std::map<std::string, Bytes> replay_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && std::filesystem::is_regular_file(entry.symlink_status()))
            result.emplace(path, read_file(entry.path()));
    }
    return result;
}
ReplayOptions replay_start(ObjectId source, std::optional<std::string> message = std::nullopt) {
    return {ReplayAction::Start, source.hex(), std::move(message)};
}
ReplayOptions replay_action(ReplayAction action) { return {action, {}, std::nullopt}; }
ReplayResult replay_run(const Repository& repo, bool reverting, const ReplayOptions& options, const TransactionHook& hook = {}) {
    return reverting ? revert_repository(repo, options, hook) : cherry_pick_repository(repo, options, hook);
}
std::uint32_t replay_permissions(const Repository& repo, std::string_view path) {
    return static_cast<std::uint32_t>(std::filesystem::symlink_status(repo.root() / path).permissions()) & 07777;
}
OperationState replay_state(const Repository& repo) {
    RepoLock lock(repo.metadata());
    const auto state = read_operation(repo);
    REQUIRE(state.has_value());
    return *state;
}
const WorkChange& replay_saved(const OperationState& state, std::string_view path) {
    const auto found = std::find_if(state.work.begin(), state.work.end(), [&](const auto& change) { return change.path == path; });
    REQUIRE(found != state.work.end());
    return *found;
}
struct ReplayFork { ObjectId current; ObjectId source; };
ReplayFork replay_fork(const Repository& repo) {
    const auto parent = replay_commit(repo, {{"file", {"base\n", "100644"}}}, "parent");
    const auto source = replay_commit(repo, {{"file", {"source\n", "100644"}}}, "source message", {parent});
    const auto current = replay_commit(repo, {{"file", {"ours\n", "100644"}}, {"outside", {"stay", "100644"}}}, "independent current");
    replay_install(repo, current);
    return {current, source};
}
}
TEST_CASE("Replay cherry-pick applies unrelated and root sources with original author and current committer", "[replay]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto current = replay_commit(repo, {{"file", {"base", "100644"}}, {"outside", {"own", "100644"}}}, "current", {},
        "Old Current <old@example.test> 0 +0000");
    ObjectId source;
    bool root = false, detached = false;
    std::optional<std::string> message;
    SECTION("unrelated source delta preserves current-only files") {
        const auto parent = replay_commit(repo, {{"file", {"base", "100644"}}}, "parent");
        source = replay_commit(repo, {{"file", {"next", "100755"}}, {"empty", {"", "100644"}}}, "source message", {parent});
    }
    SECTION("root source uses empty base and preserves detached HEAD") {
        source = replay_commit(repo, {{"new", {"root", "100644"}}}, "source root"); root = true; detached = true;
    }
    SECTION("explicit message rescues blank source message") {
        source = replay_commit(repo, {{"new", {"root", "100644"}}}, " \n\t"); root = true; message = "override message";
    }
    SECTION("same unknown incoming file is preserved by exact noop policy") {
        source = replay_commit(repo, {{"new", {"root", "100644"}}}, "source root"); root = true;
        replay_file(repo, "new", "root");
    }
    replay_install(repo, current, detached);
    replay_file(repo, "outside", "own", 0640);
    replay_file(repo, "unknown", "keep", 0600);
    const auto identity = read_file(repo.metadata() / "identity");
    const auto result = cherry_pick_repository(repo, replay_start(source, message));
    REQUIRE(result.outcome == ReplayOutcome::Applied);
    REQUIRE(result.conflicts.empty());
    REQUIRE(result.head.symbolic == (detached ? std::optional<std::string>{} : std::optional<std::string>{"refs/heads/main"}));
    const auto commit = read_commit(repo, *result.head.id);
    REQUIRE(commit.parents == std::vector<ObjectId>{current});
    REQUIRE(commit.author == source_actor);
    REQUIRE(commit.committer.starts_with("Current <current@example.test> "));
    REQUIRE(commit.message == (message ? *message : read_commit(repo, source).message));
    REQUIRE(read_file(repo.root() / "outside") == to_bytes("own"));
    REQUIRE(replay_permissions(repo, "outside") == 0640);
    REQUIRE(read_file(repo.root() / "unknown") == to_bytes("keep"));
    REQUIRE(read_file(repo.metadata() / "identity") == identity);
    if (root) REQUIRE(read_file(repo.root() / "new") == to_bytes("root"));
    else { REQUIRE(read_file(repo.root() / "file") == to_bytes("next")); REQUIRE(replay_permissions(repo, "file") == 0755); REQUIRE(read_file(repo.root() / "empty").empty()); }
    { RepoLock lock(repo.metadata());
      REQUIRE(index_snapshot(repo) == read_snapshot(repo, commit.tree));
      REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{current, result.head.id, "cherry-pick"}); }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
}
TEST_CASE("Replay revert reverses source delta and root additions with current author and one parent", "[replay]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto parent = replay_commit(repo, {{"file", {"base", "100644"}}}, "parent");
    ObjectId source, current;
    bool root = false;
    std::optional<std::string> message;
    SECTION("reverse delta can come from another history") {
        source = replay_commit(repo, {{"file", {"next", "100755"}}, {"new", {"added", "100644"}}}, "source", {parent});
        current = replay_commit(repo, {{"file", {"next", "100755"}}, {"new", {"added", "100644"}}, {"outside", {"own", "100644"}}}, "current");
    }
    SECTION("root revert removes source additions without moving to a parent") {
        source = replay_commit(repo, {{"new", {"root", "100644"}}}, "source root"); root = true;
        current = replay_commit(repo, {{"new", {"root", "100644"}}, {"outside", {"own", "100644"}}}, "current");
    }
    SECTION("explicit message overrides full-ID default") {
        source = replay_commit(repo, {{"new", {"root", "100644"}}}, "source root"); root = true; message = "undo this";
        current = source;
    }
    replay_install(repo, current);
    const auto result = revert_repository(repo, replay_start(source, message));
    REQUIRE(result.outcome == ReplayOutcome::Applied);
    const auto commit = read_commit(repo, *result.head.id);
    REQUIRE(commit.parents == std::vector<ObjectId>{current});
    REQUIRE(commit.author == commit.committer);
    REQUIRE(commit.author.starts_with("Current <current@example.test> "));
    REQUIRE(commit.message == (message ? *message : "Revert " + source.hex()));
    REQUIRE_FALSE(std::filesystem::exists(repo.root() / "new"));
    if (!root) { REQUIRE(read_file(repo.root() / "file") == to_bytes("base")); REQUIRE(replay_permissions(repo, "file") == 0644); }
    { RepoLock lock(repo.metadata()); REQUIRE(index_snapshot(repo) == read_snapshot(repo, commit.tree));
      REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{current, result.head.id, "revert"}); }
}
TEST_CASE("Replay preserves empty changes as new commits and keeps exact unchanged Index and Work", "[replay]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const ReplayFiles files{{"file", {"same", "100644"}}};
    const auto parent = replay_commit(repo, files, "parent");
    const auto source = replay_commit(repo, files, "source empty change", {parent});
    ObjectId current = source;
    bool reverting = false;
    SECTION("already applied cherry-pick still creates commit") {}
    SECTION("empty source delta revert still creates commit") { reverting = true; }
    SECTION("empty root source can be picked across histories") { current = parent; }
    replay_install(repo, current);
    replay_file(repo, "file", "same", 0640);
    const auto raw_index = read_file(repo.metadata() / "index");
    const auto working = read_file(repo.root() / "file");
    auto options = replay_start(source);
    if (current == parent) options = replay_start(replay_commit(repo, {}, "empty root"));
    const auto result = replay_run(repo, reverting, options);
    REQUIRE(result.outcome == ReplayOutcome::Applied);
    REQUIRE(result.head.id != current);
    REQUIRE(read_commit(repo, *result.head.id).parents == std::vector<ObjectId>{current});
    REQUIRE(read_commit(repo, *result.head.id).tree == read_commit(repo, current).tree);
    REQUIRE(read_file(repo.metadata() / "index") == raw_index);
    REQUIRE(read_file(repo.root() / "file") == working);
    REQUIRE(replay_permissions(repo, "file") == 0640);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
}
TEST_CASE("Replay conflict continue is kind-aware preserves source author and uses resolved Index without Work reads", "[replay]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto fork = replay_fork(repo);
    bool reverting = false;
    SECTION("cherry-pick continue preserves source author and saved override") {}
    SECTION("revert continue uses current author and ignores unstaged FIFO") { reverting = true; }
    const auto head_bytes = read_file(repo.metadata() / "HEAD");
    const auto log_bytes = read_file(repo.metadata() / "logs/HEAD");
    const auto started = replay_run(repo, reverting, replay_start(fork.source, "saved message"));
    REQUIRE(started.outcome == ReplayOutcome::Conflicted);
    REQUIRE(started.head == Head{"refs/heads/main", fork.current});
    const auto state = replay_state(repo);
    REQUIRE(state.kind == (reverting ? OperationKind::Revert : OperationKind::CherryPick));
    REQUIRE(state.message == "saved message");
    REQUIRE(read_file(repo.metadata() / "HEAD") == head_bytes);
    REQUIRE(read_file(repo.metadata() / "logs/HEAD") == log_bytes);
    const auto unchanged = replay_metadata(repo);
    for (const auto action : {ReplayAction::Continue, ReplayAction::Abort})
        REQUIRE_THROWS_AS(replay_run(repo, !reverting, replay_action(action)), Error);
    for (const auto action : {MergeAction::Continue, MergeAction::Abort})
        REQUIRE_THROWS_AS(merge_repository(repo, {action, {}, std::nullopt}), Error);
    REQUIRE_THROWS_AS(replay_run(repo, reverting, replay_action(ReplayAction::Continue)), Error);
    REQUIRE(replay_metadata(repo) == unchanged);
    replay_file(repo, "file", "resolved"); replay_file(repo, "extra", "staged extra");
    add_paths(repo, {repo.root() / "file", repo.root() / "extra"});
    const auto raw_index = read_file(repo.metadata() / "index");
    if (reverting) { std::filesystem::remove(repo.root() / "file"); REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0); }
    else replay_file(repo, "file", "unstaged", 0640);
    replay_file(repo, "outside", "unstaged outside");
    const auto result = replay_run(repo, reverting, replay_action(ReplayAction::Continue));
    REQUIRE(result.outcome == ReplayOutcome::Applied);
    REQUIRE(read_file(repo.metadata() / "index") == raw_index);
    const auto commit = read_commit(repo, *result.head.id);
    REQUIRE(commit.parents == std::vector<ObjectId>{fork.current});
    REQUIRE(commit.message == "saved message");
    REQUIRE(commit.author == (reverting ? commit.committer : std::string(source_actor)));
    if (reverting) REQUIRE(std::filesystem::symlink_status(repo.root() / "file").type() == std::filesystem::file_type::fifo);
    else { REQUIRE(read_file(repo.root() / "file") == to_bytes("unstaged")); REQUIRE(replay_permissions(repo, "file") == 0640); }
    REQUIRE(read_file(repo.root() / "outside") == to_bytes("unstaged outside"));
    { RepoLock lock(repo.metadata()); REQUIRE(index_snapshot(repo) == read_snapshot(repo, commit.tree)); }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
}
TEST_CASE("Replay abort restores exact raw HEAD whole optional Index full permissions and DF affected directory ancestors", "[replay]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    ObjectId current, source;
    bool reverting = false, missing_index = false, directory = false;
    std::string original_content = "ours\n";
    SECTION("cherry-pick textual abort restores full mode and retains outside Work") {
        const auto fork = replay_fork(repo); current = fork.current; source = fork.source;
        replay_file(repo, "file", "ours\n", 0640);
    }
    SECTION("same-content Mode conflict saves an unchanged affected File") {
        current = replay_commit(repo, {{"file", {"same", "100644"}}}, "current");
        source = replay_commit(repo, {{"file", {"same", "100755"}}}, "root source");
        replay_install(repo, current); original_content = "same";
        replay_file(repo, "file", "same", 0640);
    }
    SECTION("revert abort restores missing original Index and original absence") {
        const auto parent = replay_commit(repo, {{"file", {"parent", "100644"}}}, "parent");
        source = replay_commit(repo, {{"file", {"source", "100644"}}}, "source", {parent});
        current = replay_commit(repo, {}, "independent empty current"); reverting = true; missing_index = true;
        replay_install(repo, current); std::filesystem::remove(repo.metadata() / "index");
    }
    SECTION("DF ours directory unchanged noops preserve root and nested full modes") {
        current = replay_commit(repo, {{"r/deep/file", {"ours", "100644"}}}, "current");
        source = replay_commit(repo, {{"r", {"source", "100644"}}}, "root source"); directory = true;
        replay_install(repo, current);
        REQUIRE(::chmod((repo.root() / "r").c_str(), 0750) == 0);
        REQUIRE(::chmod((repo.root() / "r/deep").c_str(), 0710) == 0);
        replay_file(repo, "r/deep/file", "ours", 0640);
    }
    SECTION("detached uppercase HEAD is restored with no fabricated reflog record") {
        const auto fork = replay_fork(repo); current = fork.current; source = fork.source;
        auto upper = current.hex();
        std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
        atomic_write(repo.metadata() / "HEAD", to_bytes(upper + "\n"));
        replay_file(repo, "file", "ours\n", 0640);
    }
    const auto head_bytes = read_file(repo.metadata() / "HEAD");
    const auto raw_index = missing_index ? std::optional<Bytes>{} : std::optional(read_file(repo.metadata() / "index"));
    const auto log = read_file(repo.metadata() / "logs/HEAD");
    REQUIRE(replay_run(repo, reverting, replay_start(source)).outcome == ReplayOutcome::Conflicted);
    const auto state = replay_state(repo);
    if (original_content == "same") REQUIRE(replay_saved(state, "file").before == replay_saved(state, "file").after);
    if (directory) {
        REQUIRE(replay_saved(state, "r").before == replay_saved(state, "r").after);
        REQUIRE(replay_saved(state, "r/deep").before.permissions == 0710);
        std::filesystem::remove(repo.root() / "r/deep/file"); std::filesystem::remove(repo.root() / "r/deep"); std::filesystem::remove(repo.root() / "r");
        replay_file(repo, "r", "resolved file"); add_paths(repo, {repo.root() / "r"});
    } else {
        std::filesystem::remove(repo.root() / "file");
        replay_file(repo, "file/new/deeper", "tracked resolution"); add_paths(repo, {repo.root() / "file"});
    }
    replay_file(repo, "extra", "outside addition"); add_paths(repo, {repo.root() / "extra"});
    replay_file(repo, "outside", "outside edit");
    if (!read_head(repo).symbolic) atomic_write(repo.metadata() / "HEAD", to_bytes(current.hex() + "\n"));
    const auto result = replay_run(repo, reverting, replay_action(ReplayAction::Abort));
    REQUIRE(result.outcome == ReplayOutcome::Aborted);
    REQUIRE(read_file(repo.metadata() / "HEAD") == head_bytes);
    REQUIRE(read_file(repo.metadata() / "logs/HEAD") == log);
    if (raw_index) REQUIRE(read_file(repo.metadata() / "index") == *raw_index);
    else REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "index"));
    REQUIRE(read_file(repo.root() / "extra") == to_bytes("outside addition"));
    REQUIRE(read_file(repo.root() / "outside") == to_bytes("outside edit"));
    if (directory) {
        REQUIRE(read_file(repo.root() / "r/deep/file") == to_bytes("ours"));
        REQUIRE(replay_permissions(repo, "r") == 0750); REQUIRE(replay_permissions(repo, "r/deep") == 0710);
        REQUIRE(replay_permissions(repo, "r/deep/file") == 0640);
    } else if (missing_index) REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
    else { REQUIRE(read_file(repo.root() / "file") == to_bytes(original_content)); REQUIRE(replay_permissions(repo, "file") == 0640); }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
}
TEST_CASE("Replay rejects dirty unborn unsafe multiple-parent and damaged source histories before mutable publication", "[replay]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const ReplayFiles files{{"file", {"base", "100644"}}};
    auto current = replay_commit(repo, files, "current");
    const auto parent = replay_commit(repo, files, "parent");
    auto source = replay_commit(repo, {{"file", {"source", "100644"}}, {"new", {"incoming", "100644"}}}, "source", {parent});
    bool unborn = false, reverting = false;
    SECTION("unborn start is explicitly unsupported") { unborn = true; }
    SECTION("revert rejects multi-parent source even if duplicate parent IDs") {
        source = replay_commit(repo, files, "merge source", {parent, parent}); reverting = true;
    }
    SECTION("cherry-pick rejects multi-parent source") { source = replay_commit(repo, files, "merge source", {parent, current}); }
    SECTION("blank default source message is rejected without override") { source = replay_commit(repo, files, " \n\t", {parent}); }
    SECTION("source-owned tree missing is rejected") {
        RepoLock lock(repo.metadata()); source = ObjectStore(repo.metadata() / "objects").write(ObjectType::Commit,
            encode_commit({ObjectId{}, {}, std::string(source_actor), std::string(source_actor), "bad tree"}));
    }
    SECTION("single source parent missing tree is rejected") {
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
        const auto bad = store.write(ObjectType::Commit, encode_commit({ObjectId{}, {}, std::string(source_actor), std::string(source_actor), "bad parent tree"}));
        source = store.write(ObjectType::Commit, encode_commit({read_commit(repo, parent).tree, {bad}, std::string(source_actor), std::string(source_actor), "bad source"}));
    }
    SECTION("deep source ancestor integrity is checked even for empty delta") {
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
        const auto bad = store.write(ObjectType::Commit, encode_commit({read_commit(repo, parent).tree, {ObjectId{}}, std::string(source_actor), std::string(source_actor), "missing ancestor"}));
        source = store.write(ObjectType::Commit, encode_commit({read_commit(repo, parent).tree, {bad}, std::string(source_actor), std::string(source_actor), "source empty"}));
    }
    SECTION("deep current ancestry is checked even when source delta is empty") {
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
        current = store.write(ObjectType::Commit, encode_commit({read_commit(repo, current).tree, {ObjectId{}}, std::string(source_actor), std::string(source_actor), "bad current"}));
    }
    if (!unborn) replay_install(repo, current);
    SECTION("staged change differs from HEAD") { replay_file(repo, "file", "staged"); add_paths(repo, {repo.root() / "file"}); }
    SECTION("unstaged executable change is dirty") { REQUIRE(::chmod((repo.root() / "file").c_str(), 0755) == 0); }
    SECTION("unknown ignored destination is protected") { replay_file(repo, ".minigitignore", "new\n"); replay_file(repo, "new", "private"); }
    SECTION("incoming empty directory is protected") { std::filesystem::create_directory(repo.root() / "new"); }
    SECTION("tracked symlink is rejected") { std::filesystem::remove(repo.root() / "file"); std::filesystem::create_symlink("new", repo.root() / "file"); }
    SECTION("tracked FIFO is rejected without blocking") { std::filesystem::remove(repo.root() / "file"); REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0); }
    SECTION("every Index stage must reference a Blob") { RepoLock lock(repo.metadata()); save_index(repo, {{"file", "100644", current, 1}}); }
    const auto before = replay_metadata(repo);
    REQUIRE_THROWS_AS(replay_run(repo, reverting, replay_start(source)), Error);
    REQUIRE(replay_metadata(repo) == before);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
TEST_CASE("Replay lifecycle guards kind HEAD CAS invalid resolved inputs and unknown abort children without losing State", "[replay]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto fork = replay_fork(repo);
    REQUIRE(cherry_pick_repository(repo, replay_start(fork.source)).outcome == ReplayOutcome::Conflicted);
    auto options = replay_action(ReplayAction::Abort);
    SECTION("Start refuses active operation") { options = replay_start(fork.source); }
    SECTION("logical branch movement invalidates current Head CAS") { atomic_write(repo.metadata() / "refs/heads/main", to_bytes(fork.source.hex() + "\n")); }
    SECTION("continue refuses unresolved Index") { options = replay_action(ReplayAction::Continue); }
    SECTION("continue refuses valid-schema non-Blob stage zero") {
        RepoLock lock(repo.metadata()); save_index(repo, {{"file", "100644", fork.source}}); options = replay_action(ReplayAction::Continue);
    }
    SECTION("continue cannot bypass source multi-parent policy using manually altered valid State") {
        auto state = replay_state(repo);
        state.source_commit = replay_commit(repo, {}, "merge source", {fork.source, fork.current});
        atomic_write(repo.metadata() / "operation", encode_operation(state));
        replay_file(repo, "file", "resolved"); add_paths(repo, {repo.root() / "file"}); options = replay_action(ReplayAction::Continue);
    }
    SECTION("continue refuses missing identity without changing Index or State") {
        replay_file(repo, "file", "resolved"); add_paths(repo, {repo.root() / "file"});
        std::filesystem::remove(repo.metadata() / "identity"); options = replay_action(ReplayAction::Continue);
    }
    SECTION("abort refuses corrupt current Index") { atomic_write(repo.metadata() / "index", to_bytes("bad")); }
    SECTION("continue and abort forbid new source or message") { options.message = "new message"; }
    SECTION("new unknown file blocks restoration of original File") {
        std::filesystem::remove(repo.root() / "file"); replay_file(repo, "file/tracked", "resolved"); add_paths(repo, {repo.root() / "file"});
        replay_file(repo, "file/unknown", "keep");
    }
    SECTION("new unknown empty directory blocks restoration of original File") {
        std::filesystem::remove(repo.root() / "file"); replay_file(repo, "file/tracked", "resolved"); add_paths(repo, {repo.root() / "file"});
        std::filesystem::create_directory(repo.root() / "file/unknown-empty");
    }
    const auto before = replay_metadata(repo);
    REQUIRE_THROWS_AS(cherry_pick_repository(repo, options), Error);
    REQUIRE(replay_metadata(repo) == before);
    REQUIRE(std::filesystem::exists(repo.metadata() / "operation"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
TEST_CASE("Replay durable conflict preparation and committed completion recover once with typed State roots", "[replay]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto fork = replay_fork(repo);
    SECTION("Prepared cherry-pick conflict restores original Work Index and absent State") {
        replay_file(repo, "file", "ours\n", 0640);
        const auto before = replay_metadata(repo);
        bool reached = false;
        REQUIRE_THROWS_AS(cherry_pick_repository(repo, replay_start(fork.source), [&](std::size_t, std::string_view point) {
            if (point == "journal.prepared") { reached = true; throw Error("fixture stop"); }
        }), Error);
        REQUIRE(reached);
        { RepoLock lock(repo.metadata()); REQUIRE(transaction_roots(repo).contains(fork.source)); recover_transaction(repo); recover_transaction(repo); }
        REQUIRE(replay_metadata(repo) == before);
        REQUIRE(read_file(repo.root() / "file") == to_bytes("ours\n")); REQUIRE(replay_permissions(repo, "file") == 0640);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    }
    SECTION("Committed revert continue recovers one single-parent commit and one log append") {
        REQUIRE(revert_repository(repo, replay_start(fork.source)).outcome == ReplayOutcome::Conflicted);
        replay_file(repo, "file", "resolved"); add_paths(repo, {repo.root() / "file"}); replay_file(repo, "file", "unstaged", 0640);
        const auto raw_index = read_file(repo.metadata() / "index");
        bool reached = false;
        REQUIRE_THROWS_AS(revert_repository(repo, replay_action(ReplayAction::Continue), [&](std::size_t, std::string_view point) {
            if (point == "journal.committed") { reached = true; throw Error("fixture stop"); }
        }), Error);
        REQUIRE(reached);
        Head desired;
        { RepoLock lock(repo.metadata()); REQUIRE(transaction_roots(repo).contains(fork.source)); recover_transaction(repo);
          desired = read_head(repo); const auto log = read_reflog(repo, "HEAD");
          REQUIRE(log.size() == 2); REQUIRE(log.back() == ReflogEntry{fork.current, desired.id, "revert:continue"});
          recover_transaction(repo); REQUIRE(read_head(repo) == desired); REQUIRE(read_reflog(repo, "HEAD") == log); }
        REQUIRE(read_commit(repo, *desired.id).parents == std::vector<ObjectId>{fork.current});
        REQUIRE(read_file(repo.metadata() / "index") == raw_index); REQUIRE(read_file(repo.root() / "file") == to_bytes("unstaged"));
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    }
}
