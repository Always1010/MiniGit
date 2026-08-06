#include <catch2/catch_test_macros.hpp>
#include "minigit/rebase.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
#include <sys/stat.h>
using namespace minigit;
namespace {
using RebaseFiles = std::map<std::string, std::pair<std::string, std::string>>;
constexpr std::string_view rebase_author = "Source <source@example.test> 0 +0000";
ObjectId rb_commit(const Repository& repo, const RebaseFiles& files, std::string_view message,
                    const std::vector<ObjectId>& parents = {}) {
    RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects"); Snapshot snapshot;
    for (const auto& [path, file] : files)
        snapshot.emplace_hint(snapshot.end(), path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    return store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, snapshot), parents,
        std::string(rebase_author), "Fixture <fixture@example.test> 0 +0000", std::string(message)}));
}
void rb_file(const Repository& repo, std::string_view path, std::string_view content, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path); detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(content), permissions);
}
void rb_identity(const Repository& repo) {
    atomic_write(repo.metadata() / "identity", to_bytes("version=1\nuser.name=Current\nuser.email=current@example.test\n"));
}
void rb_install(const Repository& repo, ObjectId id, bool detached = false, bool identity = true) {
    RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects"); Index index;
    for (const auto& [path, file] : read_snapshot(repo, read_commit(repo, id).tree)) {
        rb_file(repo, path, to_string(store.read(file.id).payload), file.mode == "100755" ? 0755U : 0644U);
        index.push_back({path, file.mode, file.id});
    }
    save_index(repo, index);
    if (detached) atomic_write(repo.metadata() / "HEAD", to_bytes(id.hex() + "\n"));
    else update_ref(repo, "refs/heads/main", std::nullopt, id);
    const auto log = encode_reflog({{std::nullopt, id, "fixture"}});
    detail::ensure_directory(repo.metadata() / "logs/refs/heads"); atomic_write(repo.metadata() / "logs/HEAD", log);
    if (!detached) atomic_write(repo.metadata() / "logs/refs/heads/main", log);
    if (identity) rb_identity(repo);
}
void rb_stage(const Repository& repo, const RebaseFiles& files) {
    RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects"); Index index;
    for (const auto& [path, file] : files) index.push_back({path, file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    save_index(repo, index);
}
std::map<std::string, Bytes> rb_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && std::filesystem::is_regular_file(entry.symlink_status())) result.emplace(path, read_file(entry.path()));
    }
    return result;
}
OperationState rb_state(const Repository& repo) {
    RepoLock lock(repo.metadata()); const auto state = read_operation(repo); REQUIRE(state); return *state;
}
const WorkChange& rb_saved(const OperationState& state, std::string_view path) {
    const auto found = std::find_if(state.work.begin(), state.work.end(), [&](const auto& change) { return change.path == path; });
    REQUIRE(found != state.work.end()); return *found;
}
std::uint32_t rb_permissions(const Repository& repo, std::string_view path) {
    return static_cast<std::uint32_t>(std::filesystem::symlink_status(repo.root() / path).permissions()) & 07777;
}
RebaseOptions rb_start(ObjectId onto) { return {RebaseAction::Start, onto.hex()}; }
RebaseOptions rb_action(RebaseAction action) { return {action, {}}; }
void rb_recover(const Repository& repo) { RepoLock lock(repo.metadata()); recover_transaction(repo); recover_transaction(repo); }
struct RebaseFork { ObjectId base, onto, first, second, tip; };
RebaseFork rb_three(const Repository& repo, bool empty_middle = false) {
    const auto base = rb_commit(repo, {{"file", {"base", "100644"}}}, "base");
    const auto onto = rb_commit(repo, {{"file", {"base", "100644"}}, {"onto", {"keep", "100644"}}}, "onto", {base});
    const auto first = rb_commit(repo, {{"file", {"one", "100644"}}}, "first message", {base});
    const auto second = rb_commit(repo, {{"file", {empty_middle ? "one" : "two", "100644"}}}, "second message", {first});
    const auto tip = rb_commit(repo, {{"file", {"three", "100755"}}}, "third message", {second});
    return {base, onto, first, second, tip};
}
}
TEST_CASE("Rebase validates full history before up-to-date and fast-forward without identity or State", "[rebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    const auto base = rb_commit(repo, {{"file", {"base", "100644"}}}, "base");
    const auto tip = rb_commit(repo, {{"file", {"tip", "100755"}}, {"empty", {"", "100644"}}}, "tip", {base});
    bool forward = false, corrupt = false;
    SECTION("up-to-date ancestor keeps exact metadata and Work permissions") { rb_install(repo, tip, false, false); rb_file(repo, "file", "tip", 0751); }
    SECTION("same tip still validates its complete ancestor closure") {
        const auto broken = rb_commit(repo, {{"file", {"base", "100644"}}}, "damaged ancestry", {ObjectId{}});
        rb_install(repo, broken, false, false); corrupt = true;
    }
    SECTION("fast-forward keeps symbolic branch and accepts exact unknown target noops") {
        rb_install(repo, base, false, false); forward = true; rb_file(repo, "empty", "");
    }
    const auto before = rb_metadata(repo);
    if (corrupt) {
        REQUIRE_THROWS_AS(rebase_repository(repo, rb_start(*read_head(repo).id)), Error); REQUIRE(rb_metadata(repo) == before);
    } else {
        const auto result = rebase_repository(repo, rb_start(forward ? tip : base));
        REQUIRE(result.outcome == (forward ? RebaseOutcome::Applied : RebaseOutcome::UpToDate));
        REQUIRE(result.head.id == tip); REQUIRE(result.head.symbolic == "refs/heads/main");
        if (forward) { REQUIRE(read_file(repo.root() / "file") == to_bytes("tip")); REQUIRE(rb_permissions(repo, "file") == 0755); }
        else { REQUIRE(rb_metadata(repo) == before); REQUIRE(rb_permissions(repo, "file") == 0751); }
    }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation")); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "identity"));
}
TEST_CASE("Rebase replays three oldest-first sources preserves empty commits authors and original branch until finish", "[rebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); bool detached = false, empty = false;
    SECTION("symbolic branch records one final branch movement") {}
    SECTION("original detached HEAD remains detached") { detached = true; }
    SECTION("empty middle source remains a new single-parent commit") { empty = true; }
    const auto fork = rb_three(repo, empty); rb_install(repo, fork.tip, detached);
    rb_file(repo, "unknown", "private", 0600); const auto identity = read_file(repo.metadata() / "identity");
    std::size_t publications = 0;
    const auto result = rebase_repository(repo, rb_start(fork.onto), [&](std::size_t, std::string_view point) {
        if (point == "journal.committed" && ++publications < 4 && !detached) REQUIRE(read_ref(repo, "refs/heads/main") == fork.tip);
    });
    REQUIRE(result.outcome == RebaseOutcome::Applied); REQUIRE(result.head.symbolic == (detached ? std::optional<std::string>{} : std::optional<std::string>{"refs/heads/main"}));
    auto rewritten = *result.head.id;
    for (const auto& [source, message] : std::vector<std::pair<ObjectId, std::string>>{{fork.tip, "third message"}, {fork.second, "second message"}, {fork.first, "first message"}}) {
        const auto commit = read_commit(repo, rewritten); REQUIRE(commit.parents.size() == 1); REQUIRE(commit.author == rebase_author);
        REQUIRE(commit.committer.starts_with("Current <current@example.test> ")); REQUIRE(commit.message == message); REQUIRE(rewritten != source);
        rewritten = commit.parents.front();
    }
    REQUIRE(rewritten == fork.onto); REQUIRE(read_file(repo.root() / "file") == to_bytes("three")); REQUIRE(rb_permissions(repo, "file") == 0755);
    REQUIRE(read_file(repo.root() / "onto") == to_bytes("keep")); REQUIRE(read_file(repo.root() / "unknown") == to_bytes("private"));
    REQUIRE(read_file(repo.metadata() / "identity") == identity); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    if (!detached) { const auto log = read_reflog(repo, "refs/heads/main"); REQUIRE(log.size() == 2); REQUIRE(log.back().old_id == fork.tip); REQUIRE(log.back().new_id == result.head.id); }
}
TEST_CASE("Rebase Ready failures retry pending source and exact original branch CAS blocks both continue and abort", "[rebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    const auto base = rb_commit(repo, {}, "base");
    const auto onto = rb_commit(repo, {{"onto", {"keep", "100644"}}}, "onto", {base});
    const auto tip = rb_commit(repo, {{"incoming", {"tracked", "100644"}}}, "source", {base}); rb_install(repo, tip, false, false);
    auto upper = tip.hex() + "\n"; std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    atomic_write(repo.metadata() / "refs/heads/main", to_bytes(upper));
    const auto original_index = read_file(repo.metadata() / "index");
    REQUIRE_THROWS_AS(rebase_repository(repo, rb_start(onto)), Error);
    const auto ready = rb_state(repo); REQUIRE(ready.phase == OperationPhase::Ready); REQUIRE(ready.rebase->cursor == 0);
    REQUIRE(ready.source_commit == tip); REQUIRE(ready.current_head.id == onto); REQUIRE(ready.rebase->original_branch_ref == to_bytes(upper));
    SECTION("unknown overwrite refuses retry then actual absence allows the same source") {
        rb_identity(repo); rb_file(repo, "incoming", "private", 0600); const auto before = rb_metadata(repo);
        REQUIRE_THROWS_AS(rebase_repository(repo, rb_action(RebaseAction::Continue)), Error); REQUIRE(rb_metadata(repo) == before);
        REQUIRE(rb_state(repo).rebase->cursor == 0); REQUIRE(read_file(repo.root() / "incoming") == to_bytes("private"));
        std::filesystem::remove(repo.root() / "incoming");
        REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Continue)).outcome == RebaseOutcome::Applied);
        REQUIRE(read_file(repo.root() / "incoming") == to_bytes("tracked"));
    }
    SECTION("logical same ref with changed case is rejected until exact image is restored") {
        rb_identity(repo); atomic_write(repo.metadata() / "refs/heads/main", to_bytes(tip.hex() + "\n")); const auto before = rb_metadata(repo);
        REQUIRE_THROWS_AS(rebase_repository(repo, rb_action(RebaseAction::Continue)), Error);
        REQUIRE_THROWS_AS(rebase_repository(repo, rb_action(RebaseAction::Abort)), Error); REQUIRE(rb_metadata(repo) == before);
        atomic_write(repo.metadata() / "refs/heads/main", to_bytes(upper));
        REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Abort)).outcome == RebaseOutcome::Aborted);
        REQUIRE(read_file(repo.metadata() / "refs/heads/main") == to_bytes(upper)); REQUIRE(read_file(repo.metadata() / "index") == original_index);
    }
    SECTION("missing identity leaves Ready and abort requires no identity") {
        REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Abort)).outcome == RebaseOutcome::Aborted);
        REQUIRE(read_head(repo).id == tip); REQUIRE(read_file(repo.root() / "incoming") == to_bytes("tracked")); REQUIRE_FALSE(std::filesystem::exists(repo.root() / "onto"));
    }
}
TEST_CASE("Rebase Conflicted continuation preserves FIFO Work only at final source and persists nonfinal Ready before dirty rejection", "[rebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); bool another = false;
    SECTION("final resolved source does not read or rewrite unstaged FIFO") {}
    SECTION("nonfinal resolved source advances Ready then rejects dirty next baseline") { another = true; }
    const auto base = rb_commit(repo, {{"file", {"base\n", "100644"}}}, "base");
    const auto onto = rb_commit(repo, {{"file", {"ours\n", "100644"}}}, "onto", {base});
    const auto first = rb_commit(repo, {{"file", {"theirs\n", "100644"}}}, "first", {base});
    const auto tip = another ? rb_commit(repo, {{"file", {"theirs\n", "100644"}}, {"next", {"next", "100644"}}}, "second", {first}) : first;
    rb_install(repo, tip); const auto conflict = rebase_repository(repo, rb_start(onto)); REQUIRE(conflict.outcome == RebaseOutcome::Conflicted);
    rb_stage(repo, {{"file", {"resolved", "100644"}}}); std::filesystem::remove(repo.root() / "file"); REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0);
    const auto resolved_index = read_file(repo.metadata() / "index");
    if (another) {
        REQUIRE_THROWS_AS(rebase_repository(repo, rb_action(RebaseAction::Continue)), Error);
        const auto ready = rb_state(repo); REQUIRE(ready.phase == OperationPhase::Ready); REQUIRE(ready.rebase->cursor == 1); REQUIRE(ready.source_commit == tip);
        REQUIRE(read_commit(repo, *ready.current_head.id).parents == std::vector<ObjectId>{onto}); REQUIRE(read_ref(repo, "refs/heads/main") == tip);
        REQUIRE(read_file(repo.metadata() / "index") == resolved_index); REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "file")));
        std::filesystem::remove(repo.root() / "file"); rb_file(repo, "file", "resolved");
        REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Continue)).outcome == RebaseOutcome::Applied); REQUIRE(read_file(repo.root() / "next") == to_bytes("next"));
    } else {
        const auto result = rebase_repository(repo, rb_action(RebaseAction::Continue)); REQUIRE(result.outcome == RebaseOutcome::Applied);
        REQUIRE(read_commit(repo, *result.head.id).parents == std::vector<ObjectId>{onto}); REQUIRE(read_commit(repo, *result.head.id).author == rebase_author);
        REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "file"))); REQUIRE(read_file(repo.metadata() / "index") == resolved_index);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    }
}
TEST_CASE("Rebase cumulative original File and Absent ancestors survive repeated Directory changes and exact abort", "[rebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); bool missing_index = false, resolved_directory = false;
    SECTION("new children changed twice remain originally absent and unknown descendants block abort") {}
    SECTION("detached empty original preserves missing Index and uppercase raw HEAD") { missing_index = true; }
    SECTION("resolved File ancestor refreshes to actual unchanged Directory for the next child conflict") { resolved_directory = true; }
    if (missing_index) {
        const auto base = rb_commit(repo, {}, "base"); const auto onto = rb_commit(repo, {{"onto", {"new", "100644"}}}, "onto", {base});
        const auto first = rb_commit(repo, {}, "first empty", {base}); const auto tip = rb_commit(repo, {}, "second empty", {first});
        rb_install(repo, tip, true, false); std::filesystem::remove(repo.metadata() / "index");
        auto raw = tip.hex() + "\n"; std::transform(raw.begin(), raw.end(), raw.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
        atomic_write(repo.metadata() / "HEAD", to_bytes(raw)); REQUIRE_THROWS_AS(rebase_repository(repo, rb_start(onto)), Error);
        REQUIRE_FALSE(rb_state(repo).original_index_blob); REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Abort)).outcome == RebaseOutcome::Aborted);
        REQUIRE(read_file(repo.metadata() / "HEAD") == to_bytes(raw)); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "index")); REQUIRE_FALSE(std::filesystem::exists(repo.root() / "onto"));
    } else if (resolved_directory) {
        const auto base = rb_commit(repo, {{"p", {"base\n", "100644"}}}, "base");
        const auto onto = rb_commit(repo, {{"p", {"ours\n", "100644"}}}, "onto", {base});
        const auto first = rb_commit(repo, {{"p/child", {"one\n", "100644"}}}, "first", {base});
        const auto second = rb_commit(repo, {{"p/child", {"two\n", "100644"}}}, "second", {first});
        const auto tip = rb_commit(repo, {{"p", {"tip", "100755"}}}, "last", {second}); rb_install(repo, tip); rb_file(repo, "p", "tip", 0751);
        const auto raw_head = read_file(repo.metadata() / "HEAD"), raw_index = read_file(repo.metadata() / "index");
        const auto first_conflict = rebase_repository(repo, rb_start(onto)); REQUIRE(first_conflict.outcome == RebaseOutcome::Conflicted);
        REQUIRE(first_conflict.conflicts.front().kind == MergeConflictKind::DirectoryFile);
        REQUIRE(rb_saved(rb_state(repo), "p").after.kind == NodeKind::File);
        std::filesystem::remove(repo.root() / "p"); rb_file(repo, "p/child", "resolved\n", 0640);
        std::filesystem::permissions(repo.root() / "p", static_cast<std::filesystem::perms>(0750));
        rb_stage(repo, {{"p/child", {"resolved\n", "100644"}}});
        const auto next_conflict = rebase_repository(repo, rb_action(RebaseAction::Continue)); REQUIRE(next_conflict.outcome == RebaseOutcome::Conflicted);
        REQUIRE(next_conflict.conflicts.front().path == "p/child"); REQUIRE(next_conflict.conflicts.front().kind == MergeConflictKind::Text);
        const auto state = rb_state(repo); REQUIRE(state.phase == OperationPhase::Conflicted); REQUIRE(state.rebase->cursor == 1);
        REQUIRE(rb_saved(state, "p").before.kind == NodeKind::File); REQUIRE(rb_saved(state, "p").before.permissions == 0751);
        REQUIRE(rb_saved(state, "p").after.kind == NodeKind::Directory); REQUIRE(rb_saved(state, "p").after.permissions == 0750);
        REQUIRE(rb_saved(state, "p/child").before.kind == NodeKind::Absent); REQUIRE(rb_saved(state, "p/child").after.kind == NodeKind::File);
        REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Abort)).outcome == RebaseOutcome::Aborted);
        REQUIRE(read_file(repo.root() / "p") == to_bytes("tip")); REQUIRE(rb_permissions(repo, "p") == 0751);
        REQUIRE(read_file(repo.metadata() / "HEAD") == raw_head); REQUIRE(read_file(repo.metadata() / "index") == raw_index);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    } else {
        const auto base = rb_commit(repo, {{"p", {"base", "100644"}}}, "base");
        const auto onto = rb_commit(repo, {{"p", {"base", "100644"}}, {"onto", {"keep", "100644"}}}, "onto", {base});
        const auto first = rb_commit(repo, {{"p/new", {"one", "100644"}}}, "one", {base});
        const auto second = rb_commit(repo, {{"p/new", {"two", "100644"}}}, "two", {first});
        const auto tip = rb_commit(repo, {{"p", {"tip", "100755"}}}, "three", {second}); rb_install(repo, tip); rb_file(repo, "p", "tip", 0751);
        const auto raw_head = read_file(repo.metadata() / "HEAD"), raw_index = read_file(repo.metadata() / "index"); std::size_t committed = 0;
        REQUIRE_THROWS_AS(rebase_repository(repo, rb_start(onto), [&](std::size_t, std::string_view point) {
            if (point == "journal.committed" && ++committed == 3) throw Error("stop after two sources");
        }), Error); rb_recover(repo);
        const auto ready = rb_state(repo); REQUIRE(ready.rebase->cursor == 2); REQUIRE(rb_saved(ready, "p").before.kind == NodeKind::File);
        REQUIRE(rb_saved(ready, "p/new").before.kind == NodeKind::Absent); REQUIRE(read_file(repo.root() / "p/new") == to_bytes("two"));
        rb_file(repo, "p/unknown", "private", 0600); const auto before = rb_metadata(repo);
        REQUIRE_THROWS_AS(rebase_repository(repo, rb_action(RebaseAction::Abort)), Error); REQUIRE(rb_metadata(repo) == before);
        REQUIRE(read_file(repo.root() / "p/unknown") == to_bytes("private")); std::filesystem::remove(repo.root() / "p/unknown");
        REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Abort)).outcome == RebaseOutcome::Aborted);
        REQUIRE(read_file(repo.root() / "p") == to_bytes("tip")); REQUIRE(rb_permissions(repo, "p") == 0751);
        REQUIRE(read_file(repo.metadata() / "HEAD") == raw_head); REQUIRE(read_file(repo.metadata() / "index") == raw_index);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "onto")); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    }
}
TEST_CASE("Rebase cumulative DF conflict noops retain original Directory modes and normalize removed old descendants", "[rebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    const auto base = rb_commit(repo, {{"p/deep/file", {"base", "100644"}}, {"p-other", {"keep", "100644"}}}, "base");
    const auto onto = rb_commit(repo, {{"p/deep/file", {"ours", "100644"}}, {"p-other", {"keep", "100644"}}}, "onto", {base});
    const auto first = rb_commit(repo, {{"p", {"theirs", "100644"}}, {"p-other", {"keep", "100644"}}}, "first", {base});
    const auto tip = rb_commit(repo, {{"p/deep/file", {"tip", "100755"}}, {"p-other", {"keep", "100644"}}}, "second", {first}); rb_install(repo, tip);
    std::filesystem::permissions(repo.root() / "p", static_cast<std::filesystem::perms>(0750));
    std::filesystem::permissions(repo.root() / "p/deep", static_cast<std::filesystem::perms>(0710)); rb_file(repo, "p/deep/file", "tip", 0751); rb_file(repo, "p-other", "keep", 0640);
    const auto raw_index = read_file(repo.metadata() / "index"), raw_head = read_file(repo.metadata() / "HEAD");
    const auto conflict = rebase_repository(repo, rb_start(onto)); REQUIRE(conflict.outcome == RebaseOutcome::Conflicted);
    REQUIRE(conflict.conflicts.front().kind == MergeConflictKind::DirectoryFile);
    auto state = rb_state(repo); REQUIRE(rb_saved(state, "p").before.permissions == 0750); REQUIRE(rb_saved(state, "p/deep").before.permissions == 0710);
    std::filesystem::remove(repo.root() / "p/deep/file"); std::filesystem::remove(repo.root() / "p/deep"); std::filesystem::remove(repo.root() / "p");
    rb_file(repo, "p", "resolution"); rb_stage(repo, {{"p", {"resolution", "100644"}}, {"p-other", {"keep", "100644"}}});
    REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Continue)).outcome == RebaseOutcome::Conflicted);
    state = rb_state(repo); REQUIRE(state.rebase->cursor == 1); REQUIRE(rb_saved(state, "p").before.kind == NodeKind::Directory);
    REQUIRE(rb_saved(state, "p/deep").after.kind == NodeKind::Absent); REQUIRE(rb_saved(state, "p/deep/file").after.kind == NodeKind::Absent);
    REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Abort)).outcome == RebaseOutcome::Aborted);
    REQUIRE(rb_permissions(repo, "p") == 0750); REQUIRE(rb_permissions(repo, "p/deep") == 0710); REQUIRE(rb_permissions(repo, "p/deep/file") == 0751);
    REQUIRE(read_file(repo.root() / "p/deep/file") == to_bytes("tip")); REQUIRE(read_file(repo.root() / "p-other") == to_bytes("keep")); REQUIRE(rb_permissions(repo, "p-other") == 0640);
    REQUIRE(read_file(repo.metadata() / "index") == raw_index); REQUIRE(read_file(repo.metadata() / "HEAD") == raw_head);
}
TEST_CASE("Rebase rejects unsafe starts incomplete source snapshots nonlinear queues dirty baselines and pending guards", "[rebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); const auto fork = rb_three(repo); rb_install(repo, fork.tip);
    auto options = rb_start(fork.onto);
    SECTION("dirty tracked Work") { rb_file(repo, "file", "dirty", 0755); }
    SECTION("dirty Index") { rb_stage(repo, {}); }
    SECTION("unresolved Index") { RepoLock lock(repo.metadata()); const auto index = load_index(repo); auto conflicts = index; conflicts[0].stage = 2; save_index(repo, conflicts); }
    SECTION("missing final source Tree is rejected before detached Ready publication") {
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(read_commit(repo, fork.second).tree));
    }
    SECTION("blank later source message is rejected before initial publication") {
        const auto blank = rb_commit(repo, {{"file", {"blank", "100644"}}}, " \n\t", {fork.tip});
        { RepoLock lock(repo.metadata()); update_ref(repo, "refs/heads/main", fork.tip, blank); } rb_stage(repo, {{"file", {"blank", "100644"}}}); rb_file(repo, "file", "blank");
    }
    SECTION("source merge commit is not accepted as linear first-parent queue") {
        const auto merge = rb_commit(repo, {{"file", {"three", "100755"}}}, "merge", {fork.tip, fork.base});
        { RepoLock lock(repo.metadata()); update_ref(repo, "refs/heads/main", fork.tip, merge); }
    }
    SECTION("unrelated onto") { options = rb_start(rb_commit(repo, {}, "unrelated")); }
    SECTION("invalid action arguments") { options = {RebaseAction::Continue, "HEAD"}; }
    SECTION("unborn HEAD") { std::filesystem::remove(repo.metadata() / "refs/heads/main"); }
    SECTION("pending transaction") { atomic_write(repo.metadata() / "transaction", to_bytes("pending")); }
    SECTION("active operation node") { atomic_write(repo.metadata() / "operation", to_bytes("damaged active state")); }
    const auto before = rb_metadata(repo); REQUIRE_THROWS_AS(rebase_repository(repo, options), Error); REQUIRE(rb_metadata(repo) == before);
}
TEST_CASE("Rebase Prepared start and Committed intermediate step recover exact durable progress and source roots", "[rebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); const auto fork = rb_three(repo); rb_install(repo, fork.tip);
    rb_file(repo, "file", "three", 0751); const auto original = rb_metadata(repo); bool committed = false;
    SECTION("Prepared initial checkout returns original raw metadata and Work") {}
    SECTION("Committed first source resumes from next Ready rather than reapplying it") { committed = true; }
    std::size_t boundaries = 0; bool reached = false;
    REQUIRE_THROWS_AS(rebase_repository(repo, rb_start(fork.onto), [&](std::size_t, std::string_view point) {
        if (point == (committed ? "journal.committed" : "journal.prepared") && ++boundaries == (committed ? 2U : 1U)) {
            reached = true; throw Error("stop durable boundary");
        }
    }), Error); REQUIRE(reached);
    { RepoLock lock(repo.metadata()); const auto roots = transaction_roots(repo);
        for (const auto id : {fork.onto, fork.first, fork.second, fork.tip}) REQUIRE(roots.contains(id)); }
    rb_recover(repo);
    if (committed) {
        const auto ready = rb_state(repo); REQUIRE(ready.phase == OperationPhase::Ready); REQUIRE(ready.rebase->cursor == 1); REQUIRE(ready.source_commit == fork.second);
        REQUIRE(read_commit(repo, *ready.current_head.id).parents == std::vector<ObjectId>{fork.onto}); REQUIRE(read_ref(repo, "refs/heads/main") == fork.tip);
        REQUIRE(rebase_repository(repo, rb_action(RebaseAction::Continue)).outcome == RebaseOutcome::Applied);
        REQUIRE(read_file(repo.root() / "file") == to_bytes("three"));
    } else { REQUIRE(rb_metadata(repo) == original); REQUIRE(read_file(repo.root() / "file") == to_bytes("three")); REQUIRE(rb_permissions(repo, "file") == 0751); REQUIRE_FALSE(std::filesystem::exists(repo.root() / "onto")); }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction")); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
}
