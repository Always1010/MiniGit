#include <catch2/catch_test_macros.hpp>
#include "minigit/branches.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/refs.hpp"
#include "minigit/snapshot.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
using namespace minigit;
namespace {
ObjectId branch_commit(const Repository& repo, std::string_view message, std::vector<ObjectId> parents = {}, std::optional<ObjectId> tree = std::nullopt) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto root = tree ? *tree : store.write(ObjectType::Tree, encode_tree({}));
    return store.write(ObjectType::Commit, encode_commit({root, std::move(parents), "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
void branch_write(const Repository& repo, std::string_view path, const Bytes& bytes) {
    const auto target = repo.metadata() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write(target, bytes);
}
void branch_head(const Repository& repo, ObjectId id) {
    RepoLock lock(repo.metadata());
    update_ref(repo, "refs/heads/main", read_ref(repo, "refs/heads/main"), id);
    const auto log = encode_reflog({{std::nullopt, id, "fixture"}});
    branch_write(repo, "logs/HEAD", log);
    branch_write(repo, "logs/refs/heads/main", log);
}
std::map<std::string, Bytes> branch_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata()))
        if (entry.is_regular_file()) result.emplace(entry.path().lexically_relative(repo.metadata()).generic_string(), read_file(entry.path()));
    return result;
}
const MetadataChange& branch_change(const TransactionPlan& plan, std::string_view path) {
    const auto found = std::find_if(plan.metadata.begin(), plan.metadata.end(), [&](const auto& item) { return item.path == path; });
    REQUIRE(found != plan.metadata.end());
    return *found;
}
void branch_same_plan(const TransactionPlan& plan, const TransactionPlan& before) {
    REQUIRE(plan.work.size() == before.work.size());
    REQUIRE(plan.metadata.size() == before.metadata.size());
    for (std::size_t i = 0; i < plan.work.size(); ++i) {
        REQUIRE(plan.work[i].path == before.work[i].path);
        REQUIRE(plan.work[i].before == before.work[i].before);
        REQUIRE(plan.work[i].after == before.work[i].after);
    }
    for (std::size_t i = 0; i < plan.metadata.size(); ++i) {
        REQUIRE(plan.metadata[i].path == before.metadata[i].path);
        REQUIRE(plan.metadata[i].before == before.metadata[i].before);
        REQUIRE(plan.metadata[i].after == before.metadata[i].after);
    }
}
}
TEST_CASE("Branch lists unborn ordinary and detached states in short-name order", "[branches]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    REQUIRE(list_branches(repo) == std::vector<BranchInfo>{{"main", std::nullopt, true}});
    const auto first = branch_commit(repo, "first");
    branch_head(repo, first);
    create_branch(repo, "z-last");
    create_branch(repo, "a-first", first.hex());
    create_branch(repo, "中文", "refs/heads/main");
    const auto before = branch_metadata(repo);
    REQUIRE(list_branches(repo) == std::vector<BranchInfo>{{"a-first", first, false}, {"main", first, true}, {"z-last", first, false}, {"中文", first, false}});
    REQUIRE(branch_metadata(repo) == before);
    {
        RepoLock lock(repo.metadata());
        set_head(repo, first.hex());
    }
    for (const auto& branch : list_branches(repo)) REQUIRE_FALSE(branch.current);
}
TEST_CASE("Branch create planning preserves precise reference bytes and guarantees no partial plan or disk writes", "[branches]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto first = branch_commit(repo, "first");
    const auto second = branch_commit(repo, "second");
    RepoLock lock(repo.metadata());
    auto upper = first.hex();
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    const auto raw = to_bytes(upper + "\n");
    branch_write(repo, "refs/heads/topic", raw);
    branch_write(repo, "logs/refs/heads/topic", encode_reflog({{std::nullopt, first, "fixture"}}));
    const auto before = branch_metadata(repo);
    TransactionPlan plan;
    plan.work.push_back({"preserved", {}, {}});
    plan_create_branch(repo, plan, "topic", second, true);
    REQUIRE(plan.metadata.size() == 2);
    REQUIRE(branch_change(plan, "refs/heads/topic").before == raw);
    REQUIRE(branch_change(plan, "refs/heads/topic").after == to_bytes(second.hex() + "\n"));
    REQUIRE(decode_reflog(*branch_change(plan, "logs/refs/heads/topic").after).back() == ReflogEntry{first, second, "branch:update"});
    REQUIRE(branch_metadata(repo) == before);
    const auto saved = plan;
    REQUIRE_THROWS_AS(plan_create_branch(repo, plan, "topic", first, true), Error);
    branch_same_plan(plan, saved);
    const auto blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("blob"));
    const auto with_blob = branch_metadata(repo);
    REQUIRE_THROWS_AS(plan_create_branch(repo, plan, "new", blob), Error);
    branch_same_plan(plan, saved);
    REQUIRE(branch_metadata(repo) == with_blob);
    REQUIRE(before.at("HEAD") == read_file(repo.metadata() / "HEAD"));
    branch_write(repo, "logs/refs/heads/broken", to_bytes("corrupt"));
    const auto broken_disk = branch_metadata(repo);
    REQUIRE_THROWS_AS(plan_create_branch(repo, plan, "broken", first), Error);
    branch_same_plan(plan, saved);
    REQUIRE(branch_metadata(repo) == broken_disk);
    TransactionPlan fresh;
    plan_create_branch(repo, fresh, "fresh", first);
    REQUIRE(decode_reflog(*branch_change(fresh, "logs/refs/heads/fresh").after) == Reflog{{std::nullopt, first, "branch:create"}});
}
TEST_CASE("Branch force updates preserve HEAD index and work while current branches remain protected", "[branches]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto first = branch_commit(repo, "first");
    const auto second = branch_commit(repo, "second");
    branch_head(repo, first);
    {
        RepoLock lock(repo.metadata());
        const auto staged = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("staged"));
        save_index(repo, {{"file", "100644", staged}});
    }
    atomic_write(repo.root() / "file", to_bytes("working"));
    create_branch(repo, "topic");
    const auto head = read_file(repo.metadata() / "HEAD");
    const auto headlog = read_file(repo.metadata() / "logs/HEAD");
    const auto main = read_file(repo.metadata() / "refs/heads/main");
    const auto index = read_file(repo.metadata() / "index");
    const auto before = branch_metadata(repo);
    REQUIRE_THROWS_AS(create_branch(repo, "topic", first.hex()), Error);
    REQUIRE_THROWS_AS(create_branch(repo, "main", first.hex(), true), Error);
    REQUIRE_THROWS_AS(create_branch(repo, "main", second.hex(), true), Error);
    REQUIRE_THROWS_AS(delete_branch(repo, "main", true), Error);
    REQUIRE(branch_metadata(repo) == before);
    create_branch(repo, "topic", second.hex(), true);
    {
        RepoLock lock(repo.metadata());
        REQUIRE(read_reflog(repo, "refs/heads/topic") == Reflog{{std::nullopt, first, "branch:create"}, {first, second, "branch:update"}});
    }
    const auto unchanged = branch_metadata(repo);
    create_branch(repo, "topic", second.hex(), true);
    REQUIRE(branch_metadata(repo) == unchanged);
    REQUIRE(read_file(repo.metadata() / "HEAD") == head);
    REQUIRE(read_file(repo.metadata() / "logs/HEAD") == headlog);
    REQUIRE(read_file(repo.metadata() / "refs/heads/main") == main);
    REQUIRE(read_file(repo.metadata() / "index") == index);
    REQUIRE(to_string(read_file(repo.root() / "file")) == "working");
}
TEST_CASE("Branch deletion checks complete multi-parent ancestry and supports force detached and unborn policies", "[branches]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto base = branch_commit(repo, "base");
    const auto left = branch_commit(repo, "left", {base});
    const auto right = branch_commit(repo, "right", {base});
    const auto merge = branch_commit(repo, "merge", {left, right});
    const auto unrelated = branch_commit(repo, "unrelated");
    branch_head(repo, merge);
    create_branch(repo, "merged", right.hex());
    create_branch(repo, "unmerged", unrelated.hex());
    SECTION("safe delete recognizes the second-parent ancestry") {
        const auto head = read_file(repo.metadata() / "HEAD");
        const auto headlog = read_file(repo.metadata() / "logs/HEAD");
        delete_branch(repo, "merged");
        REQUIRE_FALSE(read_ref(repo, "refs/heads/merged").has_value());
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/refs/heads/merged"));
        const auto before = branch_metadata(repo);
        REQUIRE_THROWS_AS(delete_branch(repo, "unmerged"), Error);
        REQUIRE(branch_metadata(repo) == before);
        delete_branch(repo, "unmerged", true);
        REQUIRE(read_file(repo.metadata() / "HEAD") == head);
        REQUIRE(read_file(repo.metadata() / "logs/HEAD") == headlog);
    }
    SECTION("detached HEAD provides the same safe-delete ancestry") {
        { RepoLock lock(repo.metadata()); set_head(repo, merge.hex()); }
        delete_branch(repo, "merged");
        REQUIRE_FALSE(read_ref(repo, "refs/heads/merged").has_value());
    }
    SECTION("unborn HEAD requires force for deleting existing noncurrent branches") {
        { RepoLock lock(repo.metadata()); set_head(repo, "refs/heads/unborn"); }
        const auto before = branch_metadata(repo);
        REQUIRE_THROWS_AS(delete_branch(repo, "merged"), Error);
        REQUIRE_THROWS_AS(create_branch(repo, "unborn", merge.hex(), true), Error);
        REQUIRE(branch_metadata(repo) == before);
        delete_branch(repo, "merged", true);
    }
    SECTION("a missing ancestor elsewhere in HEAD closure prevents a false safe deletion") {
        const auto damaged = branch_commit(repo, "damaged", {right, ObjectId{}});
        branch_head(repo, damaged);
        const auto before = branch_metadata(repo);
        REQUIRE_THROWS_AS(delete_branch(repo, "merged"), Error);
        REQUIRE(branch_metadata(repo) == before);
    }
}
TEST_CASE("Branch deletion cleans nested ref and log parents so names can be reused in both directions", "[branches]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto id = branch_commit(repo, "fixture");
    branch_head(repo, id);
    create_branch(repo, "topic/deep/child");
    delete_branch(repo, "topic/deep/child");
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "refs/heads/topic"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/refs/heads/topic"));
    create_branch(repo, "topic");
    delete_branch(repo, "topic");
    create_branch(repo, "topic/child");
    REQUIRE(read_ref(repo, "refs/heads/topic/child") == id);
    REQUIRE(std::filesystem::is_directory(repo.metadata() / "refs/heads"));
    REQUIRE(std::filesystem::is_directory(repo.metadata() / "logs/refs/heads"));
}
TEST_CASE("Branch rejects unsafe names corrupt targets logs and unresolved transactions before publication", "[branches]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto id = branch_commit(repo, "fixture");
    branch_head(repo, id);
    SECTION("invalid names obey ref syntax reserved revisions and transaction path limits") {
        const auto before = branch_metadata(repo);
        std::string deep = "x";
        for (unsigned i = 0; i < 125; ++i) deep += "/x";
        for (const auto& name : std::vector<std::string>{"", "HEAD", "refs/heads/x", "/x", "x/", "x//y", ".x", "x.lock", "x..y", "x y", "x~", "x^", "x?", "x*", "x[", "x:", "x@{y", "x\\y", "x\ny", std::string("x\0y", 3), std::string(64, 'A'), std::string(64, 'a'), std::string(64, '0'), std::string(4096, 'x'), deep}) {
            REQUIRE_THROWS_AS(branch_ref(name), Error);
            REQUIRE_THROWS_AS(create_branch(repo, name, id.hex()), Error);
            REQUIRE_THROWS_AS(delete_branch(repo, name, true), Error);
        }
        REQUIRE(branch_metadata(repo) == before);
        REQUIRE(branch_ref("-name") == "refs/heads/-name");
    }
    SECTION("corrupt branch logs prevent delete and force update") {
        create_branch(repo, "topic");
        branch_write(repo, "logs/refs/heads/topic", to_bytes("corrupt"));
        const auto next = branch_commit(repo, "next");
        const auto before = branch_metadata(repo);
        REQUIRE_THROWS_AS(delete_branch(repo, "topic", true), Error);
        REQUIRE_THROWS_AS(create_branch(repo, "topic", next.hex(), true), Error);
        REQUIRE(branch_metadata(repo) == before);
    }
    SECTION("missing or noncommit or incomplete branch targets are not published or listed") {
        ObjectId blob;
        { RepoLock lock(repo.metadata()); blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("blob")); }
        const auto incomplete = branch_commit(repo, "incomplete", {}, ObjectId{});
        for (const auto target : {ObjectId{}, blob, incomplete}) {
            branch_write(repo, "refs/heads/bad", to_bytes(target.hex() + "\n"));
            const auto before = branch_metadata(repo);
            REQUIRE_THROWS_AS(list_branches(repo), Error);
            REQUIRE_THROWS_AS(create_branch(repo, "copy", target.hex()), Error);
            REQUIRE_THROWS_AS(create_branch(repo, "bad", target.hex(), true), Error);
            REQUIRE_THROWS_AS(delete_branch(repo, "bad", true), Error);
            REQUIRE(branch_metadata(repo) == before);
        }
    }
    SECTION("symlink log is rejected and a missing branch leaves metadata unchanged") {
        create_branch(repo, "topic");
        std::filesystem::remove(repo.metadata() / "logs/refs/heads/topic");
        std::filesystem::create_symlink(repo.metadata() / "logs/HEAD", repo.metadata() / "logs/refs/heads/topic");
        const auto before = read_file(repo.metadata() / "refs/heads/topic");
        REQUIRE_THROWS_AS(delete_branch(repo, "topic", true), Error);
        REQUIRE_THROWS_AS(delete_branch(repo, "missing", true), Error);
        REQUIRE(read_file(repo.metadata() / "refs/heads/topic") == before);
    }
    SECTION("pending transaction guard blocks reads planning and mutations") {
        atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        const auto before = branch_metadata(repo);
        REQUIRE_THROWS_AS(list_branches(repo), Error);
        REQUIRE_THROWS_AS(create_branch(repo, "topic", id.hex()), Error);
        REQUIRE_THROWS_AS(delete_branch(repo, "other", true), Error);
        { RepoLock lock(repo.metadata()); TransactionPlan plan; REQUIRE_THROWS_AS(plan_create_branch(repo, plan, "topic", id), Error); REQUIRE(plan.metadata.empty()); }
        REQUIRE(branch_metadata(repo) == before);
    }
}
TEST_CASE("Branch transaction recovery keeps ref and reflog creation together across the commit boundary", "[branches][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto id = branch_commit(repo, "fixture");
    branch_head(repo, id);
    RepoLock lock(repo.metadata());
    const auto head = read_file(repo.metadata() / "HEAD");
    const auto headlog = read_file(repo.metadata() / "logs/HEAD");
    TransactionPlan plan;
    plan_create_branch(repo, plan, "topic", id);
    bool committed = false;
    std::string stop;
    SECTION("Prepared after branch log publication restores absence") { stop = "metadata.write:logs/refs/heads/topic"; }
    SECTION("Committed preserves both reference and single log record") { stop = "journal.committed"; committed = true; }
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) { if (label == stop) throw Error("injected stop"); }), Error);
    recover_transaction(repo);
    recover_transaction(repo);
    REQUIRE(read_ref(repo, "refs/heads/topic") == (committed ? std::optional(id) : std::nullopt));
    REQUIRE(read_reflog(repo, "refs/heads/topic").size() == (committed ? 1U : 0U));
    REQUIRE(read_file(repo.metadata() / "HEAD") == head);
    REQUIRE(read_file(repo.metadata() / "logs/HEAD") == headlog);
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
