#include <catch2/catch_test_macros.hpp>
#include "minigit/move.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/worktree.hpp"
#include "minigit/worktree_update.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
#include <sys/stat.h>
#include <utility>
using namespace minigit;
namespace {
using MoveFiles = std::map<std::string, std::pair<std::string, std::string>>;
ObjectId move_commit(const Repository& repo, const MoveFiles& files) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Snapshot snapshot;
    for (const auto& [path, file] : files)
        snapshot.emplace(path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    return store.write(ObjectType::Commit, encode_commit({write_snapshot(repo, snapshot), {},
        "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "fixture"}));
}
void move_file(const Repository& repo, std::string_view path, std::string_view bytes, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(bytes), permissions);
}
void move_install(const Repository& repo, const ObjectId& id) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Index index;
    for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, id).tree)) {
        move_file(repo, path, to_string(store.read(state.id).payload), state.mode == "100755" ? 0755U : 0644U);
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
std::map<std::string, Bytes> move_metadata(const Repository& repo, bool include_index = true) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && (include_index || path != "index") && entry.is_regular_file())
            result.emplace(path, read_file(entry.path()));
    }
    return result;
}
std::size_t move_objects(const Repository& repo) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects"))
        if (entry.is_regular_file()) ++count;
    return count;
}
IndexEntry move_index_entry(const Repository& repo, std::string_view path) {
    const auto index = load_index(repo);
    const auto found = std::find_if(index.begin(), index.end(), [&](const auto& entry) { return entry.path == path; });
    REQUIRE(found != index.end());
    return *found;
}
class MoveCwd {
public:
    explicit MoveCwd(const std::filesystem::path& path) : before_(std::filesystem::current_path()) { std::filesystem::current_path(path); }
    ~MoveCwd() { std::error_code ignored; std::filesystem::current_path(before_, ignored); }
private:
    std::filesystem::path before_;
};
}
TEST_CASE("Move preserves distinct staged and working bytes executable modes and full file permissions", "[move]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = move_commit(repo, {{"file", {"A", "100644"}}, {"stay", {"same", "100644"}}}); move_install(repo, a);
    std::uint32_t working_permissions = 04751;
    SECTION("staged nonexecutable B and executable working C remain distinct") {
        move_file(repo, "file", "B"); add_paths(repo, {repo.root() / "file"});
    }
    SECTION("staged executable B and nonexecutable working C remain distinct") {
        move_file(repo, "file", "B", 0755); add_paths(repo, {repo.root() / "file"}); working_permissions = 04640;
    }
    move_file(repo, "file", "C", working_permissions);
    const auto staged = move_index_entry(repo, "file");
    const auto working = read_worktree_file(repo, "file")->state;
    const auto stable = move_metadata(repo, false);
    move_path(repo, repo.root() / "file", repo.root() / "new/deep/moved");
    REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
    REQUIRE(move_index_entry(repo, "new/deep/moved") == IndexEntry{"new/deep/moved", staged.mode, staged.id});
    REQUIRE(to_string(ObjectStore(repo.metadata() / "objects").read(staged.id).payload) == "B");
    REQUIRE(read_worktree_file(repo, "new/deep/moved")->state == working);
    REQUIRE(to_string(read_file(repo.root() / "new/deep/moved")) == "C");
    REQUIRE(read_worktree_file(repo, "new/deep/moved")->data.permissions == working_permissions);
    REQUIRE((static_cast<std::uint32_t>(std::filesystem::status(repo.root() / "new/deep").permissions()) & 07777) == 0755);
    REQUIRE(move_metadata(repo, false) == stable);
    REQUIRE(to_string(read_file(repo.root() / "stay")) == "same");
}
TEST_CASE("Move rejects destination collisions and existing directories append source basename only once", "[move]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = move_commit(repo, {{"file", {"A", "100644"}}, {"tree/child", {"child", "100644"}}}); move_install(repo, a);
    auto source = repo.root() / "file";
    auto destination = repo.root() / "dest";
    bool succeeds = false;
    bool directory = false;
    SECTION("same-byte existing file is still a collision") { move_file(repo, "dest", "A"); }
    SECTION("a distinct existing file is not overwritten") { move_file(repo, "dest", "private"); }
    SECTION("an Index-only destination cannot be overwritten") {
        RepoLock lock(repo.metadata()); auto index = load_index(repo); const auto entry = index.front();
        index.push_back({"dest", entry.mode, entry.id}); save_index(repo, index);
    }
    SECTION("an Index destination subtree cannot be overwritten even when work is absent") {
        RepoLock lock(repo.metadata()); auto index = load_index(repo); const auto entry = index.front();
        index.push_back({"dest/child", entry.mode, entry.id}); save_index(repo, index);
    }
    SECTION("existing destination directory receives a file basename and preserves unrelated contents") {
        move_file(repo, "dest/private", "private"); REQUIRE(::chmod((repo.root() / "dest").c_str(), 0711) == 0); succeeds = true;
    }
    SECTION("existing destination directory receives a directory basename") {
        std::filesystem::create_directory(repo.root() / "dest"); source = repo.root() / "tree"; directory = true; succeeds = true;
    }
    SECTION("a preexisting empty final directory is a collision without another basename append") {
        std::filesystem::create_directories(repo.root() / "dest/file");
    }
    const auto before = move_metadata(repo);
    const auto stable = move_metadata(repo, false);
    const auto objects = move_objects(repo);
    const auto destination_file = std::filesystem::is_regular_file(destination) ?
        std::optional(read_file_data(destination)) : std::optional<FileData>{};
    if (succeeds) {
        move_path(repo, source, destination);
        REQUIRE_FALSE(std::filesystem::exists(source));
        if (directory) REQUIRE(to_string(read_file(repo.root() / "dest/tree/child")) == "child");
        else {
            REQUIRE(to_string(read_file(repo.root() / "dest/file")) == "A");
            REQUIRE(to_string(read_file(repo.root() / "dest/private")) == "private");
            REQUIRE((static_cast<std::uint32_t>(std::filesystem::status(repo.root() / "dest").permissions()) & 07777) == 0711);
        }
        REQUIRE(move_metadata(repo, false) == stable);
    } else {
        REQUIRE_THROWS_AS(move_path(repo, source, destination), Error);
        REQUIRE(move_metadata(repo) == before);
        REQUIRE(move_objects(repo) == objects);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "A");
        REQUIRE(to_string(read_file(repo.root() / "tree/child")) == "child");
        if (destination_file) {
            REQUIRE(read_file_data(destination).bytes == destination_file->bytes);
            REQUIRE(read_file_data(destination).permissions == destination_file->permissions);
        }
    }
}
TEST_CASE("Move directory inventory protects unknown ignored children and empty directories without partial publication", "[move]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = move_commit(repo, {{"tree/deep/file", {"A", "100644"}}, {"tree/other", {"other", "100644"}}, {"stay", {"same", "100644"}}}); move_install(repo, a);
    move_file(repo, "tree/deep/file", "dirty", 04751); move_file(repo, "tree/other", "dirty-other", 0640);
    REQUIRE(::chmod((repo.root() / "tree").c_str(), 0700) == 0);
    REQUIRE(::chmod((repo.root() / "tree/deep").c_str(), 0750) == 0);
    bool succeeds = false;
    bool missing = false;
    bool empty = false;
    bool link = false;
    SECTION("known multi-file subtree moves and new directory permissions default to 0755") { succeeds = true; }
    SECTION("an ignored untracked file is never authorized by tracked descendants") {
        move_file(repo, "tree/private", "private"); move_file(repo, ".minigitignore", "tree/private\n");
    }
    SECTION("unknown empty directories cannot be removed with the source tree") { std::filesystem::create_directory(repo.root() / "tree/empty"); empty = true; }
    SECTION("an unknown symlink descendant is rejected by the shared inventory") {
        move_file(repo, "outside", "private"); std::filesystem::create_symlink(repo.root() / "outside", repo.root() / "tree/link"); link = true;
    }
    SECTION("a missing selected work leaf rejects the whole directory move") { std::filesystem::remove(repo.root() / "tree/other"); missing = true; }
    const auto before = move_metadata(repo);
    const auto stable = move_metadata(repo, false);
    const auto old_file = move_index_entry(repo, "tree/deep/file");
    if (succeeds) {
        move_path(repo, repo.root() / "tree", repo.root() / "new/renamed");
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "tree"));
        REQUIRE(move_index_entry(repo, "new/renamed/deep/file") == IndexEntry{"new/renamed/deep/file", old_file.mode, old_file.id});
        REQUIRE(to_string(read_file(repo.root() / "new/renamed/deep/file")) == "dirty");
        REQUIRE(read_worktree_file(repo, "new/renamed/deep/file")->data.permissions == 04751);
        REQUIRE(to_string(read_file(repo.root() / "new/renamed/other")) == "dirty-other");
        REQUIRE(read_worktree_file(repo, "new/renamed/other")->data.permissions == 0640);
        REQUIRE((static_cast<std::uint32_t>(std::filesystem::status(repo.root() / "new/renamed").permissions()) & 07777) == 0755);
        REQUIRE((static_cast<std::uint32_t>(std::filesystem::status(repo.root() / "new/renamed/deep").permissions()) & 07777) == 0755);
        REQUIRE(move_metadata(repo, false) == stable);
    } else {
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "tree", repo.root() / "new/renamed"), Error);
        REQUIRE(move_metadata(repo) == before);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "new"));
        REQUIRE(to_string(read_file(repo.root() / "tree/deep/file")) == "dirty");
        REQUIRE(read_worktree_file(repo, "tree/deep/file")->data.permissions == 04751);
        if (!missing) REQUIRE(to_string(read_file(repo.root() / "tree/other")) == "dirty-other");
        else REQUIRE_FALSE(std::filesystem::exists(repo.root() / "tree/other"));
        if (empty) REQUIRE(std::filesystem::is_directory(repo.root() / "tree/empty"));
        if (link) REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(repo.root() / "tree/link")));
        if (std::filesystem::exists(repo.root() / "tree/private")) REQUIRE(to_string(read_file(repo.root() / "tree/private")) == "private");
        if (std::filesystem::exists(repo.root() / "outside")) REQUIRE(to_string(read_file(repo.root() / "outside")) == "private");
        // Capturing C precedes shared inventory; unreachable immutable blobs are
        // permitted on late failure, while every mutable path stays unchanged.
    }
    REQUIRE(to_string(read_file(repo.root() / "stay")) == "same");
}
TEST_CASE("Move rejects self overlapping paths final Index topology and selected conflicts while retaining other stages", "[move]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = move_commit(repo, {{"file", {"A", "100644"}}, {"tree/child", {"child", "100644"}}}); move_install(repo, a);
    SECTION("self descendants ancestors and repository roots cannot be used") {
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "file", repo.root() / "file"), Error);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "tree", repo.root() / "tree/sub"), Error);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "tree/child", repo.root() / "tree"), Error);
        REQUIRE_THROWS_AS(move_path(repo, repo.root(), repo.root() / "new"), Error);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "file", repo.root()), Error);
        REQUIRE(move_metadata(repo) == before);
    }
    SECTION("staged ancestor DF conflicts reject before C is captured") {
        { RepoLock lock(repo.metadata()); auto index = load_index(repo); index.push_back({"blocker", "100644", index.front().id}); save_index(repo, index); }
        move_file(repo, "file", "uncaptured C");
        const auto before = move_metadata(repo);
        const auto objects = move_objects(repo);
        try {
            move_path(repo, repo.root() / "file", repo.root() / "blocker/child");
            FAIL("unselected Index parent must reject before reading or storing C");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("索引文件与目录路径冲突") != std::string::npos);
        }
        REQUIRE(move_metadata(repo) == before);
        REQUIRE(move_objects(repo) == objects);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "uncaptured C");
    }
    SECTION("selected conflict stages reject even when source work is a regular file") {
        { RepoLock lock(repo.metadata()); const auto index = load_index(repo); save_index(repo, {{"file", "100644", index.front().id, 1}, {"file", "100755", index.front().id, 3}, index.back()}); }
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "file", repo.root() / "moved"), Error);
        REQUIRE(move_metadata(repo) == before);
    }
    SECTION("selected parent child conflict topology also rejects") {
        { RepoLock lock(repo.metadata()); const auto index = load_index(repo); save_index(repo, {index.front(), {"tree", "100644", index.back().id, 2}, {"tree/child", "100644", index.back().id, 3}}); }
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "tree", repo.root() / "moved"), Error);
        REQUIRE(move_metadata(repo) == before);
    }
    SECTION("unselected directory file conflict stages are preserved during a normal move") {
        Index expected;
        { RepoLock lock(repo.metadata()); auto index = load_index(repo); index.push_back({"conflict", "100644", index.front().id, 2}); index.push_back({"conflict/child", "100644", index.front().id, 3}); save_index(repo, index); expected = load_index(repo); }
        for (auto& entry : expected) if (entry.path == "file") entry.path = "moved";
        move_path(repo, repo.root() / "file", repo.root() / "moved");
        REQUIRE(load_index(repo) == decode_index(encode_index(expected)));
    }
}
TEST_CASE("Move validates every Index object paths unsafe nodes and pending state before mutable publication", "[move]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = move_commit(repo, {{"file", {"A", "100644"}}, {"tree/child", {"child", "100644"}}}); move_install(repo, a);
    auto source = repo.root() / "file";
    auto destination = repo.root() / "moved";
    SECTION("unselected conflict IDs must be real typed blobs") {
        const auto old = load_index(repo);
        for (const auto invalid : {a, ObjectId{}}) {
            { RepoLock lock(repo.metadata()); auto index = old; index.push_back({"unselected", "100644", invalid, 3}); save_index(repo, index); }
            const auto before = move_metadata(repo);
            REQUIRE_THROWS_AS(move_path(repo, source, destination), Error);
            REQUIRE(move_metadata(repo) == before);
        }
    }
    SECTION("a corrupt Index is never silently rebuilt") {
        atomic_write(repo.metadata() / "index", to_bytes("damaged"));
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, source, destination), Error);
        REQUIRE(move_metadata(repo) == before);
    }
    SECTION("unknown sources and illegal lexical paths reject") {
        move_file(repo, "unknown", "private");
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "unknown", destination), Error);
        REQUIRE_THROWS_AS(move_path(repo, source, repo.root().parent_path() / "outside"), Error);
        REQUIRE_THROWS_AS(move_path(repo, source, repo.root() / ".minigit/index"), Error);
        REQUIRE_THROWS_AS(move_path(repo, source, repo.root() / std::string(4097, 'x')), Error);
        REQUIRE(move_metadata(repo) == before);
    }
    SECTION("mapped child length and depth are validated before storing C") {
        std::string deep(31, 'x');
        for (unsigned i = 1; i < 128; ++i) deep += "/" + std::string(31, 'x');
        REQUIRE(deep.size() == 4095);
        move_file(repo, "tree/child", "uncaptured child");
        const auto before = move_metadata(repo);
        const auto objects = move_objects(repo);
        try {
            move_path(repo, repo.root() / "tree", repo.root() / deep);
            FAIL("mapped child must exceed checkout path limits");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("4096 字节或 128 分量限制") != std::string::npos);
        }
        REQUIRE(move_metadata(repo) == before);
        REQUIRE(move_objects(repo) == objects);
    }
    SECTION("source symlink cannot move or touch its target") {
        std::filesystem::remove(source); move_file(repo, "outside", "private"); std::filesystem::create_symlink(repo.root() / "outside", source);
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, source, destination), Error);
        REQUIRE(move_metadata(repo) == before);
        REQUIRE(std::filesystem::is_symlink(source));
        REQUIRE(to_string(read_file(repo.root() / "outside")) == "private");
    }
    SECTION("destination symlink and FIFO are rejected without following or opening them") {
        move_file(repo, "outside", "private"); std::filesystem::create_symlink(repo.root() / "outside", destination);
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, source, destination), Error);
        std::filesystem::remove(destination); REQUIRE(::mkfifo(destination.c_str(), 0600) == 0);
        REQUIRE_THROWS_AS(move_path(repo, source, destination), Error);
        REQUIRE(move_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "outside")) == "private");
    }
    SECTION("source FIFO and blocked destination parent are rejected") {
        std::filesystem::remove(source); REQUIRE(::mkfifo(source.c_str(), 0600) == 0);
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, source, destination), Error);
        std::filesystem::remove(source); move_file(repo, "file", "A"); move_file(repo, "blocked", "private");
        REQUIRE_THROWS_AS(move_path(repo, source, repo.root() / "blocked/child"), Error);
        REQUIRE(move_metadata(repo) == before);
    }
    SECTION("pending transaction blocks moving without changing any source") {
        atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, source, destination), Error);
        REQUIRE(move_metadata(repo) == before);
        REQUIRE(to_string(read_file(source)) == "A");
    }
}
TEST_CASE("Move supports unborn staged files detached HEAD lexical optionlike paths and rejects missing work", "[move]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = move_commit(repo, {{"file", {"A", "100644"}}});
    SECTION("unborn staged B and dirty C move without creating a branch or reflog") {
        move_file(repo, "file", "B"); add_paths(repo, {repo.root() / "file"}); move_file(repo, "file", "C", 0751);
        const auto staged = move_index_entry(repo, "file");
        const auto stable = move_metadata(repo, false);
        move_path(repo, repo.root() / "file", repo.root() / "moved");
        REQUIRE(move_index_entry(repo, "moved") == IndexEntry{"moved", staged.mode, staged.id});
        REQUIRE(to_string(read_file(repo.root() / "moved")) == "C");
        REQUIRE(read_worktree_file(repo, "moved")->data.permissions == 0751);
        REQUIRE(read_head(repo) == Head{"refs/heads/main", std::nullopt});
        REQUIRE(move_metadata(repo, false) == stable);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/HEAD"));
    }
    SECTION("detached HEAD is preserved while relative lexical paths and optionlike names work") {
        move_install(repo, a); { RepoLock lock(repo.metadata()); set_head(repo, a.hex()); }
        std::filesystem::create_symlink(repo.root() / "missing", repo.root() / "link");
        const auto stable = move_metadata(repo, false);
        MoveCwd cwd(repo.root());
        move_path(repo, "link/../file", "- spaced");
        REQUIRE(to_string(read_file(repo.root() / "- spaced")) == "A");
        REQUIRE(read_head(repo) == Head{std::nullopt, a});
        REQUIRE(move_metadata(repo, false) == stable);
        REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(repo.root() / "link")));
    }
    SECTION("a staged file missing from work cannot become an Index-only move") {
        move_install(repo, a); std::filesystem::remove(repo.root() / "file");
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "file", repo.root() / "moved"), Error);
        REQUIRE(move_metadata(repo) == before);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "moved"));
    }
    SECTION("a tracked file replaced by a directory is not a supported directory source") {
        move_install(repo, a); std::filesystem::remove(repo.root() / "file"); std::filesystem::create_directory(repo.root() / "file");
        const auto before = move_metadata(repo);
        REQUIRE_THROWS_AS(move_path(repo, repo.root() / "file", repo.root() / "moved"), Error);
        REQUIRE(move_metadata(repo) == before);
        REQUIRE(std::filesystem::is_directory(repo.root() / "file"));
    }
}
TEST_CASE("Move transaction recovery restores or finishes dirty work and staged directory mappings together", "[move][transaction]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = move_commit(repo, {{"a/file", {"A", "100644"}}, {"a/sub/other", {"other", "100644"}}}); move_install(repo, a);
    move_file(repo, "a/file", "B", 0755); add_paths(repo, {repo.root() / "a/file"}); move_file(repo, "a/file", "C", 04640);
    REQUIRE(::chmod((repo.root() / "a").c_str(), 0700) == 0);
    const auto before = move_metadata(repo);
    const auto stable = move_metadata(repo, false);
    auto after_index = load_index(repo);
    for (auto& entry : after_index) entry.path.replace(0, 1, "moved");
    const auto staged = move_index_entry(repo, "a/file");
    RepoLock lock(repo.metadata());
    const auto c = capture_work_node(repo, "a/file");
    const auto other = capture_work_node(repo, "a/sub/other");
    TransactionPlan plan;
    plan_index_update(repo, plan, after_index);
    plan_worktree_update(repo, plan, {{"a", {}}, {"a/file", {}}, {"a/sub/other", {}},
        {"moved/file", {FileState{"100644", *c.blob}, c.permissions}},
        {"moved/sub/other", {FileState{"100644", *other.blob}, other.permissions}}}, {"a/file", "a/sub/other"});
    bool committed = false;
    std::string stop;
    SECTION("Prepared after partial destination publication restores source C Index B and directory permissions") { stop = "work.write:moved/file"; }
    SECTION("Committed finishes destination C and mapped Index B without HEAD or log changes") { stop = "journal.committed"; committed = true; }
    REQUIRE_THROWS_AS(execute_transaction(repo, plan, [&](std::size_t, std::string_view label) {
        if (label == stop) throw Error("injected move stop");
    }), Error);
    recover_transaction(repo); recover_transaction(repo);
    if (committed) {
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "a"));
        REQUIRE(move_index_entry(repo, "moved/file") == IndexEntry{"moved/file", staged.mode, staged.id});
        REQUIRE(to_string(read_file(repo.root() / "moved/file")) == "C");
        REQUIRE(read_worktree_file(repo, "moved/file")->data.permissions == 04640);
        REQUIRE(to_string(read_file(repo.root() / "moved/sub/other")) == "other");
        REQUIRE(move_metadata(repo, false) == stable);
    } else {
        REQUIRE(move_metadata(repo) == before);
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "moved"));
        REQUIRE(to_string(read_file(repo.root() / "a/file")) == "C");
        REQUIRE(read_worktree_file(repo, "a/file")->data.permissions == 04640);
        REQUIRE((static_cast<std::uint32_t>(std::filesystem::status(repo.root() / "a").permissions()) & 07777) == 0700);
    }
    REQUIRE_NOTHROW(require_no_transaction(repo));
}
