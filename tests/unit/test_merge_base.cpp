#include <catch2/catch_test_macros.hpp>
#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "temp_directory.hpp"
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
using namespace minigit;
namespace {
struct BaseFixture {
    test::TempDirectory temp;
    Repository repo;
    RepoLock lock;
    ObjectStore store;
    ObjectId tree;
    BaseFixture() : repo(Repository::init(temp.path())), lock(repo.metadata()),
                    store(repo.metadata() / "objects"), tree(store.write(ObjectType::Tree, encode_tree({}))) {}
    ObjectId commit(std::vector<ObjectId> parents, std::string message) {
        return store.write(ObjectType::Commit, encode_commit({tree, std::move(parents),
            "Author <author@example.test> 0 +0000", "Committer <committer@example.test> 0 +0000", std::move(message)}));
    }
};
void base_in_both_orders(const Repository& repo, const ObjectId& left, const ObjectId& right,
                         const std::optional<ObjectId>& expected) {
    REQUIRE(find_merge_base(repo, left, right) == expected);
    REQUIRE(find_merge_base(repo, right, left) == expected);
}
std::map<std::string, Bytes> repository_bytes(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.root()))
        if (entry.is_regular_file()) result.emplace(entry.path().lexically_relative(repo.root()).generic_string(), read_file(entry.path()));
    return result;
}
}
TEST_CASE("MergeBase returns the identical tip including a parentless root without writing repository state", "[merge_base]") {
    BaseFixture fixture;
    const auto root = fixture.commit({}, "root"), tip = fixture.commit({root}, "tip");
    atomic_write(fixture.repo.root() / "sentinel", to_bytes("unchanged Work"));
    const auto before = repository_bytes(fixture.repo);
    REQUIRE(find_merge_base(fixture.repo, root, root) == root);
    REQUIRE(find_merge_base(fixture.repo, tip, tip) == tip);
    REQUIRE(repository_bytes(fixture.repo) == before);
}
TEST_CASE("MergeBase selects the ancestor in either argument order along a linear history", "[merge_base]") {
    BaseFixture fixture;
    const auto root = fixture.commit({}, "root"), trunk = fixture.commit({root}, "trunk"), tip = fixture.commit({trunk}, "tip");
    base_in_both_orders(fixture.repo, root, tip, root);
    base_in_both_orders(fixture.repo, trunk, tip, trunk);
}
TEST_CASE("MergeBase selects the deepest shared fork after independent private chains", "[merge_base]") {
    BaseFixture fixture;
    const auto root = fixture.commit({}, "root"), shared = fixture.commit({root}, "shared");
    auto left = shared, right = shared;
    for (unsigned i = 0; i < 5; ++i) left = fixture.commit({left}, "left-" + std::to_string(i));
    for (unsigned i = 0; i < 7; ++i) right = fixture.commit({right}, "right-" + std::to_string(i));
    base_in_both_orders(fixture.repo, left, right, shared);
}
TEST_CASE("MergeBase considers every parent independently of parent order and first parent ancestry", "[merge_base]") {
    BaseFixture fixture;
    const auto root = fixture.commit({}, "root"), common = fixture.commit({root}, "common");
    const auto private_left = fixture.commit({}, "private-left"), private_right = fixture.commit({}, "private-right");
    const auto left = fixture.commit({private_left, common}, "left-merge");
    const auto right = fixture.commit({private_right, common}, "right-merge");
    const auto reordered_left = fixture.commit({common, private_left}, "left-reordered");
    const auto reordered_right = fixture.commit({common, private_right}, "right-reordered");
    base_in_both_orders(fixture.repo, left, right, common);
    base_in_both_orders(fixture.repo, reordered_left, right, common);
    base_in_both_orders(fixture.repo, left, reordered_right, common);
}
TEST_CASE("MergeBase removes all older common diamond nodes when a shared merge is their common descendant", "[merge_base]") {
    BaseFixture fixture;
    const auto root = fixture.commit({}, "root"), a = fixture.commit({root}, "a"), b = fixture.commit({root}, "b");
    const auto shared_merge = fixture.commit({a, b}, "shared-merge");
    const auto left = fixture.commit({shared_merge, a}, "left"), right = fixture.commit({b, shared_merge}, "right");
    base_in_both_orders(fixture.repo, left, right, shared_merge);
}
TEST_CASE("MergeBase rejects crisscross histories with two incomparable lowest common ancestors", "[merge_base]") {
    BaseFixture fixture;
    const auto root = fixture.commit({}, "root"), a = fixture.commit({root}, "a"), b = fixture.commit({root}, "b");
    const auto left = fixture.commit({a, b}, "left-merge"), right = fixture.commit({b, a}, "right-merge");
    const auto before = repository_bytes(fixture.repo);
    for (const auto& pair : {std::pair{left, right}, std::pair{right, left}}) {
        try { (void)find_merge_base(fixture.repo, pair.first, pair.second); FAIL("ambiguous lowest ancestors accepted"); }
        catch (const Error& error) { REQUIRE(std::string(error.what()).find("多个最低共同祖先") != std::string::npos); }
    }
    REQUIRE(repository_bytes(fixture.repo) == before);
}
TEST_CASE("MergeBase distinguishes unrelated commits sharing a tree and treats duplicate edges idempotently without expanding trees", "[merge_base]") {
    BaseFixture fixture;
    const auto root = fixture.commit({}, "root"), unrelated = fixture.commit({}, "unrelated same-tree root");
    const auto left = fixture.commit({root, root}, "duplicate parents"), right = fixture.commit({root}, "right");
    base_in_both_orders(fixture.repo, left, right, root);
    base_in_both_orders(fixture.repo, left, unrelated, {});
    std::filesystem::remove(fixture.store.object_path(fixture.tree));
    const auto before = repository_bytes(fixture.repo);
    base_in_both_orders(fixture.repo, left, right, root);
    base_in_both_orders(fixture.repo, root, unrelated, {});
    REQUIRE(find_merge_base(fixture.repo, left, left) == left);
    REQUIRE(repository_bytes(fixture.repo) == before);
}
TEST_CASE("MergeBase validates damaged missing and noncommit ancestry even for identical or ancestor tips", "[merge_base]") {
    BaseFixture fixture;
    const auto good = fixture.commit({}, "good");
    ObjectId broken;
    SECTION("missing ancestor") { broken = fixture.commit({ObjectId{}}, "missing-parent commit"); }
    SECTION("noncommit ancestor") {
        const auto blob = fixture.store.write(ObjectType::Blob, to_bytes("not a commit"));
        broken = fixture.commit({blob}, "blob-parent commit");
    }
    SECTION("damaged deep ancestor") {
        const auto damaged = fixture.commit({}, "damaged");
        const auto intermediate = fixture.commit({damaged}, "intermediate");
        broken = fixture.commit({intermediate}, "broken ancestry tip");
        atomic_write(fixture.store.object_path(damaged), to_bytes("damaged compressed bytes"));
    }
    SECTION("noncommit tip") { broken = fixture.store.write(ObjectType::Blob, to_bytes("blob tip")); }
    const auto child = fixture.commit({broken}, "child above broken closure");
    const auto before = repository_bytes(fixture.repo);
    REQUIRE_THROWS_AS(find_merge_base(fixture.repo, broken, broken), Error);
    REQUIRE_THROWS_AS(find_merge_base(fixture.repo, broken, child), Error);
    REQUIRE_THROWS_AS(find_merge_base(fixture.repo, child, broken), Error);
    REQUIRE_THROWS_AS(find_merge_base(fixture.repo, child, good), Error);
    REQUIRE(repository_bytes(fixture.repo) == before);
}
