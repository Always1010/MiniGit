#include <catch2/catch_test_macros.hpp>
#include "minigit/stash.hpp"
#include "minigit/stash_stack.hpp"
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
using RestoreFiles = std::map<std::string, std::pair<std::string, std::string>>;
ObjectId restore_commit(const Repository& repo, const RestoreFiles& files, const std::vector<ObjectId>& parents = {}, std::string_view message = "fixture") {
    RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects"); Snapshot snapshot;
    for (const auto& [path, file] : files) snapshot.emplace_hint(snapshot.end(), path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    return store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, snapshot), parents, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
void restore_file(const Repository& repo, std::string_view path, std::string_view content, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / path; detail::ensure_directory(target.parent_path()); atomic_write_with_permissions(target, to_bytes(content), permissions);
}
void restore_install(const Repository& repo, ObjectId head) {
    RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects"); Index index;
    for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, head).tree)) {
        restore_file(repo, path, to_string(store.read(state.id).payload), state.mode == "100755" ? 0755U : 0644U);
        index.push_back({path, state.mode, state.id});
    }
    save_index(repo, index); update_ref(repo, "refs/heads/main", read_ref(repo, "refs/heads/main"), head);
    detail::ensure_directory(repo.metadata() / "logs/refs/heads");
    const auto log = encode_reflog({{std::nullopt, head, "fixture"}});
    atomic_write(repo.metadata() / "logs/HEAD", log); atomic_write(repo.metadata() / "logs/refs/heads/main", log);
}
ObjectId restore_stash(const Repository& repo, const RestoreFiles& head, const RestoreFiles& index, const RestoreFiles& work,
                       const RestoreFiles& untracked = {}) {
    const auto h = restore_commit(repo, head); const auto i = restore_commit(repo, index, {h}); std::vector<ObjectId> parents{h, i};
    if (!untracked.empty()) parents.push_back(restore_commit(repo, untracked));
    const auto w = restore_commit(repo, work, parents, " "); // No source message or identity requirement.
    { RepoLock lock(repo.metadata()); detail::ensure_directory(repo.metadata() / "refs");
      atomic_write(repo.metadata() / "stashstack", encode_stash_stack({w})); atomic_write(repo.metadata() / "refs/stash", to_bytes(w.hex() + "\n")); }
    return w;
}
std::map<std::string, Bytes> restore_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && std::filesystem::is_regular_file(entry.symlink_status())) result.emplace(path, read_file(entry.path()));
    }
    return result;
}
void restore_history_unchanged(const Repository& repo, const std::map<std::string, Bytes>& before) {
    for (const auto& [path, bytes] : before)
        if (path == "HEAD" || path.starts_with("refs/heads/") || path.starts_with("logs/")) REQUIRE(read_file(repo.metadata() / path) == bytes);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "identity"));
}
std::string restore_index_content(const Repository& repo, const std::string& path) {
    const auto snapshot = index_snapshot(repo); return to_string(ObjectStore(repo.metadata() / "objects").read(snapshot.at(path).id).payload);
}
StashStackState restore_stack(const Repository& repo) { RepoLock lock(repo.metadata()); return read_stash_stack(repo); }
OperationState restore_state(const Repository& repo) { RepoLock lock(repo.metadata()); const auto state = read_operation(repo); REQUIRE(state); return *state; }
std::uint32_t restore_permissions(const Repository& repo, std::string_view path) {
    return static_cast<std::uint32_t>(std::filesystem::symlink_status(repo.root() / path).permissions()) & 07777;
}
void restore_stage(const Repository& repo, const RestoreFiles& files) {
    RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects"); Index index;
    for (const auto& [path, file] : files) index.push_back({path, file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    save_index(repo, index);
}
void restore_recover(const Repository& repo) { RepoLock lock(repo.metadata()); recover_transaction(repo); recover_transaction(repo); }
}
TEST_CASE("StashRestore preserves independent staged and actual Work layers including missing and deleted files", "[stashrestore]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    RestoreFiles h{{"file", {"A", "100644"}}}, i{{"file", {"B", "100755"}}}, w{{"file", {"C", "100644"}}};
    bool indexed = false; std::string scenario = "ordinary";
    SECTION("default retains current raw Index and restores W only") {}
    SECTION("index independently restores B and C") { indexed = true; }
    SECTION("staged delete still leaves saved Work present") { indexed = true; i.clear(); w = h; scenario = "delete"; }
    SECTION("staged add can remain missing from saved Work") { indexed = true; h.clear(); w.clear(); i = {{"new", {"B", "100644"}}}; scenario = "add"; }
    restore_stash(repo, h, i, w); const auto current = restore_commit(repo, h); restore_install(repo, current);
    const auto before = restore_metadata(repo); const auto raw_index = read_file(repo.metadata() / "index");
    const auto result = stash_restore(repo, {0, indexed, false});
    REQUIRE(result.outcome == StashRestoreOutcome::Applied); REQUIRE_FALSE(result.dropped); REQUIRE(result.conflicts.empty());
    if (scenario == "ordinary") {
        REQUIRE(read_file(repo.root() / "file") == to_bytes("C")); REQUIRE(restore_index_content(repo, "file") == (indexed ? "B" : "A"));
        REQUIRE(index_snapshot(repo).at("file").mode == (indexed ? "100755" : "100644"));
    } else if (scenario == "delete") {
        REQUIRE(index_snapshot(repo).empty()); REQUIRE(read_file(repo.root() / "file") == to_bytes("A"));
    } else { REQUIRE(restore_index_content(repo, "new") == "B"); REQUIRE_FALSE(std::filesystem::exists(repo.root() / "new")); }
    if (!indexed) REQUIRE(read_file(repo.metadata() / "index") == raw_index);
    REQUIRE(restore_stack(repo).entries.size() == 1); restore_history_unchanged(repo, before);
}
TEST_CASE("StashRestore merges current staged and unstaged three-line changes independently", "[stashrestore]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    const RestoreFiles h{{"file", {"a\nb\nc\n", "100644"}}};
    restore_stash(repo, h, {{"file", {"a\nB\nc\n", "100644"}}}, {{"file", {"a\nB\nC\n", "100644"}}});
    restore_install(repo, restore_commit(repo, {{"file", {"A\nb\nc\n", "100644"}}, {"outside", {"stay", "100644"}}}));
    restore_file(repo, "outside", "stay", 0640); restore_file(repo, "unknown", "keep", 0600);
    const auto before = restore_metadata(repo);
    const auto result = stash_restore(repo, {0, true, false});
    REQUIRE(result.outcome == StashRestoreOutcome::Applied);
    REQUIRE(restore_index_content(repo, "file") == "A\nB\nc\n"); REQUIRE(read_file(repo.root() / "file") == to_bytes("A\nB\nC\n"));
    REQUIRE(read_file(repo.root() / "outside") == to_bytes("stay")); REQUIRE(restore_permissions(repo, "outside") == 0640);
    REQUIRE(read_file(repo.root() / "unknown") == to_bytes("keep")); REQUIRE(restore_permissions(repo, "unknown") == 0600);
    restore_history_unchanged(repo, before);
}
TEST_CASE("StashRestore labels Work Index and shared conflicts and rejects differing stage groups atomically", "[stashrestore]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    RestoreFiles h{{"file", {"base\n", "100644"}}}, o{{"file", {"ours\n", "100644"}}};
    RestoreFiles i = h, w{{"file", {"theirs\n", "100644"}}}; bool indexed = false, reject = false;
    bool markers = true;
    auto layer = StashConflictLayer::Work;
    SECTION("default Work-only conflict abort restores full actual permissions") {}
    SECTION("Index-only conflict preserves clean Work then abort restores its noop") { indexed = true; i = w; w = h; layer = StashConflictLayer::Staging; }
    SECTION("same original groups become Both") { indexed = true; i = w; layer = StashConflictLayer::Both; }
    SECTION("mode-only conflict saves unchanged Work as an affected noop") {
        h.clear(); i.clear(); o = {{"file", {"same", "100644"}}}; w = {{"file", {"same", "100755"}}}; markers = false;
    }
    SECTION("binary conflict saves ours unchanged bytes and full permissions") {
        h = {{"file", {std::string("b\0", 2), "100644"}}}; i = h;
        o = {{"file", {std::string("o\0", 2), "100644"}}}; w = {{"file", {std::string("t\0", 2), "100644"}}}; markers = false;
    }
    SECTION("different stage3 groups are unsupported with index") { indexed = true; i = {{"file", {"other staged\n", "100644"}}}; reject = true; }
    const auto source = restore_stash(repo, h, i, w); restore_install(repo, restore_commit(repo, o));
    const auto original = o.at("file").first;
    restore_file(repo, "file", original, 0640);
    const auto before = restore_metadata(repo);
    if (reject) { REQUIRE_THROWS_AS(stash_restore(repo, {0, indexed, true}), Error); REQUIRE(restore_metadata(repo) == before); }
    else {
        const auto result = stash_restore(repo, {0, indexed, true});
        REQUIRE(result.outcome == StashRestoreOutcome::Conflicted); REQUIRE_FALSE(result.dropped); REQUIRE(result.conflicts.size() == 1);
        REQUIRE(result.conflicts.front().layer == layer); REQUIRE(restore_stack(repo).entries == StashStack{source});
        const auto state = restore_state(repo); REQUIRE(state.kind == OperationKind::StashApply); REQUIRE(state.stash->pop); REQUIRE(state.message == "stash:pop");
        REQUIRE(state.work.size() == 1); REQUIRE(state.work.front().before.permissions == 0640);
        if (layer == StashConflictLayer::Staging || !markers) REQUIRE(read_file(repo.root() / "file") == to_bytes(original));
        else REQUIRE(to_string(read_file(repo.root() / "file")).find("<<<<<<< ours\n") != std::string::npos);
        restore_file(repo, "file", "manual unstaged edit", 0751);
        const auto aborted = stash_abort(repo); REQUIRE(aborted.outcome == StashRestoreOutcome::Aborted); REQUIRE_FALSE(aborted.dropped);
        REQUIRE(restore_metadata(repo) == before);
    }
    REQUIRE(read_file(repo.root() / "file") == to_bytes(original)); REQUIRE(restore_permissions(repo, "file") == 0640);
    restore_history_unchanged(repo, before);
}
TEST_CASE("StashRestore directory-file policies preserve region boundaries and exact Directory abort modes", "[stashrestore]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    RestoreFiles h{{"p/deep/file", {"base", "100644"}}, {"p-other", {"keep", "100644"}}};
    RestoreFiles i = h, w{{"p/deep/file", {"theirs", "100644"}}, {"p-other", {"keep", "100644"}}};
    RestoreFiles o{{"p", {"ours", "100644"}}, {"p-other", {"keep", "100644"}}};
    bool indexed = false, reject = false, directory_ours = false;
    SECTION("default DF replaces only exact region and permits tracked new child rollback") {}
    SECTION("index rejects any RW DF") { indexed = true; reject = true; }
    SECTION("RI DF ancestor intersects RW ordinary conflict") {
        indexed = true; reject = true; o = h; o["p/deep/file"].first = "ours"; i = {{"p", {"staged file", "100644"}}, {"p-other", {"keep", "100644"}}};
    }
    SECTION("Index-only DF preserves p-other independent Work change and directory modes") {
        indexed = true; directory_ours = true; o = h; o["p/deep/file"].first = "ours";
        i = {{"p", {"staged file", "100644"}}, {"p-other", {"keep", "100644"}}};
        w = h; w["p-other"].first = "changed outside region";
    }
    SECTION("cross-layer normal stage0 child and nonzero parent cannot be silently discarded") {
        indexed = true; reject = true;
        h = {{"p", {"base", "100644"}}, {"p-other", {"keep", "100644"}}};
        o = {{"p-other", {"keep", "100644"}}};
        i = {{"p/new", {"staged child", "100644"}}, {"p-other", {"keep", "100644"}}};
        w = {{"p", {"modified saved Work", "100644"}}, {"p-other", {"keep", "100644"}}};
    }
    restore_stash(repo, h, i, w); restore_install(repo, restore_commit(repo, o));
    if (directory_ours) {
        std::filesystem::permissions(repo.root() / "p", static_cast<std::filesystem::perms>(0750));
        std::filesystem::permissions(repo.root() / "p/deep", static_cast<std::filesystem::perms>(0710));
    }
    const auto before = restore_metadata(repo);
    if (reject) { REQUIRE_THROWS_AS(stash_restore(repo, {0, indexed, false}), Error); REQUIRE(restore_metadata(repo) == before); return; }
    const auto result = stash_restore(repo, {0, indexed, false}); REQUIRE(result.outcome == StashRestoreOutcome::Conflicted);
    REQUIRE(result.conflicts.front().conflict.kind == MergeConflictKind::DirectoryFile);
    const auto mixed = load_index(repo);
    const auto outside = std::find_if(mixed.begin(), mixed.end(), [](const auto& entry) { return entry.path == "p-other" && entry.stage == 0; });
    REQUIRE(outside != mixed.end());
    REQUIRE(ObjectStore(repo.metadata() / "objects").read(outside->id).payload == to_bytes("keep"));
    const auto state = restore_state(repo);
    if (directory_ours) {
        const auto deep = std::find_if(state.work.begin(), state.work.end(), [](const auto& change) { return change.path == "p/deep"; });
        REQUIRE(deep != state.work.end()); REQUIRE(deep->before.permissions == 0710);
        std::filesystem::remove(repo.root() / "p/deep/file"); std::filesystem::remove(repo.root() / "p/deep"); std::filesystem::remove(repo.root() / "p");
        restore_file(repo, "p", "resolved"); restore_stage(repo, {{"p", {"resolved", "100644"}}, {"p-other", {"changed outside region", "100644"}}});
        REQUIRE(read_file(repo.root() / "p-other") == to_bytes("changed outside region"));
    } else {
        std::filesystem::remove(repo.root() / "p"); restore_file(repo, "p/new", "new tracked resolution");
        restore_stage(repo, {{"p/new", {"new tracked resolution", "100644"}}, {"p-other", {"keep", "100644"}}});
        restore_file(repo, "p/unknown", "keep unknown"); const auto failed = restore_metadata(repo);
        REQUIRE_THROWS_AS(stash_abort(repo), Error); REQUIRE(restore_metadata(repo) == failed); REQUIRE(read_file(repo.root() / "p/unknown") == to_bytes("keep unknown"));
        std::filesystem::remove(repo.root() / "p/unknown");
    }
    REQUIRE(stash_abort(repo).outcome == StashRestoreOutcome::Aborted); REQUIRE(restore_metadata(repo) == before);
    if (directory_ours) {
        REQUIRE(read_file(repo.root() / "p/deep/file") == to_bytes("ours")); REQUIRE(restore_permissions(repo, "p") == 0750); REQUIRE(restore_permissions(repo, "p/deep") == 0710);
    } else REQUIRE(read_file(repo.root() / "p") == to_bytes("ours"));
    REQUIRE(read_file(repo.root() / "p-other") == to_bytes("keep")); restore_history_unchanged(repo, before);
}
TEST_CASE("StashRestore untracked leaves require real absence and allow only planned tracked ancestor conversion", "[stashrestore]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    RestoreFiles h{{"file", {"base", "100644"}}}, w = h, u{{"new", {"saved U", "100644"}}}, o = h;
    bool reject = true; std::string collision;
    SECTION("same content File is occupied") { collision = "file"; }
    SECTION("empty Directory is occupied") { collision = "directory"; }
    SECTION("ignored occupied File is still occupied") { collision = "ignored"; }
    SECTION("FIFO is occupied and never read") { collision = "fifo"; }
    SECTION("symlink is occupied") { collision = "symlink"; }
    SECTION("untracked parent File cannot count as absent child") { collision = "parent"; u = {{"new/child", {"saved U", "100644"}}}; }
    SECTION("RW tracked File deletion explicitly permits U child conversion") { reject = false; u = {{"file/child", {"saved U", "100755"}}}; w.clear(); }
    SECTION("actual absent U preserves unrelated ignored and empty siblings") { reject = false; }
    restore_stash(repo, h, h, w, u); restore_install(repo, restore_commit(repo, o));
    restore_file(repo, "ignored", "keep", 0600); std::filesystem::create_directory(repo.root() / "empty");
    if (collision == "file" || collision == "ignored" || collision == "parent") restore_file(repo, "new", "saved U");
    if (collision == "ignored") restore_file(repo, ".minigitignore", "new\n");
    if (collision == "directory") std::filesystem::create_directory(repo.root() / "new");
    if (collision == "fifo") REQUIRE(::mkfifo((repo.root() / "new").c_str(), 0600) == 0);
    if (collision == "symlink") std::filesystem::create_symlink("file", repo.root() / "new");
    const auto before = restore_metadata(repo);
    if (reject) { REQUIRE_THROWS_AS(stash_restore(repo), Error); REQUIRE(restore_metadata(repo) == before); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation")); }
    else {
        REQUIRE(stash_restore(repo).outcome == StashRestoreOutcome::Applied);
        const auto path = u.begin()->first; REQUIRE(read_file(repo.root() / path) == to_bytes("saved U"));
        REQUIRE_FALSE(index_snapshot(repo).contains(path));
        if (path == "file/child") { REQUIRE(index_snapshot(repo).contains("file")); REQUIRE(restore_permissions(repo, path) == 0755); }
    }
    REQUIRE(read_file(repo.root() / "ignored") == to_bytes("keep")); REQUIRE(std::filesystem::is_empty(repo.root() / "empty"));
    restore_history_unchanged(repo, before);
}
TEST_CASE("StashRestore logical noop preserves raw Index and pop removes only the selected duplicate position", "[stashrestore]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); const RestoreFiles h{{"file", {"same", "100644"}}};
    const auto a = restore_stash(repo, h, h, h);
    const auto b = restore_commit(repo, h, read_commit(repo, a).parents, "different stash entry");
    restore_install(repo, restore_commit(repo, h)); restore_file(repo, "file", "same", 0640);
    { RepoLock lock(repo.metadata()); atomic_write(repo.metadata() / "stashstack", encode_stash_stack({a, b, a})); }
    bool pop = false; std::size_t position = 1;
    SECTION("apply noop pins but retains entire stack") {}
    SECTION("pop middle position preserves duplicates and top raw bytes") { pop = true; }
    SECTION("pop a non-top duplicate removes exactly that occurrence") { pop = true; position = 2; }
    auto raw_ref = to_string(read_file(repo.metadata() / "refs/stash"));
    std::transform(raw_ref.begin(), raw_ref.end(), raw_ref.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    atomic_write(repo.metadata() / "refs/stash", to_bytes(raw_ref));
    const auto before = restore_metadata(repo); const auto raw_index = read_file(repo.metadata() / "index");
    const auto result = stash_restore(repo, {position, false, pop}); REQUIRE(result.outcome == StashRestoreOutcome::Applied); REQUIRE(result.dropped == pop);
    REQUIRE(restore_stack(repo).entries == (pop ? (position == 1 ? StashStack{a, a} : StashStack{a, b}) : StashStack{a, b, a}));
    REQUIRE(read_file(repo.metadata() / "refs/stash") == to_bytes(raw_ref)); REQUIRE(read_file(repo.metadata() / "index") == raw_index);
    REQUIRE(read_file(repo.root() / "file") == to_bytes("same")); REQUIRE(restore_permissions(repo, "file") == 0640); restore_history_unchanged(repo, before);
}
TEST_CASE("StashRestore continue resolves pop or apply without Work reads and rejects changed context", "[stashrestore]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path());
    const RestoreFiles h{{"file", {"base\n", "100644"}}}, o{{"file", {"ours\n", "100644"}}}, w{{"file", {"theirs\n", "100644"}}};
    const auto source = restore_stash(repo, h, h, w); const auto head = restore_commit(repo, o); restore_install(repo, head);
    bool pop = true; std::string scenario;
    SECTION("resolved pop completes while actual Work is FIFO and remains untouched") { scenario = "resolved"; }
    SECTION("resolved nonpop completes while Work is FIFO and preserves exact stack pair") { scenario = "resolved"; pop = false; }
    SECTION("unresolved Index keeps operation and stash") { scenario = "unresolved"; }
    SECTION("same logical stack with different exact ref bytes fails both continue and abort CAS") { scenario = "stack"; }
    SECTION("different current HEAD cannot finish operation") { scenario = "head"; }
    const auto before = restore_metadata(repo);
    const auto result = stash_restore(repo, {0, false, pop}); REQUIRE(result.outcome == StashRestoreOutcome::Conflicted);
    if (scenario == "resolved") {
        restore_stage(repo, {{"file", {"resolved staged", "100644"}}}); std::filesystem::remove(repo.root() / "file");
        REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0);
    } else if (scenario == "stack") {
        auto text = source.hex() + "\n";
        std::transform(text.begin(), text.end(), text.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
        atomic_write(repo.metadata() / "refs/stash", to_bytes(text)); restore_stage(repo, {{"file", {"resolved", "100644"}}});
    } else if (scenario == "head") {
        const auto other = restore_commit(repo, o, {}, "another HEAD");
        { RepoLock lock(repo.metadata()); update_ref(repo, "refs/heads/main", head, other); }
        restore_stage(repo, {});
    }
    const auto current = restore_metadata(repo); const auto raw_index = read_file(repo.metadata() / "index");
    if (scenario == "resolved") {
        const auto continued = stash_continue(repo); REQUIRE(continued.outcome == StashRestoreOutcome::Applied); REQUIRE(continued.dropped == pop);
        REQUIRE(restore_stack(repo).entries == (pop ? StashStack{} : StashStack{source})); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
        if (!pop) {
            REQUIRE(read_file(repo.metadata() / "stashstack") == before.at("stashstack"));
            REQUIRE(read_file(repo.metadata() / "refs/stash") == before.at("refs/stash"));
        }
        REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "file")));
        REQUIRE(read_file(repo.metadata() / "index") == raw_index); restore_history_unchanged(repo, before);
    } else {
        REQUIRE_THROWS_AS(stash_continue(repo), Error);
        if (scenario == "stack") REQUIRE_THROWS_AS(stash_abort(repo), Error);
        REQUIRE(restore_metadata(repo) == current); REQUIRE(std::filesystem::exists(repo.metadata() / "operation"));
    }
}
TEST_CASE("StashRestore rejects invalid starts and recovers Prepared apply and Committed pop at durable boundaries", "[stashrestore]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); const RestoreFiles h{{"file", {"A", "100644"}}};
    const auto source = restore_stash(repo, h, {{"file", {"B", "100644"}}}, {{"file", {"C", "100644"}}});
    restore_install(repo, restore_commit(repo, h)); restore_file(repo, "file", "A", 0640);
    bool prepared = false, committed = false;
    SECTION("Prepared apply restores exact before Index Work and saved stack") { prepared = true; }
    SECTION("Committed pop finishes publication and drop only once") { committed = true; }
    SECTION("dirty Index refuses even default restore") { restore_stage(repo, {{"file", {"dirty Index", "100644"}}}); }
    SECTION("dirty actual tracked Work refuses") { restore_file(repo, "file", "dirty Work"); }
    SECTION("unborn current HEAD refuses") { std::filesystem::remove(repo.metadata() / "refs/heads/main"); }
    SECTION("missing independent I Tree refuses default restore before publication") {
        const auto parents = read_commit(repo, source).parents;
        const auto tree = read_commit(repo, parents[1]).tree;
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(tree));
    }
    const auto before = restore_metadata(repo); const auto working = read_file(repo.root() / "file"); const auto permissions = restore_permissions(repo, "file");
    if (prepared || committed) {
        bool reached = false;
        REQUIRE_THROWS_AS(stash_restore(repo, {0, true, committed}, [&](std::size_t, std::string_view point) {
            if (point == (committed ? "journal.committed" : "journal.prepared")) { reached = true; throw Error("stop durable boundary"); }
        }), Error); REQUIRE(reached);
        { RepoLock lock(repo.metadata()); REQUIRE(transaction_roots(repo).contains(source)); }
        restore_recover(repo);
        if (prepared) { REQUIRE(restore_metadata(repo) == before); REQUIRE(read_file(repo.root() / "file") == working); REQUIRE(restore_permissions(repo, "file") == permissions); }
        else { REQUIRE(restore_stack(repo).entries.empty()); REQUIRE(restore_index_content(repo, "file") == "B"); REQUIRE(read_file(repo.root() / "file") == to_bytes("C")); }
    } else { REQUIRE_THROWS_AS(stash_restore(repo), Error); REQUIRE(restore_metadata(repo) == before); REQUIRE(read_file(repo.root() / "file") == working); }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction")); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation")); restore_history_unchanged(repo, before);
}
