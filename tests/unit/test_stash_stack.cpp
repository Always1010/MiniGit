#include <catch2/catch_test_macros.hpp>
#include "minigit/stash_stack.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
#include <sys/stat.h>
#include <utility>
#include <zlib.h>
using namespace minigit;
namespace {
ObjectId stack_commit(const Repository& repo, std::string_view message,
                      const std::vector<ObjectId>& parents = {}, std::optional<ObjectId> tree = std::nullopt) {
    ObjectStore store(repo.metadata() / "objects");
    const auto root = tree ? *tree : store.write(ObjectType::Tree, encode_tree({}));
    return store.write(ObjectType::Commit, encode_commit({root, parents,
        "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
ObjectId stack_raw_commit(const Repository& repo, std::string_view text) {
    const auto payload = to_bytes(text);
    auto framed = to_bytes("commit " + std::to_string(payload.size()));
    framed.push_back(0);
    framed.insert(framed.end(), payload.begin(), payload.end());
    const auto id = sha256(framed);
    uLongf size = ::compressBound(static_cast<uLong>(framed.size()));
    Bytes compressed(static_cast<std::size_t>(size));
    REQUIRE(::compress2(compressed.data(), &size, framed.data(), static_cast<uLong>(framed.size()), Z_DEFAULT_COMPRESSION) == Z_OK);
    compressed.resize(static_cast<std::size_t>(size));
    const auto path = ObjectStore(repo.metadata() / "objects").object_path(id);
    detail::ensure_directory(path.parent_path());
    atomic_write(path, compressed);
    return id;
}
StashEntry stack_entry(const Repository& repo, std::string_view label, bool untracked = false) {
    const auto head = stack_commit(repo, std::string(label) + " H");
    const auto index = stack_commit(repo, std::string(label) + " I", {head});
    std::optional<ObjectId> extra;
    std::vector<ObjectId> parents{head, index};
    if (untracked) { extra = stack_commit(repo, std::string(label) + " U"); parents.push_back(*extra); }
    return {stack_commit(repo, std::string(label) + " W", parents), head, index, extra};
}
void stack_write(const Repository& repo, std::string_view name, const Bytes& bytes) {
    const auto path = repo.metadata() / name;
    detail::ensure_directory(path.parent_path());
    atomic_write(path, bytes);
}
void stack_install(const Repository& repo, const StashStack& stack) {
    stack_write(repo, "stashstack", encode_stash_stack(stack));
    if (!stack.empty()) stack_write(repo, "refs/stash", to_bytes(stack.front().hex() + "\n"));
}
void stack_checksum(Bytes& bytes) {
    const auto digest = sha256(std::span<const std::uint8_t>(bytes).first(bytes.size() - 32));
    std::copy(digest.bytes().begin(), digest.bytes().end(), bytes.end() - 32);
}
void stack_number(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (24U - i * 8U));
    stack_checksum(bytes);
}
const MetadataChange& stack_change(const TransactionPlan& plan, std::string_view name) {
    const auto found = std::find_if(plan.metadata.begin(), plan.metadata.end(), [&](const auto& change) { return change.path == name; });
    REQUIRE(found != plan.metadata.end());
    return *found;
}
std::map<std::string, Bytes> stack_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& item : std::filesystem::recursive_directory_iterator(repo.metadata()))
        if (std::filesystem::is_regular_file(item.symlink_status()))
            result.emplace(item.path().lexically_relative(repo.metadata()).generic_string(), read_file(item.path()));
    return result;
}
void stack_same_plan(const TransactionPlan& a, const TransactionPlan& b) {
    REQUIRE(a.work.size() == b.work.size());
    REQUIRE(a.metadata.size() == b.metadata.size());
    for (std::size_t i = 0; i < a.work.size(); ++i) {
        REQUIRE(a.work[i].path == b.work[i].path);
        REQUIRE(a.work[i].before == b.work[i].before);
        REQUIRE(a.work[i].after == b.work[i].after);
    }
    for (std::size_t i = 0; i < a.metadata.size(); ++i) {
        REQUIRE(a.metadata[i].path == b.metadata[i].path);
        REQUIRE(a.metadata[i].before == b.metadata[i].before);
        REQUIRE(a.metadata[i].after == b.metadata[i].after);
    }
}
}
TEST_CASE("StashStack codec preserves order duplicate IDs empty encoding and bounded canonical bytes", "[stashstack]") {
    const auto a = sha256(to_bytes("a"));
    const auto b = sha256(to_bytes("b"));
    const StashStack stack{b, a, b, ObjectId{}};
    const auto bytes = encode_stash_stack(stack);
    REQUIRE(bytes.size() == 44 + 32 * stack.size());
    REQUIRE(to_string(std::span<const std::uint8_t>(bytes).first(4)) == "MGST");
    REQUIRE(bytes[7] == 1);
    REQUIRE(bytes[11] == 4);
    REQUIRE(decode_stash_stack(bytes) == stack);
    REQUIRE(encode_stash_stack(decode_stash_stack(bytes)) == bytes);
    REQUIRE(encode_stash_stack({}).size() == 44);
    REQUIRE(decode_stash_stack(encode_stash_stack({})).empty());
    StashStack many(100000, a);
    REQUIRE(decode_stash_stack(encode_stash_stack(many)) == many);
    many.push_back(a);
    REQUIRE_THROWS_AS(encode_stash_stack(many), Error);
}
TEST_CASE("StashStack codec rejects authenticated framing corruption before count-driven allocation", "[stashstack]") {
    const auto original = encode_stash_stack({sha256(to_bytes("id"))});
    for (const auto length : {0U, 12U, 43U, 44U, 75U})
        REQUIRE_THROWS_AS(decode_stash_stack(std::span<const std::uint8_t>(original).first(length)), Error);
    auto broken = original;
    broken.back() ^= 1;
    REQUIRE_THROWS_AS(decode_stash_stack(broken), Error);
    broken = original; broken[0] = 'X'; stack_checksum(broken);
    REQUIRE_THROWS_AS(decode_stash_stack(broken), Error);
    for (const auto version : {0U, 2U, 0xffffffffU}) {
        broken = original; stack_number(broken, 4, version);
        REQUIRE_THROWS_AS(decode_stash_stack(broken), Error);
    }
    for (const auto count : {0U, 2U, 100001U, 0xffffffffU}) {
        broken = original; stack_number(broken, 8, count);
        REQUIRE_THROWS_AS(decode_stash_stack(broken), Error);
    }
    broken = original; broken.insert(broken.end() - 32, 0); stack_checksum(broken);
    REQUIRE_THROWS_AS(decode_stash_stack(broken), Error);
}
TEST_CASE("StashStack semantic roots include every entry and validate exact Commit roles without Trees or ancestors", "[stashstack]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto top = stack_entry(repo, "top", true);
    const auto old = stack_entry(repo, "old");
    REQUIRE(read_stash_entry(repo, top.work) == top);
    REQUIRE(stash_roots(repo, {top.work, old.work, top.work}) ==
        std::set<ObjectId>{top.work, top.head, top.index, *top.untracked, old.work, old.head, old.index});
    SECTION("repeated entries share reads and aggregate decoded budget") {
        REQUIRE(stash_roots(repo, StashStack(100000, top.work)) ==
            std::set<ObjectId>{top.work, top.head, top.index, *top.untracked});
    }
    SECTION("historical Trees and ancestor DAG are intentionally not expanded") {
        const auto head = stack_commit(repo, "history", {ObjectId{}}, ObjectId{});
        const auto index = stack_commit(repo, "index", {head}, ObjectId{});
        const auto work = stack_commit(repo, "work", {head, index}, ObjectId{});
        REQUIRE(stash_roots(repo, {work}) == std::set<ObjectId>{work, head, index});
    }
    SECTION("Work parent arity and Index parent identity are exact") {
        for (const auto& parents : std::vector<std::vector<ObjectId>>{{}, {top.head}, {top.head, top.index, *top.untracked, old.work}, {old.head, top.index}})
            REQUIRE_THROWS_AS(read_stash_entry(repo, stack_commit(repo, "bad work", parents)), Error);
        const auto wrong_index = stack_commit(repo, "wrong index", {top.head, top.head});
        REQUIRE_THROWS_AS(read_stash_entry(repo, stack_commit(repo, "work", {top.head, wrong_index})), Error);
        REQUIRE_THROWS_AS(read_stash_entry(repo, stack_commit(repo, "work", {top.head, top.index, old.index})), Error);
    }
    SECTION("non-top malformed entries and every role must be Commit typed and schema checked") {
        ObjectStore store(repo.metadata() / "objects");
        const auto blob = store.write(ObjectType::Blob, to_bytes("blob"));
        const auto invalid = stack_raw_commit(repo, "invalid commit");
        for (const auto id : {blob, invalid, ObjectId{}}) {
            REQUIRE_THROWS_AS(stash_roots(repo, {top.work, id}), Error);
            REQUIRE_THROWS_AS(read_stash_entry(repo, stack_commit(repo, "head", {id, top.index})), Error);
            REQUIRE_THROWS_AS(read_stash_entry(repo, stack_commit(repo, "index", {top.head, id})), Error);
            REQUIRE_THROWS_AS(read_stash_entry(repo, stack_commit(repo, "untracked", {top.head, top.index, id})), Error);
        }
    }
    SECTION("distinct Commit payloads share one cumulative 64 MiB limit") {
        StashStack large;
        const std::string text(1024U * 1024U, 'x');
        for (unsigned i = 0; i < 64; ++i)
            large.push_back(stack_commit(repo, text + std::to_string(i), {top.head, top.index}));
        REQUIRE_THROWS_AS(stash_roots(repo, large), Error);
    }
}
TEST_CASE("StashStack reader preserves raw metadata checks paired top and avoids live Index Work and pending guards", "[stashstack]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto top = stack_entry(repo, "top");
    REQUIRE(read_stash_stack(repo).entries.empty());
    stack_install(repo, {top.work, top.work});
    auto uppercase = top.work.hex();
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    const auto raw_ref = to_bytes(uppercase + "\n");
    stack_write(repo, "refs/stash", raw_ref);
    const auto state = read_stash_stack(repo);
    REQUIRE(state.entries == StashStack{top.work, top.work});
    REQUIRE(state.raw_stack == encode_stash_stack(state.entries));
    REQUIRE(state.raw_ref == raw_ref);
    SECTION("no dependency on current Index Work or pending transaction bytes") {
        REQUIRE(::mkfifo((repo.metadata() / "index").c_str(), 0600) == 0);
        REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0);
        stack_write(repo, "transaction", to_bytes("not a valid journal"));
        REQUIRE(read_stash_stack(repo).entries == state.entries);
    }
    SECTION("missing stack or wrong top cannot leave a dangling ref") {
        std::filesystem::remove(repo.metadata() / "stashstack");
        REQUIRE_THROWS_AS(read_stash_stack(repo), Error);
        stack_write(repo, "stashstack", encode_stash_stack({top.index}));
        REQUIRE_THROWS_AS(read_stash_stack(repo), Error);
    }
    SECTION("encoded empty requires absent ref and remains distinguishable from missing") {
        stack_write(repo, "stashstack", encode_stash_stack({}));
        REQUIRE_THROWS_AS(read_stash_stack(repo), Error);
        std::filesystem::remove(repo.metadata() / "refs/stash");
        const auto empty = read_stash_stack(repo);
        REQUIRE(empty.entries.empty()); REQUIRE(empty.raw_stack.has_value()); REQUIRE_FALSE(empty.raw_ref.has_value());
    }
    SECTION("empty invalid unsafe metadata cannot masquerade as absence") {
        stack_write(repo, "stashstack", {});
        REQUIRE_THROWS_AS(read_stash_stack(repo), Error);
        std::filesystem::remove(repo.metadata() / "stashstack");
        std::filesystem::create_symlink(repo.metadata() / "refs/stash", repo.metadata() / "stashstack");
        REQUIRE_THROWS_AS(read_stash_stack(repo), Error);
        std::filesystem::remove(repo.metadata() / "stashstack");
        REQUIRE(::mkfifo((repo.metadata() / "stashstack").c_str(), 0600) == 0);
        REQUIRE_THROWS_AS(read_stash_stack(repo), Error);
    }
}
TEST_CASE("StashStack planning binds full raw CAS retains noop bytes and updates both metadata with strong safety", "[stashstack]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto top = stack_entry(repo, "top");
    const auto old = stack_entry(repo, "old");
    stack_install(repo, {top.work, old.work});
    auto uppercase = top.work.hex();
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    stack_write(repo, "refs/stash", to_bytes(uppercase + "\n"));
    auto expected = read_stash_stack(repo);
    const auto disk = stack_metadata(repo);
    TransactionPlan plan;
    plan.work.push_back({"marker", {}, {}});
    const auto original = plan;
    SECTION("identical entries retain full uppercase bytes without metadata") {
        plan_stash_stack_update(repo, plan, expected, expected.entries);
        stack_same_plan(plan, original);
    }
    SECTION("removing non-top entry records an exact ref noop companion") {
        plan_stash_stack_update(repo, plan, expected, {top.work});
        REQUIRE(plan.metadata.size() == 2);
        REQUIRE(stack_change(plan, "stashstack").before == expected.raw_stack);
        REQUIRE(stack_change(plan, "stashstack").after == encode_stash_stack({top.work}));
        REQUIRE(stack_change(plan, "refs/stash").before == expected.raw_ref);
        REQUIRE(stack_change(plan, "refs/stash").after == expected.raw_ref);
    }
    SECTION("clear removes both optional metadata files") {
        plan_stash_stack_update(repo, plan, expected, {});
        REQUIRE_FALSE(stack_change(plan, "stashstack").after.has_value());
        REQUIRE_FALSE(stack_change(plan, "refs/stash").after.has_value());
    }
    SECTION("every expected field binds CAS") {
        auto stale = expected; stale.entries.pop_back();
        REQUIRE_THROWS_AS(plan_stash_stack_update(repo, plan, stale, {}), Error);
        stale = expected; stale.raw_stack.reset();
        REQUIRE_THROWS_AS(plan_stash_stack_update(repo, plan, stale, {}), Error);
        stale = expected; stale.raw_ref = to_bytes(top.work.hex() + "\n");
        REQUIRE_THROWS_AS(plan_stash_stack_update(repo, plan, stale, {}), Error);
        stack_same_plan(plan, original);
    }
    SECTION("bad desired entries cannot modify the caller plan") {
        REQUIRE_THROWS_AS(plan_stash_stack_update(repo, plan, expected, {top.work, old.index}), Error);
        stack_same_plan(plan, original);
    }
    SECTION("preplanned stack or ref is rejected even for an identical noop") {
        for (const auto& name : {std::string("stashstack"), std::string("refs/stash")}) {
            auto conflict = original; conflict.metadata.push_back({name, std::nullopt, std::nullopt});
            const auto saved = conflict;
            REQUIRE_THROWS_AS(plan_stash_stack_update(repo, conflict, expected, expected.entries), Error);
            stack_same_plan(conflict, saved);
        }
    }
    SECTION("combined count failure retains the original large plan") {
        TransactionPlan full;
        full.metadata.resize(99999, {"index", std::nullopt, std::nullopt});
        REQUIRE_THROWS_AS(plan_stash_stack_update(repo, full, expected, {}), Error);
        REQUIRE(full.metadata.size() == 99999);
        REQUIRE(full.metadata.front().path == "index"); REQUIRE(full.metadata.back().path == "index");
        REQUIRE_FALSE(full.metadata.back().after.has_value());
    }
    REQUIRE(stack_metadata(repo) == disk);
}
TEST_CASE("StashStack empty logical planning preserves valid empty bytes and refuses pending transactions", "[stashstack]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    SECTION("missing remains missing") {}
    SECTION("encoded empty remains byte exact") { stack_write(repo, "stashstack", encode_stash_stack({})); }
    const auto state = read_stash_stack(repo);
    TransactionPlan plan;
    plan_stash_stack_update(repo, plan, state, {});
    REQUIRE(plan.metadata.empty());
    REQUIRE(read_stash_stack(repo).raw_stack == state.raw_stack);
    stack_write(repo, "transaction", to_bytes("pending"));
    REQUIRE_THROWS_AS(plan_stash_stack_update(repo, plan, state, {}), Error);
    REQUIRE(plan.metadata.empty());
}
TEST_CASE("StashStack transactions validate paired images and all semantic entries before publication", "[stashstack]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto top = stack_entry(repo, "top");
    const auto old = stack_entry(repo, "old");
    stack_install(repo, {top.work, old.work});
    const auto state = read_stash_stack(repo);
    TransactionPlan plan;
    plan_stash_stack_update(repo, plan, state, {top.work});
    SECTION("stack record requires a ref companion") {
        std::erase_if(plan.metadata, [](const auto& change) { return change.path == "refs/stash"; });
    }
    SECTION("after top must match even if both entries independently have valid schemas") {
        for (auto& change : plan.metadata) if (change.path == "refs/stash") change.after = to_bytes(old.work.hex() + "\n");
    }
    SECTION("before pair is verified from journal bytes") {
        for (auto& change : plan.metadata) if (change.path == "refs/stash") change.before = to_bytes(old.work.hex() + "\n");
    }
    SECTION("empty after stack must have absent after ref") {
        for (auto& change : plan.metadata) if (change.path == "stashstack") change.after = encode_stash_stack({});
    }
    SECTION("non-top entry cannot evade typed stash schema validation") {
        for (auto& change : plan.metadata) if (change.path == "stashstack") change.after = encode_stash_stack({top.work, old.index});
    }
    SECTION("standalone refs stash still requires stash entry schema") {
        plan.metadata = {{"refs/stash", state.raw_ref, to_bytes(old.head.hex() + "\n")}};
    }
    const auto before = stack_metadata(repo);
    REQUIRE_THROWS_AS(execute_transaction(repo, plan), Error);
    REQUIRE(stack_metadata(repo) == before);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
}
TEST_CASE("StashStack Prepared and Committed recovery pin non-top roots and ignore inconsistent live pair intermediates", "[stashstack]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto top = stack_entry(repo, "top", true);
    const auto old = stack_entry(repo, "old");
    const auto next = stack_entry(repo, "next");
    stack_install(repo, {top.work, old.work});
    const auto state = read_stash_stack(repo);
    const StashStack desired{next.work, top.work};
    TransactionPlan plan;
    plan_stash_stack_update(repo, plan, state, desired);
    bool committed = false;
    SECTION("Prepared rollback restores old entries") {}
    SECTION("Committed rollforward completes new entries") { committed = true; }
    bool reached = false;
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view point) {
        if (point == (committed ? "journal.committed" : "journal.prepared")) { reached = true; throw Error("fixture stop"); }
    }), Error);
    REQUIRE(reached);
    // Reproduce a legal metadata-boundary intermediate, not arbitrary bytes.
    if (committed) stack_write(repo, "stashstack", *state.raw_stack);
    else stack_write(repo, "refs/stash", to_bytes(next.work.hex() + "\n"));
    REQUIRE_THROWS_AS(read_stash_stack(repo), Error);
    const auto roots = transaction_roots(repo);
    for (const auto id : {old.work, old.head, old.index, top.work, top.head, top.index, *top.untracked, next.work, next.head, next.index})
        REQUIRE(roots.contains(id));
    recover_transaction(repo);
    const auto result = read_stash_stack(repo);
    REQUIRE(result.entries == (committed ? desired : state.entries));
    if (!committed) { REQUIRE(result.raw_stack == state.raw_stack); REQUIRE(result.raw_ref == state.raw_ref); }
    recover_transaction(repo);
    REQUIRE(read_stash_stack(repo).raw_stack == result.raw_stack);
    REQUIRE(read_stash_stack(repo).raw_ref == result.raw_ref);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
