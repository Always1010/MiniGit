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
WorktreeTarget update_directory_target(std::optional<std::uint32_t> permissions = std::nullopt) {
    return {std::nullopt, permissions, true};
}
void update_directory(const Repository& repo, std::string_view path, std::uint32_t permissions) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target);
    REQUIRE(::chmod(target.c_str(), permissions) == 0);
}
std::uint32_t directory_permissions(const Repository& repo, std::string_view path) {
    return static_cast<std::uint32_t>(std::filesystem::status(repo.root() / std::string(path)).permissions()) & 07777;
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
        // The same bounded expansion applies when the leaf is an explicit
        // Directory rather than a File, without a second oversized fixture.
        for (auto& [path, target] : expanded) { (void)path; target = update_directory_target(); }
        try {
            plan_worktree_update(repo, plan, expanded, {});
            FAIL("expanded Directory path budget must reject the plan");
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
    SECTION("total work and metadata budget is rejected before writing backup blobs") {
        auto crowded = plan;
        crowded.metadata.resize(100000, plan.metadata.front());
        auto crowded_before = crowded;
        try {
            plan_worktree_update(repo, crowded, {{"file", valid}}, {"file"});
            FAIL("combined journal change budget must reject the plan");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("总数超过事务限制") != std::string::npos);
        }
        update_same_plan(crowded, crowded_before);
        REQUIRE(update_object_count(repo) == count);
        // Reuse the same metadata fixture: the Directory chmod and its required
        // unchanged File image count as two WorkChanges before any backup write.
        crowded.metadata.resize(99999);
        crowded_before.metadata.resize(99999);
        update_file(repo, "d/child", "unbacked child");
        update_directory(repo, "d", 0755);
        try {
            plan_worktree_update(repo, crowded, {{"d", update_directory_target(0700)}}, {"d/child"});
            FAIL("forced Directory inventory must count against the combined budget");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("总数超过事务限制") != std::string::npos);
        }
        update_same_plan(crowded, crowded_before);
        REQUIRE(update_object_count(repo) == count);
    }
    SECTION("combined metadata image bytes are rejected before writing backup blobs") {
        TransactionPlan oversized;
        oversized.metadata.push_back({"index", Bytes(max_file_size / 2, 'a'), Bytes(max_file_size / 2 + 1, 'b')});
        const auto oversized_before = oversized;
        REQUIRE_THROWS_AS(plan_worktree_update(repo, oversized, {{"file", valid}}, {"file"}), Error);
        update_same_plan(oversized, oversized_before);
        REQUIRE(update_object_count(repo) == count);
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
TEST_CASE("WorktreeUpdate explicit directories create and restore empty nodes with full permissions while defaults preserve existing modes", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    TransactionPlan plan;
    std::uint32_t expected = 0755;
    SECTION("new directory defaults to 0755") {
        plan_worktree_update(repo, plan, {{"empty", update_directory_target()}}, {});
    }
    SECTION("an empty directory may have only owner read and execute plus special bits") {
        expected = 02500;
        plan_worktree_update(repo, plan, {{"empty", update_directory_target(expected)}}, {});
    }
    SECTION("restoring a removed empty directory preserves its exact original image") {
        update_directory(repo, "empty", 04750);
        const auto original = capture_work_node(repo, "empty");
        REQUIRE(std::filesystem::remove(repo.root() / "empty"));
        expected = original.permissions;
        plan_worktree_update(repo, plan, {{"empty", update_directory_target(expected)}}, {});
        REQUIRE(update_change(plan, "empty").after == original);
    }
    SECTION("existing default Directory target keeps its real full permissions") {
        expected = 04750;
        update_directory(repo, "empty", expected);
        plan_worktree_update(repo, plan, {{"empty", update_directory_target()}}, {});
        REQUIRE(plan.work.empty());
    }
    execute_transaction(repo, plan);
    REQUIRE(std::filesystem::is_directory(repo.root() / "empty"));
    REQUIRE(std::filesystem::is_empty(repo.root() / "empty"));
    REQUIRE(directory_permissions(repo, "empty") == expected);
}
TEST_CASE("WorktreeUpdate explicit directories support file conversions and retain explicit parent modes through expansion", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    TransactionPlan plan;
    SECTION("tracked file becomes an explicit directory with an empty child") {
        update_file(repo, "item", "dirty original", 0640);
        plan_worktree_update(repo, plan, {{"item", update_directory_target(04700)}, {"item/empty", update_directory_target(0500)}}, {"item"});
        REQUIRE(update_change(plan, "item").before.kind == NodeKind::File);
        REQUIRE(update_change(plan, "item").after.permissions == 04700);
        execute_transaction(repo, plan);
        REQUIRE(directory_permissions(repo, "item") == 04700);
        REQUIRE(directory_permissions(repo, "item/empty") == 0500);
    }
    SECTION("owned directory becomes a file including its owned empty child") {
        update_file(repo, "item/old", "old");
        update_directory(repo, "item/empty", 0500);
        const auto file = update_target(repo, "replacement");
        plan_worktree_update(repo, plan, {{"item", file}, {"item/old", {}}}, {"item/old", "item/empty"});
        REQUIRE(update_change(plan, "item/empty").before.permissions == 0500);
        REQUIRE(update_change(plan, "item/empty").after.kind == NodeKind::Absent);
        execute_transaction(repo, plan);
        REQUIRE(to_string(read_file(repo.root() / "item")) == "replacement");
    }
    SECTION("automatic file and Directory ancestors preserve explicit permissions") {
        const WorktreeTargets targets{{"item", update_directory_target(04700)},
            {"item/deep", update_directory_target(02710)}, {"item/deep/file", update_target(repo, "child")},
            {"item/empty", update_directory_target(0500)}};
        plan_worktree_update(repo, plan, targets, {});
        REQUIRE(update_change(plan, "item").after.permissions == 04700);
        REQUIRE(update_change(plan, "item/deep").after.permissions == 02710);
        execute_transaction(repo, plan);
        REQUIRE(directory_permissions(repo, "item") == 04700);
        REQUIRE(directory_permissions(repo, "item/deep") == 02710);
        REQUIRE(directory_permissions(repo, "item/empty") == 0500);
        REQUIRE(to_string(read_file(repo.root() / "item/deep/file")) == "child");
    }
    SECTION("parent keep inventory honors a child delete inventory and does not retain file noops below it") {
        update_file(repo, "d/sub/file", "delete me");
        update_file(repo, "d/keep", "keep dirty bytes", 0640);
        plan_worktree_update(repo, plan, {{"d", update_directory_target(02700)}, {"d/sub", {}}, {"d/sub/file", {}}}, {"d/sub/file", "d/keep"});
        REQUIRE(update_change(plan, "d/sub").after.kind == NodeKind::Absent);
        REQUIRE(update_change(plan, "d/sub/file").after.kind == NodeKind::Absent);
        REQUIRE(update_change(plan, "d/keep").before == update_change(plan, "d/keep").after);
        execute_transaction(repo, plan);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "d/sub"));
        REQUIRE(to_string(read_file(repo.root() / "d/keep")) == "keep dirty bytes");
        REQUIRE(directory_permissions(repo, "d") == 02700);
    }
}
TEST_CASE("WorktreeUpdate directory permission changes record known descendant noops and execute or recover complete images", "[worktree-update][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    update_file(repo, "d/sub/file", "dirty executable", 04751);
    update_file(repo, "d/rootfile", "dirty ordinary", 0640);
    update_directory(repo, "d", 0755);
    update_directory(repo, "d/sub", 0710);
    update_directory(repo, "d/empty", 0500);
    const auto head = read_file(repo.metadata() / "HEAD");
    TransactionPlan plan;
    plan_worktree_update(repo, plan, {{"d", update_directory_target(02700)}}, {"d/sub/file", "d/rootfile", "d/empty"});
    REQUIRE(plan.work.size() == 5);
    REQUIRE(update_change(plan, "d").before.permissions == 0755);
    REQUIRE(update_change(plan, "d").after.permissions == 02700);
    for (const auto& path : {"d/sub", "d/sub/file", "d/rootfile", "d/empty"})
        REQUIRE(update_change(plan, path).before == update_change(plan, path).after);
    bool after = true;
    std::string stop;
    SECTION("ordinary transaction permits the complete forced inventory") {}
    SECTION("Prepared after root chmod restores original full directory and file images") { stop = "work.chmod:d"; after = false; }
    SECTION("Committed recovery finishes exact target images without repeated planning") { stop = "journal.committed"; }
    if (stop.empty()) execute_transaction(repo, plan);
    else {
        REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) { if (label == stop) throw Error("injected stop"); }), Error);
        recover_transaction(repo);
        recover_transaction(repo);
    }
    REQUIRE(directory_permissions(repo, "d") == (after ? 02700U : 0755U));
    REQUIRE(directory_permissions(repo, "d/sub") == 0710);
    REQUIRE(directory_permissions(repo, "d/empty") == 0500);
    REQUIRE(to_string(read_file(repo.root() / "d/sub/file")) == "dirty executable");
    REQUIRE(read_worktree_file(repo, "d/sub/file")->data.permissions == 04751);
    REQUIRE(to_string(read_file(repo.root() / "d/rootfile")) == "dirty ordinary");
    REQUIRE(read_worktree_file(repo, "d/rootfile")->data.permissions == 0640);
    REQUIRE(read_file(repo.metadata() / "HEAD") == head);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "index"));
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
TEST_CASE("WorktreeUpdate directory inventory protects unknown children while identical directory targets skip inventory", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    update_file(repo, "d/tracked", "unbacked dirty bytes");
    update_directory(repo, "d", 0750);
    const auto changed_file = update_target(repo, "after");
    TransactionPlan plan;
    plan.metadata.push_back({"HEAD", read_file(repo.metadata() / "HEAD"), read_file(repo.metadata() / "HEAD")});
    const auto saved = plan;
    bool success = false, leaf = false, default_directory = false, own_file = true;
    SECTION("exact directory ownership cannot authorize an ignored file child") {
        update_file(repo, "d/ignored", "ignored bytes");
        update_file(repo, ".minigitignore", "d/ignored\n");
    }
    SECTION("known sibling ownership cannot authorize an unknown empty directory") { update_directory(repo, "d/empty", 0500); }
    SECTION("owning a Directory root cannot authorize its actual file") { own_file = false; }
    SECTION("unsafe actual children reject a directory chmod") { std::filesystem::create_symlink(repo.root() / "d/tracked", repo.root() / "d/link"); }
    SECTION("same explicit Directory mode is a noop even with unknown children") {
        update_file(repo, "d/unknown", "unknown bytes"); update_directory(repo, "d/empty", 0500); success = true;
    }
    SECTION("default existing Directory target keeps real mode and skips unknown children") {
        update_file(repo, "d/unknown", "unknown bytes"); update_directory(repo, "d/empty", 0500); success = true; default_directory = true;
    }
    SECTION("same Directory with an owned changed leaf omits the parent and preserves siblings") {
        update_file(repo, "d/unknown", "unknown bytes"); update_directory(repo, "d/empty", 0500); success = true; leaf = true;
    }
    const auto count = update_object_count(repo);
    if (success) {
        WorktreeTargets targets{{"d", update_directory_target(default_directory ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{0750})}};
        if (leaf) targets.emplace("d/tracked", changed_file);
        plan_worktree_update(repo, plan, targets, leaf ? std::set<std::string>{"d/tracked"} : std::set<std::string>{});
        REQUIRE(plan.work.size() == (leaf ? 1 : 0));
        REQUIRE(std::none_of(plan.work.begin(), plan.work.end(), [](const auto& change) { return change.path == "d"; }));
        execute_transaction(repo, plan);
        REQUIRE(to_string(read_file(repo.root() / "d/unknown")) == "unknown bytes");
        REQUIRE(directory_permissions(repo, "d/empty") == 0500);
    } else {
        // Owning the root is never recursive file authorization. The root-only
        // section intentionally omits d/tracked from its ownership set.
        std::set<std::string> owned{"d"};
        if (own_file) owned.insert("d/tracked");
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d", update_directory_target(02700)}}, owned), Error);
        update_same_plan(plan, saved);
        REQUIRE(update_object_count(repo) == count);
    }
    if (std::filesystem::exists(repo.root() / "d/ignored")) REQUIRE(to_string(read_file(repo.root() / "d/ignored")) == "ignored bytes");
    if (std::filesystem::exists(repo.root() / "d/empty")) REQUIRE(directory_permissions(repo, "d/empty") == 0500);
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(repo.root() / "d/link")))
        REQUIRE(std::filesystem::read_symlink(repo.root() / "d/link") == repo.root() / "d/tracked");
    REQUIRE(directory_permissions(repo, "d") == 0750);
    REQUIRE(to_string(read_file(repo.root() / "d/tracked")) == (leaf ? "after" : "unbacked dirty bytes"));
}
TEST_CASE("WorktreeUpdate read-only empty directories publish and recover both conversion images", "[worktree-update][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    bool creating = true, after = false;
    std::string stop;
    SECTION("Prepared creation restores the original dirty file") { stop = "work.mkdir:item"; }
    SECTION("Committed creation finishes the read-only directory") { stop = "journal.committed"; after = true; }
    SECTION("Prepared removal recreates the original read-only directory") { creating = false; stop = "work.write:item"; }
    SECTION("Committed removal finishes the replacement file") { creating = false; stop = "journal.committed"; after = true; }
    if (creating) update_file(repo, "item", "dirty original", 0640);
    else update_directory(repo, "item", 02500);
    TransactionPlan plan;
    const auto target = creating ? update_directory_target(02500) : update_target(repo, "replacement");
    plan_worktree_update(repo, plan, {{"item", target}}, {"item"});
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) {
        if (label == stop) throw Error("injected stop");
    }), Error);
    recover_transaction(repo);
    recover_transaction(repo);
    if (creating == after) {
        REQUIRE(std::filesystem::is_empty(repo.root() / "item"));
        REQUIRE(directory_permissions(repo, "item") == 02500);
    } else {
        REQUIRE(to_string(read_file(repo.root() / "item")) == (creating ? "dirty original" : "replacement"));
        REQUIRE(read_file_data(repo.root() / "item").permissions == (creating ? 0640U : 0644U));
    }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
