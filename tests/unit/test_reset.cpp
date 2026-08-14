#include <catch2/catch_test_macros.hpp>
#include "minigit/reset.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/worktree.hpp"
#include "minigit/worktree_update.hpp"
#include "temp_directory.hpp"
#include "legacy_index.hpp"
#include <map>
#include <sys/stat.h>
#include <utility>
using namespace minigit;
namespace {
using ResetFiles = std::map<std::string, std::pair<std::string, std::string>>;
ObjectId reset_commit(const Repository& repo, const ResetFiles& files) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Snapshot snapshot;
    for (const auto& [path, file] : files)
        snapshot.emplace(path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    return store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, snapshot), {},
        "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "fixture"}));
}
void reset_file(const Repository& repo, std::string_view path, std::string_view bytes, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(bytes), permissions);
}
Index reset_index_for(const Repository& repo, const ObjectId& id) {
    Index index;
    for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, id).tree))
        index.push_back({path, state.mode, state.id});
    return index;
}
void reset_install(const Repository& repo, const ObjectId& id) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto index = reset_index_for(repo, id);
    for (const auto& entry : index)
        reset_file(repo, entry.path, to_string(store.read(entry.id).payload), entry.mode == "100755" ? 0755U : 0644U);
    save_index(repo, index);
    update_ref(repo, "refs/heads/main", std::nullopt, id);
    const auto log = encode_reflog({{std::nullopt, id, "fixture"}});
    detail::ensure_directory(repo.metadata() / "logs/refs/heads");
    atomic_write(repo.metadata() / "logs/HEAD", log);
    atomic_write(repo.metadata() / "logs/refs/heads/main", log);
    atomic_write(repo.metadata() / "identity", to_bytes("name=Test\nemail=a@b\n"));
}
std::map<std::string, Bytes> reset_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && entry.is_regular_file()) result.emplace(path, read_file(entry.path()));
    }
    return result;
}
std::size_t reset_objects(const Repository& repo) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects"))
        if (entry.is_regular_file()) ++count;
    return count;
}
}
TEST_CASE("Reset soft mixed and hard publish distinct HEAD Index work effects with one attached branch", "[reset]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = reset_commit(repo, {{"file", {"A", "100644"}}});
    const auto d = reset_commit(repo, {{"file", {"D", "100755"}}});
    reset_install(repo, a);
    reset_file(repo, "file", "B"); add_paths(repo, {repo.root() / "file"});
    reset_file(repo, "file", "C", 0640);
    const auto index_bytes = read_file(repo.metadata() / "index");
    const auto head_bytes = read_file(repo.metadata() / "HEAD");
    const auto identity = read_file(repo.metadata() / "identity");
    ResetMode mode = ResetMode::Mixed;
    std::string reason = "reset:mixed";
    SECTION("soft keeps staged and working edits") { mode = ResetMode::Soft; reason = "reset:soft"; }
    SECTION("mixed replaces Index and preserves working edits") {}
    SECTION("hard replaces Index and discards tracked edits and permissions") { mode = ResetMode::Hard; reason = "reset:hard"; }
    reset_repository(repo, {mode, d.hex()});
    REQUIRE(read_head(repo) == Head{"refs/heads/main", d});
    REQUIRE(read_file(repo.metadata() / "HEAD") == head_bytes);
    REQUIRE(read_file(repo.metadata() / "identity") == identity);
    if (mode == ResetMode::Soft) REQUIRE(read_file(repo.metadata() / "index") == index_bytes);
    else REQUIRE(load_index(repo) == reset_index_for(repo, d));
    REQUIRE(to_string(read_file(repo.root() / "file")) == (mode == ResetMode::Hard ? "D" : "C"));
    REQUIRE(read_worktree_file(repo, "file")->data.permissions == (mode == ResetMode::Hard ? 0755U : 0640U));
    RepoLock lock(repo.metadata());
    REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{a, d, reason});
    REQUIRE(read_reflog(repo, "refs/heads/main").back() == ReflogEntry{a, d, reason});
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
TEST_CASE("Reset preserves detached HEAD and same ID still resets Index or work without new logs", "[reset]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = reset_commit(repo, {{"file", {"A", "100644"}}});
    const auto d = reset_commit(repo, {{"file", {"D", "100644"}}});
    reset_install(repo, a);
    SECTION("detached HEAD moves without changing the original branch or its log") {
        { RepoLock lock(repo.metadata()); set_head(repo, a.hex()); }
        const auto branch = read_file(repo.metadata() / "refs/heads/main");
        const auto log = read_file(repo.metadata() / "logs/refs/heads/main");
        reset_repository(repo, {ResetMode::Mixed, d.hex()});
        REQUIRE(read_head(repo) == Head{std::nullopt, d});
        REQUIRE(read_file(repo.metadata() / "refs/heads/main") == branch);
        REQUIRE(read_file(repo.metadata() / "logs/refs/heads/main") == log);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "A");
        RepoLock lock(repo.metadata());
        REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{a, d, "reset:mixed"});
    }
    SECTION("same ID mixed and hard do not shortcut resetting changed Index and work") {
        reset_file(repo, "file", "B"); add_paths(repo, {repo.root() / "file"}); reset_file(repo, "file", "C");
        const auto head_log = read_file(repo.metadata() / "logs/HEAD");
        const auto branch_log = read_file(repo.metadata() / "logs/refs/heads/main");
        reset_repository(repo);
        REQUIRE(load_index(repo) == reset_index_for(repo, a));
        REQUIRE(to_string(read_file(repo.root() / "file")) == "C");
        reset_repository(repo, {ResetMode::Hard, a.hex()});
        REQUIRE(to_string(read_file(repo.root() / "file")) == "A");
        REQUIRE(read_file(repo.metadata() / "logs/HEAD") == head_log);
        REQUIRE(read_file(repo.metadata() / "logs/refs/heads/main") == branch_log);
        const auto before = reset_metadata(repo);
        const auto objects = reset_objects(repo);
        for (const auto mode : {ResetMode::Soft, ResetMode::Mixed, ResetMode::Hard}) reset_repository(repo, {mode, a.hex()});
        REQUIRE(reset_metadata(repo) == before);
        REQUIRE(reset_objects(repo) == objects);
    }
}
TEST_CASE("Reset default unborn targets stay unborn while explicit history births the current branch", "[reset]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto d = reset_commit(repo, {{"file", {"historical", "100644"}}});
    reset_file(repo, "file", "staged"); add_paths(repo, {repo.root() / "file"}); reset_file(repo, "file", "dirty");
    reset_file(repo, "unknown", "private");
    const auto staged = read_file(repo.metadata() / "index");
    const auto head_bytes = read_file(repo.metadata() / "HEAD");
    SECTION("soft preserves staged and dirty bytes") {
        reset_repository(repo, {ResetMode::Soft, std::nullopt});
        REQUIRE(read_file(repo.metadata() / "index") == staged);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
    }
    SECTION("mixed clears staged entries and preserves dirty work") {
        reset_repository(repo);
        REQUIRE(load_index(repo).empty());
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
    }
    SECTION("hard clears staged entries and deletes tracked dirty work") {
        reset_repository(repo, {ResetMode::Hard, std::nullopt});
        REQUIRE(load_index(repo).empty());
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
    }
    SECTION("explicit HEAD rejects instead of treating it as empty") {
        const auto before = reset_metadata(repo);
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Hard, "HEAD"}), Error);
        REQUIRE(reset_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
    }
    SECTION("explicit history can create the ref without changing the symbolic branch") {
        reset_repository(repo, {ResetMode::Mixed, d.hex()});
        REQUIRE(read_head(repo) == Head{"refs/heads/main", d});
        REQUIRE(load_index(repo) == reset_index_for(repo, d));
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
        RepoLock lock(repo.metadata());
        REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{std::nullopt, d, "reset:mixed"});
    }
    SECTION("absent empty Index remains absent in all default modes") {
        std::filesystem::remove(repo.metadata() / "index"); std::filesystem::remove(repo.root() / "file");
        const auto before = reset_metadata(repo);
        for (const auto mode : {ResetMode::Soft, ResetMode::Mixed, ResetMode::Hard}) reset_repository(repo, {mode, std::nullopt});
        REQUIRE(reset_metadata(repo) == before);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "index"));
    }
    REQUIRE(read_file(repo.metadata() / "HEAD") == head_bytes);
    REQUIRE(to_string(read_file(repo.root() / "unknown")) == "private");
    if (!read_head(repo).id) {
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/HEAD"));
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "refs/heads/main"));
    }
}
TEST_CASE("Reset validates all Index stages and mixed hard clear valid directory file conflicts", "[reset]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = reset_commit(repo, {{"file", {"A", "100644"}}}); reset_install(repo, a);
    const auto blob = load_index(repo).front().id;
    const Index conflicts{{"file", "100644", blob, 1}, {"file", "100755", blob, 2},
                          {"file/child", "100644", blob, 3}};
    { RepoLock lock(repo.metadata()); save_index(repo, conflicts); }
    reset_file(repo, "file", "dirty");
    SECTION("soft rejects conflicts even when HEAD ID is unchanged") {
        const auto before = reset_metadata(repo);
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Soft, a.hex()}), Error);
        REQUIRE(reset_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
    }
    SECTION("mixed clears all stages without reading work") {
        reset_repository(repo);
        REQUIRE(load_index(repo) == reset_index_for(repo, a));
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
    }
    SECTION("hard handles the conflict paths hidden under the actual file") {
        reset_repository(repo, {ResetMode::Hard, std::nullopt});
        REQUIRE(load_index(repo) == reset_index_for(repo, a));
        REQUIRE(to_string(read_file(repo.root() / "file")) == "A");
    }
    SECTION("wrong type or missing objects in any stage reject all modes") {
        for (const auto invalid : {a, ObjectId{}}) {
            { RepoLock lock(repo.metadata()); save_index(repo, {{"file", "100644", blob}, {"unselected", "100644", invalid, 3}}); }
            const auto before = reset_metadata(repo);
            for (const auto mode : {ResetMode::Soft, ResetMode::Mixed, ResetMode::Hard})
                REQUIRE_THROWS_AS(reset_repository(repo, {mode, a.hex()}), Error);
            REQUIRE(reset_metadata(repo) == before);
            REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
        }
    }
    SECTION("oversized Index paths reject before inspecting work or saving backups") {
        { RepoLock lock(repo.metadata()); atomic_write(repo.metadata() / "index", test::legacy_index_entry({std::string(4097, 'p'), "100644", blob, 1})); }
        const auto before = reset_metadata(repo);
        const auto objects = reset_objects(repo);
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Hard, a.hex()}), Error);
        REQUIRE(reset_metadata(repo) == before);
        REQUIRE(reset_objects(repo) == objects);
    }
}
TEST_CASE("Reset hard supports both directory file conversions deletion missing files and empty blobs", "[reset]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    ObjectId a, d;
    SECTION("edited tracked file becomes a directory") {
        a = reset_commit(repo, {{"item", {"old", "100644"}}});
        d = reset_commit(repo, {{"item/deep/child", {"new", "100755"}}});
        reset_install(repo, a); reset_file(repo, "item", "dirty", 0640);
    }
    SECTION("tracked directory becomes a file after explicit descendant deletions") {
        a = reset_commit(repo, {{"item/a", {"old", "100644"}}, {"item/b", {"old", "100644"}}});
        d = reset_commit(repo, {{"item", {"new", "100644"}}});
        reset_install(repo, a); reset_file(repo, "item/a", "dirty");
    }
    SECTION("staged deletion retains old HEAD ownership and Index-only additions are deleted") {
        a = reset_commit(repo, {{"file", {"A", "100755"}}, {"deleted", {"old", "100644"}}});
        d = reset_commit(repo, {{"file", {"", "100644"}}, {"new", {"new", "100644"}}});
        reset_install(repo, a);
        { RepoLock lock(repo.metadata()); save_index(repo, {}); }
        reset_file(repo, "extra", "staged-extra"); add_paths(repo, {repo.root() / "extra"});
        std::filesystem::remove(repo.root() / "file");
    }
    reset_repository(repo, {ResetMode::Hard, d.hex()});
    REQUIRE(load_index(repo) == reset_index_for(repo, d));
    REQUIRE(scan_worktree(repo) == read_snapshot(repo, read_commit(repo, d).tree));
    if (std::filesystem::exists(repo.root() / "file")) {
        REQUIRE(read_file(repo.root() / "file").empty());
        REQUIRE(read_worktree_file(repo, "file")->data.permissions == 0644);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "extra"));
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "deleted"));
    }
}
TEST_CASE("Reset hard preserves unknown ignored nodes symlinks and special files on atomic failure", "[reset]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = reset_commit(repo, {{"d/tracked", {"old", "100644"}}, {"file", {"A", "100644"}}});
    reset_install(repo, a); reset_file(repo, "file", "dirty");
    ObjectId d;
    SECTION("unknown ignored destination file") {
        d = reset_commit(repo, {{"new", {"new", "100644"}}});
        reset_file(repo, "new", "private"); reset_file(repo, ".minigitignore", "new\n");
    }
    SECTION("unknown blocking parent file") {
        d = reset_commit(repo, {{"new/child", {"new", "100644"}}}); reset_file(repo, "new", "private");
    }
    SECTION("unknown directory child file") {
        d = reset_commit(repo, {{"d", {"new", "100644"}}});
        reset_file(repo, "d/private", "private"); reset_file(repo, ".minigitignore", "d/private\n");
    }
    SECTION("unknown empty descendant directory") {
        d = reset_commit(repo, {{"d", {"new", "100644"}}}); std::filesystem::create_directory(repo.root() / "d/empty");
    }
    SECTION("tracked symlink is rejected without touching its target") {
        d = a; std::filesystem::remove(repo.root() / "d/tracked");
        reset_file(repo, "outside", "private"); std::filesystem::create_symlink(repo.root() / "outside", repo.root() / "d/tracked");
    }
    SECTION("tracked FIFO is rejected without opening it") {
        d = a; std::filesystem::remove(repo.root() / "d/tracked");
        REQUIRE(::mkfifo((repo.root() / "d/tracked").c_str(), 0600) == 0);
    }
    const auto before = reset_metadata(repo);
    const auto objects = reset_objects(repo);
    REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Hard, d.hex()}), Error);
    REQUIRE(reset_metadata(repo) == before);
    REQUIRE(reset_objects(repo) == objects);
    REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
    if (std::filesystem::exists(repo.root() / "new") && std::filesystem::is_regular_file(repo.root() / "new"))
        REQUIRE(to_string(read_file(repo.root() / "new")) == "private");
    if (std::filesystem::exists(repo.root() / "d/private")) REQUIRE(to_string(read_file(repo.root() / "d/private")) == "private");
    if (std::filesystem::exists(repo.root() / "outside")) REQUIRE(to_string(read_file(repo.root() / "outside")) == "private");
}
TEST_CASE("Reset metadata only modes avoid physical work while invalid targets Index options and pending state reject", "[reset]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = reset_commit(repo, {{"d/file", {"A", "100644"}}});
    const auto d = reset_commit(repo, {{"d/file", {"D", "100644"}}}); reset_install(repo, a);
    SECTION("soft and mixed succeed through a physical symlink parent and FIFO leaf") {
        std::filesystem::remove_all(repo.root() / "d"); std::filesystem::create_directory(repo.root() / "outside");
        std::filesystem::create_symlink(repo.root() / "outside", repo.root() / "d");
        REQUIRE(::mkfifo((repo.root() / "outside/file").c_str(), 0600) == 0);
        const auto index = read_file(repo.metadata() / "index");
        reset_repository(repo, {ResetMode::Soft, d.hex()});
        REQUIRE(read_file(repo.metadata() / "index") == index);
        reset_repository(repo);
        REQUIRE(load_index(repo) == reset_index_for(repo, d));
        REQUIRE(std::filesystem::is_symlink(repo.root() / "d"));
        REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "outside/file")));
    }
    SECTION("corrupt Index cannot be replaced even by hard") {
        atomic_write(repo.metadata() / "index", to_bytes("damaged"));
        const auto before = reset_metadata(repo);
        for (const auto mode : {ResetMode::Soft, ResetMode::Mixed, ResetMode::Hard})
            REQUIRE_THROWS_AS(reset_repository(repo, {mode, d.hex()}), Error);
        REQUIRE(reset_metadata(repo) == before);
    }
    SECTION("missing or mistyped commit and missing tree or unselected blob reject even soft") {
        const auto snapshot = read_snapshot(repo, read_commit(repo, d).tree);
        const auto blob = snapshot.at("d/file").id;
        const auto before = reset_metadata(repo);
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Soft, ObjectId{}.hex()}), Error);
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Soft, blob.hex()}), Error);
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(blob));
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Soft, d.hex()}), Error);
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(read_commit(repo, d).tree));
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Soft, d.hex()}), Error);
        REQUIRE(reset_metadata(repo) == before);
    }
    SECTION("invalid mode empty oversized revision and pending journal have no publication") {
        const auto before = reset_metadata(repo);
        REQUIRE_THROWS_AS(reset_repository(repo, {static_cast<ResetMode>(99), d.hex()}), Error);
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Hard, ""}), Error);
        REQUIRE_THROWS_AS(reset_repository(repo, {ResetMode::Hard, std::string(4097, 'a')}), Error);
        REQUIRE(reset_metadata(repo) == before);
        atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        const auto pending = reset_metadata(repo);
        for (const auto mode : {ResetMode::Soft, ResetMode::Mixed, ResetMode::Hard})
            REQUIRE_THROWS_AS(reset_repository(repo, {mode, d.hex()}), Error);
        REQUIRE(reset_metadata(repo) == pending);
    }
}
TEST_CASE("Reset transaction recovery restores or completes work Index HEAD and both reflogs together", "[reset][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = reset_commit(repo, {{"file", {"A", "100644"}}});
    const auto d = reset_commit(repo, {{"file", {"D", "100755"}}}); reset_install(repo, a);
    reset_file(repo, "file", "B"); add_paths(repo, {repo.root() / "file"}); reset_file(repo, "file", "dirty", 0640);
    const auto before = reset_metadata(repo);
    const auto target = read_snapshot(repo, read_commit(repo, d).tree).at("file");
    RepoLock lock(repo.metadata());
    TransactionPlan plan;
    plan_move_head(repo, plan, read_head(repo), {"refs/heads/main", d}, "reset:hard");
    plan_index_update(repo, plan, reset_index_for(repo, d));
    plan_worktree_update(repo, plan, {{"file", {target, std::nullopt}}}, {"file"});
    bool committed = false;
    std::string stop;
    SECTION("Prepared work publication restores actual edited bytes permissions and metadata") { stop = "work.write:file"; }
    SECTION("Prepared metadata publication restores all references and logs") { stop = "metadata.write:index"; }
    SECTION("Committed finishes the target without duplicating reflog events") { stop = "journal.committed"; committed = true; }
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) {
        if (label == stop) throw Error("injected reset stop");
    }), Error);
    recover_transaction(repo); recover_transaction(repo);
    if (committed) {
        REQUIRE(read_head(repo) == Head{"refs/heads/main", d});
        REQUIRE(load_index(repo) == reset_index_for(repo, d));
        REQUIRE(read_reflog(repo, "HEAD").size() == 2);
        REQUIRE(read_reflog(repo, "refs/heads/main").back() == ReflogEntry{a, d, "reset:hard"});
    } else REQUIRE(reset_metadata(repo) == before);
    REQUIRE(to_string(read_file(repo.root() / "file")) == (committed ? "D" : "dirty"));
    REQUIRE(read_worktree_file(repo, "file")->data.permissions == (committed ? 0755U : 0640U));
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
