#include <catch2/catch_test_macros.hpp>
#include "minigit/inspection.hpp"
#include "minigit/diff.hpp"
#include "minigit/error.hpp"
#include "minigit/format.hpp"
#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/worktree.hpp"
#include "temp_directory.hpp"
#include <sys/stat.h>
#include <utility>
using namespace minigit;
namespace {
void file(const Repository& repo, std::string_view path, std::string_view bytes) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write(target, to_bytes(bytes));
}
void identity(const Repository& repo) {
    configure_identity(repo, "user.name", "差异作者");
    configure_identity(repo, "user.email", "diff@example.test");
}
std::map<std::string, Bytes> metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata()))
        if (entry.is_regular_file()) result.emplace(entry.path().lexically_relative(repo.metadata()).generic_string(), read_file(entry.path()));
    return result;
}
void initialize_lock(const Repository& repo) { RepoLock lock(repo.metadata()); }
ObjectId commit_snapshot(const Repository& repo, const Snapshot& snapshot, std::vector<ObjectId> parents = {}) {
    RepoLock lock(repo.metadata());
    const auto tree = write_snapshot(repo, snapshot);
    return ObjectStore(repo.metadata() / "objects").write(ObjectType::Commit,
        encode_commit({tree, std::move(parents), "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "fixture"}));
}
std::string expected(std::string_view path, std::string_view before, std::string_view after) {
    const auto old_bytes = to_bytes(before), new_bytes = to_bytes(after);
    return format_file_diff(path, DiffFileView{"100644", old_bytes}, DiffFileView{"100644", new_bytes});
}
}
TEST_CASE("Inspection selects HEAD index worktree and explicit revision directions without writes", "[inspection]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    identity(repo);
    file(repo, "file", "A\n");
    add_paths(repo, {repo.root() / "file"});
    const auto a = create_commit(repo, "A");
    file(repo, "file", "B\n");
    add_paths(repo, {repo.root() / "file"});
    const auto b = commit_snapshot(repo, index_snapshot(repo), {a});
    file(repo, "file", "C\n");
    file(repo, "untracked", "untracked\n");
    std::filesystem::create_symlink(repo.root() / "file", repo.root() / "unrelated-link");
    const auto original = metadata(repo);
    REQUIRE(repository_diff(repo) == expected("file", "B\n", "C\n"));
    REQUIRE(repository_diff(repo, {.staged = true}) == expected("file", "A\n", "B\n"));
    REQUIRE(repository_diff(repo, {.revisions = {a.hex()}}) == expected("file", "A\n", "C\n"));
    REQUIRE(repository_diff(repo, {.revisions = {a.hex(), b.hex()}}) == expected("file", "A\n", "B\n"));
    REQUIRE(repository_diff(repo, {.staged = true, .revisions = {a.hex()}}) == expected("file", "A\n", "B\n"));
    REQUIRE(repository_diff(repo, {.staged = true, .revisions = {b.hex()}}).empty());
    REQUIRE(repository_diff(repo, {.revisions = {"main"}}) == expected("file", "A\n", "C\n"));
    REQUIRE(metadata(repo) == original);
    SECTION("single revision includes newly indexed paths") {
        std::filesystem::remove(repo.root() / "unrelated-link");
        file(repo, "new", "new staged\n");
        add_paths(repo, {repo.root() / "new"});
        file(repo, "new", "new working\n");
        const auto output = repository_diff(repo, {.revisions = {a.hex()}});
        REQUIRE(output.find("new file mode 100644\n") != std::string::npos);
        REQUIRE(output.find("+new working\n") != std::string::npos);
        REQUIRE(output.find("untracked") == std::string::npos);
    }
    SECTION("single revision compares a base path removed from index and rebuilt in worktree") {
        std::filesystem::remove(repo.root() / "file");
        add_paths(repo, {repo.root() / "file"});
        file(repo, "file", "rebuilt\n");
        REQUIRE(repository_diff(repo).empty());
        REQUIRE(repository_diff(repo, {.revisions = {a.hex()}}) == expected("file", "A\n", "rebuilt\n"));
    }
}
TEST_CASE("Inspection handles unborn staged empty binary mode and directory file changes", "[inspection]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    initialize_lock(repo);
    REQUIRE(repository_diff(repo).empty());
    REQUIRE(repository_diff(repo, {.staged = true}).empty());
    file(repo, "empty", "");
    file(repo, "dir/child", "child\n");
    add_paths(repo, {}, true);
    const auto staged = repository_diff(repo, {.staged = true});
    REQUIRE(staged.find("diff --minigit a/empty b/empty\nnew file mode 100644\n") != std::string::npos);
    REQUIRE(staged.find("--- /dev/null\n+++ b/dir/child\n") != std::string::npos);
    identity(repo);
    const auto initial = create_commit(repo, "initial");
    SECTION("worktree mode and deletion") {
        std::filesystem::permissions(repo.root() / "empty", std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
        std::filesystem::remove(repo.root() / "dir/child");
        const auto output = repository_diff(repo);
        REQUIRE(output.find("old mode 100644\nnew mode 100755\n") != std::string::npos);
        REQUIRE(output.find("deleted file mode 100644\n") != std::string::npos);
    }
    SECTION("binary bytes are read directly from the worktree") {
        atomic_write(repo.root() / "empty", Bytes{0, 255, '\n'});
        const auto original = metadata(repo);
        REQUIRE(repository_diff(repo).find("Binary files a/empty and b/empty differ\n") != std::string::npos);
        REQUIRE(metadata(repo) == original);
    }
    SECTION("directory to file and file to directory") {
        std::filesystem::remove_all(repo.root() / "dir");
        file(repo, "dir", "replacement\n");
        REQUIRE(repository_diff(repo).find("deleted file mode 100644\n") != std::string::npos);
        add_paths(repo, {repo.root() / "dir"});
        const auto changed = repository_diff(repo, {.staged = true});
        REQUIRE(changed.find("+++ b/dir\n") != std::string::npos);
        REQUIRE(changed.find("--- a/dir/child\n+++ /dev/null\n") != std::string::npos);
        const auto next = create_commit(repo, "directory to file");
        REQUIRE_FALSE(repository_diff(repo, {.revisions = {initial.hex(), next.hex()}}).empty());
        std::filesystem::remove(repo.root() / "dir");
        file(repo, "dir/child", "restored\n");
        REQUIRE(repository_diff(repo).find("deleted file mode 100644\n") != std::string::npos);
    }
}
TEST_CASE("Inspection rejects index conflicts unsafe worktree paths and invalid requests but historical diff remains available", "[inspection]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    identity(repo);
    file(repo, "file", "base\n");
    add_paths(repo, {repo.root() / "file"});
    const auto a = create_commit(repo, "base");
    file(repo, "file", "next\n");
    add_paths(repo, {repo.root() / "file"});
    const auto b = create_commit(repo, "next");
    SECTION("conflict stages") {
        const auto id = load_index(repo)[0].id;
        { RepoLock lock(repo.metadata()); save_index(repo, {{"file", "100644", id, 1}}); }
        const auto original = metadata(repo);
        REQUIRE_THROWS_AS(repository_diff(repo), Error);
        REQUIRE_THROWS_AS(repository_diff(repo, {.staged = true}), Error);
        REQUIRE_THROWS_AS(repository_diff(repo, {.revisions = {a.hex()}}), Error);
        REQUIRE(repository_diff(repo, {.revisions = {a.hex(), b.hex()}}) == expected("file", "base\n", "next\n"));
        REQUIRE(metadata(repo) == original);
    }
    SECTION("symlink and unsupported FIFO") {
        std::filesystem::remove(repo.root() / "file");
        std::filesystem::create_symlink(repo.root() / "other", repo.root() / "file");
        const auto original = metadata(repo);
        REQUIRE_THROWS_AS(repository_diff(repo), Error);
        std::filesystem::remove(repo.root() / "file");
        REQUIRE(::mkfifo((repo.root() / "file").c_str(), 0600) == 0);
        REQUIRE_THROWS_AS(repository_diff(repo), Error);
        REQUIRE(metadata(repo) == original);
    }
    SECTION("invalid requests") {
        const auto original = metadata(repo);
        REQUIRE_THROWS_AS(repository_diff(repo, {.revisions = {"missing"}}), Error);
        REQUIRE_THROWS_AS(repository_diff(repo, {.revisions = {a.hex(), b.hex(), a.hex()}}), Error);
        REQUIRE_THROWS_AS(repository_diff(repo, {.staged = true, .revisions = {a.hex(), b.hex()}}), Error);
        REQUIRE_THROWS_AS(repository_diff(repo, {.context = 100001}), Error);
        REQUIRE(metadata(repo) == original);
    }
}
TEST_CASE("Inspection validates public snapshot paths modes and unchanged object integrity", "[inspection]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto blob = store.write(ObjectType::Blob, to_bytes("file\n"));
    const auto tree = store.write(ObjectType::Tree, encode_tree({}));
    const auto original = metadata(repo);
    const Snapshot valid{{"file", {"100644", blob}}};
    REQUIRE(diff_snapshots(repo, valid, valid).empty());
    REQUIRE(diff_snapshots(repo, valid, {{"file", {"100755", blob}}}).find("old mode 100644\nnew mode 100755\n") != std::string::npos);
    for (const auto& snapshot : std::vector<Snapshot>{
        {{"../file", {"100644", blob}}}, {{std::string(4097, 'a'), {"100644", blob}}},
        {{"file", {"120000", blob}}}, {{"file", {"100644", ObjectId{}}}}, {{"file", {"100644", tree}}},
        {{"a", {"100644", blob}}, {"a-", {"100644", blob}}, {"a/child", {"100644", blob}}}
    }) REQUIRE_THROWS_AS(diff_snapshots(repo, snapshot, snapshot), Error);
    REQUIRE(metadata(repo) == original);
    atomic_write(store.object_path(blob), to_bytes("damaged compressed blob"));
    const auto damaged = metadata(repo);
    REQUIRE_THROWS_AS(diff_snapshots(repo, valid, valid), Error);
    REQUIRE(metadata(repo) == damaged);
}
TEST_CASE("Show preserves commit formatting and displays root and first parent patches", "[inspection]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    initialize_lock(repo);
    ObjectId base, other, merge;
    Commit initial, merged;
    {
        RepoLock lock(repo.metadata());
        ObjectStore store(repo.metadata() / "objects");
        const auto a = store.write(ObjectType::Blob, to_bytes("A\n"));
        const auto b = store.write(ObjectType::Blob, to_bytes("B\n"));
        const auto c = store.write(ObjectType::Blob, to_bytes("C\n"));
        const auto tree_a = write_snapshot(repo, {{"file", {"100644", a}}});
        const auto tree_b = write_snapshot(repo, {{"file", {"100644", b}}});
        const auto tree_c = write_snapshot(repo, {{"file", {"100644", c}}});
        initial = {tree_a, {}, "Author <author@example.test> 0 +0000", "Committer <committer@example.test> 0 +0000", "title\n\nbody\n"};
        base = store.write(ObjectType::Commit, encode_commit(initial));
        other = store.write(ObjectType::Commit, encode_commit({tree_b, {base}, initial.author, initial.committer, "other"}));
        merged = {tree_c, {base, other}, initial.author, initial.committer, "merge\nbody"};
        merge = store.write(ObjectType::Commit, encode_commit(merged));
        update_ref(repo, "refs/heads/main", std::nullopt, base);
        update_ref(repo, "refs/tags/merge", std::nullopt, merge);
    }
    const auto original = metadata(repo);
    const auto full = "commit " + base.hex() + "\nauthor " + initial.author + "\ncommitter " + initial.committer + "\n\n    title\n    \n    body\n\n";
    REQUIRE(format_commit(base, initial) == full);
    REQUIRE(format_commit(base, initial, true) == base.hex().substr(0, 12) + " title\n");
    auto empty_message = initial;
    empty_message.message.clear();
    REQUIRE(format_commit(base, empty_message).ends_with("\n\n    \n\n"));
    REQUIRE(format_commit(base, empty_message, true) == base.hex().substr(0, 12) + " \n");
    auto oversized_format = initial;
    oversized_format.message.assign(max_file_size - 2048, 'x');
    oversized_format.message.replace(0, 4096, 4096, '\n');
    REQUIRE_THROWS_AS(format_commit(base, oversized_format), Error);
    oversized_format.message.clear();
    const auto root_show = to_string(show_object(repo));
    REQUIRE(root_show.starts_with(full));
    REQUIRE(root_show.find("new file mode 100644\n") != std::string::npos);
    REQUIRE(root_show.find("+A\n") != std::string::npos);
    REQUIRE(show_object(repo, "main") == show_object(repo, base.hex()));
    const auto merge_show = to_string(show_object(repo, "merge"));
    REQUIRE(merge_show.starts_with(format_commit(merge, merged)));
    REQUIRE(merge_show.find("parent " + base.hex() + "\nparent " + other.hex() + "\n") != std::string::npos);
    REQUIRE(merge_show.find("-A\n+C\n") != std::string::npos);
    REQUIRE(merge_show.find("-B\n") == std::string::npos);
    REQUIRE(metadata(repo) == original);
}
TEST_CASE("Show returns raw blob tree and revision or indexed stage file contents without writes", "[inspection]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    initialize_lock(repo);
    const Bytes binary{0, 255, '\n', 0};
    ObjectId blob, tree, commit, one, two, three;
    {
        RepoLock lock(repo.metadata());
        ObjectStore store(repo.metadata() / "objects");
        blob = store.write(ObjectType::Blob, binary);
        one = store.write(ObjectType::Blob, to_bytes("one"));
        two = store.write(ObjectType::Blob, to_bytes("two"));
        three = store.write(ObjectType::Blob, to_bytes("three"));
        tree = write_snapshot(repo, {{"dir/file", {"100755", blob}}, {"space file", {"100644", one}}});
        commit = store.write(ObjectType::Commit, encode_commit({tree, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "fixture"}));
        update_ref(repo, "refs/heads/main", std::nullopt, commit);
        save_index(repo, {{"conflict", "100644", one, 1}, {"conflict", "100644", two, 2},
                          {"conflict", "100644", three, 3}, {"name:colon", "100644", blob},
                          {"1:literal", "100644", one}, {"safe", "100644", blob}});
    }
    const auto original = metadata(repo);
    REQUIRE(show_object(repo, blob.hex()) == binary);
    const ObjectStore store(repo.metadata() / "objects");
    const auto entries = decode_tree(store.read(tree).payload);
    REQUIRE(to_string(show_object(repo, tree.hex())) == "40000 " + entries[0].id.hex() + " dir\n100644 " + one.hex() + " \"space file\"\n");
    REQUIRE(show_object(repo, "HEAD:dir/file") == binary);
    REQUIRE(show_object(repo, commit.hex() + ":space file") == to_bytes("one"));
    REQUIRE(show_object(repo, ":safe") == binary);
    REQUIRE(show_object(repo, ":0:name:colon") == binary);
    REQUIRE(show_object(repo, ":name:colon") == binary);
    REQUIRE(show_object(repo, ":0:1:literal") == to_bytes("one"));
    REQUIRE(show_object(repo, ":1:conflict") == to_bytes("one"));
    REQUIRE(show_object(repo, ":2:conflict") == to_bytes("two"));
    REQUIRE(show_object(repo, ":3:conflict") == to_bytes("three"));
    REQUIRE_THROWS_AS(show_object(repo, ":conflict"), Error);
    REQUIRE_THROWS_AS(show_object(repo, ":0:conflict"), Error);
    REQUIRE(metadata(repo) == original);
}
TEST_CASE("Show rejects invalid selections and damaged selected data while historical objects ignore index corruption", "[inspection]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    initialize_lock(repo);
    REQUIRE_THROWS_AS(show_object(repo), Error);
    ObjectId blob, tree, commit;
    {
        RepoLock lock(repo.metadata());
        ObjectStore store(repo.metadata() / "objects");
        blob = store.write(ObjectType::Blob, to_bytes("safe\n"));
        tree = write_snapshot(repo, {{"dir/file", {"100644", blob}}});
        commit = store.write(ObjectType::Commit, encode_commit({tree, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "fixture"}));
        update_ref(repo, "refs/heads/main", std::nullopt, commit);
        save_index(repo, {{"safe", "100644", blob}});
    }
    const auto original = metadata(repo);
    for (const auto spec : {"unknown", "HEAD:dir", "HEAD:missing", "HEAD:../file", "HEAD:/file", "HEAD:.minigit/file", ":missing", ":1:safe", ":../safe", ":", "HEAD:"})
        REQUIRE_THROWS_AS(show_object(repo, spec), Error);
    REQUIRE_THROWS_AS(show_object(repo, std::string(8194, 'a')), Error);
    REQUIRE_THROWS_AS(show_object(repo, "HEAD:" + std::string(4097, 'a')), Error);
    REQUIRE_THROWS_AS(show_object(repo, ":0:" + std::string(4097, 'a')), Error);
    REQUIRE_THROWS_AS(show_object(repo, "HEAD", 100001), Error);
    REQUIRE_THROWS_AS(show_object(repo, ObjectId{}.hex()), Error);
    REQUIRE(metadata(repo) == original);
    SECTION("corrupt index affects indexed selection only") {
        atomic_write(repo.metadata() / "index", to_bytes("damaged index"));
        const auto damaged = metadata(repo);
        REQUIRE_THROWS_AS(show_object(repo, ":safe"), Error);
        REQUIRE(show_object(repo, "HEAD:dir/file") == to_bytes("safe\n"));
        REQUIRE_FALSE(show_object(repo).empty());
        REQUIRE(show_object(repo, blob.hex()) == to_bytes("safe\n"));
        REQUIRE_FALSE(show_object(repo, tree.hex()).empty());
        REQUIRE(metadata(repo) == damaged);
    }
    SECTION("wrong type and missing indexed blobs") {
        { RepoLock lock(repo.metadata()); save_index(repo, {{"safe", "100644", tree}, {"missing", "100644", ObjectId{}}}); }
        const auto damaged = metadata(repo);
        REQUIRE_THROWS_AS(show_object(repo, ":safe"), Error);
        REQUIRE_THROWS_AS(show_object(repo, ":missing"), Error);
        REQUIRE_FALSE(show_object(repo).empty());
        REQUIRE(metadata(repo) == damaged);
    }
    SECTION("corrupt blob affects raw historical and indexed selection") {
        atomic_write(ObjectStore(repo.metadata() / "objects").object_path(blob), to_bytes("corrupt compressed blob"));
        const auto damaged = metadata(repo);
        REQUIRE_THROWS_AS(show_object(repo, blob.hex()), Error);
        REQUIRE_THROWS_AS(show_object(repo, "HEAD:dir/file"), Error);
        REQUIRE_THROWS_AS(show_object(repo, ":safe"), Error);
        REQUIRE_THROWS_AS(show_object(repo), Error);
        REQUIRE(metadata(repo) == damaged);
    }
}
