#include <catch2/catch_test_macros.hpp>
#include "minigit/worktree_update.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/worktree.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <sys/stat.h>
using namespace minigit;
namespace {
void update_file(const Repository& repo, std::string_view path, std::string_view content, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(content), permissions);
}
WorktreeTarget update_target(const Repository& repo, std::string_view content, std::string mode = "100644", std::optional<std::uint32_t> permissions = std::nullopt) {
    return {FileState{std::move(mode), ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes(content))}, permissions};
}
const WorkChange& update_change(const TransactionPlan& plan, std::string_view path) {
    const auto found = std::find_if(plan.work.begin(), plan.work.end(), [&](const auto& change) { return change.path == path; });
    REQUIRE(found != plan.work.end());
    return *found;
}
void update_same_plan(const TransactionPlan& left, const TransactionPlan& right) {
    REQUIRE(left.work.size() == right.work.size());
    REQUIRE(left.metadata.size() == right.metadata.size());
    for (std::size_t i = 0; i < left.work.size(); ++i) {
        REQUIRE(left.work[i].path == right.work[i].path);
        REQUIRE(left.work[i].before == right.work[i].before);
        REQUIRE(left.work[i].after == right.work[i].after);
    }
    for (std::size_t i = 0; i < left.metadata.size(); ++i) {
        REQUIRE(left.metadata[i].path == right.metadata[i].path);
        REQUIRE(left.metadata[i].before == right.metadata[i].before);
        REQUIRE(left.metadata[i].after == right.metadata[i].after);
    }
}
std::size_t update_object_count(const Repository& repo) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects")) if (entry.is_regular_file()) ++count;
    return count;
}
}
TEST_CASE("WorktreeUpdate plans multiple files deletion empty blobs permissions and new parent directories", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    update_file(repo, "old", "before", 0640);
    update_file(repo, "keep-parent/deleted", "deleted");
    WorktreeTargets targets{{"old", update_target(repo, "after", "100755", 04751)}, {"new/deep/empty", update_target(repo, "")}, {"keep-parent/deleted", {}}};
    TransactionPlan plan;
    plan.metadata.push_back({"HEAD", read_file(repo.metadata() / "HEAD"), read_file(repo.metadata() / "HEAD")});
    plan_worktree_update(repo, plan, targets, {"old", "keep-parent/deleted"});
    REQUIRE(update_change(plan, "old").before.permissions == 0640);
    REQUIRE(update_change(plan, "old").after.permissions == 04751);
    REQUIRE(update_change(plan, "new").after.kind == NodeKind::Directory);
    REQUIRE(update_change(plan, "new/deep").after.permissions == 0755);
    REQUIRE(update_change(plan, "new/deep/empty").after.kind == NodeKind::File);
    REQUIRE(update_change(plan, "new/deep/empty").after.permissions == 0644);
    REQUIRE(std::none_of(plan.work.begin(), plan.work.end(), [](const auto& change) { return change.path == "keep-parent"; }));
    REQUIRE(to_string(read_file(repo.root() / "old")) == "before");
    REQUIRE_FALSE(std::filesystem::exists(repo.root() / "new"));
    execute_transaction(repo, plan);
    REQUIRE(to_string(read_file(repo.root() / "old")) == "after");
    REQUIRE(read_worktree_file(repo, "old")->data.permissions == 04751);
    REQUIRE(read_file(repo.root() / "new/deep/empty").empty());
    REQUIRE_FALSE(std::filesystem::exists(repo.root() / "keep-parent/deleted"));
    REQUIRE(std::filesystem::is_directory(repo.root() / "keep-parent"));
}
TEST_CASE("WorktreeUpdate supports both directory file conversions with complete inventory and explicit parent deletion", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    TransactionPlan plan;
    SECTION("tracked file becomes a directory containing a new file") {
        update_file(repo, "item", "original", 0640);
        const WorktreeTargets targets{{"item", {}}, {"item/deep/child", update_target(repo, "child")}};
        plan_worktree_update(repo, plan, targets, {"item"});
        REQUIRE(update_change(plan, "item").before.kind == NodeKind::File);
        REQUIRE(update_change(plan, "item").after.kind == NodeKind::Directory);
        execute_transaction(repo, plan);
        REQUIRE(to_string(read_file(repo.root() / "item/deep/child")) == "child");
    }
    SECTION("directory becomes a file and records every old node") {
        update_file(repo, "item/deep/child", "child");
        update_file(repo, "item/other", "other");
        const WorktreeTargets targets{{"item", update_target(repo, "replacement")}, {"item/deep/child", {}}, {"item/other", {}}};
        plan_worktree_update(repo, plan, targets, {"item/deep/child", "item/other"});
        REQUIRE(plan.work.size() == 4);
        REQUIRE(update_change(plan, "item/deep").after.kind == NodeKind::Absent);
        execute_transaction(repo, plan);
        REQUIRE(to_string(read_file(repo.root() / "item")) == "replacement");
    }
    SECTION("an owned empty directory can be replaced as the exact tracked path") {
        std::filesystem::create_directory(repo.root() / "item");
        plan_worktree_update(repo, plan, {{"item", update_target(repo, "replacement")}}, {"item"});
        execute_transaction(repo, plan);
        REQUIRE(std::filesystem::is_regular_file(repo.root() / "item"));
    }
}
TEST_CASE("WorktreeUpdate preserves ignored unknown siblings and empty directories when parents are unchanged", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    update_file(repo, "d/tracked", "before");
    update_file(repo, "d/ignored", "ignored");
    update_file(repo, ".minigitignore", "d/ignored\n");
    std::filesystem::create_directory(repo.root() / "d/unknown-empty");
    WorktreeTarget target;
    SECTION("updating a leaf keeps unrelated contents") { target = update_target(repo, "after"); }
    SECTION("deleting a leaf keeps its parent and unrelated contents") {}
    TransactionPlan plan;
    plan_worktree_update(repo, plan, {{"d/tracked", target}}, {"d/tracked"});
    REQUIRE(plan.work.size() == 1);
    REQUIRE(plan.work.front().path == "d/tracked");
    execute_transaction(repo, plan);
    REQUIRE(to_string(read_file(repo.root() / "d/ignored")) == "ignored");
    REQUIRE(std::filesystem::is_directory(repo.root() / "d/unknown-empty"));
    REQUIRE(std::filesystem::is_directory(repo.root() / "d"));
}
TEST_CASE("WorktreeUpdate requires exact file ownership explicit descendant deletion and unknown empty directory protection", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    update_file(repo, "d/tracked", "tracked");
    TransactionPlan plan;
    SECTION("owning an ancestor does not authorize overwriting its child") {
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d/tracked", update_target(repo, "after")}}, {"d"}), Error);
    }
    SECTION("owning a child does not authorize deleting its sibling including ignored files") {
        update_file(repo, "d/sibling", "sibling");
        update_file(repo, ".minigitignore", "d/sibling\n");
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d/tracked", {}}, {"d/sibling", {}}}, {"d/tracked"}), Error);
    }
    SECTION("same bytes but different permissions still require exact ownership") {
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d/tracked", update_target(repo, "tracked", "100644", 0640)}}, {}), Error);
    }
    SECTION("directory replacement requires explicit deletion of every existing file") {
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d", update_target(repo, "replacement")}}, {"d/tracked"}), Error);
    }
    SECTION("unknown empty directories cannot be removed with a known sibling") {
        std::filesystem::create_directory(repo.root() / "d/empty");
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d", update_target(repo, "replacement")}, {"d/tracked", {}}}, {"d/tracked"}), Error);
    }
    SECTION("blocking owned parent files require an explicit null target") {
        update_file(repo, "parent", "parent");
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"parent/child", update_target(repo, "child")}}, {"parent"}), Error);
    }
    REQUIRE(plan.work.empty());
    REQUIRE(to_string(read_file(repo.root() / "d/tracked")) == "tracked");
}
TEST_CASE("WorktreeUpdate rejects invalid schemas unsafe types budgets and conflicting plans before backup capture", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    update_file(repo, "file", "before");
    const auto valid = update_target(repo, "after");
    const auto tree = ObjectStore(repo.metadata() / "objects").write(ObjectType::Tree, encode_tree({}));
    TransactionPlan plan;
    plan.metadata.push_back({"HEAD", read_file(repo.metadata() / "HEAD"), read_file(repo.metadata() / "HEAD")});
    const auto saved = plan;
    const auto count = update_object_count(repo);
    SECTION("target mode ID and permissions are validated") {
        const std::vector<WorktreeTarget> invalid{{FileState{"100644", ObjectId{}}, {}}, {FileState{"100644", tree}, {}}, {FileState{"120000", valid.file->id}, {}}, {valid.file, 010000}, {valid.file, 0000}, {valid.file, 0755}, {std::nullopt, 0644}};
        for (const auto& target : invalid) REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"file", target}}, {"file"}), Error);
    }
    SECTION("nonnull parent and child cannot both be files") {
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"file", valid}, {"file/child", valid}}, {"file"}), Error);
    }
    SECTION("path and input count limits are enforced") {
        for (const auto& path : std::vector<std::string>{"../file", "file\\child", std::string(4097, 'x')})
            REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{path, valid}}, {}), Error);
        std::string deep = "x";
        for (unsigned i = 0; i < 128; ++i) deep += "/x";
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{deep, valid}}, {}), Error);
        std::set<std::string> many;
        // Sorted keys and an end hint keep this oversized fixture linear;
        // the boundary under test is still exactly 200001 distinct paths.
        for (unsigned i = 0; i <= 200000; ++i) {
            const auto digits = std::to_string(i);
            many.insert(many.end(), "p" + std::string(6 - digits.size(), '0') + digits);
        }
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {}, many), Error);
    }
    SECTION("expanded ancestor path bytes are bounded before filesystem inspection and backup capture") {
        WorktreeTargets expanded;
        constexpr std::size_t paths = 258;
        constexpr std::size_t depth = 128;
        constexpr std::size_t component_length = 31;
        std::size_t input_bytes = 0;
        std::size_t expanded_bytes = 0;
        for (std::size_t i = 0; i < paths; ++i) {
            auto root = "r" + std::to_string(i);
            root.append(component_length - root.size(), 'r');
            auto path = root;
            expanded_bytes += path.size();
            for (std::size_t level = 1; level < depth; ++level) {
                path += "/" + std::string(component_length, 'x');
                expanded_bytes += path.size();
            }
            REQUIRE(path.size() == 4095);
            input_bytes += path.size();
            expanded.emplace(std::move(path), valid);
        }
        REQUIRE(input_bytes < 2U * 1024U * 1024U);
        REQUIRE(expanded_bytes > max_file_size);
        // Distinct first components make every ancestor key distinct. Nothing
        // exists at those paths, so rejection must come from expansion budget.
        try {
            plan_worktree_update(repo, plan, expanded, {});
            FAIL("expanded path budget must reject the plan");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("展开目标路径") != std::string::npos);
        }
    }
    SECTION("symlinks and FIFOs are rejected") {
        std::filesystem::create_symlink(repo.root() / "file", repo.root() / "link");
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"link", valid}}, {"link"}), Error);
        REQUIRE(::mkfifo((repo.root() / "fifo").c_str(), 0600) == 0);
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"fifo", valid}}, {"fifo"}), Error);
    }
    SECTION("an existing work plan cannot be replaced") {
        plan.work.push_back({"elsewhere", {}, {}});
        const auto occupied = plan;
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"file", valid}}, {"file"}), Error);
        update_same_plan(plan, occupied);
        plan.work.clear();
    }
    update_same_plan(plan, saved);
    REQUIRE(update_object_count(repo) == count);
    REQUIRE(to_string(read_file(repo.root() / "file")) == "before");
}
TEST_CASE("WorktreeUpdate index planning distinguishes missing bytes no-op and conflict stages while rejecting corrupt inputs", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto blob = update_target(repo, "blob").file->id;
    const auto tree = ObjectStore(repo.metadata() / "objects").write(ObjectType::Tree, encode_tree({}));
    TransactionPlan plan;
    SECTION("missing index is distinct from a persisted empty index") {
        plan_index_update(repo, plan, {});
        REQUIRE(plan.metadata.size() == 1);
        REQUIRE_FALSE(plan.metadata.front().before.has_value());
        REQUIRE(plan.metadata.front().after == encode_index({}));
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "index"));
        save_index(repo, {});
        plan = {};
        plan_index_update(repo, plan, {});
        REQUIRE(plan.metadata.empty());
    }
    SECTION("all conflict stages retain precise before bytes and validate every object") {
        save_index(repo, {{"file", "100644", blob}});
        const auto before = read_file(repo.metadata() / "index");
        const Index conflicts{{"file", "100644", blob, 1}, {"file", "100755", blob, 2}, {"file", "100644", blob, 3}};
        plan_index_update(repo, plan, conflicts);
        REQUIRE(plan.metadata.front().before == before);
        REQUIRE(decode_index(*plan.metadata.front().after) == conflicts);
        const auto saved = plan;
        REQUIRE_THROWS_AS(plan_index_update(repo, plan, conflicts), Error);
        update_same_plan(plan, saved);
        TransactionPlan invalid;
        REQUIRE_THROWS_AS(plan_index_update(repo, invalid, {{"file", "100644", blob, 1}, {"file", "100644", tree, 3}}), Error);
        REQUIRE_THROWS_AS(plan_index_update(repo, invalid, {{"file", "100644", ObjectId{}, 3}}), Error);
        REQUIRE(invalid.metadata.empty());
        REQUIRE(read_file(repo.metadata() / "index") == before);
    }
    SECTION("damaged index cannot be silently replaced even by a valid empty target") {
        atomic_write(repo.metadata() / "index", to_bytes("damaged"));
        REQUIRE_THROWS_AS(plan_index_update(repo, plan, {}), Error);
        REQUIRE(plan.metadata.empty());
        REQUIRE(to_string(read_file(repo.metadata() / "index")) == "damaged");
    }
    SECTION("before index object corruption is validated as well") {
        save_index(repo, {{"file", "100644", tree, 1}});
        REQUIRE_THROWS_AS(plan_index_update(repo, plan, {}), Error);
        REQUIRE(plan.metadata.empty());
    }
}
TEST_CASE("WorktreeUpdate transaction recovery restores worktree permissions and index together at two key boundaries", "[worktree-update][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    update_file(repo, "file", "before", 0640);
    const auto old = update_target(repo, "before");
    const auto next = update_target(repo, "after", "100755");
    save_index(repo, {{"file", old.file->mode, old.file->id}});
    const auto index_before = read_file(repo.metadata() / "index");
    const auto head_before = read_file(repo.metadata() / "HEAD");
    TransactionPlan plan;
    plan_worktree_update(repo, plan, {{"file", next}}, {"file"});
    plan_index_update(repo, plan, {{"file", next.file->mode, next.file->id}});
    bool after = false;
    std::string stop;
    SECTION("Prepared after work publication restores old work and index") { stop = "work.write:file"; }
    SECTION("Committed restores target work and index without repeated business planning") { stop = "journal.committed"; after = true; }
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) { if (label == stop) throw Error("injected stop"); }), Error);
    recover_transaction(repo);
    recover_transaction(repo);
    REQUIRE(to_string(read_file(repo.root() / "file")) == (after ? "after" : "before"));
    REQUIRE(read_worktree_file(repo, "file")->data.permissions == (after ? 0755U : 0640U));
    REQUIRE(read_file(repo.metadata() / "index") == (after ? *plan.metadata.front().after : index_before));
    REQUIRE(read_file(repo.metadata() / "HEAD") == head_before);
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
