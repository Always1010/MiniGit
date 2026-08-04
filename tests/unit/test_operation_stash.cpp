#include <catch2/catch_test_macros.hpp>
#include "minigit/operation.hpp"
#include "minigit/stash_stack.hpp"
#include "minigit/error.hpp"
#include "minigit/index.hpp"
#include "minigit/object_store.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
#include <sys/stat.h>
#include <utility>
#include <zlib.h>
using namespace minigit;
namespace {
OperationState stash_operation_state(ObjectId head, ObjectId source, ObjectId stack_blob, ObjectId top, bool pop = false) {
    OperationState state;
    state.kind = OperationKind::StashApply;
    state.original_head = {"refs/heads/main", head};
    state.current_head = state.original_head;
    state.original_head_bytes = to_bytes("ref: refs/heads/main\n");
    state.source_commit = source;
    state.message = pop ? "stash:pop" : "stash:apply";
    state.stash = StashOperation{pop, 1, stack_blob, to_bytes(top.hex() + "\n")};
    return state;
}
void stash_operation_checksum(Bytes& bytes) {
    const auto checksum = sha256(std::span<const std::uint8_t>(bytes).first(bytes.size() - 32));
    std::copy(checksum.bytes().begin(), checksum.bytes().end(), bytes.end() - 32);
}
void stash_operation_number(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (24U - i * 8U));
    stash_operation_checksum(bytes);
}
ObjectId stash_operation_commit(const Repository& repo, std::string_view message, const std::vector<ObjectId>& parents = {},
                                std::optional<ObjectId> tree = std::nullopt) {
    ObjectStore store(repo.metadata() / "objects");
    return store.write(ObjectType::Commit, encode_commit({tree ? *tree : store.write(ObjectType::Tree, encode_tree({})), parents,
        "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
ObjectId stash_operation_raw_commit(const Repository& repo) {
    const auto payload = to_bytes("invalid commit");
    auto frame = to_bytes("commit " + std::to_string(payload.size()));
    frame.push_back(0);
    frame.insert(frame.end(), payload.begin(), payload.end());
    const auto id = sha256(frame);
    uLongf size = ::compressBound(static_cast<uLong>(frame.size()));
    Bytes compressed(static_cast<std::size_t>(size));
    REQUIRE(::compress2(compressed.data(), &size, frame.data(), static_cast<uLong>(frame.size()), Z_DEFAULT_COMPRESSION) == Z_OK);
    compressed.resize(static_cast<std::size_t>(size));
    const auto path = ObjectStore(repo.metadata() / "objects").object_path(id);
    detail::ensure_directory(path.parent_path()); atomic_write(path, compressed);
    return id;
}
StashEntry stash_operation_entry(const Repository& repo, std::string_view name, bool untracked = false,
                                std::optional<ObjectId> tree = std::nullopt) {
    const auto head = stash_operation_commit(repo, std::string(name) + " H", {}, tree);
    const auto index = stash_operation_commit(repo, std::string(name) + " I", {head}, tree);
    std::optional<ObjectId> extra;
    std::vector<ObjectId> parents{head, index};
    if (untracked) { extra = stash_operation_commit(repo, std::string(name) + " U", {}, tree); parents.push_back(*extra); }
    return {stash_operation_commit(repo, std::string(name) + " W", parents, tree), head, index, extra};
}
struct StashOperationFixture {
    StashEntry top;
    StashEntry selected;
    OperationState state;
};
StashOperationFixture stash_operation_fixture(const Repository& repo) {
    const auto top = stash_operation_entry(repo, "top", true);
    const auto selected = stash_operation_entry(repo, "selected");
    const auto blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, encode_stash_stack({top.work, selected.work, top.work}));
    auto state = stash_operation_state(selected.head, selected.work, blob, top.work);
    auto upper = top.work.hex() + "\n";
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    state.stash->raw_ref = to_bytes(upper);
    return {top, selected, std::move(state)};
}
std::map<std::string, Bytes> stash_operation_disk(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.root()))
        if (std::filesystem::is_regular_file(entry.symlink_status()))
            result.emplace(entry.path().lexically_relative(repo.root()).generic_string(), read_file(entry.path()));
    return result;
}
void stash_operation_same_plan(const TransactionPlan& a, const TransactionPlan& b) {
    REQUIRE(a.work.size() == b.work.size()); REQUIRE(a.metadata.size() == b.metadata.size());
    for (std::size_t i = 0; i < a.work.size(); ++i) {
        REQUIRE(a.work[i].path == b.work[i].path); REQUIRE(a.work[i].before == b.work[i].before); REQUIRE(a.work[i].after == b.work[i].after);
    }
    for (std::size_t i = 0; i < a.metadata.size(); ++i) {
        REQUIRE(a.metadata[i].path == b.metadata[i].path); REQUIRE(a.metadata[i].before == b.metadata[i].before); REQUIRE(a.metadata[i].after == b.metadata[i].after);
    }
}
}
TEST_CASE("OperationStash codec preserves legacy v1 bytes and canonical v2 context", "[operationstash]") {
    OperationState legacy;
    legacy.original_head = {std::nullopt, ObjectId{}};
    legacy.current_head = legacy.original_head;
    legacy.original_head_bytes = to_bytes(std::string(64, '0') + "\n");
    legacy.message = "m";
    // Golden digests of the existing 221-byte v1 layout, computed from its fixed
    // framing rather than from the current encoder. Source and HEAD IDs are zero.
    const std::map<OperationKind, std::string> golden{
        {OperationKind::Merge, "04257a10a749343e84b4cc55e70bc7db943102461c44873a59bf95cf9b220892"},
        {OperationKind::CherryPick, "8c493d5efdd82d055787c5a7671c1e263dd2bcdccbc765d0d37c91d0903a9a46"},
        {OperationKind::Revert, "320a20015346e7718e34894cb9b78a1255d6885a9d63f47cbf06bac605d3541f"}};
    for (const auto& [kind, digest] : golden) {
        legacy.kind = kind; const auto bytes = encode_operation(legacy);
        REQUIRE(bytes.size() == 221); REQUIRE(bytes[7] == 1); REQUIRE(sha256(bytes).hex() == digest);
        REQUIRE_FALSE(decode_operation(bytes).stash); REQUIRE(encode_operation(decode_operation(bytes)) == bytes);
    }
    auto state = stash_operation_state(sha256(to_bytes("head")), sha256(to_bytes("selected")), sha256(to_bytes("stack")), sha256(to_bytes("top")));
    state.work = {{"d", {NodeKind::Directory, std::nullopt, 0710}, {NodeKind::Directory, std::nullopt, 0750}},
        {"d/file", {NodeKind::File, sha256(to_bytes("before")), 0640}, {NodeKind::File, sha256(to_bytes("after")), 0751}},
        {"empty", {NodeKind::Directory, std::nullopt, 0500}, {}}};
    SECTION("apply context preserves exact saved uppercase ref and node images") {
        auto raw = state.stash->raw_ref;
        std::transform(raw.begin(), raw.end(), raw.begin(), [](std::uint8_t c) { return c >= 'a' && c <= 'f' ? static_cast<std::uint8_t>(c - 'a' + 'A') : c; });
        state.stash->raw_ref = std::move(raw);
    }
    SECTION("pop context and detached HEAD round trip") {
        state.stash->pop = true; state.message = "stash:pop";
        state.original_head.symbolic.reset(); state.current_head = state.original_head;
        state.original_head_bytes = to_bytes(state.original_head.id->hex() + "\n");
    }
    const auto bytes = encode_operation(state);
    REQUIRE(bytes[7] == 2); REQUIRE(bytes[8] == 4);
    const auto decoded = decode_operation(bytes);
    REQUIRE(decoded.stash == state.stash); REQUIRE(decoded.message == state.message);
    REQUIRE(decoded.original_head_bytes == state.original_head_bytes);
    TransactionPlan a, b; a.work = state.work; b.work = decoded.work; stash_operation_same_plan(a, b);
    REQUIRE(encode_operation(decoded) == bytes);
}
TEST_CASE("OperationStash codec rejects authenticated version field and allocation bounds", "[operationstash]") {
    auto state = stash_operation_state(sha256(to_bytes("head")), sha256(to_bytes("source")), sha256(to_bytes("stack")), sha256(to_bytes("top")));
    const auto original = encode_operation(state);
    const auto context = original.size() - 32 - 107;
    SECTION("versions kinds phases and required presence form a closed canonical mapping") {
        for (const auto version : {0U, 1U, 3U, 0xffffffffU}) {
            auto broken = original; stash_operation_number(broken, 4, version); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        for (const auto kind : {0U, 1U, 2U, 3U, 5U, 255U}) {
            auto broken = original; broken[8] = static_cast<std::uint8_t>(kind); stash_operation_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        for (const auto value : {0U, 2U, 255U}) {
            auto broken = original; broken[context] = static_cast<std::uint8_t>(value); stash_operation_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        auto broken = original; broken[9] = 2; stash_operation_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        state.stash.reset(); REQUIRE_THROWS_AS(encode_operation(state), Error);
        state = stash_operation_state(ObjectId{}, ObjectId{}, ObjectId{}, ObjectId{}); state.kind = OperationKind::Merge;
        REQUIRE_THROWS_AS(encode_operation(state), Error);
    }
    SECTION("context booleans position and exact ref bytes are validated") {
        for (const auto flag : {2U, 255U}) {
            auto broken = original; broken[context + 1] = static_cast<std::uint8_t>(flag); stash_operation_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        for (const auto position : {100000U, 0xffffffffU}) {
            auto broken = original; stash_operation_number(broken, context + 2, position); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        for (const auto length : {0U, 64U, 66U, 0xffffffffU}) {
            auto broken = original; stash_operation_number(broken, context + 38, length); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        for (const auto offset : {context + 42, context + 106}) {
            auto broken = original; broken[offset] = 'x'; stash_operation_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        state.stash->pop = true; REQUIRE_THROWS_AS(encode_operation(state), Error);
        state.message = "stash:pop"; state.stash->raw_ref.clear(); REQUIRE_THROWS_AS(encode_operation(state), Error);
    }
    SECTION("unknown trailing fields truncated context and work count fail before publication") {
        auto broken = original; broken.insert(broken.end() - 32, 0); stash_operation_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        broken = original; broken.erase(broken.begin() + static_cast<std::ptrdiff_t>(context + 80)); stash_operation_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        broken = original; stash_operation_number(broken, context - 4, 100001); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        state.current_head.id = state.source_commit; REQUIRE_THROWS_AS(encode_operation(state), Error);
        state.current_head = state.original_head; state.work = {{"d", {NodeKind::Directory, std::nullopt, 0500}, {}}, {"d/file", {}, {}}};
        REQUIRE_THROWS_AS(encode_operation(state), Error);
    }
}
TEST_CASE("OperationStash roots validate complete saved stack roles and selected source without live reads", "[operationstash]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    auto fixture = stash_operation_fixture(repo); auto state = fixture.state;
    const auto before_blob = store.write(ObjectType::Blob, to_bytes("before"));
    const auto after_blob = store.write(ObjectType::Blob, to_bytes("after"));
    const auto third_blob = store.write(ObjectType::Blob, to_bytes("third"));
    state.work = {{"unsafe/file", {NodeKind::File, before_blob, 0640}, {NodeKind::File, after_blob, 0751}}};
    state.original_index_blob = store.write(ObjectType::Blob, encode_index({{"conflict", "100644", before_blob, 1},
        {"conflict", "100755", after_blob, 2}, {"conflict", "100644", third_blob, 3}}));
    SECTION("all non-top and stage roots survive invalid current metadata and unsupported Work") {
        atomic_write(repo.metadata() / "HEAD", to_bytes("invalid live HEAD")); atomic_write(repo.metadata() / "index", to_bytes("invalid live Index"));
        atomic_write(repo.metadata() / "stashstack", to_bytes("invalid live stack"));
        detail::ensure_directory(repo.metadata() / "refs"); atomic_write(repo.metadata() / "refs/stash", to_bytes("invalid live ref"));
        REQUIRE(::mkfifo((repo.root() / "fifo").c_str(), 0600) == 0);
        std::filesystem::create_symlink("fifo", repo.root() / "unsafe");
        atomic_write(repo.metadata() / "operation", encode_operation(state)); atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        const auto disk = stash_operation_disk(repo);
        auto expected = stash_roots(repo, {fixture.top.work, fixture.selected.work});
        expected.insert({state.stash->stack_blob, *state.original_index_blob, before_blob, after_blob, third_blob});
        REQUIRE(state_roots(repo, state) == expected);
        const auto read = read_operation(repo); REQUIRE(read); REQUIRE(read->stash == state.stash);
        REQUIRE(stash_operation_disk(repo) == disk);
    }
    SECTION("typed-only saved history permits missing Trees and missing ancestors") {
        const auto head = stash_operation_commit(repo, "historical H", {ObjectId{}}, ObjectId{});
        const auto index = stash_operation_commit(repo, "historical I", {head}, ObjectId{});
        const auto untracked = stash_operation_commit(repo, "historical U", {}, ObjectId{});
        const StashEntry old{stash_operation_commit(repo, "historical W", {head, index, untracked}, ObjectId{}), head, index, untracked};
        state.source_commit = old.work; state.stash->position = 0;
        state.stash->stack_blob = store.write(ObjectType::Blob, encode_stash_stack({old.work}));
        state.stash->raw_ref = to_bytes(old.work.hex() + "\n");
        REQUIRE(state_roots(repo, state).contains(*old.untracked));
    }
    SECTION("selected position source top ref and every historical role are mandatory") {
        auto bad = state; bad.stash->position = 3; REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        bad = state; bad.source_commit = fixture.top.work; REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        bad = state; bad.stash->raw_ref = to_bytes(fixture.selected.work.hex() + "\n"); REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        const auto broken = stash_operation_raw_commit(repo);
        for (const auto id : {ObjectId{}, before_blob, broken, fixture.top.index}) {
            bad = state;
            bad.stash->stack_blob = store.write(ObjectType::Blob, encode_stash_stack({fixture.top.work, fixture.selected.work, id}));
            REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        }
        const auto wrong_index = stash_operation_commit(repo, "wrong I", {fixture.top.head});
        const auto wrong_source = stash_operation_commit(repo, "wrong W", {fixture.selected.head, wrong_index});
        bad = state; bad.source_commit = wrong_source;
        bad.stash->stack_blob = store.write(ObjectType::Blob, encode_stash_stack({fixture.top.work, wrong_source}));
        REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        const auto disk = stash_operation_disk(repo);
        TransactionPlan publication;
        publication.metadata.push_back({"operation", std::nullopt, encode_operation(bad)});
        REQUIRE_THROWS_AS(execute_transaction(repo, publication), Error);
        REQUIRE(stash_operation_disk(repo) == disk);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
    }
    SECTION("typed and encoded stack and all backup stages enforce their limits") {
        for (const auto id : {ObjectId{}, fixture.top.work}) {
            auto bad = state; bad.stash->stack_blob = id; REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        }
        auto malformed = encode_stash_stack({}); stash_operation_number(malformed, 8, 100001);
        auto bad = state; bad.stash->stack_blob = store.write(ObjectType::Blob, malformed); REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        bad = state; bad.stash->stack_blob = store.write(ObjectType::Blob, encode_stash_stack({})); REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        bad = state; bad.original_index_blob = store.write(ObjectType::Blob, encode_index({{"conflict", "100644", fixture.top.work, 2}}));
        REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        auto too_many = encode_index({}); stash_operation_number(too_many, 8, 300001);
        bad = state; bad.original_index_blob = store.write(ObjectType::Blob, too_many); REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
    }
}
TEST_CASE("OperationStash plans bind exact expected context and preserve strong create delete noop guarantees", "[operationstash]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); RepoLock lock(repo.metadata());
    const auto fixture = stash_operation_fixture(repo); const auto state = fixture.state;
    TransactionPlan plan; plan.work = {{"unchanged", {}, {}}};
    SECTION("creation deletion and identical context noop retain exact encoded bytes") {
        const auto disk = stash_operation_disk(repo); plan_operation_update(repo, plan, std::nullopt, state);
        REQUIRE(plan.metadata.size() == 1); REQUIRE_FALSE(plan.metadata[0].before); REQUIRE(plan.metadata[0].after == encode_operation(state));
        REQUIRE(stash_operation_disk(repo) == disk);
        atomic_write(repo.metadata() / "operation", encode_operation(state));
        TransactionPlan noop; plan_operation_update(repo, noop, state, state); REQUIRE(noop.metadata.empty());
        TransactionPlan deletion; plan_operation_update(repo, deletion, state, std::nullopt);
        REQUIRE(deletion.metadata[0].before == encode_operation(state)); REQUIRE_FALSE(deletion.metadata[0].after);
        auto stale = state;
        stale.stash->raw_ref = to_bytes(fixture.top.work.hex() + "\n");
        REQUIRE(stale.stash->raw_ref != state.stash->raw_ref);
        REQUIRE_THROWS_AS(plan_operation_update(repo, noop, stale, std::nullopt), Error); REQUIRE(noop.metadata.empty());
    }
    SECTION("valid different context is rejected as replacement and expected mismatch") {
        atomic_write(repo.metadata() / "operation", encode_operation(state));
        auto pop = state; pop.stash->pop = true; pop.message = "stash:pop";
        const auto original = plan; const auto disk = stash_operation_disk(repo);
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, pop, std::nullopt), Error);
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, pop), Error);
        auto bad = state; bad.stash->position = 0;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, bad), Error);
        stash_operation_same_plan(plan, original); REQUIRE(stash_operation_disk(repo) == disk);
    }
    SECTION("preplanned State and pending journal fail without modifying plan") {
        const auto original = plan;
        plan.metadata.push_back({"operation", std::nullopt, std::nullopt}); const auto duplicate = plan;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, std::nullopt, state), Error); stash_operation_same_plan(plan, duplicate);
        plan = original; atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, std::nullopt, state), Error); stash_operation_same_plan(plan, original);
    }
    SECTION("identical noop still validates missing semantic roots") {
        atomic_write(repo.metadata() / "operation", encode_operation(state));
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(state.stash->stack_blob));
        const auto original = plan;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, state), Error); stash_operation_same_plan(plan, original);
    }
}
TEST_CASE("OperationStash Prepared create and Committed delete pin non-top stack roots through exact recovery", "[operationstash]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto fixture = stash_operation_fixture(repo); auto state = fixture.state;
    const auto before_blob = store.write(ObjectType::Blob, to_bytes("before"));
    const auto after_blob = store.write(ObjectType::Blob, to_bytes("after"));
    const auto raw_index = encode_index({{"file", "100644", before_blob}});
    state.original_index_blob = store.write(ObjectType::Blob, raw_index);
    state.work = {{"file", {NodeKind::File, before_blob, 0640}, {NodeKind::File, after_blob, 0751}}};
    bool deleting = false;
    SECTION("Prepared creation restores missing State and original actual Index Work") {}
    SECTION("Committed deletion retains source roots until cleanup then removes State once") { deleting = true; }
    atomic_write_with_permissions(repo.root() / "file", to_bytes(deleting ? "after" : "before"), deleting ? 0751U : 0640U);
    atomic_write(repo.metadata() / "index", deleting ? encode_index({{"file", "100755", after_blob}}) : raw_index);
    if (deleting) atomic_write(repo.metadata() / "operation", encode_operation(state));
    const auto original_head = read_file(repo.metadata() / "HEAD");
    TransactionPlan plan;
    if (deleting) plan_operation_update(repo, plan, state, std::nullopt);
    else {
        plan.work = state.work;
        plan.metadata.push_back({"index", raw_index, encode_index({{"file", "100755", after_blob}})});
        plan_operation_update(repo, plan, std::nullopt, state);
    }
    bool reached = false;
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view point) {
        if (point == (deleting ? "journal.committed" : "journal.prepared")) { reached = true; throw Error("stop durable boundary"); }
    }), Error);
    REQUIRE(reached);
    // Semantic recovery uses the saved stack Blob, never this unrelated live pair.
    atomic_write(repo.metadata() / "stashstack", to_bytes("broken live stack"));
    detail::ensure_directory(repo.metadata() / "refs"); atomic_write(repo.metadata() / "refs/stash", to_bytes("broken live ref"));
    const auto roots = transaction_roots(repo);
    for (const auto id : {fixture.top.work, fixture.top.head, fixture.top.index, *fixture.top.untracked,
            fixture.selected.work, fixture.selected.head, fixture.selected.index, state.stash->stack_blob, *state.original_index_blob})
        REQUIRE(roots.contains(id));
    recover_transaction(repo); recover_transaction(repo);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
    REQUIRE(read_file(repo.root() / "file") == to_bytes(deleting ? "after" : "before"));
    REQUIRE((static_cast<std::uint32_t>(std::filesystem::symlink_status(repo.root() / "file").permissions()) & 07777) == (deleting ? 0751U : 0640U));
    REQUIRE(read_file(repo.metadata() / "index") == (deleting ? encode_index({{"file", "100755", after_blob}}) : raw_index));
    REQUIRE(read_file(repo.metadata() / "HEAD") == original_head);
    REQUIRE(read_file(repo.metadata() / "stashstack") == to_bytes("broken live stack"));
    REQUIRE(read_file(repo.metadata() / "refs/stash") == to_bytes("broken live ref"));
}