TEST_CASE("WorktreeUpdate rejects conflicting directory schemas permissions and both image topology before backup capture", "[worktree-update]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    update_file(repo, "d/file", "unbacked dirty bytes", 0640);
    update_directory(repo, "d", 0700);
    const auto valid = update_target(repo, "valid");
    TransactionPlan plan;
    plan.metadata.push_back({"HEAD", read_file(repo.metadata() / "HEAD"), read_file(repo.metadata() / "HEAD")});
    const auto saved = plan;
    const auto count = update_object_count(repo);
    SECTION("File Directory and Absent schema errors") {
        const std::vector<WorktreeTarget> invalid{{valid.file, {}, true}, {std::nullopt, 0400, true},
            {std::nullopt, 0100, true}, {std::nullopt, 0000, true}, {std::nullopt, 010000, true}, {std::nullopt, 0700, false}};
        for (const auto& target : invalid) REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d", target}}, {"d", "d/file"}), Error);
    }
    SECTION("File target cannot be ancestor of a positive Directory target") {
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d", valid}, {"d/child", update_directory_target()}}, {"d/file"}), Error);
    }
    SECTION("new Directory without owner write cannot have a planned Directory descendant") {
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"new", update_directory_target(0500)}, {"new/child", update_directory_target()}}, {}), Error);
    }
    SECTION("after Directory needs owner write even when its planned child is deleted") {
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d", update_directory_target(0500)}, {"d/file", {}}}, {"d/file"}), Error);
    }
    SECTION("before Directory needs owner write for its forced actual descendants") {
        update_directory(repo, "d", 0500);
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d", update_directory_target(0700)}}, {"d/file"}), Error);
    }
    SECTION("keep inventory with a deleted subdirectory still requires explicit descendant deletion") {
        update_file(repo, "d/sub/child", "child");
        REQUIRE_THROWS_AS(plan_worktree_update(repo, plan, {{"d", update_directory_target(02700)}, {"d/sub", {}}}, {"d/file", "d/sub/child"}), Error);
    }
    update_same_plan(plan, saved);
    REQUIRE(update_object_count(repo) == count);
    REQUIRE(to_string(read_file(repo.root() / "d/file")) == "unbacked dirty bytes");
}
