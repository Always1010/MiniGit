#include <catch2/catch_test_macros.hpp>
#include "minigit/stash.hpp"
#include "minigit/stash_stack.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/refs.hpp"
#include "minigit/worktree.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <sys/stat.h>
#include <utility>
using namespace minigit;
namespace {
using StashFiles = std::map<std::string, std::pair<std::string, std::string>>;
void stash_file(const Repository& repo, std::string_view path, std::string_view content, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(content), permissions);
}
ObjectId stash_fixture_commit(const Repository& repo, const StashFiles& files) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Snapshot snapshot;
    for (const auto& [path, file] : files)
        snapshot.emplace_hint(snapshot.end(), path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    return store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, snapshot), {},
        "Fixture <fixture@example.test> 0 +0000", "Fixture <fixture@example.test> 0 +0000", "head"}));
}
void stash_install(const Repository& repo, ObjectId head, bool detached = false) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Index index;
    for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, head).tree)) {
        stash_file(repo, path, to_string(store.read(state.id).payload), state.mode == "100755" ? 0755U : 0644U);
        index.push_back({path, state.mode, state.id});
    }
    save_index(repo, index);
    if (detached) atomic_write(repo.metadata() / "HEAD", to_bytes(head.hex() + "\n"));
    else update_ref(repo, "refs/heads/main", std::nullopt, head);
    const auto log = encode_reflog({{std::nullopt, head, "fixture"}});
    detail::ensure_directory(repo.metadata() / "logs/refs/heads");
    atomic_write(repo.metadata() / "logs/HEAD", log);
    if (!detached) atomic_write(repo.metadata() / "logs/refs/heads/main", log);
    atomic_write(repo.metadata() / "identity", to_bytes("version=1\nuser.name=Current\nuser.email=current@example.test\n"));
}
void stash_index(const Repository& repo, const StashFiles& files) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Index index;
    for (const auto& [path, file] : files)
        index.push_back({path, file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    save_index(repo, index);
}
std::map<std::string, Bytes> stash_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && std::filesystem::is_regular_file(entry.symlink_status())) result.emplace(path, read_file(entry.path()));
    }
    return result;
}
std::size_t stash_object_count(const Repository& repo) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects"))
        if (std::filesystem::is_regular_file(entry.symlink_status())) ++count;
    return count;
}
std::uint32_t stash_permissions(const Repository& repo, std::string_view path) {
    return static_cast<std::uint32_t>(std::filesystem::symlink_status(repo.root() / path).permissions()) & 07777;
}
struct SavedStash { StashEntry entry; Snapshot head; Snapshot index; Snapshot work; Snapshot untracked; };
SavedStash stash_saved(const Repository& repo, ObjectId work) {
    RepoLock lock(repo.metadata());
    const auto entry = read_stash_entry(repo, work);
    return {entry, read_snapshot(repo, read_commit(repo, entry.head).tree), read_snapshot(repo, read_commit(repo, entry.index).tree),
        read_snapshot(repo, read_commit(repo, entry.work).tree), entry.untracked ? read_snapshot(repo, read_commit(repo, *entry.untracked).tree) : Snapshot{}};
}
std::string stash_content(const Repository& repo, const Snapshot& snapshot, const std::string& path) {
    return to_string(ObjectStore(repo.metadata() / "objects").read(snapshot.at(path).id).payload);
}
StashStackState stash_stack(const Repository& repo) { RepoLock lock(repo.metadata()); return read_stash_stack(repo); }
void stash_recover(const Repository& repo) { RepoLock lock(repo.metadata()); recover_transaction(repo); recover_transaction(repo); }
void stash_assert_history(const Repository& repo, const std::map<std::string, Bytes>& before) {
    for (const auto& [path, bytes] : before)
        if (path == "HEAD" || path.starts_with("refs/heads/") || path.starts_with("logs/") || path == "identity")
            REQUIRE(read_file(repo.metadata() / path) == bytes);
}
}
TEST_CASE("Stash push saves independent HEAD Index and actual Work without moving history", "[stash]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto head = stash_fixture_commit(repo, {{"file", {"A", "100644"}}});
    bool detached = false;
    SECTION("symbolic HEAD keeps its branch and both logs") {}
    SECTION("detached HEAD keeps exact detached bytes") { detached = true; }
    stash_install(repo, head, detached);
    stash_index(repo, {{"file", {"B", "100755"}}});
    stash_file(repo, "file", "C", 0640);
    const auto before = stash_metadata(repo);
    const auto pushed = stash_push(repo, {false, "save B and C"});
    REQUIRE(pushed.has_value());
    const auto saved = stash_saved(repo, *pushed);
    REQUIRE(saved.entry.head == head);
    REQUIRE_FALSE(saved.entry.untracked.has_value());
    REQUIRE(stash_content(repo, saved.head, "file") == "A");
    REQUIRE(stash_content(repo, saved.index, "file") == "B");
    REQUIRE(saved.index.at("file").mode == "100755");
    REQUIRE(stash_content(repo, saved.work, "file") == "C");
    REQUIRE(saved.work.at("file").mode == "100644");
    REQUIRE(read_commit(repo, saved.entry.work).parents == std::vector<ObjectId>{head, saved.entry.index});
    REQUIRE(read_commit(repo, saved.entry.index).parents == std::vector<ObjectId>{head});
    REQUIRE(read_commit(repo, *pushed).message == "save B and C");
    REQUIRE(read_commit(repo, *pushed).author.starts_with("Current <current@example.test> "));
    REQUIRE(read_file(repo.root() / "file") == to_bytes("A"));
    REQUIRE(stash_permissions(repo, "file") == 0644);
    REQUIRE(index_snapshot(repo) == saved.head);
    REQUIRE(stash_stack(repo).entries == StashStack{*pushed});
    REQUIRE(read_ref(repo, "refs/stash") == pushed);
    stash_assert_history(repo, before);
    const auto shown = stash_show(repo);
    REQUIRE(shown.find("-A") != std::string::npos);
    REQUIRE(shown.find("+C") != std::string::npos);
    REQUIRE(shown.find("+B") == std::string::npos);
}
TEST_CASE("Stash push distinguishes staged deletion addition missing Work binary and directory conversions", "[stash]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto head = stash_fixture_commit(repo, {{"item", {"A", "100644"}}});
    stash_install(repo, head);
    std::string scenario;
    SECTION("HEAD-only ignored file remains tracked saved Work") {
        scenario = "delete";
        stash_index(repo, {});
        stash_file(repo, "item", "A", 0640);
        stash_file(repo, ".minigitignore", "item\n");
    }
    SECTION("staged addition missing from Work is absent not empty") {
        scenario = "missing";
        stash_index(repo, {{"item", {"A", "100644"}}, {"new", {"B", "100644"}}});
    }
    SECTION("binary executable Work and empty file are saved independently") {
        scenario = "binary";
        stash_index(repo, {{"item", {"B", "100644"}}, {"empty", {"", "100644"}}});
        stash_file(repo, "item", std::string("C\0D", 3), 0710);
        stash_file(repo, "empty", "");
    }
    SECTION("tracked Directory layout restores original HEAD File") {
        scenario = "directory";
        stash_index(repo, {{"item/deep/file", {"B", "100755"}}});
        std::filesystem::remove(repo.root() / "item");
        stash_file(repo, "item/deep/file", "C", 0751);
    }
    const auto before = stash_metadata(repo);
    const auto pushed = stash_push(repo);
    REQUIRE(pushed.has_value());
    const auto saved = stash_saved(repo, *pushed);
    if (scenario == "delete") {
        REQUIRE(saved.index.empty()); REQUIRE(stash_content(repo, saved.work, "item") == "A");
        REQUIRE(stash_permissions(repo, "item") == 0640);
    } else if (scenario == "missing") {
        REQUIRE(saved.index.contains("new")); REQUIRE_FALSE(saved.work.contains("new"));
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "new"));
    } else if (scenario == "binary") {
        REQUIRE(stash_content(repo, saved.work, "item") == std::string("C\0D", 3));
        REQUIRE(saved.work.at("item").mode == "100755");
        REQUIRE(saved.work.contains("empty")); REQUIRE(stash_content(repo, saved.work, "empty").empty());
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "empty"));
    } else {
        REQUIRE_FALSE(saved.work.contains("item")); REQUIRE(saved.index.at("item/deep/file").mode == "100755");
        REQUIRE(stash_content(repo, saved.work, "item/deep/file") == "C");
    }
    REQUIRE(read_file(repo.root() / "item") == to_bytes("A"));
    REQUIRE(index_snapshot(repo) == saved.head);
    REQUIRE(read_commit(repo, *pushed).message == "WIP " + head.hex());
    stash_assert_history(repo, before);
}
TEST_CASE("Stash no changes needs no identity and preserves missing or exact Index and stack", "[stash]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    ObjectId head;
    SECTION("empty committed HEAD missing Index and identity stays missing") {
        head = stash_fixture_commit(repo, {}); stash_install(repo, head);
        std::filesystem::remove(repo.metadata() / "index"); std::filesystem::remove(repo.metadata() / "identity");
    }
    SECTION("same content with full permissions and malformed identity stays exact") {
        head = stash_fixture_commit(repo, {{"file", {"A", "100644"}}}); stash_install(repo, head);
        stash_file(repo, "file", "A", 0640);
        atomic_write(repo.metadata() / "identity", to_bytes("bad identity"));
        atomic_write(repo.metadata() / "stashstack", encode_stash_stack({}));
    }
    std::filesystem::create_directory(repo.root() / "unknown-empty");
    const auto before = stash_metadata(repo);
    const auto objects = stash_object_count(repo);
    REQUIRE_FALSE(stash_push(repo).has_value());
    REQUIRE(stash_metadata(repo) == before);
    REQUIRE(stash_object_count(repo) == objects);
    REQUIRE(std::filesystem::is_empty(repo.root() / "unknown-empty"));
    REQUIRE(stash_list(repo).empty());
    REQUIRE_THROWS_AS(stash_show(repo), Error);
}
TEST_CASE("Stash include-untracked uses ignore rules and exact tracked exceptions while preserving siblings", "[stash]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto head = stash_fixture_commit(repo, {{".minigitignore", {"track\nprivate\n", "100644"}}, {"track", {"A", "100644"}}});
    stash_install(repo, head);
    stash_file(repo, "track", "C");
    stash_file(repo, "u/file", "untracked", 0751);
    stash_file(repo, "private", "ignored", 0600);
    std::filesystem::create_directory(repo.root() / "empty-dir");
    bool include = false;
    SECTION("default push leaves untracked files untouched") {}
    SECTION("include-untracked adds an independent root and removes only its File") { include = true; }
    const auto pushed = stash_push(repo, {include, std::nullopt});
    REQUIRE(pushed.has_value());
    const auto saved = stash_saved(repo, *pushed);
    REQUIRE(stash_content(repo, saved.work, "track") == "C");
    REQUIRE(saved.entry.untracked.has_value() == include);
    REQUIRE_FALSE(saved.work.contains("u/file"));
    REQUIRE(read_file(repo.root() / "private") == to_bytes("ignored"));
    REQUIRE(stash_permissions(repo, "private") == 0600);
    REQUIRE(std::filesystem::is_empty(repo.root() / "empty-dir"));
    REQUIRE(read_file(repo.root() / "track") == to_bytes("A"));
    if (include) {
        REQUIRE(stash_content(repo, saved.untracked, "u/file") == "untracked"); REQUIRE(saved.untracked.at("u/file").mode == "100755");
        REQUIRE(read_commit(repo, *saved.entry.untracked).parents.empty());
        REQUIRE(read_commit(repo, *pushed).parents == std::vector<ObjectId>{head, saved.entry.index, *saved.entry.untracked});
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "u/file"));
        REQUIRE(std::filesystem::is_empty(repo.root() / "u"));
        REQUIRE(stash_show(repo).find("+untracked") != std::string::npos);
    } else REQUIRE(read_file(repo.root() / "u/file") == to_bytes("untracked"));
}
TEST_CASE("Stash queries and middle drop clear ignore live Index Work and preserve exact top ref", "[stash]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto head = stash_fixture_commit(repo, {{"file", {"A", "100644"}}});
    stash_install(repo, head);
    StashStack entries;
    for (const auto& content : {"one", "two", "three"}) {
        stash_file(repo, "file", content);
        entries.insert(entries.begin(), *stash_push(repo, {false, std::string(content)}));
    }
    atomic_write(repo.metadata() / "index", to_bytes("corrupt current index"));
    REQUIRE(::mkfifo((repo.root() / "fifo").c_str(), 0600) == 0);
    atomic_write(repo.metadata() / "operation", to_bytes("invalid active operation"));
    const auto before = stash_metadata(repo);
    const auto listed = stash_list(repo);
    REQUIRE(listed.size() == 3);
    for (std::size_t position = 0; position < listed.size(); ++position) {
        REQUIRE(listed[position].position == position); REQUIRE(listed[position].work == entries[position]);
    }
    REQUIRE(listed[0].message == "three");
    REQUIRE(stash_show(repo, 1).find("+two") != std::string::npos);
    REQUIRE_THROWS_AS(stash_drop(repo, 1), Error); REQUIRE_THROWS_AS(stash_clear(repo), Error);
    REQUIRE(stash_metadata(repo) == before);
    std::filesystem::remove(repo.metadata() / "operation");
    auto uppercase = entries.front().hex();
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const auto raw_ref = to_bytes(uppercase + "\n");
    atomic_write(repo.metadata() / "refs/stash", raw_ref);
    const auto raw_index = read_file(repo.metadata() / "index");
    stash_drop(repo, 1);
    REQUIRE(stash_stack(repo).entries == StashStack{entries[0], entries[2]});
    REQUIRE(read_file(repo.metadata() / "refs/stash") == raw_ref);
    REQUIRE(read_file(repo.metadata() / "index") == raw_index);
    REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "fifo")));
    stash_clear(repo);
    REQUIRE(stash_list(repo).empty());
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "stashstack")); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "refs/stash"));
    REQUIRE_THROWS_AS(stash_drop(repo), Error);
    stash_clear(repo);
    REQUIRE(read_file(repo.metadata() / "index") == raw_index);
    stash_assert_history(repo, before);
}
TEST_CASE("Stash cleanup protects unknown ignored children and empty directories in tracked conversions", "[stash]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto head = stash_fixture_commit(repo, {{"item", {"A", "100644"}}});
    stash_install(repo, head);
    stash_index(repo, {{"item/known", {"B", "100644"}}});
    std::filesystem::remove(repo.root() / "item");
    stash_file(repo, "item/known", "C", 0640);
    bool include = false, success = false;
    SECTION("default does not own unknown File") { stash_file(repo, "item/private", "keep"); }
    SECTION("include still does not own unknown empty Directory") { include = true; std::filesystem::create_directory(repo.root() / "item/empty"); }
    SECTION("include still does not own ignored File") { include = true; stash_file(repo, "item/private", "keep"); stash_file(repo, ".minigitignore", "private\n"); }
    SECTION("include owns its actual nonignored File and allows full conversion") { include = true; success = true; stash_file(repo, "item/private", "save"); }
    const auto before = stash_metadata(repo);
    if (success) {
        const auto pushed = stash_push(repo, {include, std::nullopt}); REQUIRE(pushed.has_value());
        const auto saved = stash_saved(repo, *pushed);
        REQUIRE(stash_content(repo, saved.work, "item/known") == "C");
        REQUIRE(stash_content(repo, saved.untracked, "item/private") == "save");
        REQUIRE(read_file(repo.root() / "item") == to_bytes("A"));
    } else {
        REQUIRE_THROWS_AS(stash_push(repo, {include, std::nullopt}), Error);
        REQUIRE(stash_metadata(repo) == before);
        REQUIRE(read_file(repo.root() / "item/known") == to_bytes("C")); REQUIRE(stash_permissions(repo, "item/known") == 0640);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
    }
    stash_assert_history(repo, before);
}
TEST_CASE("Stash rejects invalid live or saved inputs before mutable publication", "[stash]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto head = stash_fixture_commit(repo, {{"file", {"A", "100644"}}});
    stash_install(repo, head);
    stash_file(repo, "file", "C", 0640);
    bool include = false, query = false, tree_budget = false;
    StashPushOptions options;
    SECTION("unborn is refused") { std::filesystem::remove(repo.metadata() / "refs/heads/main"); }
    SECTION("typed conflict stages are refused") {
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
        const auto id = store.write(ObjectType::Blob, to_bytes("B")); save_index(repo, {{"file", "100644", id, 1}, {"file", "100644", id, 2}});
    }
    SECTION("nonblob Index target is refused even at nonzero stage") {
        RepoLock lock(repo.metadata()); save_index(repo, {{"file", "100644", head, 2}});
    }
    SECTION("missing identity for actual changes is refused") { std::filesystem::remove(repo.metadata() / "identity"); }
    SECTION("tracked symlink is refused") { std::filesystem::remove(repo.root() / "file"); std::filesystem::create_symlink("missing", repo.root() / "file"); }
    SECTION("include scan refuses ignored unsupported nodes") { include = true; stash_file(repo, ".minigitignore", "link\n"); std::filesystem::create_symlink("file", repo.root() / "link"); }
    SECTION("invalid explicit message is refused") { options.message = std::string("bad\0message", 11); }
    SECTION("desynchronized live stack pair is refused") { atomic_write(repo.metadata() / "refs/stash", to_bytes(head.hex() + "\n")); }
    SECTION("legal Index file count can exceed complete saved Tree entry budget") {
        tree_budget = true;
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
        const auto id = store.write(ObjectType::Blob, to_bytes("shared"));
        Index index;
        // 800 files fit the Index budget, while their disjoint depth-128 paths
        // require 102400 total Tree entries. No worktree directories are made.
        for (std::size_t i = 0; i < 800; ++i) {
            auto path = "root" + std::to_string(i);
            for (std::size_t depth = 1; depth < 128; ++depth) path += "/d";
            index.push_back({std::move(path), "100644", id});
        }
        save_index(repo, index);
    }
    SECTION("saved Index Tree must be complete for show but list is typed only") {
        stash_index(repo, {{"file", {"unique independent Index", "100644"}}});
        const auto pushed = stash_push(repo); REQUIRE(pushed.has_value());
        const auto saved = stash_saved(repo, *pushed);
        const auto tree = read_commit(repo, saved.entry.index).tree;
        const auto hex = tree.hex(); std::filesystem::remove(repo.metadata() / "objects" / hex.substr(0, 2) / hex.substr(2));
        REQUIRE(stash_list(repo).size() == 1); query = true;
    }
    options.include_untracked = include;
    const auto before = stash_metadata(repo);
    const auto objects = stash_object_count(repo);
    if (query) REQUIRE_THROWS_AS(stash_show(repo), Error);
    else if (tree_budget) {
        try { (void)stash_push(repo, options); FAIL("oversized saved Tree must be rejected"); }
        catch (const Error& error) { REQUIRE(std::string(error.what()).find("保存树超过 100000") != std::string::npos); }
        REQUIRE(stash_object_count(repo) == objects);
    }
    else REQUIRE_THROWS_AS(stash_push(repo, options), Error);
    REQUIRE(stash_metadata(repo) == before);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
TEST_CASE("Stash Prepared push and Committed drop recover exact states without repeated stack entries", "[stash]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto head = stash_fixture_commit(repo, {{"file", {"A", "100644"}}});
    stash_install(repo, head);
    SECTION("Prepared push rolls back actual dirty Work full permissions and Index") {
        stash_index(repo, {{"file", {"B", "100755"}}}); stash_file(repo, "file", "C", 0640);
        const auto before = stash_metadata(repo);
        REQUIRE_THROWS_AS(stash_push(repo, {}, [](std::size_t, std::string_view point) { if (point == "journal.prepared") throw Error("stop prepared"); }), Error);
        { RepoLock lock(repo.metadata()); REQUIRE(transaction_roots(repo).contains(head)); }
        stash_recover(repo);
        REQUIRE(stash_metadata(repo) == before);
        REQUIRE(read_file(repo.root() / "file") == to_bytes("C")); REQUIRE(stash_permissions(repo, "file") == 0640);
        REQUIRE(stash_list(repo).empty());
    }
    SECTION("Committed drop rolls forward once and retains both-side roots during recovery") {
        stash_file(repo, "file", "first"); const auto first = *stash_push(repo);
        stash_file(repo, "file", "second"); const auto second = *stash_push(repo);
        const auto before = stash_metadata(repo);
        REQUIRE_THROWS_AS(stash_drop(repo, 0, [](std::size_t, std::string_view point) { if (point == "journal.committed") throw Error("stop committed"); }), Error);
        { RepoLock lock(repo.metadata()); const auto roots = transaction_roots(repo); REQUIRE(roots.contains(first)); REQUIRE(roots.contains(second)); }
        stash_recover(repo);
        REQUIRE(stash_stack(repo).entries == StashStack{first});
        REQUIRE(stash_list(repo).size() == 1);
        REQUIRE(read_file(repo.root() / "file") == to_bytes("A"));
        REQUIRE(read_file(repo.metadata() / "index") == before.at("index"));
        stash_assert_history(repo, before);
    }
}
