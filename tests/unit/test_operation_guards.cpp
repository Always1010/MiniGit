#include <catch2/catch_test_macros.hpp>
#include "minigit/branches.hpp"
#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/inspection.hpp"
#include "minigit/move.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "minigit/remove.hpp"
#include "minigit/reset.hpp"
#include "minigit/restore.hpp"
#include "minigit/status.hpp"
#include "minigit/switch.hpp"
#include "minigit/tags.hpp"
#include "minigit/worktree.hpp"
#include "minigit/worktree_update.hpp"
#include "temp_directory.hpp"
#include <map>
#include <utility>
using namespace minigit;
namespace {
OperationState guard_fixture(const Repository& repo) {
    configure_identity(repo, "user.name", "Test");
    configure_identity(repo, "user.email", "test@example.test");
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto base = store.write(ObjectType::Blob, to_bytes("base\n"));
    const auto other = store.write(ObjectType::Blob, to_bytes("movable\n"));
    const auto incoming = store.write(ObjectType::Blob, to_bytes("incoming\n"));
    const Snapshot original{{"move", {"100644", other}}, {"note", {"100644", base}}};
    auto source = original;
    source.at("note").id = incoming;
    const std::string actor = "Test <test@example.test> 0 +0000";
    const auto head = store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, original), {}, actor, actor, "original"}));
    const auto theirs = store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, source), {head}, actor, actor, "source"}));
    update_ref(repo, "refs/heads/main", std::nullopt, head);
    update_ref(repo, "refs/heads/topic", std::nullopt, head);
    Index index;
    for (const auto& [path, file] : original) {
        index.push_back({path, file.mode, file.id});
        atomic_write_with_permissions(repo.root() / path, store.read(file.id).payload, 0640);
    }
    save_index(repo, index);
    OperationState state;
    state.original_head = state.current_head = read_head(repo);
    state.original_head_bytes = read_file(repo.metadata() / "HEAD");
    state.original_index_blob = store.write(ObjectType::Blob, read_file(repo.metadata() / "index"));
    state.source_commit = theirs;
    state.message = "Merge source";
    TransactionPlan plan;
    plan_operation_update(repo, plan, std::nullopt, state);
    execute_transaction(repo, std::move(plan));
    return state;
}
using GuardFiles = std::map<std::string, std::pair<Bytes, std::uint32_t>>;
GuardFiles guard_files(const Repository& repo) {
    GuardFiles files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.root())) {
        if (!std::filesystem::is_regular_file(entry.symlink_status())) continue;
        auto data = read_file_data(entry.path());
        files.emplace(entry.path().lexically_relative(repo.root()).generic_string(),
                      std::make_pair(std::move(data.bytes), data.permissions));
    }
    return files;
}
void guard_conflict(const Repository& repo, const OperationState& state) {
    RepoLock lock(repo.metadata());
    const auto incoming = read_snapshot(repo, read_commit(repo, state.source_commit).tree).at("note");
    Index conflicted;
    for (const auto& entry : load_index(repo)) {
        if (entry.path != "note") conflicted.push_back(entry);
        else {
            conflicted.push_back({entry.path, entry.mode, entry.id, 1});
            conflicted.push_back({entry.path, entry.mode, entry.id, 2});
            conflicted.push_back({entry.path, incoming.mode, incoming.id, 3});
        }
    }
    save_index(repo, conflicted);
}
}
TEST_CASE("Operation guards reject history movement for valid damaged and unsafe state nodes without publication", "[operation][guard]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path() / "repo");
    const auto state = guard_fixture(repo);
    const auto operation = repo.metadata() / "operation";
    bool symlink = false, directory = false;
    SECTION("a valid unresolved operation blocks even otherwise valid history mutations") {}
    SECTION("an existing empty state fails closed") { atomic_write(operation, {}); }
    SECTION("a corrupt state fails closed") { atomic_write(operation, to_bytes("damaged")); }
    SECTION("a state symlink preserves its external target") {
        std::filesystem::remove(operation);
        atomic_write(temp.path() / "outside", to_bytes("external state"));
        std::filesystem::create_symlink(temp.path() / "outside", operation);
        symlink = true;
    }
    SECTION("a directory at the state path fails closed") {
        std::filesystem::remove(operation);
        std::filesystem::create_directory(operation);
        directory = true;
    }
    const auto before = guard_files(repo);
    REQUIRE_THROWS_AS(create_commit(repo, "blocked", true), Error);
    REQUIRE_THROWS_AS(switch_worktree(repo, {std::nullopt, false, "topic"}), Error);
    REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Hard, std::nullopt}), Error);
    REQUIRE_THROWS_AS(create_branch(repo, "new"), Error);
    REQUIRE_THROWS_AS(create_branch(repo, "topic", "HEAD", true), Error);
    REQUIRE_THROWS_AS(delete_branch(repo, "topic"), Error);
    REQUIRE_THROWS_AS(delete_branch(repo, "topic", true), Error);
    REQUIRE(guard_files(repo) == before);
    REQUIRE(read_head(repo) == state.current_head);
    if (symlink) {
        REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(operation)));
        REQUIRE(to_string(read_file(temp.path() / "outside")) == "external state");
    }
    if (directory) REQUIRE(std::filesystem::is_directory(operation));
}
TEST_CASE("Operation guards allow identity queries conflict resolution tags and caller-lock primitives", "[operation][guard]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto state = guard_fixture(repo);
    const auto saved = read_file(repo.metadata() / "operation");
    guard_conflict(repo, state);
    const auto status = collect_status(repo);
    REQUIRE(status.entries.size() == 1);
    REQUIRE(status.entries[0] == StatusEntry{"note", 'U', 'U'});
    REQUIRE(log_history(repo).size() == 1);
    REQUIRE(list_branches(repo).size() == 2);
    REQUIRE(to_string(show_object(repo, "HEAD:note")) == "base\n");
    DiffOptions diff;
    diff.revisions = {"HEAD", "HEAD"};
    REQUIRE(repository_diff(repo, diff).empty());
    configure_identity(repo, "user.name", "Resolver");
    REQUIRE(identity_value(repo, "user.name") == "Resolver");
    create_tag(repo, "during", "HEAD");
    REQUIRE(list_tags(repo).at("during") == *state.current_head.id);
    delete_tag(repo, "during");
    {
        RepoLock lock(repo.metadata());
        REQUIRE(read_reflog(repo, "HEAD").empty());
        TransactionPlan plan;
        plan_move_head(repo, plan, state.current_head, state.current_head, "internal");
        plan_create_branch(repo, plan, "internal", *state.current_head.id);
        plan_index_update(repo, plan, load_index(repo));
        plan_worktree_update(repo, plan, {}, {});
        REQUIRE_FALSE(plan.metadata.empty());
        // Continue/abort must be able to call these primitives under the lock.
        const auto stored = store_commit_snapshot(repo, {}, {*state.current_head.id}, "internal snapshot");
        REQUIRE(read_commit(repo, stored).parents == std::vector<ObjectId>{*state.current_head.id});
        ObjectStore store(repo.metadata() / "objects");
        REQUIRE(store.read(store.write(ObjectType::Blob, to_bytes("immutable"))).type == ObjectType::Blob);
    }
    SECTION("add stages the chosen resolution") {
        atomic_write(repo.root() / "note", to_bytes("resolved\n"));
        add_paths(repo, {repo.root() / "note"});
        REQUIRE(load_index(repo).back().stage == 0);
    }
    SECTION("explicit restore clears selected conflicts") {
        restore_paths(repo, {repo.root() / "note"}, {std::string("HEAD"), true, true});
        REQUIRE(load_index(repo).back().stage == 0);
        REQUIRE(to_string(read_file(repo.root() / "note")) == "base\n");
    }
    SECTION("rm retains its existing conflict protection and supports explicit force") {
        REQUIRE_THROWS_AS(remove_paths(repo, {repo.root() / "note"}), Error);
        remove_paths(repo, {repo.root() / "note"}, {true, true, false});
        REQUIRE(load_index(repo).size() == 1);
        REQUIRE(to_string(read_file(repo.root() / "note")) == "base\n");
    }
    SECTION("mv protects a selected conflict while carrying an unrelated clean file") {
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "note", repo.root() / "new-note"), Error);
        move_path(repo, repo.root() / "move", repo.root() / "moved");
        REQUIRE(load_index(repo).front().path == "moved");
        REQUIRE(load_index(repo).back().stage == 3);
        REQUIRE(to_string(read_file(repo.root() / "moved")) == "movable\n");
    }
    REQUIRE(read_head(repo) == state.current_head);
    REQUIRE(read_file(repo.metadata() / "operation") == saved);
    REQUIRE_FALSE(read_ref(repo, "refs/heads/internal"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
