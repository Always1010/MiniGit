#include <catch2/catch_test_macros.hpp>
#include "minigit/operation.hpp"
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
OperationState rebase_state(ObjectId onto, const std::vector<ObjectId>& queue) {
    OperationState state;
    state.kind = OperationKind::Rebase; state.phase = OperationPhase::Ready;
    state.original_head = {"refs/heads/main", queue.back()};
    state.current_head = {std::nullopt, onto};
    state.original_head_bytes = to_bytes("ref: refs/heads/main\n");
    state.source_commit = queue.front(); state.message = "rebase";
    state.rebase = RebaseOperation{onto, queue, 0, to_bytes(queue.back().hex() + "\n")};
    return state;
}
ObjectId rebase_commit(const Repository& repo, std::string_view message, const std::vector<ObjectId>& parents = {},
                       std::optional<ObjectId> tree = std::nullopt) {
    ObjectStore store(repo.metadata() / "objects");
    return store.write(ObjectType::Commit, encode_commit({tree ? *tree : store.write(ObjectType::Tree, encode_tree({})), parents,
        "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
OperationState rebase_fixture(const Repository& repo) {
    const auto onto = rebase_commit(repo, "onto");
    const auto first = rebase_commit(repo, "first", {onto});
    return rebase_state(onto, {first, rebase_commit(repo, "tip", {first})});
}
ObjectId rebase_raw_commit(const Repository& repo) {
    const auto payload = to_bytes("invalid commit");
    auto frame = to_bytes("commit " + std::to_string(payload.size())); frame.push_back(0);
    frame.insert(frame.end(), payload.begin(), payload.end()); const auto id = sha256(frame);
    uLongf size = ::compressBound(static_cast<uLong>(frame.size())); Bytes compressed(static_cast<std::size_t>(size));
    REQUIRE(::compress2(compressed.data(), &size, frame.data(), static_cast<uLong>(frame.size()), Z_DEFAULT_COMPRESSION) == Z_OK);
    compressed.resize(static_cast<std::size_t>(size));
    const auto path = ObjectStore(repo.metadata() / "objects").object_path(id);
    detail::ensure_directory(path.parent_path()); atomic_write(path, compressed); return id;
}
void rebase_checksum(Bytes& bytes) {
    const auto checksum = sha256(std::span<const std::uint8_t>(bytes).first(bytes.size() - 32));
    std::copy(checksum.bytes().begin(), checksum.bytes().end(), bytes.end() - 32);
}
void rebase_number(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (24U - i * 8U));
    rebase_checksum(bytes);
}
std::map<std::string, Bytes> rebase_disk(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.root()))
        if (std::filesystem::is_regular_file(entry.symlink_status()))
            result.emplace(entry.path().lexically_relative(repo.root()).generic_string(), read_file(entry.path()));
    return result;
}
void rebase_same_plan(const TransactionPlan& a, const TransactionPlan& b) {
    REQUIRE(a.work.size() == b.work.size()); REQUIRE(a.metadata.size() == b.metadata.size());
    for (std::size_t i = 0; i < a.work.size(); ++i) {
        REQUIRE(a.work[i].path == b.work[i].path); REQUIRE(a.work[i].before == b.work[i].before); REQUIRE(a.work[i].after == b.work[i].after);
    }
    for (std::size_t i = 0; i < a.metadata.size(); ++i) {
        REQUIRE(a.metadata[i].path == b.metadata[i].path); REQUIRE(a.metadata[i].before == b.metadata[i].before); REQUIRE(a.metadata[i].after == b.metadata[i].after);
    }
}
OperationState rebase_next(const Repository& repo, OperationState state) {
    state.current_head.id = rebase_commit(repo, "rewritten first", {*state.current_head.id});
    state.phase = OperationPhase::Ready; ++state.rebase->cursor;
    state.source_commit = state.rebase->queue[state.rebase->cursor]; return state;
}
}
TEST_CASE("OperationRebase codec preserves v1 and Stash v2 bytes and round trips both phases", "[operationrebase]") {
    OperationState legacy;
    legacy.original_head = {std::nullopt, ObjectId{}}; legacy.current_head = legacy.original_head;
    legacy.original_head_bytes = to_bytes(std::string(64, '0') + "\n"); legacy.message = "m";
    // Fixed old-layout digests, independent of the Rebase encoder path.
    for (const auto& [kind, digest] : std::map<OperationKind, std::string>{
        {OperationKind::Merge, "04257a10a749343e84b4cc55e70bc7db943102461c44873a59bf95cf9b220892"},
        {OperationKind::CherryPick, "8c493d5efdd82d055787c5a7671c1e263dd2bcdccbc765d0d37c91d0903a9a46"},
        {OperationKind::Revert, "320a20015346e7718e34894cb9b78a1255d6885a9d63f47cbf06bac605d3541f"}}) {
        legacy.kind = kind; const auto bytes = encode_operation(legacy);
        REQUIRE(bytes.size() == 221); REQUIRE(sha256(bytes).hex() == digest);
        REQUIRE_FALSE(decode_operation(bytes).rebase); REQUIRE(encode_operation(decode_operation(bytes)) == bytes);
        auto ready = bytes; ready[9] = 2; rebase_checksum(ready); REQUIRE_THROWS_AS(decode_operation(ready), Error);
    }
    legacy.kind = OperationKind::StashApply; legacy.message = "stash:apply";
    legacy.stash = StashOperation{false, 0, ObjectId{}, to_bytes(std::string(64, '0') + "\n")};
    const auto stash_bytes = encode_operation(legacy);
    REQUIRE(stash_bytes.size() == 338);
    REQUIRE(sha256(stash_bytes).hex() == "91cd24e505100a59c03db5a79527a634b7b2116a2f6d7464709907f25f03dcdb");
    REQUIRE(encode_operation(decode_operation(stash_bytes)) == stash_bytes);
    legacy.phase = OperationPhase::Ready; REQUIRE_THROWS_AS(encode_operation(legacy), Error);
    auto state = rebase_state(sha256(to_bytes("onto")), {sha256(to_bytes("first")), sha256(to_bytes("tip"))});
    state.work = {{"d", {NodeKind::File, sha256(to_bytes("old")), 0640}, {NodeKind::Directory, std::nullopt, 0710}},
        {"d/new", {}, {NodeKind::File, sha256(to_bytes("new")), 0751}}};
    SECTION("Ready retains uppercase exact symbolic branch image") {
        auto& raw = *state.rebase->original_branch_ref;
        std::transform(raw.begin(), raw.end(), raw.begin(), [](std::uint8_t c) { return c >= 'a' && c <= 'f' ? static_cast<std::uint8_t>(c - 'a' + 'A') : c; });
    }
    SECTION("Conflicted original detached HEAD needs no branch backup and may equal current ID") {
        state.phase = OperationPhase::Conflicted; state.original_head.symbolic.reset();
        state.current_head = state.original_head; state.rebase->original_branch_ref.reset();
        auto raw = state.original_head.id->hex() + "\n";
        std::transform(raw.begin(), raw.end(), raw.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
        state.original_head_bytes = to_bytes(raw);
    }
    const auto bytes = encode_operation(state); REQUIRE(bytes[7] == 2); REQUIRE(bytes[8] == 5);
    const auto decoded = decode_operation(bytes);
    REQUIRE(decoded.rebase == state.rebase); REQUIRE(decoded.phase == state.phase);
    REQUIRE(decoded.original_head == state.original_head); REQUIRE(decoded.current_head == state.current_head);
    REQUIRE(decoded.original_head_bytes == state.original_head_bytes); REQUIRE_FALSE(decoded.stash);
    TransactionPlan a, b; a.work = state.work; b.work = decoded.work; rebase_same_plan(a, b);
    REQUIRE(encode_operation(decoded) == bytes);
}
TEST_CASE("OperationRebase codec rejects authenticated context counts shapes and topology before allocation", "[operationrebase]") {
    auto state = rebase_state(sha256(to_bytes("onto")), {sha256(to_bytes("first")), sha256(to_bytes("tip"))});
    const auto bytes = encode_operation(state);
    const auto context = bytes.size() - 32 - (42 + 64 + 69);
    SECTION("version kind phase and required context remain closed") {
        for (const auto version : {0U, 1U, 3U, 0xffffffffU}) {
            auto broken = bytes; rebase_number(broken, 4, version); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        for (const auto value : {0U, 6U, 255U}) {
            auto broken = bytes; broken[8] = static_cast<std::uint8_t>(value); rebase_checksum(broken);
            REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        for (const auto value : {0U, 3U, 255U}) {
            auto broken = bytes; broken[9] = static_cast<std::uint8_t>(value); rebase_checksum(broken);
            REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        for (const auto value : {0U, 2U}) {
            auto broken = bytes; broken[context] = static_cast<std::uint8_t>(value); rebase_checksum(broken);
            REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        auto broken = bytes; broken.insert(broken.end() - 32, 0); rebase_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        broken = bytes; broken.erase(broken.end() - 33); rebase_checksum(broken); REQUIRE_THROWS_AS(decode_operation(broken), Error);
    }
    SECTION("authenticated count bombs and truncated queue fail the count precheck") {
        for (const auto count : {0U, 100000U, 100001U, 0xffffffffU}) {
            auto broken = bytes; rebase_number(broken, context + 33, count);
            try { (void)decode_operation(broken); FAIL("queue count was accepted"); }
            catch (const Error& error) { REQUIRE(std::string(error.what()).find("队列数量") != std::string::npos); }
        }
        state.rebase->queue.assign(100001, *state.original_head.id); REQUIRE_THROWS_AS(encode_operation(state), Error);
    }
    SECTION("queue identity source cursor and exact branch bytes are mandatory") {
        auto bad = state; bad.rebase->queue[0] = bad.rebase->queue[1]; REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.rebase->queue.back() = sha256(to_bytes("other tip")); REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.source_commit = bad.rebase->queue.back(); REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.rebase->cursor = 2; REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.rebase->original_branch_ref.reset(); REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.rebase->original_branch_ref = to_bytes(state.rebase->onto.hex() + "\n"); REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.current_head.symbolic = "refs/heads/main"; REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.current_head.id.reset(); REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.message = "different"; REQUIRE_THROWS_AS(encode_operation(bad), Error);
        bad = state; bad.stash = StashOperation{}; REQUIRE_THROWS_AS(encode_operation(bad), Error);
        auto broken = bytes; rebase_number(broken, context + 106, 64); REQUIRE_THROWS_AS(decode_operation(broken), Error);
        broken = bytes; rebase_number(broken, context + 101, 2); REQUIRE_THROWS_AS(decode_operation(broken), Error);
    }
    SECTION("both image topology permissions and raw HEAD bounds remain enforced") {
        const auto blob = sha256(to_bytes("file"));
        state.work = {{"d", {NodeKind::File, blob, 0644}, {NodeKind::Directory, std::nullopt, 0700}},
            {"d/new", {NodeKind::File, blob, 0644}, {NodeKind::File, blob, 0644}}};
        REQUIRE_THROWS_AS(encode_operation(state), Error);
        state.work[1].before = {}; state.work[0].after.permissions = 0500; REQUIRE_THROWS_AS(encode_operation(state), Error);
        state.work.clear(); state.original_head.symbolic.reset(); state.rebase->original_branch_ref.reset();
        state.original_head_bytes.assign(4103, 'x'); REQUIRE_THROWS_AS(encode_operation(state), Error);
    }
}
TEST_CASE("OperationRebase roots keep every typed source and backup without live Tree or DAG reads", "[operationrebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects"); auto state = rebase_fixture(repo);
    const auto before = store.write(ObjectType::Blob, to_bytes("before")); const auto after = store.write(ObjectType::Blob, to_bytes("after"));
    const auto stage = store.write(ObjectType::Blob, to_bytes("stage"));
    state.work = {{"file", {NodeKind::File, before, 0640}, {NodeKind::File, after, 0751}}};
    state.original_index_blob = store.write(ObjectType::Blob, encode_index({{"conflict", "100644", before, 1},
        {"conflict", "100755", stage, 2}, {"conflict", "100644", after, 3}}));
    SECTION("missing historical Trees parents and broken live HEAD Index Work are not inspected") {
        const auto onto = rebase_commit(repo, "historical onto", {ObjectId{}}, ObjectId{});
        const auto first = rebase_commit(repo, "historical first", {ObjectId{}}, ObjectId{});
        const auto tip = rebase_commit(repo, "historical tip", {first, ObjectId{}}, ObjectId{});
        auto historical = rebase_state(onto, {first, tip}); historical.work = state.work; historical.original_index_blob = state.original_index_blob;
        atomic_write(repo.metadata() / "HEAD", to_bytes("broken live HEAD")); atomic_write(repo.metadata() / "index", to_bytes("broken Index"));
        detail::ensure_directory(repo.metadata() / "refs/heads"); atomic_write(repo.metadata() / "refs/heads/main", to_bytes("broken branch"));
        REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0);
        atomic_write(repo.metadata() / "operation", encode_operation(historical));
        const auto disk = rebase_disk(repo); const auto roots = state_roots(repo, historical);
        for (const auto id : {onto, first, tip, before, after, stage, *state.original_index_blob}) REQUIRE(roots.contains(id));
        REQUIRE(encode_operation(*read_operation(repo)) == encode_operation(historical)); REQUIRE(rebase_disk(repo) == disk);
    }
    SECTION("all queue onto current original roles and every saved stage must be typed and schema valid") {
        const auto invalid = rebase_raw_commit(repo);
        for (const auto id : {ObjectId{}, before, invalid}) {
            auto bad = state; bad.rebase->onto = id; REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
            bad = state; bad.current_head.id = id; REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
            bad = state; bad.rebase->queue[0] = id; bad.source_commit = id; REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
            bad = state; bad.rebase->queue.back() = id; bad.original_head.id = id;
            bad.rebase->original_branch_ref = to_bytes(id.hex() + "\n"); REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        }
        auto bad = state; bad.original_index_blob = store.write(ObjectType::Blob, encode_index({{"conflict", "100644", state.rebase->onto, 2}}));
        REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        bad = state; bad.work[0].after.blob = state.rebase->onto; REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        const auto disk = rebase_disk(repo); TransactionPlan publication;
        publication.metadata.push_back({"operation", std::nullopt, encode_operation(bad)});
        REQUIRE_THROWS_AS(execute_transaction(repo, publication), Error); REQUIRE(rebase_disk(repo) == disk);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
    }
    SECTION("distinct Rebase Commit payloads are bounded and repeated roles are charged once") {
        const auto large = rebase_commit(repo, std::string(max_file_size / 2, 'a'));
        auto one = rebase_state(large, {large}); REQUIRE(state_roots(repo, one) == std::set<ObjectId>{large});
        const auto other = rebase_commit(repo, std::string(max_file_size / 2, 'b'));
        auto over = rebase_state(large, {large, other}); const auto disk = rebase_disk(repo);
        REQUIRE_THROWS_AS(state_roots(repo, over), Error); REQUIRE(rebase_disk(repo) == disk);
    }
}
TEST_CASE("OperationRebase constrained plans retain original backups and first Work before with strong CAS", "[operationrebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects"); auto state = rebase_fixture(repo);
    const auto original = store.write(ObjectType::Blob, to_bytes("original")); const auto latest = store.write(ObjectType::Blob, to_bytes("latest"));
    state.original_index_blob = store.write(ObjectType::Blob, encode_index({}));
    state.work = {{"file", {NodeKind::File, original, 0640}, {NodeKind::File, original, 0640}}};
    TransactionPlan plan; plan.work = {{"unrelated", {}, {}}};
    SECTION("create identical noop and delete all bind the canonical exact State") {
        const auto disk = rebase_disk(repo); plan_operation_update(repo, plan, std::nullopt, state);
        REQUIRE(plan.metadata.size() == 1); REQUIRE_FALSE(plan.metadata[0].before); REQUIRE(plan.metadata[0].after == encode_operation(state));
        REQUIRE(rebase_disk(repo) == disk); atomic_write(repo.metadata() / "operation", encode_operation(state));
        TransactionPlan noop; plan_operation_update(repo, noop, state, state); REQUIRE(noop.metadata.empty());
        plan_operation_update(repo, noop, state, std::nullopt); REQUIRE(noop.metadata[0].before == encode_operation(state)); REQUIRE_FALSE(noop.metadata[0].after);
    }
    SECTION("Ready conflict retains same commit while adding keys and updating only latest after") {
        atomic_write(repo.metadata() / "operation", encode_operation(state)); auto desired = state;
        desired.phase = OperationPhase::Conflicted; desired.work[0].after = {NodeKind::File, latest, 0751};
        desired.work.push_back({"new", {}, {NodeKind::File, latest, 0644}});
        const auto disk = rebase_disk(repo); plan_operation_update(repo, plan, state, desired);
        REQUIRE(plan.metadata[0].before == encode_operation(state)); REQUIRE(plan.metadata[0].after == encode_operation(desired)); REQUIRE(rebase_disk(repo) == disk);
    }
    SECTION("Ready clean and Conflicted resolved advance exactly one source with sole old-current parent") {
        auto desired = rebase_next(repo, state);
        for (const auto phase : {OperationPhase::Ready, OperationPhase::Conflicted}) {
            auto prior = state; prior.phase = phase; atomic_write(repo.metadata() / "operation", encode_operation(prior));
            TransactionPlan advance; plan_operation_update(repo, advance, prior, desired);
            REQUIRE(advance.metadata[0].before == encode_operation(prior)); REQUIRE(advance.metadata[0].after == encode_operation(desired));
        }
    }
    SECTION("fixed originals removed keys changed first before and illegal phase cursor parent fail without plan mutation") {
        atomic_write(repo.metadata() / "operation", encode_operation(state));
        const auto rewritten = rebase_next(repo, state); const auto empty_parent = rebase_commit(repo, "wrong root");
        const auto multi_parent = rebase_commit(repo, "two parents", {*state.current_head.id, *state.current_head.id});
        std::vector<OperationState> invalid;
        auto bad = state; bad.phase = OperationPhase::Conflicted; bad.rebase->onto = state.source_commit; invalid.push_back(bad);
        bad = state; bad.phase = OperationPhase::Conflicted; bad.rebase->queue[0] = bad.rebase->onto; bad.source_commit = bad.rebase->onto; invalid.push_back(bad);
        bad = state; bad.phase = OperationPhase::Conflicted; bad.original_head.symbolic = "refs/heads/topic";
        bad.original_head_bytes = to_bytes("ref: refs/heads/topic\n"); invalid.push_back(bad);
        bad = state; bad.phase = OperationPhase::Conflicted; bad.original_index_blob.reset(); invalid.push_back(bad);
        bad = state; bad.phase = OperationPhase::Conflicted;
        auto& raw = *bad.rebase->original_branch_ref;
        std::transform(raw.begin(), raw.end(), raw.begin(), [](std::uint8_t c) { return c >= 'a' && c <= 'f' ? static_cast<std::uint8_t>(c - 'a' + 'A') : c; }); invalid.push_back(bad);
        bad = state; bad.phase = OperationPhase::Conflicted; bad.work.clear(); invalid.push_back(bad);
        bad = state; bad.phase = OperationPhase::Conflicted; bad.work[0].before = {NodeKind::File, latest, 0640}; invalid.push_back(bad);
        bad = state; bad.work[0].after.permissions = 0751; invalid.push_back(bad); // Ready at same cursor cannot rewrite arbitrary state.
        bad = rewritten; bad.phase = OperationPhase::Conflicted; invalid.push_back(bad);
        bad = rewritten; bad.current_head.id = empty_parent; invalid.push_back(bad);
        bad = rewritten; bad.current_head.id = multi_parent; invalid.push_back(bad);
        bad = rewritten; bad.current_head.id = *state.original_head.id; invalid.push_back(bad); // source tip's parent is not onto.
        const auto disk = rebase_disk(repo); const auto initial = plan;
        for (const auto& desired : invalid) { REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, desired), Error); rebase_same_plan(plan, initial); }
        auto stale = state; stale.phase = OperationPhase::Conflicted;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, stale, std::nullopt), Error); rebase_same_plan(plan, initial);
        REQUIRE(rebase_disk(repo) == disk);
        atomic_write(repo.metadata() / "operation", encode_operation(stale));
        auto conflict_rewrite = stale; conflict_rewrite.work[0].after.permissions = 0751;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, stale, conflict_rewrite), Error); rebase_same_plan(plan, initial);
    }
    SECTION("missing semantic root noop preplanned operation and pending journal retain strong guarantees") {
        atomic_write(repo.metadata() / "operation", encode_operation(state)); const auto initial = plan;
        plan.metadata.push_back({"operation", std::nullopt, std::nullopt}); const auto duplicate = plan;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, state), Error); rebase_same_plan(plan, duplicate);
        plan = initial; atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, state), Error); rebase_same_plan(plan, initial);
        std::filesystem::remove(repo.metadata() / "transaction"); std::filesystem::remove(store.object_path(state.rebase->queue.front()));
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, state), Error); rebase_same_plan(plan, initial);
    }
}
TEST_CASE("OperationRebase Prepared and Committed replacements recover exact two-sided progress without live reads", "[operationrebase]") {
    test::TempDirectory temp; const auto repo = Repository::init(temp.path()); RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects"); auto before = rebase_fixture(repo);
    const auto old_blob = store.write(ObjectType::Blob, to_bytes("before")); const auto new_blob = store.write(ObjectType::Blob, to_bytes("marker"));
    const auto raw_index = encode_index({{"file", "100644", old_blob}}); before.original_index_blob = store.write(ObjectType::Blob, raw_index);
    before.work = {{"file", {NodeKind::File, old_blob, 0640}, {NodeKind::File, old_blob, 0640}}};
    auto after = before; after.phase = OperationPhase::Conflicted; after.work[0].after = {NodeKind::File, new_blob, 0751};
    bool committed = false;
    SECTION("Prepared replacement restores Ready cursor and complete original images") {}
    SECTION("Committed replacement retains Conflicted cursor and last published images") { committed = true; }
    atomic_write(repo.metadata() / "operation", encode_operation(before)); atomic_write(repo.metadata() / "index", raw_index);
    atomic_write_with_permissions(repo.root() / "file", to_bytes("before"), 0640);
    const auto raw_head = read_file(repo.metadata() / "HEAD");
    TransactionPlan plan; plan.work = after.work;
    plan.metadata.push_back({"index", raw_index, encode_index({{"file", "100644", old_blob, 1},
        {"file", "100644", old_blob, 2}, {"file", "100755", new_blob, 3}})});
    plan_operation_update(repo, plan, before, after); bool reached = false;
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view point) {
        if (point == (committed ? "journal.committed" : "journal.prepared")) { reached = true; throw Error("stop durable boundary"); }
    }), Error); REQUIRE(reached);
    // Saved state roots do not consult or repair an unrelated live original ref.
    detail::ensure_directory(repo.metadata() / "refs/heads"); atomic_write(repo.metadata() / "refs/heads/main", to_bytes("broken live original ref"));
    const auto roots = transaction_roots(repo);
    for (const auto id : {before.rebase->onto, before.rebase->queue.front(), before.rebase->queue.back(),
        old_blob, new_blob, *before.original_index_blob}) REQUIRE(roots.contains(id));
    recover_transaction(repo); recover_transaction(repo);
    REQUIRE(read_file(repo.metadata() / "operation") == encode_operation(committed ? after : before));
    REQUIRE(read_operation(repo)->phase == (committed ? OperationPhase::Conflicted : OperationPhase::Ready));
    REQUIRE(read_file(repo.root() / "file") == to_bytes(committed ? "marker" : "before"));
    REQUIRE((static_cast<std::uint32_t>(std::filesystem::symlink_status(repo.root() / "file").permissions()) & 07777) == (committed ? 0751U : 0640U));
    REQUIRE(read_file(repo.metadata() / "index") == (committed ? *plan.metadata[0].after : raw_index));
    REQUIRE(read_file(repo.metadata() / "HEAD") == raw_head);
    REQUIRE(read_file(repo.metadata() / "refs/heads/main") == to_bytes("broken live original ref"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction")); REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
}
