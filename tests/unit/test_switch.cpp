#include <catch2/catch_test_macros.hpp>
#include "minigit/switch.hpp"
#include "minigit/branches.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/tags.hpp"
#include "minigit/worktree.hpp"
#include "temp_directory.hpp"
#include <map>
#include <sys/stat.h>
#include <utility>
using namespace minigit;
namespace {
using SwitchFiles = std::map<std::string, std::pair<std::string, std::string>>;
ObjectId switch_commit(const Repository& repo, const SwitchFiles& files, std::string_view message) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Snapshot snapshot;
    for (const auto& [path, file] : files) snapshot.emplace(path, FileState{file.second, store.write(ObjectType::Blob, to_bytes(file.first))});
    const auto tree = write_snapshot(repo, snapshot);
    return store.write(ObjectType::Commit, encode_commit({tree, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(message)}));
}
void switch_file(const Repository& repo, std::string_view path, std::string_view bytes, std::uint32_t permissions = 0644) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(bytes), permissions);
}
void switch_install(const Repository& repo, const ObjectId& id) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto snapshot = read_snapshot(repo, read_commit(repo, id).tree);
    Index index;
    for (const auto& [path, state] : snapshot) {
        switch_file(repo, path, to_string(store.read(state.id).payload), state.mode == "100755" ? 0755U : 0644U);
        index.push_back({path, state.mode, state.id});
    }
    save_index(repo, index);
    update_ref(repo, "refs/heads/main", std::nullopt, id);
    const auto logs = encode_reflog({{std::nullopt, id, "fixture"}});
    detail::ensure_directory(repo.metadata() / "logs/refs/heads");
    atomic_write(repo.metadata() / "logs/HEAD", logs);
    atomic_write(repo.metadata() / "logs/refs/heads/main", logs);
}
std::map<std::string, Bytes> switch_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && entry.is_regular_file()) result.emplace(path, read_file(entry.path()));
    }
    return result;
}
SwitchOptions to_branch(std::string name) { return {std::nullopt, false, std::move(name)}; }
}
TEST_CASE("Switch carries staged and unstaged changes when HEAD or Index already matches the target", "[switch]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = switch_commit(repo, {{"file", {"A", "100644"}}}, "A");
    switch_install(repo, a);
    ObjectId target;
    SECTION("A equals D preserves B and C including a mode change") {
        target = a;
        switch_file(repo, "file", "B", 0751);
        add_paths(repo, {repo.root() / "file"});
        switch_file(repo, "file", "C", 0740);
    }
    SECTION("B equals D preserves C and the exact Index bytes") {
        target = switch_commit(repo, {{"file", {"B", "100644"}}}, "B");
        switch_file(repo, "file", "B");
        add_paths(repo, {repo.root() / "file"});
        switch_file(repo, "file", "C", 0640);
    }
    create_branch(repo, "topic", target.hex());
    const auto index = read_file(repo.metadata() / "index");
    const auto working = read_worktree_file(repo, "file")->data;
    const auto mainlog = read_file(repo.metadata() / "logs/refs/heads/main");
    const auto topiclog = read_file(repo.metadata() / "logs/refs/heads/topic");
    REQUIRE(switch_worktree(repo, to_branch("topic")) == Head{"refs/heads/topic", target});
    REQUIRE(read_file(repo.metadata() / "index") == index);
    REQUIRE(read_worktree_file(repo, "file")->data.bytes == working.bytes);
    REQUIRE(read_worktree_file(repo, "file")->data.permissions == working.permissions);
    REQUIRE(read_file(repo.metadata() / "logs/refs/heads/main") == mainlog);
    REQUIRE(read_file(repo.metadata() / "logs/refs/heads/topic") == topiclog);
    { RepoLock lock(repo.metadata()); REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{a, target, "switch:topic"}); }
}
TEST_CASE("Switch rejects incompatible staged working or deleted content before publishing any state", "[switch]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = switch_commit(repo, {{"file", {"A", "100644"}}}, "A");
    const auto d = switch_commit(repo, {{"file", {"D", "100644"}}}, "D");
    switch_install(repo, a);
    create_branch(repo, "topic", d.hex());
    std::optional<Bytes> work;
    SECTION("A B C and D all differ") {
        switch_file(repo, "file", "B"); add_paths(repo, {repo.root() / "file"}); switch_file(repo, "file", "C");
        work = to_bytes("C");
    }
    SECTION("B differs from both A and D even when work equals B") {
        switch_file(repo, "file", "B"); add_paths(repo, {repo.root() / "file"}); work = to_bytes("B");
    }
    SECTION("clean Index with dirty work") { switch_file(repo, "file", "C"); work = to_bytes("C"); }
    SECTION("work matching D cannot conceal an unstaged change") { switch_file(repo, "file", "D"); work = to_bytes("D"); }
    SECTION("a tracked local deletion is preserved by rejecting a needed rewrite") { std::filesystem::remove(repo.root() / "file"); }
    const auto before = switch_metadata(repo);
    REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("topic")), Error);
    REQUIRE(switch_metadata(repo) == before);
    if (work) REQUIRE(read_file(repo.root() / "file") == *work);
    else REQUIRE_FALSE(std::filesystem::exists(repo.root() / "file"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
TEST_CASE("Switch restores clean content executable modes empty files and directory file conversions", "[switch]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    ObjectId a;
    ObjectId d;
    SECTION("clean rewrite restores mode and distinguishes an empty file from deletion") {
        a = switch_commit(repo, {{"file", {"A", "100644"}}, {"deleted", {"old", "100644"}}}, "A");
        d = switch_commit(repo, {{"file", {"D", "100755"}}, {"empty", {"", "100644"}}}, "D");
    }
    SECTION("file becomes a directory") {
        a = switch_commit(repo, {{"item", {"old", "100644"}}}, "A");
        d = switch_commit(repo, {{"item/deep/child", {"new", "100644"}}}, "D");
    }
    SECTION("directory becomes a file") {
        a = switch_commit(repo, {{"item/deep/child", {"old", "100644"}}, {"item/other", {"other", "100644"}}}, "A");
        d = switch_commit(repo, {{"item", {"new", "100644"}}}, "D");
    }
    switch_install(repo, a);
    create_branch(repo, "topic", d.hex());
    REQUIRE(switch_worktree(repo, to_branch("topic")) == Head{"refs/heads/topic", d});
    { RepoLock lock(repo.metadata()); REQUIRE(index_snapshot(repo) == read_snapshot(repo, read_commit(repo, d).tree)); }
    REQUIRE(scan_worktree(repo) == read_snapshot(repo, read_commit(repo, d).tree));
    if (std::filesystem::exists(repo.root() / "file")) {
        REQUIRE(read_worktree_file(repo, "file")->data.permissions == 0755);
        REQUIRE(read_file(repo.root() / "empty").empty());
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "deleted"));
    }
}
TEST_CASE("Switch protects unknown ignored files parent collisions and unknown empty directories", "[switch]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = switch_commit(repo, {}, "A");
    switch_install(repo, a);
    ObjectId d;
    SECTION("an untracked destination is rejected even with matching target bytes") {
        d = switch_commit(repo, {{"new", {"D", "100644"}}}, "D"); switch_file(repo, "new", "D");
    }
    SECTION("ignored destinations remain protected") {
        d = switch_commit(repo, {{"new", {"D", "100644"}}}, "D"); switch_file(repo, ".minigitignore", "new\n"); switch_file(repo, "new", "unknown");
    }
    SECTION("an unknown parent file cannot be converted to a directory") {
        d = switch_commit(repo, {{"item/child", {"D", "100644"}}}, "D"); switch_file(repo, "item", "unknown");
    }
    SECTION("an unknown empty directory cannot be replaced by a target file") {
        d = switch_commit(repo, {{"item", {"D", "100644"}}}, "D"); std::filesystem::create_directory(repo.root() / "item");
    }
    create_branch(repo, "topic", d.hex());
    const auto before = switch_metadata(repo);
    REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("topic")), Error);
    REQUIRE(switch_metadata(repo) == before);
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
}
TEST_CASE("Switch validates carried Index topology and avoids reading unchanged or unrelated unsafe work paths", "[switch]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    SECTION("carried staged paths and the target cannot exceed the combined Index snapshot budget") {
        const auto a = switch_commit(repo, {}, "empty A");
        const auto d = switch_commit(repo, {{"new", {"D", "100644"}}}, "D");
        switch_install(repo, a);
        {
            RepoLock lock(repo.metadata());
            const auto shared = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("B"));
            Index staged;
            staged.reserve(100000);
            for (unsigned i = 0; i < 100000; ++i) {
                const auto number = std::to_string(i);
                auto name = "b" + std::string(6 - number.size(), '0') + number;
                staged.push_back({std::move(name), "100644", shared});
            }
            save_index(repo, std::move(staged));
        }
        create_branch(repo, "topic", d.hex());
        // This obstructing node would fail a worktree read; the combined Index
        // budget must reject earlier, after carrying all 100000 staged paths.
        REQUIRE(::mkfifo((repo.root() / "new").c_str(), 0600) == 0);
        const auto before = switch_metadata(repo);
        try {
            (void)switch_worktree(repo, to_branch("topic"));
            FAIL("combined Index snapshot budget must reject 100001 entries");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("索引条目超过快照限制") != std::string::npos);
        }
        REQUIRE(switch_metadata(repo) == before);
        REQUIRE(std::filesystem::is_fifo(std::filesystem::symlink_status(repo.root() / "new")));
        REQUIRE_FALSE(std::filesystem::exists(repo.root() / "b000000"));
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "transaction"));
    }
    SECTION("carried staged child cannot coexist with a new target parent file") {
        const auto a = switch_commit(repo, {{"item/old", {"old", "100644"}}}, "A");
        const auto d = switch_commit(repo, {{"item", {"D", "100644"}}}, "D");
        switch_install(repo, a);
        switch_file(repo, "item/staged", "staged"); add_paths(repo, {repo.root() / "item/staged"});
        create_branch(repo, "topic", d.hex());
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("topic")), Error);
        REQUIRE(switch_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "item/staged")) == "staged");
    }
    SECTION("unchanged tracked symlink and unrelated FIFO are not scanned") {
        const auto a = switch_commit(repo, {{"stay", {"same", "100644"}}}, "A");
        const auto d = switch_commit(repo, {{"stay", {"same", "100644"}}, {"dir/new", {"new", "100644"}}}, "D");
        switch_install(repo, a);
        std::filesystem::remove(repo.root() / "stay");
        std::filesystem::create_symlink(repo.root() / "outside", repo.root() / "stay");
        REQUIRE(::mkfifo((repo.root() / "fifo").c_str(), 0600) == 0);
        create_branch(repo, "topic", d.hex());
        REQUIRE(switch_worktree(repo, to_branch("topic")).id == d);
        REQUIRE(std::filesystem::is_symlink(repo.root() / "stay"));
        REQUIRE(to_string(read_file(repo.root() / "dir/new")) == "new");
    }
    SECTION("directory conversion protects ignored descendants outside the matrix path union") {
        const auto a = switch_commit(repo, {{"item/tracked", {"old", "100644"}}}, "A");
        const auto d = switch_commit(repo, {{"item", {"D", "100644"}}}, "D");
        switch_install(repo, a);
        switch_file(repo, ".minigitignore", "item/ignored\n");
        switch_file(repo, "item/ignored", "unknown");
        std::filesystem::create_directory(repo.root() / "item/unknown-empty");
        create_branch(repo, "topic", d.hex());
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("topic")), Error);
        REQUIRE(switch_metadata(repo) == before);
        REQUIRE(to_string(read_file(repo.root() / "item/ignored")) == "unknown");
        REQUIRE(std::filesystem::is_directory(repo.root() / "item/unknown-empty"));
        REQUIRE(to_string(read_file(repo.root() / "item/tracked")) == "old");
    }
}
TEST_CASE("Switch creates a branch atomically preserves compatible dirt and never publishes a failed creation", "[switch]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = switch_commit(repo, {{"file", {"A", "100644"}}}, "A");
    switch_install(repo, a);
    ObjectId d;
    bool succeeds = false;
    SECTION("new branch and changed paths publish while unchanged paths retain B and C") {
        d = switch_commit(repo, {{"file", {"A", "100644"}}, {"new", {"new", "100644"}}}, "D");
        switch_file(repo, "file", "B"); add_paths(repo, {repo.root() / "file"}); switch_file(repo, "file", "C"); succeeds = true;
    }
    SECTION("new branch is not created when the matrix would overwrite local edits") {
        d = switch_commit(repo, {{"file", {"D", "100644"}}}, "D"); switch_file(repo, "file", "C");
    }
    const auto before = switch_metadata(repo);
    const SwitchOptions options{"created", false, d.hex()};
    if (succeeds) {
        REQUIRE(switch_worktree(repo, options) == Head{"refs/heads/created", d});
        REQUIRE(read_ref(repo, "refs/heads/created") == d);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "C");
        REQUIRE(to_string(read_file(repo.root() / "new")) == "new");
        RepoLock lock(repo.metadata());
        REQUIRE(read_reflog(repo, "refs/heads/created") == Reflog{{std::nullopt, d, "branch:create"}});
        REQUIRE(read_reflog(repo, "HEAD").back() == ReflogEntry{a, d, "switch:created"});
    } else {
        REQUIRE_THROWS_AS(switch_worktree(repo, options), Error);
        REQUIRE(switch_metadata(repo) == before);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "refs/heads/created"));
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "logs/refs/heads/created"));
    }
}
TEST_CASE("Switch handles unborn branch creation detached targets short branch resolution and exact no-ops", "[switch]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    // Include the permanent lock file in the initial metadata baseline.
    { RepoLock lock(repo.metadata()); }
    SECTION("unborn current branch is a no-op and creating another unborn branch preserves Index") {
        const auto before = switch_metadata(repo);
        REQUIRE(switch_worktree(repo, to_branch("main")) == Head{"refs/heads/main", std::nullopt});
        REQUIRE(switch_metadata(repo) == before);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "index"));
        switch_file(repo, "file", "staged"); add_paths(repo, {repo.root() / "file"}); switch_file(repo, "file", "working");
        const auto index = read_file(repo.metadata() / "index");
        REQUIRE(switch_worktree(repo, {"new", false, ""}) == Head{"refs/heads/new", std::nullopt});
        REQUIRE(read_file(repo.metadata() / "index") == index);
        REQUIRE(to_string(read_file(repo.root() / "file")) == "working");
        REQUIRE_FALSE(read_ref(repo, "refs/heads/new").has_value());
        RepoLock lock(repo.metadata());
        REQUIRE(read_reflog(repo, "HEAD") == Reflog{{std::nullopt, std::nullopt, "switch:new"}});
        REQUIRE(read_reflog(repo, "refs/heads/new").empty());
    }
    SECTION("unborn HEAD cannot resolve explicit HEAD source or detach") {
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, {"new", false, "HEAD"}), Error);
        REQUIRE_THROWS_AS(switch_worktree(repo, {std::nullopt, true, ""}), Error);
        REQUIRE_THROWS_AS(switch_worktree(repo, {"main", false, ""}), Error);
        REQUIRE(switch_metadata(repo) == before);
    }
    SECTION("ordinary branch targets ignore a same-name tag while detach resolves revisions") {
        const auto a = switch_commit(repo, {{"file", {"A", "100644"}}}, "A");
        const auto d = switch_commit(repo, {{"file", {"D", "100644"}}}, "D");
        switch_install(repo, a); create_branch(repo, "topic", d.hex()); create_tag(repo, "topic", a.hex());
        REQUIRE(switch_worktree(repo, to_branch("topic")) == Head{"refs/heads/topic", d});
        const auto before = switch_metadata(repo);
        REQUIRE(switch_worktree(repo, to_branch("topic")) == Head{"refs/heads/topic", d});
        REQUIRE(switch_metadata(repo) == before);
        REQUIRE_THROWS_AS(switch_worktree(repo, {std::nullopt, true, "topic"}), Error);
        REQUIRE(switch_worktree(repo, {std::nullopt, true, "refs/tags/topic"}) == Head{std::nullopt, a});
        REQUIRE(switch_worktree(repo, {std::nullopt, true, ""}) == Head{std::nullopt, a});
    }
}
TEST_CASE("Switch rejects conflicts corrupt targets unsafe changed paths bad options and pending transactions", "[switch]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto a = switch_commit(repo, {{"file", {"A", "100644"}}}, "A");
    const auto d = switch_commit(repo, {{"file", {"D", "100644"}}}, "D");
    switch_install(repo, a); create_branch(repo, "topic", d.hex());
    SECTION("conflict stages prevent even a same-branch no-op") {
        { RepoLock lock(repo.metadata()); const auto entry = load_index(repo).front(); save_index(repo, {{entry.path, entry.mode, entry.id, 1}, {entry.path, entry.mode, entry.id, 3}}); }
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("main")), Error);
        REQUIRE(switch_metadata(repo) == before);
    }
    SECTION("corrupt index cannot be hidden by an unchanged target") {
        atomic_write(repo.metadata() / "index", to_bytes("corrupt"));
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("main")), Error);
        REQUIRE(switch_metadata(repo) == before);
    }
    SECTION("noncommit target and incomplete tree are rejected before any publication") {
        ObjectId blob;
        { RepoLock lock(repo.metadata()); blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("blob")); }
        atomic_write(repo.metadata() / "refs/heads/topic", to_bytes(blob.hex() + "\n"));
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("topic")), Error);
        REQUIRE(switch_metadata(repo) == before);
        std::filesystem::remove(ObjectStore(repo.metadata() / "objects").object_path(read_commit(repo, d).tree));
        REQUIRE_THROWS_AS(switch_worktree(repo, {"new", false, d.hex()}), Error);
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "refs/heads/new"));
    }
    SECTION("a changed tracked symlink is rejected") {
        std::filesystem::remove(repo.root() / "file"); std::filesystem::create_symlink(repo.root() / "elsewhere", repo.root() / "file");
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("topic")), Error);
        REQUIRE(switch_metadata(repo) == before);
        REQUIRE(std::filesystem::is_symlink(repo.root() / "file"));
    }
    SECTION("missing branches and incompatible option combinations are rejected") {
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, {}), Error);
        REQUIRE_THROWS_AS(switch_worktree(repo, {"new", true, "HEAD"}), Error);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("missing")), Error);
        REQUIRE_THROWS_AS(switch_worktree(repo, {"topic", false, "HEAD"}), Error);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("refs/heads/main")), Error);
        REQUIRE(switch_metadata(repo) == before);
    }
    SECTION("pending transaction blocks switching") {
        atomic_write(repo.metadata() / "transaction", to_bytes("pending"));
        const auto before = switch_metadata(repo);
        REQUIRE_THROWS_AS(switch_worktree(repo, to_branch("topic")), Error);
        REQUIRE(switch_metadata(repo) == before);
    }
}
