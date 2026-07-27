#include <catch2/catch_test_macros.hpp>
#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "temp_directory.hpp"
#include <algorithm>
using namespace minigit;
namespace {
Commit fixture(const ObjectId& tree, std::vector<ObjectId> parents, std::string message) {
    return {tree, std::move(parents), "Author <author@example.test> 0 +0000", "Committer <committer@example.test> 0 +0000", std::move(message)};
}
ObjectId write_commit(ObjectStore& store, const ObjectId& tree, std::vector<ObjectId> parents, std::string message) {
    return store.write(ObjectType::Commit, encode_commit(fixture(tree, std::move(parents), std::move(message))));
}
std::size_t position(const std::vector<ObjectId>& order, const ObjectId& id) {
    const auto found = std::find(order.begin(), order.end(), id);
    REQUIRE(found != order.end());
    return static_cast<std::size_t>(found - order.begin());
}
}
TEST_CASE("DAG loads complete linear and shared diamond ancestry with deterministic child first order", "[dag]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto tree = store.write(ObjectType::Tree, encode_tree({}));
    const auto base = write_commit(store, tree, {}, "base");
    const auto trunk = write_commit(store, tree, {base}, "trunk");
    const auto left = write_commit(store, tree, {trunk}, "left");
    const auto right = write_commit(store, tree, {trunk}, "right");
    const auto merge = write_commit(store, tree, {right, left}, "merge");
    const auto graph = load_commit_graph(repo, {merge});
    REQUIRE(graph.size() == 5);
    REQUIRE((graph.at(merge).parents == std::vector<ObjectId>{right, left}));
    const std::vector<ObjectId> expected{merge, std::min(left, right), std::max(left, right), trunk, base};
    REQUIRE(topological_order(graph) == expected);
    for (const auto& [id, commit] : graph)
        for (const auto& parent : commit.parents) REQUIRE(position(expected, id) < position(expected, parent));
    REQUIRE((topological_order(load_commit_graph(repo, {trunk})) == std::vector<ObjectId>{trunk, base}));
    REQUIRE(topological_order(load_commit_graph(repo, {})).empty());
}
TEST_CASE("DAG handles multiple roots and duplicate parent edges without duplicate nodes", "[dag]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto tree = store.write(ObjectType::Tree, encode_tree({}));
    const auto base = write_commit(store, tree, {}, "base");
    const auto left = write_commit(store, tree, {base, base}, "redundant parent");
    const auto right = write_commit(store, tree, {base}, "other root");
    const auto graph = load_commit_graph(repo, {right, left, right, base});
    REQUIRE(graph.size() == 3);
    REQUIRE(graph.at(left).parents.size() == 2);
    const std::vector<ObjectId> expected{std::min(left, right), std::max(left, right), base};
    REQUIRE(topological_order(graph) == expected);
    REQUIRE(topological_order(load_commit_graph(repo, {base, left, right})) == expected);
}
TEST_CASE("DAG rejects missing noncommit and damaged ancestors plus incomplete or cyclic graphs", "[dag]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto tree = store.write(ObjectType::Tree, encode_tree({}));
    const auto blob = store.write(ObjectType::Blob, to_bytes("not a commit"));
    const auto missing = write_commit(store, tree, {ObjectId{}}, "missing parent");
    const auto wrong = write_commit(store, tree, {blob}, "wrong parent");
    REQUIRE_THROWS_AS(load_commit_graph(repo, {missing}), Error);
    REQUIRE_THROWS_AS(load_commit_graph(repo, {wrong}), Error);
    REQUIRE_THROWS_AS(load_commit_graph(repo, {blob}), Error);
    const auto damaged = write_commit(store, tree, {}, "damaged ancestor");
    const auto tip = write_commit(store, tree, {damaged}, "tip");
    atomic_write(store.object_path(damaged), to_bytes("damaged compressed data"));
    REQUIRE_THROWS_AS(load_commit_graph(repo, {tip}), Error);
    const auto a = sha256(to_bytes("graph node a"));
    const auto b = sha256(to_bytes("graph node b"));
    REQUIRE_THROWS_AS(topological_order({{a, fixture(tree, {b}, "dangling")}}), Error);
    REQUIRE_THROWS_AS(topological_order({{a, fixture(tree, {a}, "self cycle")}}), Error);
    REQUIRE_THROWS_AS(topological_order({{a, fixture(tree, {b}, "a")}, {b, fixture(tree, {a}, "b")}}), Error);
}
TEST_CASE("Log preserves IDs multiline messages names and validates complete closure before display limits", "[dag]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    REQUIRE(log_history(repo).empty());
    REQUIRE(log_history(repo, "HEAD", 0).empty());
    ObjectId base, tip, broken;
    {
        RepoLock lock(repo.metadata());
        ObjectStore store(repo.metadata() / "objects");
        const auto tree = store.write(ObjectType::Tree, encode_tree({}));
        base = write_commit(store, tree, {}, "base");
        tip = write_commit(store, tree, {base}, "title\n\nmessage body\nsecond line\n");
        broken = write_commit(store, tree, {ObjectId{}}, "missing ancestor");
        update_ref(repo, "refs/heads/main", std::nullopt, tip);
        update_ref(repo, "refs/tags/release", std::nullopt, tip);
    }
    const auto history = log_history(repo);
    REQUIRE(history.size() == 2);
    REQUIRE(history[0].id == tip);
    REQUIRE(history[1].id == base);
    REQUIRE(history[0].commit.message == "title\n\nmessage body\nsecond line\n");
    REQUIRE(history[0].commit.parents == std::vector<ObjectId>{base});
    REQUIRE(log_history(repo, "main", 1).size() == 1);
    REQUIRE(log_history(repo, "release", 1)[0].id == tip);
    REQUIRE(log_history(repo, "refs/heads/main")[0].id == tip);
    REQUIRE(log_history(repo, tip.hex())[0].id == tip);
    REQUIRE(log_history(repo, "HEAD", 0).empty());
    REQUIRE(log_history(repo, "HEAD", 10).size() == 2);
    REQUIRE_THROWS_AS(log_history(repo, broken.hex(), 0), Error);
    REQUIRE_THROWS_AS(log_history(repo, broken.hex(), 1), Error);
}
