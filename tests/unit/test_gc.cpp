#include <catch2/catch_test_macros.hpp>
#include "minigit/gc.hpp"
#include "minigit/error.hpp"
#include "minigit/index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "minigit/stash_stack.hpp"
#include "temp_directory.hpp"
#include <chrono>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>
using namespace minigit;
namespace {
struct GcFixture {
    test::TempDirectory directory;
    Repository repo = Repository::init(directory.path());
    GcFixture() { RepoLock lock(repo.metadata()); }
};
struct GcTip { ObjectId blob, tree, commit; };
GcTip gc_tip(const Repository& repo, std::string_view text, const std::vector<ObjectId>& parents = {}) {
    ObjectStore store(repo.metadata() / "objects");
    const auto blob = store.write(ObjectType::Blob, to_bytes(text));
    const auto tree = store.write(ObjectType::Tree, encode_tree({{"file", "100644", blob}}));
    const auto commit = store.write(ObjectType::Commit, encode_commit({tree, parents,
        "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(text)}));
    return {blob, tree, commit};
}
void gc_write(const Repository& repo, std::string_view path, const Bytes& bytes) {
    const auto absolute = repo.metadata() / path;
    detail::ensure_directory(absolute.parent_path()); atomic_write(absolute, bytes);
}
void gc_ref(const Repository& repo, std::string_view name, ObjectId id) {
    gc_write(repo, name, to_bytes(id.hex() + "\n"));
}
StashEntry gc_stash(const Repository& repo, std::string_view label) {
    const auto head = gc_tip(repo, std::string(label) + " H").commit;
    const auto index = gc_tip(repo, std::string(label) + " I", {head}).commit;
    const auto extra = gc_tip(repo, std::string(label) + " U").commit;
    return {gc_tip(repo, std::string(label) + " W", {head, index, extra}).commit, head, index, extra};
}
std::set<ObjectId> gc_objects(const Repository& repo) {
    std::set<ObjectId> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects"))
        if (std::filesystem::is_regular_file(entry.symlink_status()))
            result.insert(ObjectId::from_hex(entry.path().parent_path().filename().string() + entry.path().filename().string()));
    return result;
}
using GcDisk = std::map<std::string, std::pair<std::filesystem::file_type, Bytes>>;
GcDisk gc_disk(const Repository& repo) {
    GcDisk result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto type = entry.symlink_status().type();
        result.emplace(entry.path().lexically_relative(repo.metadata()).generic_string(),
            std::pair{type, type == std::filesystem::file_type::regular ? read_file(entry.path()) : Bytes{}});
    }
    return result;
}
std::map<std::string, Bytes> gc_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto relative = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!relative.starts_with("objects/") && std::filesystem::is_regular_file(entry.symlink_status()))
            result.emplace(relative, read_file(entry.path()));
    }
    return result;
}
void gc_has(const Repository& repo, const GcTip& tip) {
    const auto objects = gc_objects(repo);
    REQUIRE(objects.contains(tip.blob)); REQUIRE(objects.contains(tip.tree)); REQUIRE(objects.contains(tip.commit));
}
ObjectId gc_bad_commit(const Repository& repo) {
    const auto payload = to_bytes("invalid commit");
    auto frame = to_bytes("commit " + std::to_string(payload.size())); frame.push_back(0);
    frame.insert(frame.end(), payload.begin(), payload.end()); const auto id = sha256(frame);
    uLongf size = ::compressBound(static_cast<uLong>(frame.size())); Bytes compressed(static_cast<std::size_t>(size));
    REQUIRE(::compress2(compressed.data(), &size, frame.data(), static_cast<uLong>(frame.size()), Z_DEFAULT_COMPRESSION) == Z_OK);
    compressed.resize(static_cast<std::size_t>(size)); const auto path = ObjectStore(repo.metadata() / "objects").object_path(id);
    detail::ensure_directory(path.parent_path()); atomic_write(path, compressed); return id;
}
OperationState gc_operation(ObjectId head, ObjectId source) {
    OperationState state;
    state.original_head = {"refs/heads/main", head}; state.current_head = state.original_head;
    state.original_head_bytes = to_bytes("ref: refs/heads/main\n"); state.source_commit = source; state.message = "merge";
    return state;
}
void gc_publish_operation(const Repository& repo, const OperationState& state) {
    TransactionPlan plan; plan_operation_update(repo, plan, std::nullopt, state); execute_transaction(repo, plan);
}
struct GcPermissions {
    std::filesystem::path path;
    std::filesystem::perms saved;
    explicit GcPermissions(std::filesystem::path value) : path(std::move(value)), saved(std::filesystem::status(path).permissions()) {}
    void restore() { std::filesystem::permissions(path, saved); }
    ~GcPermissions() { std::error_code ec; std::filesystem::permissions(path, saved, ec); }
};
}
TEST_CASE("Gc retains HEAD every ref all parents and nested Tree closure while pruning orphan data", "[gc]") {
    GcFixture fixture; const auto& repo = fixture.repo;
    GcTip left, right, tag; ObjectId orphan, merge, nested;
    {
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
        left = gc_tip(repo, "left"); right = gc_tip(repo, "right"); tag = gc_tip(repo, "tag only");
        nested = store.write(ObjectType::Tree, encode_tree({{"sub", "40000", left.tree}}));
        merge = store.write(ObjectType::Commit, encode_commit({nested, {left.commit, right.commit, left.commit},
            "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "merge"}));
        gc_ref(repo, "refs/heads/main", merge); gc_ref(repo, "refs/tags/only", tag.commit);
        orphan = store.write(ObjectType::Blob, to_bytes("unreferenced"));
    }
    SECTION("symbolic HEAD") {}
    SECTION("detached HEAD") { RepoLock lock(repo.metadata()); gc_ref(repo, "HEAD", merge); }
    const auto metadata = gc_metadata(repo); const auto before = gc_objects(repo);
    const auto fanout = ObjectStore(repo.metadata() / "objects").object_path(orphan).parent_path();
    const auto result = collect_garbage(repo, {true});
    REQUIRE(result.removed == 1); REQUIRE(result.retained == before.size() - 1);
    gc_has(repo, left); gc_has(repo, right); gc_has(repo, tag);
    REQUIRE(gc_objects(repo).contains(merge)); REQUIRE(gc_objects(repo).contains(nested));
    REQUIRE_FALSE(gc_objects(repo).contains(orphan)); REQUIRE(std::filesystem::is_directory(fanout));
    REQUIRE(gc_metadata(repo) == metadata); REQUIRE(collect_garbage(repo, {true}).removed == 0);
}
TEST_CASE("Gc roots every Index stage without reading Work or requiring an identity", "[gc]") {
    GcFixture fixture; const auto& repo = fixture.repo; std::set<ObjectId> expected; ObjectId orphan;
    {
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects"); Index index;
        for (std::uint8_t stage = 1; stage <= 3; ++stage) {
            const auto id = store.write(ObjectType::Blob, to_bytes("stage " + std::to_string(stage)));
            expected.insert(id); index.push_back({"conflict", stage == 3 ? "100755" : "100644", id, stage});
        }
        const auto added = store.write(ObjectType::Blob, to_bytes("staged addition")); expected.insert(added);
        index.push_back({"new", "100644", added}); save_index(repo, index);
        orphan = store.write(ObjectType::Blob, to_bytes("orphan"));
        REQUIRE(::mkfifo((repo.root() / "conflict").c_str(), 0600) == 0);
        std::filesystem::create_symlink(fixture.directory.path() / "outside", repo.root() / "new");
    }
    const auto metadata = gc_metadata(repo); const auto result = collect_garbage(repo, {true});
    REQUIRE(result.retained == expected.size()); REQUIRE(result.removed == 1); REQUIRE(gc_objects(repo) == expected);
    REQUIRE_FALSE(gc_objects(repo).contains(orphan)); REQUIRE(gc_metadata(repo) == metadata);
    REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(repo.root() / "new")));
    REQUIRE(std::filesystem::symlink_status(repo.root() / "conflict").type() == std::filesystem::file_type::fifo);
}
TEST_CASE("Gc keeps all reflog endpoints including deleted nested branches and HEAD history", "[gc]") {
    GcFixture fixture; const auto& repo = fixture.repo; GcTip old, next, detached; ObjectId orphan;
    {
        RepoLock lock(repo.metadata()); old = gc_tip(repo, "deleted old"); next = gc_tip(repo, "deleted new"); detached = gc_tip(repo, "old detached");
        gc_write(repo, "logs/refs/heads/deleted/nested", encode_reflog({{std::nullopt, old.commit, "create"},
            {old.commit, next.commit, "move"}, {next.commit, std::nullopt, "delete"}}));
        gc_write(repo, "logs/HEAD", encode_reflog({{detached.commit, std::nullopt, "unborn"}}));
        orphan = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("orphan"));
    }
    const auto metadata = gc_metadata(repo); const auto result = collect_garbage(repo, {true});
    REQUIRE(result.removed == 1); gc_has(repo, old); gc_has(repo, next); gc_has(repo, detached);
    REQUIRE_FALSE(gc_objects(repo).contains(orphan)); REQUIRE(gc_metadata(repo) == metadata);
}
TEST_CASE("Gc grace treats fresh objects as roots with closure and always protects future timestamps", "[gc]") {
    GcFixture fixture; const auto& repo = fixture.repo; GcTip chain; ObjectId orphan;
    {
        RepoLock lock(repo.metadata()); chain = gc_tip(repo, "fresh root with old children");
        orphan = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("old orphan"));
    }
    const ObjectStore store(repo.metadata() / "objects"); const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& id : gc_objects(repo)) std::filesystem::last_write_time(store.object_path(id), now - std::chrono::hours(72));
    SECTION("ordinary fresh Commit preserves old Tree and Blob") {
        std::filesystem::last_write_time(store.object_path(chain.commit), now - std::chrono::hours(1));
        const auto result = collect_garbage(repo); REQUIRE(result.retained == 3); REQUIRE(result.removed == 1); gc_has(repo, chain);
    }
    SECTION("prune now removes a fresh Commit and its old closure") {
        std::filesystem::last_write_time(store.object_path(chain.commit), now - std::chrono::hours(1));
        const auto result = collect_garbage(repo, {true}); REQUIRE(result.retained == 0); REQUIRE(result.removed == 4);
    }
    SECTION("future Commit remains a root even with prune now") {
        std::filesystem::last_write_time(store.object_path(chain.commit), now + std::chrono::hours(48));
        const auto result = collect_garbage(repo, {true}); REQUIRE(result.retained == 3); REQUIRE(result.removed == 1); gc_has(repo, chain);
    }
    REQUIRE_FALSE(gc_objects(repo).contains(orphan));
}
TEST_CASE("Gc retains every live Stash entry and its independent Index and untracked snapshots", "[gc]") {
    GcFixture fixture; const auto& repo = fixture.repo; StashEntry top, old; std::set<ObjectId> expected;
    {
        RepoLock lock(repo.metadata()); top = gc_stash(repo, "top"); old = gc_stash(repo, "non top");
        gc_write(repo, "stashstack", encode_stash_stack({top.work, old.work, top.work})); gc_ref(repo, "refs/stash", top.work);
        expected = gc_objects(repo); (void)ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("discard"));
    }
    const auto metadata = gc_metadata(repo); const auto result = collect_garbage(repo, {true});
    REQUIRE(result.removed == 1); REQUIRE(result.retained == expected.size()); REQUIRE(gc_objects(repo) == expected);
    ObjectStore store(repo.metadata() / "objects");
    for (const auto id : {old.work, old.head, old.index, *old.untracked}) {
        const auto commit = decode_commit(store.read(id).payload); REQUIRE(gc_objects(repo).contains(commit.tree));
        for (const auto& entry : decode_tree(store.read(commit.tree).payload)) REQUIRE(gc_objects(repo).contains(entry.id));
    }
    REQUIRE(gc_metadata(repo) == metadata);
}
TEST_CASE("Gc allows active operations and retains saved Work Index Stash and all Rebase sources", "[gc]") {
    GcFixture fixture; const auto& repo = fixture.repo; std::set<ObjectId> expected; ObjectId orphan;
    {
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
        const auto head = gc_tip(repo, "head"); const auto source = gc_tip(repo, "source"); gc_ref(repo, "refs/heads/main", head.commit);
        auto state = gc_operation(head.commit, source.commit);
        const auto before = store.write(ObjectType::Blob, to_bytes("work before"));
        const auto after = store.write(ObjectType::Blob, to_bytes("work after"));
        const auto stage = store.write(ObjectType::Blob, to_bytes("saved Index only"));
        state.original_index_blob = store.write(ObjectType::Blob, encode_index({{"saved", "100644", stage, 1},
            {"saved", "100644", before, 2}, {"saved", "100755", after, 3}}));
        state.work = {{"affected", {NodeKind::File, before, 0640}, {NodeKind::File, after, 0751}}};
        SECTION("Merge Conflicted mirrors retain both sides and all original stages") {}
        SECTION("Rebase Ready queue roots include sources absent from live refs and reflogs") {
            const auto first = gc_tip(repo, "queued independent first").commit;
            const auto onto = gc_tip(repo, "onto").commit;
            state.kind = OperationKind::Rebase; state.phase = OperationPhase::Ready; state.message = "rebase";
            state.current_head = {std::nullopt, onto}; gc_ref(repo, "HEAD", onto);
            state.source_commit = first; state.rebase = RebaseOperation{onto, {first, head.commit}, 0, to_bytes(head.commit.hex() + "\n")};
            SECTION("Ready retains the pending queue") {}
            SECTION("Conflicted retains the same pending queue") { state.phase = OperationPhase::Conflicted; }
            // Remove the unrelated ordinary source from this variant's expected roots.
            detail::remove_regular(store.object_path(source.commit)); detail::remove_regular(store.object_path(source.tree)); detail::remove_regular(store.object_path(source.blob));
        }
        SECTION("Stash context keeps every saved entry even with no live stack or stash ref") {
            const auto top = gc_stash(repo, "saved top"); const auto selected = gc_stash(repo, "saved selected");
            state.kind = OperationKind::StashApply; state.message = "stash:pop"; state.source_commit = selected.work;
            const auto stack_blob = store.write(ObjectType::Blob, encode_stash_stack({top.work, selected.work, top.work}));
            state.stash = StashOperation{true, 1, stack_blob, to_bytes(top.work.hex() + "\n")};
            detail::remove_regular(store.object_path(source.commit)); detail::remove_regular(store.object_path(source.tree)); detail::remove_regular(store.object_path(source.blob));
        }
        expected = gc_objects(repo); gc_publish_operation(repo, state);
        orphan = store.write(ObjectType::Blob, to_bytes("unreachable after operation"));
        REQUIRE(::mkfifo((repo.root() / "affected").c_str(), 0600) == 0);
    }
    const auto metadata = gc_metadata(repo); const auto result = collect_garbage(repo, {true});
    REQUIRE(result.retained == expected.size()); REQUIRE(gc_objects(repo) == expected); REQUIRE(result.removed >= 1);
    REQUIRE_FALSE(gc_objects(repo).contains(orphan)); REQUIRE(gc_metadata(repo) == metadata);
    REQUIRE(std::filesystem::symlink_status(repo.root() / "affected").type() == std::filesystem::file_type::fifo);
}
TEST_CASE("Gc refuses real pending transactions and retries a partial sweep without dangling object edges", "[gc]") {
    GcFixture fixture; const auto& repo = fixture.repo;
    SECTION("durable Prepared transaction blocks collection without deleting any root or orphan") {
        {
            RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
            const auto blob = store.write(ObjectType::Blob, to_bytes("Index after"));
            (void)store.write(ObjectType::Blob, to_bytes("orphan"));
            TransactionPlan plan; plan.metadata.push_back({"index", std::nullopt, encode_index({{"file", "100644", blob}})});
            bool reached = false;
            REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view point) {
                if (point == "journal.prepared") { reached = true; throw Error("stop Prepared"); }
            }), Error); REQUIRE(reached);
        }
        const auto before = gc_disk(repo); REQUIRE_THROWS_AS(collect_garbage(repo, {true}), Error); REQUIRE(gc_disk(repo) == before);
        { RepoLock lock(repo.metadata()); recover_transaction(repo); }
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
    }
    SECTION("a real unlink failure leaves remaining objects valid for the next complete preflight") {
        if (::getuid() == 0) SKIP("root bypasses the fanout permission failure fixture");
        GcTip chain; std::string payload;
        {
            RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects");
            // Choose deterministic payloads until the three objects have distinct
            // fanouts; each attempt's unused objects are removed while locked.
            for (unsigned attempt = 0; ; ++attempt) {
                payload = "partial sweep " + std::to_string(attempt); chain = gc_tip(repo, payload);
                const auto blob_dir = store.object_path(chain.blob).parent_path();
                const auto tree_dir = store.object_path(chain.tree).parent_path();
                const auto commit_dir = store.object_path(chain.commit).parent_path();
                if (blob_dir != tree_dir && blob_dir != commit_dir && tree_dir != commit_dir) break;
                detail::remove_regular(store.object_path(chain.commit)); detail::remove_regular(store.object_path(chain.tree)); detail::remove_regular(store.object_path(chain.blob));
                REQUIRE(attempt < 100);
            }
        }
        const ObjectStore store(repo.metadata() / "objects"); const auto directory = store.object_path(chain.blob).parent_path();
        GcPermissions permissions(directory); std::filesystem::permissions(directory, static_cast<std::filesystem::perms>(0500));
        REQUIRE_THROWS_AS(collect_garbage(repo, {true}), Error);
        REQUIRE_FALSE(std::filesystem::exists(store.object_path(chain.commit))); REQUIRE_FALSE(std::filesystem::exists(store.object_path(chain.tree)));
        REQUIRE(store.read(chain.blob).payload == to_bytes(payload));
        permissions.restore(); const auto result = collect_garbage(repo, {true}); REQUIRE(result.removed == 1); REQUIRE(result.retained == 0);
    }
}
TEST_CASE("Gc rejects corrupt orphans typed edges unsafe inventories and malformed roots before any deletion", "[gc]") {
    GcFixture fixture; const auto& repo = fixture.repo; ObjectId good;
    {
        RepoLock lock(repo.metadata()); ObjectStore store(repo.metadata() / "objects"); good = store.write(ObjectType::Blob, to_bytes("would be pruned"));
        SECTION("orphan compressed bytes and schema are checked") { (void)gc_bad_commit(repo); }
        SECTION("orphan zlib corruption") { atomic_write(store.object_path(good), to_bytes("not zlib")); }
        SECTION("orphan Commit missing its Tree") { (void)store.write(ObjectType::Commit, encode_commit({ObjectId{}, {}, "Test 0 +0000", "Test 0 +0000", "orphan"})); }
        SECTION("orphan Commit parent must be a Commit") {
            const auto tree = store.write(ObjectType::Tree, encode_tree({}));
            (void)store.write(ObjectType::Commit, encode_commit({tree, {good}, "Test 0 +0000", "Test 0 +0000", "bad parent"}));
        }
        SECTION("orphan directory Tree edge must target Tree") { (void)store.write(ObjectType::Tree, encode_tree({{"child", "40000", good}})); }
        SECTION("orphan file Tree edge must target Blob") {
            const auto target = gc_tip(repo, "wrong file role").commit;
            (void)store.write(ObjectType::Tree, encode_tree({{"child", "100644", target}}));
        }
        SECTION("unknown object fanout") { detail::ensure_directory(repo.metadata() / "objects/unknown"); }
        SECTION("uppercase fanout") { detail::ensure_directory(repo.metadata() / "objects/AB"); }
        SECTION("unknown file within canonical fanout") { atomic_write(store.object_path(good).parent_path() / "unexpected", to_bytes("unknown")); }
        SECTION("object symlink") {
            const auto target = store.object_path(good); std::filesystem::remove(target); std::filesystem::create_symlink(repo.metadata() / "HEAD", target);
        }
        SECTION("object FIFO") { const auto path = store.object_path(good); std::filesystem::remove(path); REQUIRE(::mkfifo(path.c_str(), 0600) == 0); }
        SECTION("object path cannot be a nested directory") { const auto path = store.object_path(good); std::filesystem::remove(path); detail::ensure_directory(path); }
        SECTION("Index roots must be typed even in conflict stages") { save_index(repo, {{"file", "100644", gc_tip(repo, "wrong Index type").commit, 2}}); }
        SECTION("malformed Index") { gc_write(repo, "index", to_bytes("bad Index")); }
        SECTION("ref root must be a Commit") { gc_ref(repo, "refs/tags/blob", good); }
        SECTION("HEAD cannot treat an existing directory as unborn") { detail::ensure_directory(repo.metadata() / "refs/heads/main"); }
        SECTION("HEAD cannot treat a File-blocked descendant as unborn") {
            gc_ref(repo, "refs/heads/parent", gc_tip(repo, "parent").commit); gc_write(repo, "HEAD", to_bytes("ref: refs/heads/parent/child\n"));
        }
        SECTION("reflog endpoint must be a Commit") { gc_write(repo, "logs/HEAD", encode_reflog({{good, std::nullopt, "bad endpoint"}})); }
        SECTION("corrupt deleted branch log") { gc_write(repo, "logs/refs/heads/deleted", to_bytes("bad log")); }
        SECTION("unsupported log namespace") { gc_write(repo, "logs/refs/tags/tag", encode_reflog({})); }
        SECTION("stash stack and top ref must agree") { gc_write(repo, "stashstack", encode_stash_stack({gc_stash(repo, "missing ref").work})); }
        SECTION("empty stash cannot treat a Directory ref as missing") { detail::ensure_directory(repo.metadata() / "refs/stash"); }
        SECTION("empty stash cannot treat a Directory ref namespace as missing") {
            gc_ref(repo, "refs/stash/child", gc_tip(repo, "stash child").commit);
        }
        SECTION("non-top Stash shape must be valid") {
            const auto valid = gc_stash(repo, "top"); const auto bad = gc_tip(repo, "not a Stash").commit;
            gc_write(repo, "stashstack", encode_stash_stack({valid.work, bad})); gc_ref(repo, "refs/stash", valid.work);
        }
        SECTION("active operation must decode and validate before sweeping") { gc_write(repo, "operation", to_bytes("bad operation")); }
        SECTION("root metadata symlink") { std::filesystem::create_symlink(repo.metadata() / "HEAD", repo.metadata() / "index"); }
        SECTION("root metadata FIFO") { REQUIRE(::mkfifo((repo.metadata() / "stashstack").c_str(), 0600) == 0); }
    }
    const auto before = gc_disk(repo); REQUIRE_THROWS_AS(collect_garbage(repo, {true}), Error); REQUIRE(gc_disk(repo) == before);
}
