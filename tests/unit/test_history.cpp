#include <catch2/catch_test_macros.hpp>
#include "minigit/history.hpp"
#include "minigit/worktree.hpp"
#include "minigit/refs.hpp"
#include "minigit/object_store.hpp"
#include "minigit/error.hpp"
#include "temp_directory.hpp"
#include <algorithm>
using namespace minigit;
namespace {
void identity(const Repository& repo) {
    configure_identity(repo, "user.name", "测试作者");
    configure_identity(repo, "user.email", "author@example.test");
}
void file(const Repository& repo, std::string_view path, std::string_view content) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write(target, to_bytes(content));
}
ObjectId fixture_commit(const Repository& repo) {
    ObjectStore store(repo.metadata() / "objects");
    const auto tree = store.write(ObjectType::Tree, encode_tree({}));
    return store.write(ObjectType::Commit, encode_commit({tree, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "fixture"}));
}
}
TEST_CASE("Snapshots round trip nested binary executable and empty files canonically", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto binary = store.write(ObjectType::Blob, Bytes{0, 255, 128, '\n', 0});
    const auto empty = store.write(ObjectType::Blob, {});
    const Snapshot snapshot{{"nested/binary", {"100644", binary}}, {"empty", {"100644", empty}}, {"run", {"100755", binary}}};
    const auto tree = write_snapshot(repo, snapshot);
    REQUIRE(read_snapshot(repo, tree) == snapshot);
    REQUIRE(write_snapshot(repo, read_snapshot(repo, tree)) == tree);
    const auto root = decode_tree(store.read(tree).payload);
    REQUIRE(root.size() == 3);
    REQUIRE(root[1].mode == "40000");
    REQUIRE(read_snapshot(repo, write_snapshot(repo, {})).empty());
    REQUIRE(head_snapshot(repo).empty());
}
TEST_CASE("Snapshots reject missing wrong typed unsafe and excessive depth objects", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto tree = store.write(ObjectType::Tree, encode_tree({}));
    const auto blob = store.write(ObjectType::Blob, to_bytes("a"));
    REQUIRE_THROWS_AS(write_snapshot(repo, {{"x", {"100644", tree}}}), Error);
    REQUIRE_THROWS_AS(write_snapshot(repo, {{"x", {"100644", ObjectId{}}}}), Error);
    REQUIRE_THROWS_AS(write_snapshot(repo, {{"../x", {"100644", blob}}}), Error);
    REQUIRE_THROWS_AS(write_snapshot(repo, {{"x", {"120000", blob}}}), Error);
    REQUIRE_THROWS_AS(write_snapshot(repo, {{"x", {"100644", blob}}, {"x/y", {"100644", blob}}}), Error);
    REQUIRE_THROWS_AS(read_snapshot(repo, blob), Error);
    const auto wrong_file = store.write(ObjectType::Tree, encode_tree({{"x", "100644", tree}}));
    const auto wrong_directory = store.write(ObjectType::Tree, encode_tree({{"x", "40000", blob}}));
    REQUIRE_THROWS_AS(read_snapshot(repo, wrong_file), Error);
    REQUIRE_THROWS_AS(read_snapshot(repo, wrong_directory), Error);
    auto deep = tree;
    for (unsigned i = 0; i < 128; ++i) deep = store.write(ObjectType::Tree, encode_tree({{"dir", "40000", deep}}));
    REQUIRE_THROWS_AS(read_snapshot(repo, deep), Error);
    std::string path;
    for (unsigned i = 0; i < 128; ++i) path += "dir/";
    REQUIRE_THROWS_AS(write_snapshot(repo, {{path + "file", {"100644", blob}}}), Error);
}
TEST_CASE("Snapshots expand shared subtrees separately under each prefix", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto blob = store.write(ObjectType::Blob, to_bytes("a"));
    const auto subtree = store.write(ObjectType::Tree, encode_tree({{"file", "100644", blob}}));
    const auto tree = store.write(ObjectType::Tree, encode_tree({{"left", "40000", subtree}, {"right", "40000", subtree}}));
    const Snapshot expected{{"left/file", {"100644", blob}}, {"right/file", {"100644", blob}}};
    REQUIRE(read_snapshot(repo, tree) == expected);
}
TEST_CASE("Commit records staged content immutable trees and parent links", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    identity(repo);
    file(repo, "dir/file", "staged");
    add_paths(repo, {repo.root() / "dir/file"});
    const auto staged = load_index(repo);
    file(repo, "dir/file", "later worktree edit");
    const auto first = create_commit(repo, "first");
    const auto initial = read_commit(repo, first);
    REQUIRE(initial.parents.empty());
    REQUIRE(initial.message == "first");
    REQUIRE(initial.author.starts_with("测试作者 <author@example.test> "));
    REQUIRE(initial.author.ends_with(" +0000"));
    REQUIRE(initial.author == initial.committer);
    REQUIRE(head_snapshot(repo).at("dir/file").id == staged[0].id);
    REQUIRE(load_index(repo) == staged);
    add_paths(repo, {repo.root() / "dir/file"});
    const auto second = create_commit(repo, "second");
    const auto next = read_commit(repo, second);
    REQUIRE(next.parents == std::vector<ObjectId>{first});
    REQUIRE(next.tree != initial.tree);
    REQUIRE(read_snapshot(repo, initial.tree).at("dir/file").id == staged[0].id);
    REQUIRE(read_head(repo).id == second);
    REQUIRE(resolve_commit(repo, "main") == second);
    REQUIRE_THROWS_AS(create_commit(repo, "unchanged"), Error);
    REQUIRE(read_head(repo).id == second);
    const auto empty = create_commit(repo, "explicit empty", true);
    REQUIRE(read_commit(repo, empty).tree == next.tree);
    REQUIRE(read_commit(repo, empty).parents == std::vector<ObjectId>{second});
}
TEST_CASE("Commit rejects absent identity empty messages conflicts and invalid index objects without moving HEAD", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    file(repo, "file", "data");
    add_paths(repo, {repo.root() / "file"});
    const auto unborn = read_head(repo);
    REQUIRE_THROWS_AS(create_commit(repo, "missing identity"), Error);
    REQUIRE(read_head(repo) == unborn);
    identity(repo);
    for (const auto message : {"", " \t\n", "\r\n"}) REQUIRE_THROWS_AS(create_commit(repo, message), Error);
    REQUIRE_THROWS_AS(create_commit(repo, std::string("x\0y", 3)), Error);
    const auto first = create_commit(repo, "first");
    const auto original = load_index(repo)[0];
    {
        RepoLock lock(repo.metadata());
        save_index(repo, {{original.path, original.mode, original.id, 1}});
    }
    REQUIRE_THROWS_AS(create_commit(repo, "conflict"), Error);
    REQUIRE(read_head(repo).id == first);
    {
        RepoLock lock(repo.metadata());
        save_index(repo, {{"file", "100644", ObjectId{}}});
    }
    REQUIRE_THROWS_AS(create_commit(repo, "missing object"), Error);
    REQUIRE(read_head(repo).id == first);
    {
        RepoLock lock(repo.metadata());
        save_index(repo, {{"file", "100644", read_commit(repo, first).tree}});
    }
    REQUIRE_THROWS_AS(create_commit(repo, "wrong object"), Error);
    REQUIRE(read_head(repo).id == first);
}
TEST_CASE("Commit handles explicit unborn empty and detached HEAD commits", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    identity(repo);
    REQUIRE_THROWS_AS(create_commit(repo, "empty"), Error);
    const auto first = create_commit(repo, "allow empty", true);
    REQUIRE(read_commit(repo, first).parents.empty());
    REQUIRE(head_snapshot(repo).empty());
    {
        RepoLock lock(repo.metadata());
        set_head(repo, first.hex());
    }
    const auto detached = create_commit(repo, "detached", true);
    REQUIRE_FALSE(read_head(repo).symbolic.has_value());
    REQUIRE(read_head(repo).id == detached);
    REQUIRE(read_commit(repo, detached).parents == std::vector<ObjectId>{first});
    REQUIRE(read_ref(repo, "refs/heads/main") == first);
}
TEST_CASE("Commit rejects damaged HEAD objects and missing parent trees without moving HEAD", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    identity(repo);
    file(repo, "file", "data");
    add_paths(repo, {repo.root() / "file"});
    const auto first = create_commit(repo, "first");
    const auto tree = read_commit(repo, first).tree;
    const ObjectStore store(repo.metadata() / "objects");
    SECTION("damaged commit") {
        atomic_write(store.object_path(first), to_bytes("invalid compressed object"));
        REQUIRE_THROWS_AS(resolve_commit(repo, "HEAD"), Error);
        REQUIRE_THROWS_AS(create_commit(repo, "damaged", true), Error);
    }
    SECTION("missing tree") {
        std::filesystem::remove(store.object_path(tree));
        REQUIRE_THROWS_AS(create_commit(repo, "missing tree", true), Error);
    }
    REQUIRE(read_head(repo).id == first);
}
TEST_CASE("Identity uses explicit strict schema and preserves configuration on bad input", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto config = read_file(repo.metadata() / "config");
    REQUIRE_FALSE(identity_value(repo, "user.name").has_value());
    identity(repo);
    const auto original = read_file(repo.metadata() / "identity");
    REQUIRE(identity_value(repo, "user.name") == "测试作者");
    REQUIRE(identity_value(repo, "user.email") == "author@example.test");
    for (const auto value : {"", "   ", "A\nB", "A\rB", "A<B", "A>B", "A\tB"})
        REQUIRE_THROWS_AS(configure_identity(repo, "user.name", value), Error);
    for (const auto value : {"missing", "@domain", "local@", "a@@b", "a @b"})
        REQUIRE_THROWS_AS(configure_identity(repo, "user.email", value), Error);
    REQUIRE_THROWS_AS(configure_identity(repo, "user.name", std::string("x\0y", 3)), Error);
    REQUIRE_THROWS_AS(configure_identity(repo, "unsupported", "value"), Error);
    REQUIRE_THROWS_AS(identity_value(repo, "unsupported"), Error);
    REQUIRE(read_file(repo.metadata() / "identity") == original);
    REQUIRE(read_file(repo.metadata() / "config") == config);
    for (const auto malformed : {"version=2\n", "version=1\nunknown=x\n", "version=1\nuser.name=x\nuser.name=y\n", "version=1\nuser.email=a@b\nuser.name=x\n", "version=1\nuser.name=x"}) {
        atomic_write(repo.metadata() / "identity", to_bytes(malformed));
        REQUIRE_THROWS_AS(identity_value(repo, "user.name"), Error);
        REQUIRE_THROWS_AS(configure_identity(repo, "user.name", "safe"), Error);
        REQUIRE(to_string(read_file(repo.metadata() / "identity")) == malformed);
    }
}
TEST_CASE("References support unborn symbolic detached CAS deletion and resolution", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    REQUIRE(read_head(repo).symbolic == "refs/heads/main");
    REQUIRE_FALSE(read_head(repo).id.has_value());
    REQUIRE_THROWS_AS(resolve_commit(repo, "HEAD"), Error);
    RepoLock lock(repo.metadata());
    const auto id = fixture_commit(repo);
    update_ref(repo, "refs/heads/main", std::nullopt, id);
    REQUIRE(resolve_commit(repo, "HEAD") == id);
    REQUIRE(resolve_commit(repo, id.hex()) == id);
    auto uppercase = id.hex();
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    REQUIRE(resolve_commit(repo, uppercase) == id);
    REQUIRE(resolve_commit(repo, "refs/heads/main") == id);
    REQUIRE_THROWS_AS(update_ref(repo, "refs/heads/main", std::nullopt, std::nullopt), Error);
    REQUIRE(read_ref(repo, "refs/heads/main") == id);
    update_ref(repo, "refs/tags/release", std::nullopt, id);
    REQUIRE(resolve_commit(repo, "release") == id);
    update_ref(repo, "refs/heads/release", std::nullopt, id);
    REQUIRE_THROWS_AS(resolve_commit(repo, "release"), Error);
    REQUIRE(list_refs(repo, "refs/heads/").size() == 2);
    REQUIRE(list_refs(repo, "refs/tags/").size() == 1);
    update_ref(repo, "refs/heads-extra/other", std::nullopt, id);
    REQUIRE(list_refs(repo, "refs/heads").size() == 2);
    REQUIRE(list_refs(repo, "refs/heads/main").size() == 1);
    set_head(repo, "refs/heads/new");
    REQUIRE_FALSE(read_head(repo).id.has_value());
    set_head(repo, id.hex());
    REQUIRE_FALSE(read_head(repo).symbolic.has_value());
    REQUIRE(read_head(repo).id == id);
    update_ref(repo, "refs/heads/release", id, std::nullopt);
    REQUIRE_FALSE(read_ref(repo, "refs/heads/release").has_value());
    REQUIRE(resolve_commit(repo, "release") == id);
    update_ref(repo, "refs/heads/topic/sub", std::nullopt, id);
    update_ref(repo, "refs/heads/topic/deep/leaf", std::nullopt, id);
    update_ref(repo, "refs/heads/topic/sub", id, std::nullopt);
    REQUIRE(std::filesystem::is_directory(repo.metadata() / "refs/heads/topic"));
    update_ref(repo, "refs/heads/topic/deep/leaf", id, std::nullopt);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "refs/heads/topic"));
    update_ref(repo, "refs/heads/topic/deep/leaf", std::nullopt, std::nullopt);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "refs/heads/topic"));
    update_ref(repo, "refs/heads/topic", std::nullopt, id);
    update_ref(repo, "refs/heads/topic", id, id);
    REQUIRE(read_ref(repo, "refs/heads/topic") == id);
    update_ref(repo, "refs/heads/topic", id, std::nullopt);
    // Simulate deletion completed before empty ancestor cleanup was durable.
    update_ref(repo, "refs/heads/topic/sub", std::nullopt, id);
    std::filesystem::remove(repo.metadata() / "refs/heads/topic/sub");
    update_ref(repo, "refs/heads/topic/sub", std::nullopt, std::nullopt);
    update_ref(repo, "refs/heads/topic", std::nullopt, id);
    REQUIRE(read_ref(repo, "refs/heads/topic") == id);
    update_ref(repo, "refs/heads/topic", id, std::nullopt);
    REQUIRE(std::filesystem::is_directory(repo.metadata() / "refs"));
    REQUIRE(std::filesystem::is_directory(repo.metadata() / "refs/heads"));
    REQUIRE(std::filesystem::is_directory(repo.metadata() / "refs/tags"));
}
TEST_CASE("References reject incomplete snapshot publication without changing refs or HEAD", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto valid = fixture_commit(repo);
    update_ref(repo, "refs/heads/main", std::nullopt, valid);
    const auto head = read_file(repo.metadata() / "HEAD");
    const auto blob = store.write(ObjectType::Blob, to_bytes("blob"));
    const auto empty_tree = store.write(ObjectType::Tree, encode_tree({}));
    const auto missing_blob = store.write(ObjectType::Tree, encode_tree({{"file", "100644", ObjectId{}}}));
    const auto wrong_blob = store.write(ObjectType::Tree, encode_tree({{"file", "100644", empty_tree}}));
    const auto missing_subtree = store.write(ObjectType::Tree, encode_tree({{"dir", "40000", ObjectId{}}}));
    for (const auto tree : {ObjectId{}, blob, missing_blob, wrong_blob, missing_subtree}) {
        const auto incomplete = store.write(ObjectType::Commit, encode_commit({tree, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "incomplete"}));
        REQUIRE_THROWS_AS(update_ref(repo, "refs/heads/main", valid, incomplete), Error);
        REQUIRE_THROWS_AS(update_ref(repo, "refs/heads/new", std::nullopt, incomplete), Error);
        REQUIRE_FALSE(read_ref(repo, "refs/heads/new").has_value());
        REQUIRE_THROWS_AS(set_head(repo, incomplete.hex()), Error);
        // Deliberately inject corrupt external metadata to check symbolic HEAD.
        atomic_write(repo.metadata() / "refs/heads/bad", to_bytes(incomplete.hex() + "\n"));
        REQUIRE_THROWS_AS(set_head(repo, "refs/heads/bad"), Error);
        REQUIRE_THROWS_AS(update_ref(repo, "refs/heads/bad", incomplete, incomplete), Error);
        REQUIRE(read_file(repo.metadata() / "HEAD") == head);
        REQUIRE(read_ref(repo, "refs/heads/main") == valid);
        REQUIRE(read_ref(repo, "refs/heads/bad") == incomplete);
    }
}
TEST_CASE("References reject unsafe corrupt missing and noncommit targets", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto id = fixture_commit(repo);
    const auto head = read_file(repo.metadata() / "HEAD");
    for (const auto name : {"", "main", "/refs/heads/x", "refs/heads/", "refs//x", "refs/heads/a..b", "refs/heads/.x", "refs/heads/x.lock", "refs/heads/x/../y", "refs/heads/a\\b", "refs/heads/a b", "refs/heads/x@{y", "refs/heads/x~", "refs/heads/x^", "refs/heads/x?", "refs/heads/x*", "refs/heads/x[", "refs/heads/x:", "refs/heads/x."}) {
        REQUIRE_THROWS_AS(validate_ref_name(name), Error);
        REQUIRE_THROWS_AS(update_ref(repo, name, std::nullopt, id), Error);
    }
    REQUIRE_THROWS_AS(validate_ref_name(std::string("refs/heads/a\0b", 14)), Error);
    REQUIRE_THROWS_AS(set_head(repo, "refs/tags/tag"), Error);
    ObjectStore store(repo.metadata() / "objects");
    const auto blob = store.write(ObjectType::Blob, to_bytes("data"));
    REQUIRE_THROWS_AS(set_head(repo, blob.hex()), Error);
    REQUIRE_THROWS_AS(update_ref(repo, "refs/heads/blob", std::nullopt, blob), Error);
    REQUIRE_THROWS_AS(resolve_commit(repo, blob.hex()), Error);
    REQUIRE_THROWS_AS(resolve_commit(repo, ObjectId{}.hex()), Error);
    REQUIRE(read_file(repo.metadata() / "HEAD") == head);
    atomic_write(repo.metadata() / "refs/heads/broken", to_bytes("invalid\n"));
    REQUIRE_THROWS_AS(read_ref(repo, "refs/heads/broken"), Error);
    REQUIRE_THROWS_AS(list_refs(repo, "refs/heads/"), Error);
    atomic_write(repo.metadata() / "refs/heads/broken", to_bytes(blob.hex() + "\n"));
    REQUIRE_THROWS_AS(resolve_commit(repo, "broken"), Error);
    atomic_write(repo.metadata() / "refs/heads/broken", to_bytes(ObjectId{}.hex() + "\n"));
    REQUIRE_THROWS_AS(resolve_commit(repo, "broken"), Error);
    atomic_write(repo.metadata() / "HEAD", to_bytes("ref: refs/tags/broken\n"));
    REQUIRE_THROWS_AS(read_head(repo), Error);
}
TEST_CASE("References reject symlinks at file and intermediate directory levels", "[history]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path() / "repo");
    detail::ensure_directory(temp.path() / "outside");
    atomic_write(temp.path() / "outside/file", to_bytes(ObjectId{}.hex() + "\n"));
    std::filesystem::create_directory_symlink(temp.path() / "outside", repo.metadata() / "refs/heads/link");
    std::filesystem::create_symlink(temp.path() / "outside/file", repo.metadata() / "refs/heads/filelink");
    RepoLock lock(repo.metadata());
    const auto id = fixture_commit(repo);
    REQUIRE_THROWS_AS(read_ref(repo, "refs/heads/link/file"), Error);
    REQUIRE_THROWS_AS(read_ref(repo, "refs/heads/filelink"), Error);
    REQUIRE_THROWS_AS(update_ref(repo, "refs/heads/link/new", std::nullopt, id), Error);
    REQUIRE_THROWS_AS(update_ref(repo, "refs/heads/filelink", std::nullopt, id), Error);
    REQUIRE_THROWS_AS(list_refs(repo, "refs/heads/"), Error);
    REQUIRE_FALSE(std::filesystem::exists(temp.path() / "outside/new"));
}
