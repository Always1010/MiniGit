#include <catch2/catch_test_macros.hpp>
#include "minigit/operation.hpp"
#include "minigit/error.hpp"
#include "minigit/index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/worktree_update.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
#include <utility>
using namespace minigit;
namespace {
OperationState operation_state(ObjectId head, ObjectId source, bool symbolic = true) {
    OperationState state;
    state.original_head = {symbolic ? std::optional<std::string>("refs/heads/main") : std::nullopt, head};
    state.current_head = state.original_head;
    state.original_head_bytes = to_bytes(symbolic ? "ref: refs/heads/main\n" : head.hex() + "\n");
    state.source_commit = source;
    state.message = "merge fixture";
    return state;
}
ObjectId operation_commit(const Repository& repo, std::string_view message, std::optional<ObjectId> tree = std::nullopt) {
    ObjectStore store(repo.metadata() / "objects");
    const auto root = tree ? *tree : store.write(ObjectType::Tree, encode_tree({}));
    return store.write(ObjectType::Commit, encode_commit({root, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
NodeImage operation_file(const Repository& repo, std::string_view content, std::uint32_t permissions = 0644) {
    return {NodeKind::File, ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes(content)), permissions};
}
void operation_checksum(Bytes& bytes) {
    const auto digest = sha256(std::span<const std::uint8_t>(bytes).first(bytes.size() - 32));
    std::copy(digest.bytes().begin(), digest.bytes().end(), bytes.end() - 32);
}
void operation_number(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
    operation_checksum(bytes);
}
std::map<std::string, Bytes> operation_disk(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.root()))
        if (std::filesystem::is_regular_file(entry.symlink_status()))
            result.emplace(entry.path().lexically_relative(repo.root()).generic_string(), read_file(entry.path()));
    return result;
}
void operation_same_plan(const TransactionPlan& left, const TransactionPlan& right) {
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
void operation_equal(const OperationState& left, const OperationState& right) {
    REQUIRE(left.kind == right.kind);
    REQUIRE(left.phase == right.phase);
    REQUIRE(left.original_head == right.original_head);
    REQUIRE(left.current_head == right.current_head);
    REQUIRE(left.original_head_bytes == right.original_head_bytes);
    REQUIRE(left.original_index_blob == right.original_index_blob);
    REQUIRE(left.source_commit == right.source_commit);
    REQUIRE(left.message == right.message);
    TransactionPlan a, b;
    a.work = left.work;
    b.work = right.work;
    operation_same_plan(a, b);
}
}
TEST_CASE("Operation codec preserves exact HEAD bytes optional Index and actual two-sided node images canonically", "[operation]") {
    const auto head = sha256(to_bytes("head"));
    const auto source = sha256(to_bytes("source"));
    const auto before = sha256(to_bytes("before"));
    const auto after = sha256(to_bytes("after"));
    auto state = operation_state(head, source);
    state.message = "合并\n\nbody" + std::string(1, char(128));
    state.original_index_blob = sha256(to_bytes("raw Index"));
    state.work = {
        {"d", {NodeKind::File, before, 0640}, {NodeKind::Directory, std::nullopt, 0750}},
        {"d/new", {}, {NodeKind::File, after, 06755}},
        {"empty", {NodeKind::Directory, std::nullopt, 0500}, {NodeKind::Directory, std::nullopt, 0500}},
        {"gone", {NodeKind::File, before, 0600}, {}},
        {"stay", {NodeKind::File, after, 0644}, {NodeKind::File, after, 0644}}
    };
    SECTION("symbolic HEAD and complete node images") {}
    SECTION("detached uppercase raw HEAD stays exact") {
        state.original_head.symbolic.reset();
        state.current_head = state.original_head;
        auto raw = head.hex() + "\n";
        std::transform(raw.begin(), raw.end(), raw.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
        state.original_head_bytes = to_bytes(raw);
    }
    SECTION("missing original Index and zero Work records are distinct valid states") {
        state.original_index_blob.reset();
        state.work.clear();
    }
    SECTION("replay kinds change only the existing kind byte and checksum without changing Merge bytes") {
        const auto merge_bytes = encode_operation(state);
        REQUIRE(merge_bytes[8] == 1);
        for (const auto kind : {OperationKind::CherryPick, OperationKind::Revert}) {
            state.kind = kind;
            auto expected = merge_bytes;
            expected[8] = static_cast<std::uint8_t>(kind);
            operation_checksum(expected);
            REQUIRE(encode_operation(state) == expected);
            operation_equal(decode_operation(expected), state);
        }
        state.kind = OperationKind::Merge;
        REQUIRE(encode_operation(state) == merge_bytes);
    }
    const auto bytes = encode_operation(state);
    REQUIRE(to_string(std::span<const std::uint8_t>(bytes).first(4)) == "MGOP");
    operation_equal(decode_operation(bytes), state);
    REQUIRE(encode_operation(decode_operation(bytes)) == bytes);
}
TEST_CASE("Operation codec rejects authenticated malformed fields unsafe topology and incremental bounds", "[operation]") {
    auto state = operation_state(sha256(to_bytes("head")), sha256(to_bytes("source")), false);
    state.message = "m";
    SECTION("checksum truncation versions enums and presence") {
        const auto original = encode_operation(state);
        for (const auto length : {0U, 10U, 41U, 42U, 100U})
            REQUIRE_THROWS_AS(decode_operation(std::span<const std::uint8_t>(original).first(length)), Error);
        auto broken = original;
        broken.back() ^= 1;
        REQUIRE_THROWS_AS(decode_operation(broken), Error);
        for (const auto offset : {0U, 7U, 8U, 9U, 10U, 11U, 44U, 45U, 147U}) {
            broken = original;
            broken[offset] = 255;
            operation_checksum(broken);
            REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
        broken = original;
        broken.insert(broken.end() - 32, 0);
        operation_checksum(broken);
        REQUIRE_THROWS_AS(decode_operation(broken), Error);
    }
    SECTION("kind is a closed set and authenticated unknown values remain invalid") {
        const auto original = encode_operation(state);
        for (const auto value : {0U, 4U, 255U}) {
            state.kind = static_cast<OperationKind>(value);
            REQUIRE_THROWS_AS(encode_operation(state), Error);
            auto broken = original;
            broken[8] = static_cast<std::uint8_t>(value);
            operation_checksum(broken);
            REQUIRE_THROWS_AS(decode_operation(broken), Error);
        }
    }
    SECTION("length and count bounds reject before count-driven allocation") {
        // Minimal detached state: raw HEAD length at 78, message length at 180,
        // Work count at 185. Authenticated absurd lengths must not reserve them.
        auto broken = encode_operation(state);
        operation_number(broken, 78, 4103);
        REQUIRE_THROWS_AS(decode_operation(broken), Error);
        broken = encode_operation(state);
        operation_number(broken, 180, 0xffffffffU);
        REQUIRE_THROWS_AS(decode_operation(broken), Error);
        broken = encode_operation(state);
        operation_number(broken, 185, 100001);
        REQUIRE_THROWS_AS(decode_operation(broken), Error);
    }
    SECTION("HEAD kind and logical original current relation are strict") {
        state.current_head.id = state.source_commit;
        REQUIRE_THROWS_AS(encode_operation(state), Error);
        state.current_head = state.original_head;
        state.original_head_bytes = to_bytes(state.source_commit.hex() + "\n");
        REQUIRE_THROWS_AS(encode_operation(state), Error);
        state.original_head_bytes = to_bytes(state.original_head.id->hex() + "\n");
        state.original_head.id.reset();
        state.current_head = state.original_head;
        REQUIRE_THROWS_AS(encode_operation(state), Error);
        state = operation_state(ObjectId{}, ObjectId{});
        state.original_head.symbolic = "refs/tags/main";
        state.current_head = state.original_head;
        REQUIRE_THROWS_AS(encode_operation(state), Error);
    }
    SECTION("message permissions path order depth and unsafe paths") {
        for (const auto& message : std::vector<std::string>{"", " \t\n", std::string("x\0y", 3)}) {
            state.message = message;
            REQUIRE_THROWS_AS(encode_operation(state), Error);
        }
        state.message = "m";
        for (const auto& image : std::vector<NodeImage>{{NodeKind::Absent, std::nullopt, 0644},
                {NodeKind::Absent, ObjectId{}, 0}, {NodeKind::File, std::nullopt, 0644},
                {NodeKind::File, ObjectId{}, 0200}, {NodeKind::Directory, ObjectId{}, 0755},
                {NodeKind::Directory, std::nullopt, 0400}, {NodeKind::Directory, std::nullopt, 010000},
                {static_cast<NodeKind>(9), std::nullopt, 0}}) {
            state.work = {{"f", {}, image}};
            REQUIRE_THROWS_AS(encode_operation(state), Error);
        }
        std::string deep = "a";
        for (unsigned i = 1; i < 129; ++i) deep += "/a";
        for (const auto& path : std::vector<std::string>{"../escape", ".minigit/x", std::string(4097, 'x'), deep}) {
            state.work = {{path, {}, {}}};
            REQUIRE_THROWS_AS(encode_operation(state), Error);
        }
        state.work = {{"z", {}, {}}, {"a", {}, {}}};
        REQUIRE_THROWS_AS(encode_operation(state), Error);
        state.work = {{"a", {}, {}}, {"a", {}, {}}};
        REQUIRE_THROWS_AS(encode_operation(state), Error);
    }
    SECTION("both sides enforce nonadjacent ancestor topology and descendant directory permissions") {
        const NodeImage file{NodeKind::File, ObjectId{}, 0644};
        for (const bool after : {false, true}) {
            state.work = {{"p", {}, {}}, {"p-other", {}, {}}, {"p/x", {}, {}}};
            (after ? state.work[0].after : state.work[0].before) = file;
            (after ? state.work[2].after : state.work[2].before) = file;
            REQUIRE_THROWS_AS(encode_operation(state), Error);
            (after ? state.work[0].after : state.work[0].before) = {NodeKind::Directory, std::nullopt, 0500};
            REQUIRE_THROWS_AS(encode_operation(state), Error);
            (after ? state.work[0].after : state.work[0].before).permissions = 0700;
            REQUIRE_NOTHROW(encode_operation(state));
        }
    }
    SECTION("encoded bytes include framing before allocating the output") {
        state.message.assign(max_file_size, 'm');
        REQUIRE_THROWS_AS(encode_operation(state), Error);
    }
}
TEST_CASE("Operation reads and roots validate every typed backup stage without inspecting live HEAD Work or historical trees", "[operation]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path() / "repo");
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto head = operation_commit(repo, "head");
    const auto source = operation_commit(repo, "historical missing tree", ObjectId{});
    auto state = operation_state(head, source);
    const auto base = operation_file(repo, "base");
    const auto ours = operation_file(repo, "ours");
    const auto theirs = operation_file(repo, "theirs");
    const auto empty = store.write(ObjectType::Blob, {});
    const auto raw_index = encode_index({{"conflict", "100644", *base.blob, 1}, {"conflict", "100755", *ours.blob, 2},
        {"conflict", "100644", *theirs.blob, 3}, {"normal", "100644", empty, 0}});
    state.original_index_blob = store.write(ObjectType::Blob, raw_index);
    state.work = {{"link/file", base, ours}};
    atomic_write(repo.metadata() / "HEAD", to_bytes("corrupt live HEAD\n"));
    atomic_write(repo.metadata() / "index", to_bytes("corrupt live Index"));
    atomic_write(temp.path() / "outside", to_bytes("outside"));
    std::filesystem::create_symlink(temp.path() / "outside", repo.root() / "link");
    SECTION("all stage roots and historical partial Commit roots survive read while short pending") {
        REQUIRE_FALSE(read_operation(repo));
        REQUIRE_NOTHROW(require_no_operation(repo));
        atomic_write(repo.metadata() / "operation", encode_operation(state));
        atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        const auto before = operation_disk(repo);
        const auto roots = state_roots(repo, state);
        const std::set<ObjectId> expected{head, source, *base.blob, *ours.blob, *theirs.blob, empty, *state.original_index_blob};
        REQUIRE(roots == expected);
        const auto actual = read_operation(repo);
        REQUIRE(actual);
        operation_equal(*actual, state);
        REQUIRE_THROWS_AS(require_no_operation(repo), Error);
        REQUIRE(operation_disk(repo) == before);
    }
    SECTION("a missing original Index differs from valid empty Index and empty backup bytes") {
        state.work.clear();
        state.original_index_blob.reset();
        REQUIRE(state_roots(repo, state) == std::set<ObjectId>{head, source});
        state.original_index_blob = store.write(ObjectType::Blob, encode_index({}));
        REQUIRE(state_roots(repo, state).contains(*state.original_index_blob));
        state.original_index_blob = empty;
        REQUIRE_THROWS_AS(state_roots(repo, state), Error);
    }
    SECTION("missing wrong typed roots and schema-invalid raw Index are errors") {
        auto bad = state;
        for (const auto id : {ObjectId{}, *base.blob}) {
            bad.source_commit = id;
            REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        }
        bad = state;
        bad.work[0].before.blob = head; // Same ID already checked as Commit.
        REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        bad = state;
        bad.work[0].after.blob = ObjectId{};
        REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        bad = state;
        bad.original_index_blob = head;
        REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        for (const auto& bytes : {to_bytes("bad Index"), encode_index({{"conflict", "100644", head, 3}}),
                encode_index({{"conflict", "100644", ObjectId{}, 2}})}) {
            bad.original_index_blob = store.write(ObjectType::Blob, bytes);
            REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
        }
        auto excessive_count = encode_index({});
        operation_number(excessive_count, 8, 300001);
        bad.original_index_blob = store.write(ObjectType::Blob, excessive_count);
        REQUIRE_THROWS_AS(state_roots(repo, bad), Error);
    }
    SECTION("existing empty corrupt directory and symlink operation nodes fail closed") {
        const auto path = repo.metadata() / "operation";
        for (const auto& bytes : {Bytes{}, to_bytes("corrupt")}) {
            atomic_write(path, bytes);
            REQUIRE_THROWS_AS(read_operation(repo), Error);
            REQUIRE_THROWS_AS(require_no_operation(repo), Error);
            std::filesystem::remove(path);
        }
        std::filesystem::create_directory(path);
        REQUIRE_THROWS_AS(read_operation(repo), Error);
        REQUIRE_THROWS_AS(require_no_operation(repo), Error);
        std::filesystem::remove(path);
        std::filesystem::create_symlink(temp.path() / "absent", path);
        REQUIRE_THROWS_AS(read_operation(repo), Error);
        REQUIRE_THROWS_AS(require_no_operation(repo), Error);
    }
}
TEST_CASE("Operation update plans bind expected canonical State preserve raw CAS and strong exception guarantees", "[operation]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    const auto head = operation_commit(repo, "head");
    const auto source = operation_commit(repo, "source");
    auto state = operation_state(head, source, false);
    auto raw_head = head.hex() + "\n";
    std::transform(raw_head.begin(), raw_head.end(), raw_head.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    state.original_head_bytes = to_bytes(raw_head);
    const auto bytes = encode_operation(state);
    TransactionPlan plan;
    plan.work = {{"unrelated", {}, {}}};
    SECTION("create delete identical and absent noops preserve exact metadata images") {
        const auto before = operation_disk(repo);
        plan_operation_update(repo, plan, std::nullopt, state);
        REQUIRE(plan.metadata.size() == 1);
        REQUIRE(plan.metadata[0].path == "operation");
        REQUIRE_FALSE(plan.metadata[0].before);
        REQUIRE(plan.metadata[0].after == bytes);
        REQUIRE(operation_disk(repo) == before);
        const auto saved = plan;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, std::nullopt, state), Error);
        operation_same_plan(plan, saved);
        atomic_write(repo.metadata() / "operation", bytes);
        TransactionPlan noop;
        plan_operation_update(repo, noop, state, state);
        REQUIRE(noop.metadata.empty());
        TransactionPlan deletion;
        const auto existing = operation_disk(repo);
        plan_operation_update(repo, deletion, state, std::nullopt);
        REQUIRE(deletion.metadata[0].before == bytes);
        REQUIRE_FALSE(deletion.metadata[0].after);
        REQUIRE(operation_disk(repo) == existing);
        std::filesystem::remove(repo.metadata() / "operation");
        plan_operation_update(repo, noop, std::nullopt, std::nullopt);
        REQUIRE(noop.metadata.empty());
    }
    SECTION("expectation mismatch replacement and invalid desired never modify caller plan") {
        atomic_write(repo.metadata() / "operation", bytes);
        auto different = state;
        different.message = "different";
        const auto before = operation_disk(repo);
        const auto saved = plan;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, std::nullopt, state), Error);
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, different, std::nullopt), Error);
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, different), Error);
        different.source_commit = ObjectId{};
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, different), Error);
        operation_same_plan(plan, saved);
        REQUIRE(operation_disk(repo) == before);
        atomic_write(repo.metadata() / "operation", to_bytes("corrupt"));
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, state, std::nullopt), Error);
        operation_same_plan(plan, saved);
    }
    SECTION("a metadata budget valid in isolation can fail after adding the State") {
        plan.metadata.push_back({"index", Bytes(max_file_size, 0), std::nullopt});
        const auto before = operation_disk(repo);
        REQUIRE_NOTHROW(validate_transaction_budget(plan));
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, std::nullopt, state), Error);
        REQUIRE(plan.metadata.size() == 1);
        REQUIRE(plan.metadata[0].path == "index");
        REQUIRE(plan.metadata[0].before->size() == max_file_size);
        REQUIRE(std::all_of(plan.metadata[0].before->begin(), plan.metadata[0].before->end(), [](auto byte) { return byte == 0; }));
        REQUIRE_FALSE(plan.metadata[0].after);
        REQUIRE(plan.work.size() == 1);
        REQUIRE(plan.work[0].path == "unrelated");
        REQUIRE(operation_disk(repo) == before);
    }
    SECTION("noops still reject preplanned operation and pending transaction") {
        plan.metadata.push_back({"operation", std::nullopt, std::nullopt});
        auto saved = plan;
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, std::nullopt, std::nullopt), Error);
        operation_same_plan(plan, saved);
        plan.metadata.clear();
        saved = plan;
        atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        REQUIRE_THROWS_AS(plan_operation_update(repo, plan, std::nullopt, std::nullopt), Error);
        operation_same_plan(plan, saved);
    }
}
TEST_CASE("Operation transactions reject invalid semantic roots and recover exact State Index and Work at two durable boundaries", "[operation]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto head = operation_commit(repo, "head");
    const auto source = operation_commit(repo, "source");
    update_ref(repo, "refs/heads/main", std::nullopt, head);
    auto state = operation_state(head, source);
    const auto original_head = read_file(repo.metadata() / "HEAD");
    const auto old = operation_file(repo, "old dirty bytes", 0640);
    const auto marker = operation_file(repo, "conflict marker", 0644);
    const auto original_index = encode_index({{"f", "100644", *old.blob}});
    const auto conflict_index = encode_index({{"f", "100644", *old.blob, 1}, {"f", "100644", *marker.blob, 2}});
    state.original_index_blob = store.write(ObjectType::Blob, original_index);
    state.work = {{"f", old, marker}};
    SECTION("Prepared create retains after-State roots and restores original images") {
        atomic_write(repo.metadata() / "index", original_index);
        atomic_write_with_permissions(repo.root() / "f", to_bytes("old dirty bytes"), 0640);
        TransactionPlan plan;
        plan_index_update(repo, plan, decode_index(conflict_index));
        plan_worktree_update(repo, plan, {{"f", {FileState{"100644", *marker.blob}, 0644}}}, {"f"});
        state.work = plan.work;
        plan_operation_update(repo, plan, std::nullopt, state);
        bool reached = false;
        REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) {
            if (label == "journal.prepared") { reached = true; throw Error("interrupt Prepared"); }
        }), Error);
        REQUIRE(reached);
        const auto roots = transaction_roots(repo);
        for (const auto& id : state_roots(repo, state)) REQUIRE(roots.contains(id));
        recover_transaction(repo);
        recover_transaction(repo);
        REQUIRE_FALSE(read_operation(repo));
        REQUIRE(read_file(repo.metadata() / "index") == original_index);
        REQUIRE(to_string(read_file(repo.root() / "f")) == "old dirty bytes");
        REQUIRE(read_file_data(repo.root() / "f").permissions == 0640);
        REQUIRE(transaction_roots(repo).empty());
    }
    SECTION("Committed delete retains before-State roots and restores final images without duplicate work") {
        const auto operation_bytes = encode_operation(state);
        atomic_write(repo.metadata() / "operation", operation_bytes);
        atomic_write(repo.metadata() / "index", conflict_index);
        atomic_write_with_permissions(repo.root() / "f", to_bytes("conflict marker"), 0644);
        TransactionPlan plan;
        plan_index_update(repo, plan, decode_index(original_index));
        plan_worktree_update(repo, plan, {{"f", {FileState{"100644", *old.blob}, 0640}}}, {"f"});
        plan_operation_update(repo, plan, state, std::nullopt);
        bool reached = false;
        REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) {
            if (label == "journal.committed") { reached = true; throw Error("interrupt Committed"); }
        }), Error);
        REQUIRE(reached);
        const auto roots = transaction_roots(repo);
        for (const auto& id : state_roots(repo, state)) REQUIRE(roots.contains(id));
        REQUIRE_FALSE(read_operation(repo));
        recover_transaction(repo);
        recover_transaction(repo);
        REQUIRE_FALSE(read_operation(repo));
        REQUIRE(read_file(repo.metadata() / "index") == original_index);
        REQUIRE(to_string(read_file(repo.root() / "f")) == "old dirty bytes");
        REQUIRE(read_file_data(repo.root() / "f").permissions == 0640);
        REQUIRE(transaction_roots(repo).empty());
    }
    SECTION("missing noncommit source and nonblob Work or Index stages fail before publication") {
        const auto bad_index = store.write(ObjectType::Blob, encode_index({{"f", "100644", head, 3}}));
        for (unsigned variant = 0; variant < 5; ++variant) {
            auto bad = state;
            if (variant == 0) bad.source_commit = ObjectId{};
            if (variant == 1) bad.source_commit = *old.blob;
            if (variant == 2) bad.work[0].after.blob = source;
            if (variant == 3) bad.original_index_blob = source;
            if (variant == 4) bad.original_index_blob = bad_index;
            const auto before = operation_disk(repo);
            TransactionPlan plan;
            plan.metadata.push_back({"operation", std::nullopt, encode_operation(bad)});
            REQUIRE_THROWS_AS(execute_transaction(repo, plan), Error);
            REQUIRE(operation_disk(repo) == before);
            REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "operation"));
            REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
            REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction-staging"));
        }
    }
    REQUIRE(read_file(repo.metadata() / "HEAD") == original_head);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/HEAD"));
}
