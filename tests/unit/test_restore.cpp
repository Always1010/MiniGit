#include <catch2/catch_test_macros.hpp>
#include "minigit/restore.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/worktree.hpp"
#include "minigit/worktree_update.hpp"
#include "temp_directory.hpp"
#include <map>
#include <sys/stat.h>
#include <utility>
using namespace minigit;
namespace {
using RestoreFiles = std::map<std::string, std::pair<std::string, std::string>>;
ObjectId restore_commit(const Repository& repo, const RestoreFiles& files) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Snapshot snapshot;
    for (const auto& [path, file] : files)
        snapshot.emplace(path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    const auto tree = write_snapshot(repo, snapshot);
    return store.write(ObjectType::Commit, encode_commit({tree, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "fixture"}));
}
void restore_file(const Repository& repo, std::string_view path, std::string_view bytes, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(bytes), permissions);
}
void restore_install(const Repository& repo, const ObjectId& id) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Index index;
    for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, id).tree)) {
        restore_file(repo, path, to_string(store.read(state.id).payload), state.mode == "100755" ? 0755U : 0644U);
        index.push_back({path, state.mode, state.id});
    }
    save_index(repo, index);
    update_ref(repo, "refs/heads/main", std::nullopt, id);
    const auto log = encode_reflog({{std::nullopt, id, "fixture"}});
    detail::ensure_directory(repo.metadata() / "logs/refs/heads");
    atomic_write(repo.metadata() / "logs/HEAD", log);
    atomic_write(repo.metadata() / "logs/refs/heads/main", log);
    atomic_write(repo.metadata() / "identity", to_bytes("name=Test\nemail=a@b\n"));
}
std::map<std::string, Bytes> restore_metadata(const Repository& repo, bool include_index = true) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && (include_index || path != "index") && entry.is_regular_file())
            result.emplace(path, read_file(entry.path()));
    }
    return result;
}
std::size_t restore_objects(const Repository& repo) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects"))
        if (entry.is_regular_file()) ++count;
    return count;
}
std::vector<std::filesystem::path> restore_selection(const Repository& repo, std::string_view path) {
    return {repo.root() / std::string(path)};
}
class RestoreCwd {
public:
    explicit RestoreCwd(const std::filesystem::path& path) : before_(std::filesystem::current_path()) { std::filesystem::current_path(path); }
    ~RestoreCwd() { std::error_code ignored; std::filesystem::current_path(before_, ignored); }
private:
    std::filesystem::path before_;
};
}
TEST_CASE("Restore chooses Index HEAD and explicit sources without moving references or reflogs", "[restore]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = restore_commit(repo, {{"file", {"A", "100644"}}, {"stay", {"same", "100644"}}});
    const auto d = restore_commit(repo, {{"file", {"D", "100755"}}});
    restore_install(repo, a);
    restore_file(repo, "file", "B"); add_paths(repo, restore_selection(repo, "file"));
    restore_file(repo, "file", "C", 0640);
    const auto stable = restore_metadata(repo, false);
    const auto index_before = read_file(repo.metadata() / "index");
    RestoreOptions options;
    std::string staged = "B";
    std::string working = "B";
    std::uint32_t permissions = 0644;
    SECTION("default restores work from Index") {}
    SECTION("staged restores Index from HEAD and leaves work untouched") {
        options.staged = true; staged = "A"; working = "C"; permissions = 0640;
    }
    SECTION("both destinations default to HEAD") {
        options.staged = true; options.worktree = true; staged = "A"; working = "A";
    }
    SECTION("explicit source updates work without updating Index") {
        options.source = d.hex(); working = "D"; permissions = 0755;
    }
    SECTION("explicit source updates both destinations") {
        options = {d.hex(), true, true}; staged = "D"; working = "D"; permissions = 0755;
    }
    restore_paths(repo, restore_selection(repo, "file"), options);
    REQUIRE(restore_metadata(repo, false) == stable);
    REQUIRE(to_string(read_file(repo.root() / "file")) == working);
    REQUIRE(read_worktree_file(repo, "file")->data.permissions == permissions);
    const auto index = load_index(repo);
    REQUIRE(index.size() == 2);
    REQUIRE(to_string(ObjectStore(repo.metadata() / "objects").read(index.front().id).payload) == staged);
    if (!options.staged) REQUIRE(read_file(repo.metadata() / "index") == index_before);
    REQUIRE(to_string(read_file(repo.root() / "stay")) == "same");
}
TEST_CASE("Restore selects lexical exact and prefix paths and validates every argument before publication", "[restore]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = restore_commit(repo, {{"dir/file", {"A", "100644"}}, {"dir/- spaced", {"name", "100644"}}, {"directory", {"other", "100644"}}});
    restore_install(repo, a);
    restore_file(repo, "dir/file", "dirty"); restore_file(repo, "dir/- spaced", "dirty");
    SECTION("relative CWD paths absolute paths and overlapping prefixes select each path once") {
        RestoreCwd cwd(repo.root() / "dir");
        restore_paths(repo, {"file", repo.root() / "dir", "./- spaced", "../dir/file"});
        REQUIRE(to_string(read_file(repo.root() / "dir/file")) == "A");
        REQUIRE(to_string(read_file(repo.root() / "dir/- spaced")) == "name");
    }
    SECTION("root selection includes all known paths even when work is missing") {
        std::filesystem::remove(repo.root() / "directory");
        restore_paths(repo, restore_selection(repo, "."));
        REQUIRE(to_string(read_file(repo.root() / "directory")) == "other");
        REQUIRE(to_string(read_file(repo.root() / "dir/file")) == "A");
    }
    SECTION("an unmatched later argument or outside path cannot hide behind a root selector") {
        const auto before = restore_metadata(repo);
        for (const auto& invalid : std::vector<std::filesystem::path>{repo.root() / "missing", repo.root().parent_path() / "outside", repo.root() / ".minigit/index"})
            REQUIRE_THROWS_AS(restore_paths(repo, {repo.root(), invalid}), Error);
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "dir/file")) == "dirty");
    }
    SECTION("prefix matching has a slash boundary") {
        restore_file(repo, "directory", "dirty-other");
        restore_paths(repo, restore_selection(repo, "dir"));
        REQUIRE(to_string(read_file(repo.root() / "directory")) == "dirty-other");
    }
    SECTION("a source-only historical path can be restored when absent") {
        const auto historical = restore_commit(repo, {{"old/deleted", {"historical", "100644"}}});
        restore_paths(repo, restore_selection(repo, "old"), {historical.hex()});
        REQUIRE(to_string(read_file(repo.root() / "old/deleted")) == "historical");
        REQUIRE(load_index(repo).size() == 3);
    }
    SECTION("old HEAD ownership permits restoring a file removed from Index") {
        { RepoLock lock(repo.metadata()); auto index = load_index(repo); index.pop_back(); save_index(repo, index); }
        restore_file(repo, "directory", "unstaged-after-staged-delete");
        const auto before = read_file(repo.metadata() / "index");
        restore_paths(repo, restore_selection(repo, "directory"), {a.hex()});
        REQUIRE(to_string(read_file(repo.root() / "directory")) == "other");
        REQUIRE(read_file(repo.metadata() / "index") == before);
    }
}
TEST_CASE("Restore preserves unselected conflict stages and only staged restoration resolves selected conflicts", "[restore]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = restore_commit(repo, {{"conflict", {"A", "100644"}}, {"normal", {"normal", "100644"}}});
    restore_install(repo, a);
    const auto entries = load_index(repo);
    const Index conflicted{{"conflict", "100644", entries.front().id, 1}, {"conflict", "100755", entries.front().id, 2},
                           {"conflict", "100644", entries.front().id, 3}, entries.back()};
    { RepoLock lock(repo.metadata()); save_index(repo, conflicted); }
    restore_file(repo, "conflict", "dirty-conflict"); restore_file(repo, "normal", "dirty-normal");
    const auto before = restore_metadata(repo);
    SECTION("default Index source rejects a selected conflict atomically") {
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, ".")), Error);
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "normal")) == "dirty-normal");
    }
    SECTION("default work restore of another path retains all conflict stages") {
        restore_paths(repo, restore_selection(repo, "normal"));
        REQUIRE(load_index(repo) == conflicted);
        REQUIRE(to_string(read_file(repo.root() / "normal")) == "normal");
        REQUIRE(to_string(read_file(repo.root() / "conflict")) == "dirty-conflict");
    }
    SECTION("explicit source work-only changes content without clearing conflict stages") {
        restore_paths(repo, restore_selection(repo, "conflict"), {a.hex()});
        REQUIRE(load_index(repo) == conflicted);
        REQUIRE(to_string(read_file(repo.root() / "conflict")) == "A");
    }
    SECTION("staged destination clears selected stages but leaves work untouched") {
        restore_paths(repo, restore_selection(repo, "conflict"), {std::nullopt, true});
        REQUIRE(load_index(repo) == entries);
        REQUIRE(to_string(read_file(repo.root() / "conflict")) == "dirty-conflict");
    }
    SECTION("resolving another path preserves unselected directory-file conflict stages") {
        const Index df{{"df", "100644", entries.front().id, 2}, {"df/child", "100644", entries.front().id, 3}, entries.back()};
        auto staged_df = df;
        {
            RepoLock lock(repo.metadata());
            staged_df.back().id = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("staged-normal"));
            save_index(repo, staged_df);
        }
        restore_paths(repo, restore_selection(repo, "normal"), {std::nullopt, true});
        REQUIRE(load_index(repo) == df);
        REQUIRE(to_string(read_file(repo.root() / "normal")) == "dirty-normal");
    }
}
TEST_CASE("Restore handles unborn HEAD explicit historical sources and exact no-op Index bytes", "[restore]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto source = restore_commit(repo, {{"file", {"historical", "100644"}}});
    restore_file(repo, "file", "staged"); add_paths(repo, restore_selection(repo, "file"));
    restore_file(repo, "file", "dirty");
    const auto stable = restore_metadata(repo, false);
    SECTION("default staged and explicit HEAD sources reject unborn HEAD") {
        const auto before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {std::nullopt, true}), Error);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {"HEAD"}), Error);
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
    }
    SECTION("default work source is valid in an unborn repository") {
        restore_paths(repo, restore_selection(repo, "file"));
        REQUIRE(to_string(read_file(repo.root() / "file")) == "staged");
    }
    SECTION("explicit staged source leaves work and unborn HEAD intact") {
        restore_paths(repo, restore_selection(repo, "file"), {source.hex(), true});
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
        REQUIRE(load_index(repo).front().id == read_snapshot(repo, read_commit(repo, source).tree).at("file").id);
    }
    SECTION("explicit source can update both destinations without birthing a branch") {
        restore_paths(repo, restore_selection(repo, "file"), {source.hex(), true, true});
        REQUIRE(to_string(read_file(repo.root() / "file")) == "historical");
    }
    SECTION("a repeated restoration preserves exact metadata and adds no backup objects") {
        restore_paths(repo, restore_selection(repo, "file"), {source.hex(), true, true});
        const auto before = restore_metadata(repo);
        const auto objects = restore_objects(repo);
        restore_paths(repo, {repo.root() / "file", repo.root() / "."}, {source.hex(), true, true});
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(restore_objects(repo) == objects);
    }
    REQUIRE(read_head(repo) == Head{"refs/heads/main", std::nullopt});
    REQUIRE(restore_metadata(repo, false) == stable);
}
TEST_CASE("Restore supports missing-source deletion empty blobs default permissions and both directory file conversions", "[restore]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    ObjectId a;
    ObjectId source;
    SECTION("deletion and empty blob are different targets and permissions reset to defaults") {
        a = restore_commit(repo, {{"deleted", {"old", "100644"}}, {"file", {"old", "100644"}}});
        source = restore_commit(repo, {{"empty", {"", "100644"}}, {"file", {"new", "100755"}}});
        restore_install(repo, a); restore_file(repo, "file", "dirty", 04751);
    }
    SECTION("a file can become directories and a source file") {
        a = restore_commit(repo, {{"item", {"old", "100644"}}});
        source = restore_commit(repo, {{"item/deep/child", {"new", "100644"}}});
        restore_install(repo, a);
    }
    SECTION("a directory can become a file after explicit selected descendant deletion") {
        a = restore_commit(repo, {{"item/deep/child", {"old", "100644"}}, {"item/other", {"other", "100644"}}});
        source = restore_commit(repo, {{"item", {"new", "100644"}}});
        restore_install(repo, a);
    }
    const auto stable = restore_metadata(repo, false);
    restore_paths(repo, restore_selection(repo, "."), {source.hex(), true, true});
    REQUIRE(restore_metadata(repo, false) == stable);
    { RepoLock lock(repo.metadata()); REQUIRE(index_snapshot(repo) == read_snapshot(repo, read_commit(repo, source).tree)); }
    REQUIRE(scan_worktree(repo) == read_snapshot(repo, read_commit(repo, source).tree));
    if (std::filesystem::exists(repo.root() / "file")) {
        REQUIRE(read_worktree_file(repo, "file")->data.permissions == 0755);
        REQUIRE(read_file(repo.root() / "empty").empty());
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "deleted"));
    }
}
TEST_CASE("Restore protects unknown nodes while Index-only selection never probes physical work paths", "[restore]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = restore_commit(repo, {{"d/tracked", {"old", "100644"}}});
    restore_install(repo, a);
    SECTION("unknown or ignored source-only destination files cannot be overwritten") {
        const auto source = restore_commit(repo, {{"new", {"replacement", "100644"}}});
        restore_file(repo, "new", "unknown"); restore_file(repo, ".minigitignore", "new\n");
        const auto before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "new"), {source.hex()}), Error);
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "new")) == "unknown");
    }
    SECTION("an unknown blocking parent file cannot be removed implicitly") {
        const auto source = restore_commit(repo, {{"new/child", {"replacement", "100644"}}});
        restore_file(repo, "new", "unknown");
        const auto before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "new"), {source.hex()}), Error);
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "new")) == "unknown");
    }
    SECTION("directory replacement protects unknown descendants and empty directories") {
        const auto source = restore_commit(repo, {{"d", {"replacement", "100644"}}});
        restore_file(repo, "d/ignored", "unknown"); restore_file(repo, ".minigitignore", "d/ignored\n");
        const auto before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "d"), {source.hex(), true, true}), Error);
        REQUIRE(restore_metadata(repo) == before);
        std::filesystem::remove(repo.root() / "d/ignored"); std::filesystem::create_directory(repo.root() / "d/empty");
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "d"), {source.hex(), true, true}), Error);
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(std::filesystem::is_directory(repo.root() / "d/empty"));
    }
    SECTION("updating a leaf preserves unrelated ignored siblings and empty directories") {
        restore_file(repo, "d/tracked", "dirty"); restore_file(repo, "d/ignored", "unknown");
        restore_file(repo, ".minigitignore", "d/ignored\n"); std::filesystem::create_directory(repo.root() / "d/empty");
        restore_paths(repo, restore_selection(repo, "d/tracked"));
        REQUIRE(to_string(read_file(repo.root() / "d/tracked")) == "old");
        REQUIRE(to_string(read_file(repo.root() / "d/ignored")) == "unknown");
        REQUIRE(std::filesystem::is_directory(repo.root() / "d/empty"));
    }
    SECTION("staged-only symlink parents FIFO leaves and link dot-dot selectors remain lexical") {
        const auto source = restore_commit(repo, {{"d/tracked", {"new", "100644"}}});
        std::filesystem::remove(repo.root() / "d/tracked"); std::filesystem::remove(repo.root() / "d");
        std::filesystem::create_directory(repo.root() / "outside");
        std::filesystem::create_symlink(repo.root() / "outside", repo.root() / "d");
        std::filesystem::create_symlink(repo.root() / "outside", repo.root() / "link");
        REQUIRE(::mkfifo((repo.root() / "outside/tracked").c_str(), 0600) == 0);
        restore_paths(repo, {repo.root() / "link/../d/tracked"}, {source.hex(), true});
        REQUIRE(load_index(repo).front().id == read_snapshot(repo, read_commit(repo, source).tree).at("d/tracked").id);
        REQUIRE(std::filesystem::is_symlink(repo.root() / "d"));
        REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "outside/tracked")));
        // The successful Index transaction stores metadata backup blobs; the
        // subsequent rejected work restore must add no further objects.
        const auto objects = restore_objects(repo);
        const auto staged_metadata = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, {repo.root() / "link/../d/tracked"}, {source.hex()}), Error);
        REQUIRE(restore_metadata(repo) == staged_metadata);
        REQUIRE(restore_objects(repo) == objects);
        REQUIRE(std::filesystem::is_symlink(repo.root() / "d"));
        REQUIRE(std::filesystem::is_symlink(repo.root() / "link"));
        REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "outside/tracked")));
    }
}
TEST_CASE("Restore preflights all source and Index objects topology inputs and pending transactions", "[restore]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = restore_commit(repo, {{"file", {"A", "100644"}}});
    restore_install(repo, a); restore_file(repo, "file", "dirty");
    SECTION("every unselected conflict-stage object is typed and must exist") {
        { RepoLock lock(repo.metadata()); save_index(repo, {{"file", "100644", load_index(repo).front().id}, {"unselected", "100644", a, 3}}); }
        const auto before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file")), Error);
        REQUIRE(restore_metadata(repo) == before);
        { RepoLock lock(repo.metadata()); save_index(repo, {{"file", "100644", load_index(repo).front().id}, {"unselected", "100644", ObjectId{}, 1}}); }
        const auto missing_before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file")), Error);
        REQUIRE(restore_metadata(repo) == missing_before);
    }
    SECTION("a damaged Index is never silently replaced") {
        atomic_write(repo.metadata() / "index", to_bytes("damaged"));
        const auto before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {a.hex(), true, true}), Error);
        REQUIRE(restore_metadata(repo) == before);
    }
    SECTION("missing commit noncommit missing tree and unselected missing blob sources all reject") {
        const auto source = restore_commit(repo, {{"file", {"source", "100644"}}, {"unselected", {"bad", "100644"}}});
        const auto snapshot = read_snapshot(repo, read_commit(repo, source).tree);
        const auto blob = snapshot.at("unselected").id;
        const auto before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {ObjectId{}.hex()}), Error);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {blob.hex()}), Error);
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(blob));
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {source.hex()}), Error);
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(read_commit(repo, source).tree));
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {source.hex()}), Error);
        REQUIRE(restore_metadata(repo) == before);
    }
    SECTION("final Index DF conflict rejects before any working backup or physical probe") {
        const auto source = restore_commit(repo, {{"item/child", {"new", "100644"}}});
        { RepoLock lock(repo.metadata()); const auto entry = load_index(repo).front(); save_index(repo, {entry, {"item", "100644", entry.id}}); }
        // Selecting only the new child retains the unselected staged parent.
        // Its physical FIFO must never be probed: Index topology rejects first.
        REQUIRE(::mkfifo((repo.root() / "item").c_str(), 0600) == 0);
        const auto before = restore_metadata(repo);
        const auto objects = restore_objects(repo);
        try {
            restore_paths(repo, restore_selection(repo, "item/child"), {source.hex(), true, true});
            FAIL("unselected staged parent must cause Index DF rejection");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("索引文件与目录路径冲突") != std::string::npos);
        }
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(restore_objects(repo) == objects);
        REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "item")));
    }
    SECTION("empty oversized deep and unmatched inputs fail before backup capture") {
        const auto before = restore_metadata(repo);
        const auto objects = restore_objects(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, {}), Error);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {""}), Error);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file"), {std::string(4097, 'a')}), Error);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, std::string(4097, 'x'))), Error);
        std::string deep = "x";
        for (unsigned i = 0; i < 128; ++i) deep += "/x";
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, deep)), Error);
        // Count preflight rejects before examining inputs; empty paths keep the
        // oversized vector cheap without allocating 200001 absolute strings.
        try {
            restore_paths(repo, std::vector<std::filesystem::path>(200001));
            FAIL("path argument count must be checked first");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("数量超过限制") != std::string::npos);
        }
        REQUIRE(restore_metadata(repo) == before);
        REQUIRE(restore_objects(repo) == objects);
    }
    SECTION("pending journal blocks restoration before any state changes") {
        atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        const auto before = restore_metadata(repo);
        REQUIRE_THROWS_AS(restore_paths(repo, restore_selection(repo, "file")), Error);
        REQUIRE(restore_metadata(repo) == before);
    }
    REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
}
TEST_CASE("Restore transaction recovery keeps selected Index and work coherent without changing HEAD or logs", "[restore][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = restore_commit(repo, {{"file", {"A", "100644"}}});
    const auto source = restore_commit(repo, {{"file", {"source", "100755"}}});
    restore_install(repo, a); restore_file(repo, "file", "dirty", 0640);
    const auto old_entry = load_index(repo).front();
    const Index before_index{old_entry, {"unselected", "100644", old_entry.id, 1}, {"unselected", "100644", old_entry.id, 3}};
    { RepoLock lock(repo.metadata()); save_index(repo, before_index); }
    const auto stable = restore_metadata(repo, false);
    const auto before_bytes = read_file(repo.metadata() / "index");
    const auto target = read_snapshot(repo, read_commit(repo, source).tree).at("file");
    Index after_index = before_index;
    after_index.front() = {"file", target.mode, target.id};
    RepoLock lock(repo.metadata());
    TransactionPlan plan;
    plan_index_update(repo, plan, after_index);
    plan_worktree_update(repo, plan, {{"file", {target, std::nullopt}}}, {"file", "unselected"});
    bool committed = false;
    std::string stop;
    SECTION("Prepared rolls both destinations back to exact before bytes and permissions") { stop = "work.write:file"; }
    SECTION("Committed completes both destinations and retains unselected conflict stages") { stop = "journal.committed"; committed = true; }
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) { if (label == stop) throw Error("injected stop"); }), Error);
    recover_transaction(repo); recover_transaction(repo);
    REQUIRE(read_file(repo.metadata() / "index") == (committed ? *plan.metadata.front().after : before_bytes));
    REQUIRE(to_string(read_file(repo.root() / "file")) == (committed ? "source" : "dirty"));
    REQUIRE(read_worktree_file(repo, "file")->data.permissions == (committed ? 0755U : 0640U));
    REQUIRE(restore_metadata(repo, false) == stable);
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
