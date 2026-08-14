#include <catch2/catch_test_macros.hpp>
#include "minigit/remove.hpp"
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
using RemoveFiles = std::map<std::string, std::pair<std::string, std::string>>;
ObjectId remove_commit(const Repository& repo, const RemoveFiles& files) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Snapshot snapshot;
    for (const auto& [path, file] : files)
        snapshot.emplace(path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    return store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, snapshot), {},
        "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "fixture"}));
}
void remove_file(const Repository& repo, std::string_view path, std::string_view bytes, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(bytes), permissions);
}
void remove_install(const Repository& repo, const ObjectId& id) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Index index;
    for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, id).tree)) {
        remove_file(repo, path, to_string(store.read(state.id).payload), state.mode == "100755" ? 0755U : 0644U);
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
std::map<std::string, Bytes> remove_metadata(const Repository& repo, bool include_index = true) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && (include_index || path != "index") && entry.is_regular_file())
            result.emplace(path, read_file(entry.path()));
    }
    return result;
}
std::size_t remove_objects(const Repository& repo) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects"))
        if (entry.is_regular_file()) ++count;
    return count;
}
std::vector<std::filesystem::path> removal(const Repository& repo, std::string_view path) {
    return {repo.root() / std::string(path)};
}
class RemoveCwd {
public:
    explicit RemoveCwd(const std::filesystem::path& path) : before_(std::filesystem::current_path()) { std::filesystem::current_path(path); }
    ~RemoveCwd() { std::error_code ignored; std::filesystem::current_path(before_, ignored); }
private:
    std::filesystem::path before_;
};
}
TEST_CASE("Remove applies the ordinary HEAD Index work safety matrix atomically", "[remove]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = remove_commit(repo, {{"file", {"A", "100644"}}, {"stay", {"same", "100644"}}});
    remove_install(repo, a);
    bool succeeds = false;
    bool absent = false;
    std::string stay = "same";
    std::vector<std::filesystem::path> paths = removal(repo, "file");
    SECTION("clean file with different nonexecutable full permissions can be removed") {
        remove_file(repo, "file", "A", 0640); succeeds = true;
    }
    SECTION("a genuinely missing clean tracked file only changes Index") {
        std::filesystem::remove(repo.root() / "file"); succeeds = true; absent = true;
    }
    SECTION("a unique staged version cannot be discarded even when work equals Index") {
        remove_file(repo, "file", "B"); add_paths(repo, paths);
    }
    SECTION("an unstaged content edit cannot be discarded") { remove_file(repo, "file", "C"); }
    SECTION("a staged executable mode change counts as a staged edit") {
        remove_file(repo, "file", "A", 0755); add_paths(repo, paths);
    }
    SECTION("an unstaged executable mode change counts as a work edit") { remove_file(repo, "file", "A", 0751); }
    SECTION("a missing work file cannot hide a unique staged version") {
        remove_file(repo, "file", "B"); add_paths(repo, paths); std::filesystem::remove(repo.root() / "file"); absent = true;
    }
    SECTION("an invalid later selection prevents an earlier clean deletion") { paths.push_back(repo.root() / "missing"); }
    SECTION("a later dirty tracked selection prevents an earlier clean deletion") {
        remove_file(repo, "stay", "dirty-stay"); stay = "dirty-stay"; paths.push_back(repo.root() / "stay");
    }
    const auto before = remove_metadata(repo);
    const auto stable = remove_metadata(repo, false);
    const auto objects = remove_objects(repo);
    const auto work_before = absent ? std::optional<FileData>{} : std::optional(read_file_data(repo.root() / "file"));
    if (succeeds) {
        remove_paths(repo, paths);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
        REQUIRE(load_index(repo).size() == 1);
        REQUIRE(load_index(repo).front().path == "stay");
        REQUIRE(remove_metadata(repo, false) == stable);
    } else {
        REQUIRE_THROWS_AS(remove_paths(repo, paths), Error);
        REQUIRE(remove_metadata(repo) == before);
        REQUIRE(remove_objects(repo) == objects);
        if (work_before) {
            REQUIRE(read_file_data(repo.root() / "file").bytes == work_before->bytes);
            REQUIRE(read_file_data(repo.root() / "file").permissions == work_before->permissions);
        } else REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
    }
    REQUIRE(to_string(read_file(repo.root() / "stay")) == stay);
}
TEST_CASE("Remove cached checks HEAD first and force never probes unsafe physical work", "[remove]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = remove_commit(repo, {{"d/file", {"A", "100644"}}}); remove_install(repo, a);
    RemoveOptions options{true};
    bool succeeds = true;
    bool link = false;
    bool fifo = false;
    std::string working = "A";
    SECTION("Index equals HEAD preserves dirty bytes and full permissions") {
        remove_file(repo, "d/file", "C", 04751); working = "C";
    }
    SECTION("Index equals HEAD never reads a tracked symlink parent") { link = true; }
    SECTION("Index equals HEAD never opens a tracked FIFO") { fifo = true; }
    SECTION("Index differs from HEAD but equals work so cached removal is safe") {
        remove_file(repo, "d/file", "B"); add_paths(repo, removal(repo, "d/file")); working = "B";
    }
    SECTION("HEAD Index and work all different reject") {
        remove_file(repo, "d/file", "B"); add_paths(repo, removal(repo, "d/file"));
        remove_file(repo, "d/file", "C"); working = "C"; succeeds = false;
    }
    SECTION("cached force removes unique staged content without probing a symlink parent") {
        remove_file(repo, "d/file", "B"); add_paths(repo, removal(repo, "d/file")); options.force = true; link = true;
    }
    SECTION("cached needs work when Index differs from HEAD and rejects that unsafe work") {
        remove_file(repo, "d/file", "B"); add_paths(repo, removal(repo, "d/file")); link = true; succeeds = false;
    }
    if (link) {
        std::filesystem::remove(repo.root() / "d/file"); std::filesystem::remove(repo.root() / "d");
        remove_file(repo, "outside/file", "private");
        std::filesystem::create_symlink(repo.root() / "outside", repo.root() / "d");
    } else if (fifo) {
        std::filesystem::remove(repo.root() / "d/file");
        REQUIRE(::mkfifo((repo.root() / "d/file").c_str(), 0600) == 0);
    }
    const auto before = remove_metadata(repo);
    const auto stable = remove_metadata(repo, false);
    const auto work_before = !link && !fifo ? std::optional(read_file_data(repo.root() / "d/file")) : std::optional<FileData>{};
    if (succeeds) {
        remove_paths(repo, removal(repo, "d/file"), options);
        REQUIRE(load_index(repo).empty());
        REQUIRE(remove_metadata(repo, false) == stable);
    } else {
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "d/file"), options), Error);
        REQUIRE(remove_metadata(repo) == before);
    }
    if (link) {
        REQUIRE(std::filesystem::is_symlink(repo.root() / "d"));
        REQUIRE(to_string(read_file(repo.root() / "outside/file")) == "private");
    } else if (fifo) REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "d/file")));
    else {
        REQUIRE(to_string(read_file(repo.root() / "d/file")) == working);
        REQUIRE(read_file_data(repo.root() / "d/file").bytes == work_before->bytes);
        REQUIRE(read_file_data(repo.root() / "d/file").permissions == work_before->permissions);
    }
}
TEST_CASE("Remove selects each lexical Index argument and requires recursive directory prefixes", "[remove]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = remove_commit(repo, {{"dir/- spaced", {"name", "100644"}}, {"dir/file", {"A", "100644"}}, {"directory", {"other", "100644"}}});
    remove_install(repo, a);
    SECTION("directory prefix and root require recursive even with force or cached") {
        const auto before = remove_metadata(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "dir")), Error);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "."), {true, true}), Error);
        REQUIRE(remove_metadata(repo) == before);
    }
    SECTION("relative exact absolute prefixes duplicates and optionlike names combine safely") {
        RemoveCwd cwd(repo.root() / "dir");
        remove_paths(repo, {"file", repo.root() / "dir", "./- spaced", "../dir/file"}, {false, false, true});
        REQUIRE(load_index(repo).size() == 1);
        REQUIRE(load_index(repo).front().path == "directory");
        REQUIRE(to_string(read_file(repo.root() / "directory")) == "other");
        REQUIRE(std::filesystem::is_directory(repo.root() / "dir"));
    }
    SECTION("an exact optionlike file selector does not require recursive") {
        remove_paths(repo, removal(repo, "dir/- spaced"));
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "dir/- spaced"));
        REQUIRE(std::filesystem::exists(repo.root() / "dir/file"));
    }
    SECTION("every argument must match before a root selector hides it") {
        const auto before = remove_metadata(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, {repo.root(), repo.root() / "missing"}, {true, true, true}), Error);
        REQUIRE_THROWS_AS(remove_paths(repo, {repo.root(), repo.root().parent_path() / "outside"}, {true, true, true}), Error);
        REQUIRE(remove_metadata(repo) == before);
    }
    SECTION("recursive root only removes tracked leaves and retains unknown siblings and empty directories") {
        remove_file(repo, "dir/ignored", "private"); remove_file(repo, ".minigitignore", "dir/ignored\n");
        std::filesystem::create_directory(repo.root() / "dir/empty");
        remove_paths(repo, removal(repo, "."), {false, false, true});
        REQUIRE(load_index(repo).empty());
        REQUIRE(to_string(read_file(repo.root() / "dir/ignored")) == "private");
        REQUIRE(std::filesystem::is_directory(repo.root() / "dir/empty"));
        REQUIRE(std::filesystem::is_directory(repo.root() / "dir"));
    }
    SECTION("cached selectors normalize link dot-dot lexically without reading it") {
        std::filesystem::create_symlink(repo.root() / "elsewhere", repo.root() / "link");
        remove_paths(repo, {repo.root() / "link/../dir/file"}, {true});
        REQUIRE(load_index(repo).size() == 2);
        REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(repo.root() / "link")));
        REQUIRE(to_string(read_file(repo.root() / "dir/file")) == "A");
    }
}
TEST_CASE("Remove force clears selected conflict stages and preserves unselected conflict topology", "[remove]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = remove_commit(repo, {{"normal", {"normal", "100644"}}, {"selected", {"A", "100644"}}}); remove_install(repo, a);
    const auto normal = load_index(repo).front();
    const auto blob = load_index(repo).back().id;
    const Index conflicts{{"normal", normal.mode, normal.id}, {"selected", "100644", blob, 1},
                          {"selected", "100755", blob, 2}, {"selected", "100644", blob, 3},
                          {"unselected", "100644", blob, 2}, {"unselected/child", "100644", blob, 3}};
    { RepoLock lock(repo.metadata()); save_index(repo, conflicts); }
    remove_file(repo, "selected", "dirty");
    SECTION("nonforce ordinary and cached both reject selected conflict stages") {
        const auto before = remove_metadata(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "selected")), Error);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "selected"), {true}), Error);
        REQUIRE(remove_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "selected")) == "dirty");
    }
    SECTION("force removes every selected stage and dirty work only") {
        remove_paths(repo, removal(repo, "selected"), {false, true});
        REQUIRE(load_index(repo) == Index{normal, conflicts[4], conflicts[5]});
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "selected"));
    }
    SECTION("cached force resolves selected conflict by dropping its stages and preserving work") {
        remove_paths(repo, removal(repo, "selected"), {true, true});
        REQUIRE(load_index(repo) == Index{normal, conflicts[4], conflicts[5]});
        REQUIRE(to_string(read_file(repo.root() / "selected")) == "dirty");
    }
    SECTION("ordinary removal of a normal path retains all unselected stages") {
        remove_paths(repo, removal(repo, "normal"));
        auto expected = conflicts; expected.erase(expected.begin());
        REQUIRE(load_index(repo) == expected);
        REQUIRE(to_string(read_file(repo.root() / "selected")) == "dirty");
    }
    SECTION("directory file conflict selection needs recursive and force removes actual known child inventory") {
        remove_file(repo, "unselected/child", "dirty-child");
        const auto before = remove_metadata(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "unselected"), {false, true}), Error);
        REQUIRE(remove_metadata(repo) == before);
        remove_paths(repo, removal(repo, "unselected"), {false, true, true});
        REQUIRE(load_index(repo) == Index{conflicts[0], conflicts[1], conflicts[2], conflicts[3]});
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "unselected"));
    }
}
TEST_CASE("Remove distinguishes absent work from blocked or converted paths and protects unknown descendants", "[remove]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = remove_commit(repo, {{"d/tracked", {"old", "100644"}}, {"file", {"A", "100644"}}}); remove_install(repo, a);
    RemoveOptions options;
    auto paths = removal(repo, "file");
    bool succeeds = false;
    SECTION("a tracked file replaced by an empty directory is not Absent in ordinary remove") {
        std::filesystem::remove(repo.root() / "file"); std::filesystem::create_directory(repo.root() / "file");
    }
    SECTION("a tracked parent directory replaced by a regular file is not Absent") {
        std::filesystem::remove(repo.root() / "d/tracked"); std::filesystem::remove(repo.root() / "d");
        remove_file(repo, "d", "unknown-parent"); paths = removal(repo, "d/tracked");
    }
    SECTION("force can delete an empty directory at the exact previously tracked file path") {
        std::filesystem::remove(repo.root() / "file"); std::filesystem::create_directory(repo.root() / "file");
        options.force = true; succeeds = true;
    }
    SECTION("force cannot use tracked ancestor ownership to delete unknown ignored child files") {
        std::filesystem::remove(repo.root() / "file"); remove_file(repo, "file/private", "private");
        remove_file(repo, ".minigitignore", "file/private\n"); options.force = true;
    }
    SECTION("force cannot remove an unknown empty child directory") {
        std::filesystem::remove(repo.root() / "file"); std::filesystem::create_directories(repo.root() / "file/empty");
        options.force = true;
    }
    SECTION("force cannot delete a tracked symlink or its external target") {
        std::filesystem::remove(repo.root() / "file"); remove_file(repo, "outside", "private");
        std::filesystem::create_symlink(repo.root() / "outside", repo.root() / "file"); options.force = true;
    }
    SECTION("force rejects a tracked FIFO without opening it") {
        std::filesystem::remove(repo.root() / "file"); REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0); options.force = true;
    }
    const auto before = remove_metadata(repo);
    const auto stable = remove_metadata(repo, false);
    const auto objects = remove_objects(repo);
    if (succeeds) {
        remove_paths(repo, paths, options);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
        REQUIRE(remove_metadata(repo, false) == stable);
    } else {
        REQUIRE_THROWS_AS(remove_paths(repo, paths, options), Error);
        REQUIRE(remove_metadata(repo) == before);
        REQUIRE(remove_objects(repo) == objects);
        if (std::filesystem::is_regular_file(repo.root() / "d")) REQUIRE(to_string(read_file(repo.root() / "d")) == "unknown-parent");
        if (std::filesystem::exists(repo.root() / "file/private")) REQUIRE(to_string(read_file(repo.root() / "file/private")) == "private");
        if (std::filesystem::exists(repo.root() / "outside")) REQUIRE(to_string(read_file(repo.root() / "outside")) == "private");
    }
}
TEST_CASE("Remove handles unborn detached corruption input budgets and pending journals without publication", "[remove]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = remove_commit(repo, {{"file", {"A", "100644"}}});
    SECTION("ordinary unborn removal rejects staged additions while cached unstages matching work") {
        remove_file(repo, "file", "staged"); add_paths(repo, removal(repo, "file"));
        const auto before = remove_metadata(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "file")), Error);
        REQUIRE(remove_metadata(repo) == before);
        remove_paths(repo, removal(repo, "file"), {true});
        REQUIRE(load_index(repo).empty());
        REQUIRE(to_string(read_file(repo.root() / "file")) == "staged");
        REQUIRE(read_head(repo) == Head{"refs/heads/main", std::nullopt});
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/HEAD"));
    }
    SECTION("force unborn removal deletes tracked dirty work without birthing a reference") {
        remove_file(repo, "file", "staged"); add_paths(repo, removal(repo, "file")); remove_file(repo, "file", "dirty");
        const auto stable = remove_metadata(repo, false);
        remove_paths(repo, removal(repo, "file"), {false, true});
        REQUIRE(load_index(repo).empty());
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
        REQUIRE(remove_metadata(repo, false) == stable);
    }
    SECTION("detached ordinary removal preserves raw HEAD and all existing logs") {
        remove_install(repo, a); { RepoLock lock(repo.metadata()); set_head(repo, a.hex()); }
        const auto stable = remove_metadata(repo, false);
        remove_paths(repo, removal(repo, "file"));
        REQUIRE(read_head(repo) == Head{std::nullopt, a});
        REQUIRE(remove_metadata(repo, false) == stable);
    }
    SECTION("force cached still validates wrong typed or missing unselected stage objects") {
        remove_install(repo, a);
        const auto entry = load_index(repo).front();
        for (const auto invalid : {a, ObjectId{}}) {
            { RepoLock lock(repo.metadata()); save_index(repo, {entry, {"unselected", "100644", invalid, 3}}); }
            const auto before = remove_metadata(repo);
            REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "file"), {true, true}), Error);
            REQUIRE(remove_metadata(repo) == before);
            REQUIRE(to_string(read_file(repo.root() / "file")) == "A");
        }
    }
    SECTION("damaged Index and incomplete HEAD snapshots cannot be bypassed by force") {
        remove_install(repo, a);
        const auto valid = read_file(repo.metadata() / "index");
        atomic_write(repo.metadata() / "index", to_bytes("damaged"));
        const auto corrupt = remove_metadata(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "file"), {true, true}), Error);
        REQUIRE(remove_metadata(repo) == corrupt);
        atomic_write(repo.metadata() / "index", valid);
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(read_commit(repo, a).tree));
        const auto incomplete = remove_metadata(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "file"), {true, true}), Error);
        REQUIRE(remove_metadata(repo) == incomplete);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "A");
    }
    SECTION("small invalid path and Index budget fixtures reject before any backup") {
        remove_install(repo, a);
        const auto before = remove_metadata(repo);
        const auto objects = remove_objects(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, {}), Error);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, std::string(4097, 'x')), {true, true}), Error);
        std::string deep = "x";
        for (unsigned i = 0; i < 128; ++i) deep += "/x";
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, deep), {true, true}), Error);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, ".minigit/index"), {true, true}), Error);
        REQUIRE(remove_metadata(repo) == before);
        REQUIRE(remove_objects(repo) == objects);
        { RepoLock lock(repo.metadata()); atomic_write(repo.metadata() / "index", test::legacy_index_entry({std::string(4097, 'p'), "100644", load_index(repo).front().id, 1})); }
        const auto oversized = remove_metadata(repo);
        REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "file"), {true, true}), Error);
        REQUIRE(remove_metadata(repo) == oversized);
        REQUIRE(remove_objects(repo) == objects);
    }
    SECTION("pending transactions block ordinary cached and forced modes") {
        remove_install(repo, a); atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        const auto before = remove_metadata(repo);
        for (const auto options : {RemoveOptions{}, RemoveOptions{true}, RemoveOptions{false, true}, RemoveOptions{true, true}})
            REQUIRE_THROWS_AS(remove_paths(repo, removal(repo, "file"), options), Error);
        REQUIRE(remove_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "A");
    }
}
TEST_CASE("Remove transaction recovery restores exact work permissions and Index at two key boundaries", "[remove][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = remove_commit(repo, {{"file", {"A", "100644"}}}); remove_install(repo, a);
    remove_file(repo, "file", "dirty", 04751);
    const auto before = remove_metadata(repo);
    const auto stable = remove_metadata(repo, false);
    RepoLock lock(repo.metadata());
    TransactionPlan plan;
    plan_index_update(repo, plan, {});
    plan_worktree_update(repo, plan, {{"file", {}}}, {"file"});
    bool committed = false;
    std::string stop;
    SECTION("Prepared after work unlink restores actual dirty bytes full permissions and Index") { stop = "work.unlink:file"; }
    SECTION("Committed finishes removal while preserving HEAD refs and logs") { stop = "journal.committed"; committed = true; }
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) {
        if (label == stop) throw Error("injected remove stop");
    }), Error);
    recover_transaction(repo); recover_transaction(repo);
    if (committed) {
        REQUIRE(load_index(repo).empty());
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
        REQUIRE(remove_metadata(repo, false) == stable);
    } else {
        REQUIRE(remove_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "dirty");
        REQUIRE(read_worktree_file(repo, "file")->data.permissions == 04751);
    }
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
