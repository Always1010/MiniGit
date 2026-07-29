#include <catch2/catch_test_macros.hpp>
#include "minigit/reflog.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/snapshot.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
using namespace minigit;
namespace {
void reflog_write(const Repository& repo, std::string_view path, const Bytes& bytes) {
    const auto target = repo.metadata() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write(target, bytes);
}
ObjectId reflog_commit(const Repository& repo, std::string_view message, std::optional<ObjectId> tree = std::nullopt) {
    ObjectStore store(repo.metadata() / "objects");
    const auto root = tree ? *tree : store.write(ObjectType::Tree, encode_tree({}));
    return store.write(ObjectType::Commit, encode_commit({root, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
const MetadataChange& reflog_change(const TransactionPlan& plan, std::string_view path) {
    const auto found = std::find_if(plan.metadata.begin(), plan.metadata.end(), [&](const auto& change) { return change.path == path; });
    REQUIRE(found != plan.metadata.end());
    return *found;
}
void reflog_same_plan(const TransactionPlan& left, const TransactionPlan& right) {
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
std::map<std::string, Bytes> reflog_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata()))
        if (entry.is_regular_file()) result.emplace(entry.path().lexically_relative(repo.metadata()).generic_string(), read_file(entry.path()));
    return result;
}
void reflog_checksum(Bytes& bytes) {
    const auto hash = sha256(std::span<const std::uint8_t>(bytes).first(bytes.size() - 32));
    std::copy(hash.bytes().begin(), hash.bytes().end(), bytes.end() - 32);
}
}
TEST_CASE("Reflog codec preserves optional IDs UTF-8 and deterministic bounded records", "[reflog]") {
    const auto id = sha256(to_bytes("id"));
    const Reflog entries{{std::nullopt, id, "初始提交"}, {id, id, "switch: 同一提交"}, {id, std::nullopt, "delete"}, {std::nullopt, ObjectId{}, "zero is present"}};
    const auto bytes = encode_reflog(entries);
    REQUIRE(to_string(std::span<const std::uint8_t>(bytes).first(4)) == "MGRL");
    REQUIRE(decode_reflog(bytes) == entries);
    REQUIRE(encode_reflog(decode_reflog(bytes)) == bytes);
    REQUIRE(decode_reflog(encode_reflog({})).empty());
    SECTION("entry and reason boundaries") {
        const Reflog largest_reason{{std::nullopt, std::nullopt, std::string(4096, 'x')}};
        REQUIRE(decode_reflog(encode_reflog(largest_reason)) == largest_reason);
        Reflog many(100000, {std::nullopt, std::nullopt, "x"});
        REQUIRE(decode_reflog(encode_reflog(many)).size() == many.size());
        many.push_back({std::nullopt, std::nullopt, "x"});
        REQUIRE_THROWS_AS(encode_reflog(many), Error);
    }
    SECTION("encoded byte budget") {
        Reflog large(2044, {std::nullopt, std::nullopt, std::string(4096, 'x')});
        REQUIRE(encode_reflog(large).size() <= 8U * 1024U * 1024U);
        large.push_back(large.front());
        REQUIRE_THROWS_AS(encode_reflog(large), Error);
        REQUIRE_THROWS_AS(decode_reflog(Bytes(8U * 1024U * 1024U + 1)), Error);
    }
}
TEST_CASE("Reflog codec rejects malformed authenticated framing and invalid reasons", "[reflog]") {
    const auto original = encode_reflog({{std::nullopt, std::nullopt, "x"}});
    SECTION("checksum and truncation") {
        auto broken = original;
        broken.back() ^= 1;
        REQUIRE_THROWS_AS(decode_reflog(broken), Error);
        for (const auto length : {0U, 12U, 43U, 44U})
            REQUIRE_THROWS_AS(decode_reflog(std::span<const std::uint8_t>(original).first(length)), Error);
    }
    SECTION("authenticated fields and trailing data") {
        for (const auto offset : {0U, 7U, 11U, 12U, 13U, 14U, 18U}) {
            auto broken = original;
            broken[offset] = offset == 18 ? 0 : 255;
            reflog_checksum(broken);
            REQUIRE_THROWS_AS(decode_reflog(broken), Error);
        }
        auto tail = original;
        tail.insert(tail.end() - 32, 0);
        reflog_checksum(tail);
        REQUIRE_THROWS_AS(decode_reflog(tail), Error);
    }
    SECTION("invalid reason boundaries and UTF-8") {
        for (const auto& reason : std::vector<std::string>{"", "line\nnext", "tab\t", "carriage\r", std::string("x\0y", 3), std::string(1, char(127)), std::string(4097, 'x'), std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe4\xb8", 2)})
            REQUIRE_THROWS_AS(encode_reflog({{std::nullopt, std::nullopt, reason}}), Error);
    }
}
TEST_CASE("Reflog append plans preserve original bytes continuity and strong exception safety without writes", "[reflog]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto first = sha256(to_bytes("first"));
    const auto second = sha256(to_bytes("second"));
    const auto third = sha256(to_bytes("third"));
    REQUIRE(read_reflog(repo, "HEAD").empty());
    const auto original = encode_reflog({{std::nullopt, first, "initial"}});
    reflog_write(repo, "logs/HEAD", original);
    const auto before_disk = reflog_metadata(repo);
    TransactionPlan plan;
    plan.work.push_back({"unchanged", {}, {}});
    plan_append_reflog(repo, plan, "HEAD", {first, second, "next"});
    plan_append_reflog(repo, plan, "HEAD", {second, third, "again"});
    REQUIRE(plan.metadata.size() == 1);
    REQUIRE(plan.metadata.front().before == original);
    REQUIRE(decode_reflog(*plan.metadata.front().after) == Reflog{{std::nullopt, first, "initial"}, {first, second, "next"}, {second, third, "again"}});
    const auto saved = plan;
    REQUIRE_THROWS_AS(plan_append_reflog(repo, plan, "HEAD", {first, second, "discontinuous"}), Error);
    reflog_same_plan(plan, saved);
    for (const auto& ref : std::vector<std::string>{"main", "refs/tags/tag", "refs/heads/../bad", "refs/heads/" + std::string(4096, 'x')}) {
        REQUIRE_THROWS_AS(plan_append_reflog(repo, plan, ref, {third, first, "next"}), Error);
        reflog_same_plan(plan, saved);
    }
    REQUIRE_THROWS_AS(plan_append_reflog(repo, plan, "HEAD", {third, first, "invalid\nreason"}), Error);
    reflog_same_plan(plan, saved);
    REQUIRE(reflog_metadata(repo) == before_disk);
    TransactionPlan fresh;
    plan_append_reflog(repo, fresh, "refs/heads/legacy", {first, second, "legacy first record"});
    REQUIRE_FALSE(fresh.metadata.front().before.has_value());
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/refs/heads/legacy"));
    reflog_write(repo, "logs/refs/heads/empty", {});
    REQUIRE_THROWS_AS(read_reflog(repo, "refs/heads/empty"), Error);
    REQUIRE_THROWS_AS(plan_append_reflog(repo, fresh, "refs/heads/empty", {first, second, "next"}), Error);
    REQUIRE(fresh.metadata.size() == 1);
    std::filesystem::create_symlink(repo.metadata() / "logs/HEAD", repo.metadata() / "logs/refs/heads/link");
    REQUIRE_THROWS_AS(read_reflog(repo, "refs/heads/link"), Error);
}
TEST_CASE("Reflog HEAD planning advances symbolic branches with exact CAS bytes and validates targets before no-op", "[reflog]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto first = reflog_commit(repo, "first");
    const auto second = reflog_commit(repo, "second");
    auto uppercase = first.hex();
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    const auto raw_ref = to_bytes(uppercase + "\n");
    reflog_write(repo, "refs/heads/main", raw_ref);
    const auto expected = read_head(repo);
    TransactionPlan plan;
    const auto before = reflog_metadata(repo);
    plan_move_head(repo, plan, expected, {expected.symbolic, second}, "commit");
    REQUIRE(plan.metadata.size() == 3);
    REQUIRE(reflog_change(plan, "refs/heads/main").before == raw_ref);
    REQUIRE(reflog_change(plan, "refs/heads/main").after == to_bytes(second.hex() + "\n"));
    REQUIRE(decode_reflog(*reflog_change(plan, "logs/HEAD").after) == Reflog{{first, second, "commit"}});
    REQUIRE(reflog_change(plan, "logs/HEAD").after == reflog_change(plan, "logs/refs/heads/main").after);
    REQUIRE(reflog_metadata(repo) == before);
    TransactionPlan no_op;
    plan_move_head(repo, no_op, expected, expected, "same");
    REQUIRE(no_op.metadata.empty());
    const auto saved = plan;
    REQUIRE_THROWS_AS(plan_move_head(repo, plan, {expected.symbolic, second}, expected, "stale"), Error);
    const auto bad = reflog_commit(repo, "missing tree", ObjectId{});
    REQUIRE_THROWS_AS(plan_move_head(repo, plan, expected, {expected.symbolic, bad}, "bad"), Error);
    const auto blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("blob"));
    REQUIRE_THROWS_AS(plan_move_head(repo, plan, expected, {std::nullopt, blob}, "bad"), Error);
    reflog_same_plan(plan, saved);
    SECTION("prior HEAD and current-branch plans cannot be masked by a move or no-op") {
        TransactionPlan switched;
        switched.metadata.push_back({"HEAD", read_file(repo.metadata() / "HEAD"), to_bytes("ref: refs/heads/topic\n")});
        const auto switched_before = switched;
        REQUIRE_THROWS_AS(plan_move_head(repo, switched, expected, {expected.symbolic, second}, "commit"), Error);
        reflog_same_plan(switched, switched_before);
        REQUIRE_THROWS_AS(plan_move_head(repo, switched, expected, expected, "same"), Error);
        reflog_same_plan(switched, switched_before);
        // The existing plan already advances main to second despite disk HEAD
        // still resolving to first; an apparent no-op must not conceal that.
        REQUIRE_THROWS_AS(plan_move_head(repo, plan, expected, expected, "same"), Error);
        reflog_same_plan(plan, saved);
    }
    // A malformed target cannot be accepted merely because HEAD would not move.
    reflog_write(repo, "refs/heads/main", to_bytes(bad.hex() + "\n"));
    const auto damaged = read_head(repo);
    REQUIRE_THROWS_AS(plan_move_head(repo, no_op, damaged, damaged, "same"), Error);
    REQUIRE(no_op.metadata.empty());
}
TEST_CASE("Reflog HEAD planning distinguishes detached switches planned branches and unborn heads", "[reflog]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto first = reflog_commit(repo, "first");
    const auto second = reflog_commit(repo, "second");
    update_ref(repo, "refs/heads/main", std::nullopt, first);
    const auto expected = read_head(repo);
    TransactionPlan plan;
    SECTION("detaching and advancing detached HEAD write only HEAD and its log") {
        plan_move_head(repo, plan, expected, {std::nullopt, second}, "detach");
        REQUIRE(plan.metadata.size() == 2);
        REQUIRE(reflog_change(plan, "HEAD").after == to_bytes(second.hex() + "\n"));
        set_head(repo, first.hex());
        plan = {};
        plan_move_head(repo, plan, read_head(repo), {std::nullopt, second}, "detached commit");
        REQUIRE(plan.metadata.size() == 2);
        REQUIRE_THROWS_AS(plan_move_head(repo, plan, read_head(repo), {std::nullopt, std::nullopt}, "invalid"), Error);
    }
    SECTION("same-ID branch switching still records HEAD without updating the branch") {
        update_ref(repo, "refs/heads/topic", std::nullopt, first);
        plan_move_head(repo, plan, expected, {"refs/heads/topic", first}, "switch");
        REQUIRE(plan.metadata.size() == 2);
        REQUIRE(decode_reflog(*reflog_change(plan, "logs/HEAD").after) == Reflog{{first, first, "switch"}});
    }
    SECTION("switching can consume a branch prepared in the same transaction") {
        plan.metadata.push_back({"refs/heads/new", std::nullopt, to_bytes(second.hex() + "\n")});
        plan_append_reflog(repo, plan, "refs/heads/new", {std::nullopt, second, "branch create"});
        plan_move_head(repo, plan, expected, {"refs/heads/new", second}, "switch");
        REQUIRE(plan.metadata.size() == 4);
        REQUIRE(decode_reflog(*reflog_change(plan, "logs/refs/heads/new").after).size() == 1);
        const auto saved = plan;
        REQUIRE_THROWS_AS(plan_move_head(repo, plan, expected, {"refs/heads/new", first}, "mismatch"), Error);
        reflog_same_plan(plan, saved);
    }
    SECTION("symbolic unborn targets are valid and their first commit has a null old ID") {
        plan_move_head(repo, plan, expected, {"refs/heads/unborn", std::nullopt}, "switch unborn");
        REQUIRE(plan.metadata.size() == 2);
        set_head(repo, "refs/heads/unborn");
        plan = {};
        plan_move_head(repo, plan, read_head(repo), {"refs/heads/unborn", first}, "initial commit");
        REQUIRE(plan.metadata.size() == 3);
        REQUIRE_FALSE(reflog_change(plan, "refs/heads/unborn").before.has_value());
        REQUIRE(decode_reflog(*reflog_change(plan, "logs/HEAD").after) == Reflog{{std::nullopt, first, "initial commit"}});
    }
}
TEST_CASE("Reflog transaction recovery restores exact before or after bytes without duplicate appends", "[reflog][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto first = reflog_commit(repo, "first");
    const auto second = reflog_commit(repo, "second");
    update_ref(repo, "refs/heads/main", std::nullopt, first);
    const auto initial = encode_reflog({{std::nullopt, first, "initial"}});
    reflog_write(repo, "logs/HEAD", initial);
    reflog_write(repo, "logs/refs/heads/main", initial);
    TransactionPlan plan;
    plan_move_head(repo, plan, read_head(repo), {"refs/heads/main", second}, "commit");
    bool after = false;
    std::string stop;
    SECTION("Prepared after partial log publication rolls back") { stop = "metadata.write:logs/HEAD"; }
    SECTION("Committed replays after images") { stop = "journal.committed"; after = true; }
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) { if (label == stop) throw Error("injected stop"); }), Error);
    REQUIRE(std::filesystem::exists(repo.metadata() / "transaction"));
    const auto roots = transaction_roots(repo);
    REQUIRE(roots.contains(first));
    REQUIRE(roots.contains(second));
    recover_transaction(repo);
    const auto once = reflog_metadata(repo);
    recover_transaction(repo);
    REQUIRE(reflog_metadata(repo) == once);
    for (const auto& change : plan.metadata) {
        const auto& bytes = after ? change.after : change.before;
        REQUIRE(bytes.has_value());
        REQUIRE(read_file(repo.metadata() / change.path) == *bytes);
    }
    REQUIRE(read_head(repo).id == (after ? second : first));
    REQUIRE(read_reflog(repo, "HEAD").size() == (after ? 2U : 1U));
    REQUIRE(read_reflog(repo, "refs/heads/main").size() == (after ? 2U : 1U));
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
TEST_CASE("Reflog transactions reject missing or noncommit semantic roots before publication", "[reflog][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto proper = reflog_commit(repo, "proper");
    const auto head = read_file(repo.metadata() / "HEAD");
    Reflog entries;
    bool succeeds = false;
    SECTION("a missing commit ID is rejected") {
        entries = {{ObjectId{}, proper, "missing old root"}};
    }
    SECTION("a blob cannot be an old commit root") {
        const auto blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("blob"));
        entries = {{blob, proper, "noncommit old root"}};
    }
    SECTION("a blob cannot be a new commit root") {
        const auto blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("blob"));
        entries = {{proper, blob, "noncommit new root"}};
    }
    SECTION("a historical commit root does not require expanding its missing tree") {
        const auto historical = reflog_commit(repo, "historical missing tree", ObjectId{});
        entries = {{historical, proper, "historical commit root"}};
        succeeds = true;
    }
    TransactionPlan plan;
    plan.metadata.push_back({"logs/HEAD", std::nullopt, encode_reflog(entries)});
    if (succeeds) {
        REQUIRE_NOTHROW(execute_transaction(repo, plan));
        REQUIRE(read_reflog(repo, "HEAD") == entries);
    } else {
        REQUIRE_THROWS_AS(execute_transaction(repo, plan), Error);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/HEAD"));
    }
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
    REQUIRE(read_file(repo.metadata() / "HEAD") == head);
}
