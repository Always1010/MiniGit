#include <catch2/catch_test_macros.hpp>
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/worktree.hpp"
#include "temp_directory.hpp"
#include "legacy_index.hpp"
#include <algorithm>
using namespace minigit;
namespace {
void write(const Repository& repo, std::string_view path, const Bytes& data) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write(target, data);
}
const IndexEntry& find_entry(const Index& index, std::string_view path) {
    const auto found = std::find_if(index.begin(), index.end(), [&](const auto& entry) { return entry.path == path; });
    if (found == index.end()) throw Error("测试索引缺少路径");
    return *found;
}
void refresh_checksum(Bytes& bytes) {
    const auto digest = sha256(std::span<const std::uint8_t>(bytes).first(bytes.size() - 32));
    std::copy(digest.bytes().begin(), digest.bytes().end(), bytes.end() - 32);
}
class CurrentDirectory {
public:
    explicit CurrentDirectory(const std::filesystem::path& path) : previous_(std::filesystem::current_path()) { std::filesystem::current_path(path); }
    ~CurrentDirectory() { std::error_code ignored; std::filesystem::current_path(previous_, ignored); }
private:
    std::filesystem::path previous_;
};
}
TEST_CASE("Worktree file reads retain bytes permissions and matching state without publishing objects", "[staging][worktree]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const Bytes binary{0, 255, 10};
    atomic_write_with_permissions(repo.root() / "file", binary, 0640);
    const auto plain = read_worktree_file(repo, "file");
    REQUIRE(plain.has_value());
    REQUIRE(plain->data.bytes == binary);
    REQUIRE(plain->data.permissions == 0640);
    REQUIRE((plain->state == FileState{"100644", hash_object(ObjectType::Blob, binary)}));
    REQUIRE(read_worktree_entry(repo, "file") == plain->state);
    atomic_write_with_permissions(repo.root() / "file", to_bytes("new"), 0710);
    const auto executable = read_worktree_file(repo, "file");
    REQUIRE(executable->data.permissions == 0710);
    REQUIRE((executable->state == FileState{"100755", hash_object(ObjectType::Blob, to_bytes("new"))}));
    REQUIRE(plain->data.bytes == binary);
    REQUIRE_FALSE(read_worktree_file(repo, "missing").has_value());
    REQUIRE_FALSE(read_worktree_file(repo, "file/child").has_value());
    std::filesystem::create_directory(repo.root() / "directory");
    REQUIRE_FALSE(read_worktree_file(repo, "directory").has_value());
    std::filesystem::create_symlink(repo.root() / "file", repo.root() / "link");
    REQUIRE_THROWS_AS(read_worktree_file(repo, "link"), Error);
    REQUIRE_THROWS_AS(read_worktree_file(repo, "../file"), Error);
    REQUIRE_THROWS_AS(read_worktree_file(repo, ".minigit/HEAD"), Error);
    SECTION("scan bounds visited directories even without file descendants") {
        std::filesystem::remove(repo.root() / "link");
        auto nested = repo.root();
        for (std::size_t depth = 0; depth < 128; ++depth) nested /= "d";
        detail::ensure_directory(nested);
        REQUIRE(scan_worktree(repo).size() == 1);
        std::filesystem::create_directory(nested / "d");
        REQUIRE_THROWS_AS(scan_worktree(repo), Error);
    }
    REQUIRE(std::filesystem::is_empty(repo.metadata() / "objects"));
    REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "index"));
}
TEST_CASE("Index round trips sorted UTF-8 paths and conflict stages", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    REQUIRE(load_index(repo).empty());
    const auto id = hash_object(ObjectType::Blob, to_bytes("payload"));
    const Index input{{"中/file", "100755", id}, {"a", "100644", id, 3}, {"a", "100644", id, 1}};
    RepoLock lock(repo.metadata());
    save_index(repo, input);
    const auto decoded = load_index(repo);
    REQUIRE(encode_index(input) == read_file(repo.metadata() / "index"));
    REQUIRE(decode_index(encode_index(input)) == decoded);
    REQUIRE(decoded.size() == 3);
    REQUIRE(decoded[0] == input[2]);
    REQUIRE(decoded[1] == input[1]);
    REQUIRE(decoded[2] == input[0]);
    const auto original = read_file(repo.metadata() / "index");
    save_index(repo, decoded);
    REQUIRE(read_file(repo.metadata() / "index") == original);
    save_index(repo, {});
    REQUIRE(load_index(repo).empty());
}
TEST_CASE("Add enforces shared path limits before publishing and can clear a legacy index", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    auto directory = repo.root();
    for (std::size_t depth = 0; depth < 127; ++depth) directory /= "d";
    detail::ensure_directory(directory);
    atomic_write(directory / "f", to_bytes("content"));
    add_paths(repo, {directory / "f"});
    const auto original = read_file(repo.metadata() / "index");
    const auto id = load_index(repo).front().id;
    REQUIRE(load_index(repo).front().path.size() == 255);

    std::filesystem::create_directory(directory / "d");
    atomic_write(directory / "d/f", to_bytes("too deep"));
    REQUIRE_THROWS_AS(add_paths(repo, {directory / "d/f"}), Error);
    REQUIRE_THROWS_AS(add_paths(repo, {}, true), Error);
    REQUIRE(read_file(repo.metadata() / "index") == original);
    REQUIRE_THROWS_AS(encode_index({{std::string(4097, 'x'), "100644", id}}), Error);

    // Authenticate an old-format index whose path predates the write limits.
    const auto legacy_path = (directory / "d/f").lexically_relative(repo.root()).generic_string();
    const auto legacy = test::legacy_index_entry({legacy_path, "100644", id});
    atomic_write(repo.metadata() / "index", legacy);
    REQUIRE(load_index(repo).front().path == legacy_path);
    std::filesystem::remove_all(repo.root() / "d");
    add_paths(repo, {}, true);
    REQUIRE(load_index(repo).empty());
}
TEST_CASE("Invalid index entries never replace the persisted index", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto id = sha256(to_bytes("file"));
    RepoLock lock(repo.metadata());
    save_index(repo, {{"valid", "100644", id}});
    const auto original = read_file(repo.metadata() / "index");
    for (const auto& path : {"", "/a", "a/", "a//b", "a/./b", "a/../b", "a\\b", ".git/a", "a/.minigit/b"}) {
        REQUIRE_THROWS_AS(save_index(repo, {{path, "100644", id}}), Error);
        REQUIRE(read_file(repo.metadata() / "index") == original);
    }
    REQUIRE_THROWS_AS(validate_repo_path(std::string("a\0b", 3)), Error);
    REQUIRE_THROWS_AS(validate_repo_path(std::string("\xc0\xaf", 2)), Error);
    REQUIRE_THROWS_AS(save_index(repo, {{"x", "120000", id}}), Error);
    REQUIRE_THROWS_AS(save_index(repo, {{"x", "100644", id, 4}}), Error);
    REQUIRE_THROWS_AS(save_index(repo, {{"x", "100644", id}, {"x", "100644", id}}), Error);
    REQUIRE_THROWS_AS(save_index(repo, {{"x", "100644", id}, {"x", "100644", id, 1}}), Error);
    REQUIRE_THROWS_AS(save_index(repo, {{"x", "100644", id}, {"x/a", "100644", id}}), Error);
    REQUIRE_THROWS_AS(save_index(repo, {{"x", "100644", id, 1}, {"x/a", "100644", id}}), Error);
    REQUIRE(read_file(repo.metadata() / "index") == original);
}
TEST_CASE("Index decoding rejects corrupt framing and authenticated malformed entries", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto id = sha256(to_bytes("file"));
    RepoLock lock(repo.metadata());
    save_index(repo, {{"a", "100644", id}});
    const auto original = read_file(repo.metadata() / "index");
    SECTION("checksum") {
        auto bytes = original;
        bytes[16] = 'b';
        atomic_write(repo.metadata() / "index", bytes);
        REQUIRE_THROWS_AS(load_index(repo), Error);
    }
    SECTION("truncated") {
        for (const auto length : {0U, 12U, 43U, 60U}) {
            atomic_write(repo.metadata() / "index", std::span<const std::uint8_t>(original).first(length));
            REQUIRE_THROWS_AS(load_index(repo), Error);
        }
    }
    SECTION("authenticated malformed") {
        for (const auto offset : {0U, 7U, 11U, 15U, 16U, 20U, 21U}) {
            auto bytes = original;
            bytes[offset] = 255;
            refresh_checksum(bytes);
            atomic_write(repo.metadata() / "index", bytes);
            REQUIRE_THROWS_AS(load_index(repo), Error);
        }
    }
    SECTION("tail") {
        auto bytes = original;
        bytes.insert(bytes.end() - 32, 0);
        refresh_checksum(bytes);
        atomic_write(repo.metadata() / "index", bytes);
        REQUIRE_THROWS_AS(load_index(repo), Error);
    }
}
TEST_CASE("Add snapshots binary nested empty and executable files persistently", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const Bytes binary{0, 255, 128, '\n', 0};
    write(repo, "nested/binary", binary);
    write(repo, "empty", {});
    write(repo, "script", to_bytes("run"));
    std::filesystem::permissions(repo.root() / "script", std::filesystem::perms::group_exec, std::filesystem::perm_options::add);
    add_paths(repo, {}, true);
    const auto index = load_index(Repository::discover(repo.root()));
    REQUIRE(index.size() == 3);
    REQUIRE(find_entry(index, "script").mode == "100755");
    ObjectStore store(repo.metadata() / "objects");
    REQUIRE(store.read(find_entry(index, "nested/binary").id).payload == binary);
    REQUIRE(store.read(find_entry(index, "empty").id).payload.empty());
    write(repo, "nested/binary", to_bytes("modified"));
    REQUIRE(load_index(repo) == index);
    REQUIRE(store.read(find_entry(index, "nested/binary").id).payload == binary);
    REQUIRE(read_worktree_entry(repo, "nested/binary")->id != find_entry(index, "nested/binary").id);
    const auto snapshot = scan_worktree(repo);
    REQUIRE(snapshot.size() == 3);
    REQUIRE(snapshot.at("script").mode == "100755");
}
TEST_CASE("Add accepts paths from a subdirectory and stages selected or all deletions", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    write(repo, "dir/a", to_bytes("a"));
    write(repo, "b", to_bytes("b"));
    {
        CurrentDirectory cwd(repo.root() / "dir");
        add_paths(repo, {"a"});
    }
    REQUIRE(load_index(repo).size() == 1);
    add_paths(repo, {repo.root() / "b"});
    std::filesystem::remove(repo.root() / "dir/a");
    REQUIRE_FALSE(read_worktree_entry(repo, "dir/a").has_value());
    add_paths(repo, {repo.root() / "dir/a"});
    REQUIRE(load_index(repo).size() == 1);
    std::filesystem::remove(repo.root() / "b");
    add_paths(repo, {}, true);
    REQUIRE(load_index(repo).empty());
    REQUIRE_THROWS_AS(add_paths(repo, {repo.root() / "missing"}), Error);
}
TEST_CASE("Selected add rejects unsafe paths without scanning unrelated symlinks", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path() / "repo");
    write(repo, "safe", to_bytes("safe"));
    std::filesystem::create_directory(repo.root() / "dir");
    std::filesystem::create_directory_symlink(repo.root() / "dir", repo.root() / "link");
    add_paths(repo, {repo.root() / "safe"});
    const auto original = read_file(repo.metadata() / "index");
    REQUIRE_THROWS_AS(add_paths(repo, {temp.path() / "outside"}), Error);
    REQUIRE_THROWS_AS(add_paths(repo, {repo.root() / "link" / ".." / "safe"}), Error);
    REQUIRE_THROWS_AS(add_paths(repo, {repo.root() / "missing" / ".." / "safe"}), Error);
    REQUIRE_THROWS_AS(add_paths(repo, {repo.root() / "safe" / ".." / "safe"}), Error);
    REQUIRE_THROWS_AS(add_paths(repo, {repo.root() / "missing" / ".." / "link" / ".." / "safe"}), Error);
    REQUIRE_THROWS_AS(add_paths(repo, {repo.metadata()}), Error);
    REQUIRE_THROWS_AS(add_paths(repo, {}, true), Error);
    REQUIRE_THROWS_AS(scan_worktree(repo), Error);
    REQUIRE(read_file(repo.metadata() / "index") == original);
}
TEST_CASE("Ignore rules support anchored paths basename globs and tracked exceptions", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    write(repo, "logs/tracked", to_bytes("before"));
    add_paths(repo, {repo.root() / "logs"});
    write(repo, ".minigitignore", to_bytes("# comment\n\nlogs/\n*.tmp\n/root-only\na?/discard\n"));
    write(repo, "logs/tracked", to_bytes("after"));
    write(repo, "logs/untracked", {});
    write(repo, "dir/ignored.tmp", {});
    write(repo, "root-only", {});
    write(repo, "dir/root-only", {});
    write(repo, "ab/discard", {});
    write(repo, "keep", {});
    add_paths(repo, {}, true);
    const auto index = load_index(repo);
    REQUIRE(index.size() == 4);
    REQUIRE(find_entry(index, "logs/tracked").id == hash_object(ObjectType::Blob, to_bytes("after")));
    REQUIRE(scan_worktree(repo).size() == 4);
    const auto original = read_file(repo.metadata() / "index");
    REQUIRE_THROWS_AS(add_paths(repo, {repo.root() / "logs/untracked"}), Error);
    REQUIRE(read_file(repo.metadata() / "index") == original);
    for (const auto& unsupported : {"!keep\n", "**/file\n"}) {
        write(repo, ".minigitignore", to_bytes(unsupported));
        REQUIRE_THROWS_AS(add_paths(repo, {repo.root() / "keep"}), Error);
        REQUIRE_THROWS_AS(scan_worktree(repo), Error);
        REQUIRE(read_file(repo.metadata() / "index") == original);
    }
}
TEST_CASE("Add handles directory file conversions and resolves conflict stages", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    write(repo, "item/child", to_bytes("child"));
    add_paths(repo, {}, true);
    std::filesystem::remove_all(repo.root() / "item");
    write(repo, "item", to_bytes("file"));
    REQUIRE_FALSE(read_worktree_entry(repo, "item/child").has_value());
    add_paths(repo, {repo.root() / "item"});
    REQUIRE(load_index(repo).size() == 1);
    REQUIRE(load_index(repo)[0].path == "item");
    std::filesystem::remove(repo.root() / "item");
    write(repo, "item/child", to_bytes("new"));
    add_paths(repo, {repo.root() / "item/child"});
    REQUIRE(load_index(repo).size() == 1);
    REQUIRE(load_index(repo)[0].path == "item/child");
    const auto id = sha256(to_bytes("conflict"));
    {
        RepoLock lock(repo.metadata());
        save_index(repo, {{"item/child", "100644", id, 1}, {"item/child", "100644", id, 2}, {"item/child", "100644", id, 3}});
    }
    add_paths(repo, {repo.root() / "item/child"});
    REQUIRE(load_index(repo).size() == 1);
    REQUIRE(load_index(repo)[0].stage == 0);
    std::filesystem::remove(repo.root() / "item/child");
    {
        RepoLock lock(repo.metadata());
        save_index(repo, {{"item/child", "100644", id, 1}, {"item/child", "100644", id, 2}});
    }
    add_paths(repo, {repo.root() / "item/child"});
    REQUIRE(load_index(repo).empty());
}
TEST_CASE("Invalid later add selections preserve the original index", "[staging]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    write(repo, "first", to_bytes("original"));
    add_paths(repo, {repo.root() / "first"});
    const auto original = read_file(repo.metadata() / "index");
    write(repo, "first", to_bytes("changed"));
    REQUIRE_THROWS_AS(add_paths(repo, {repo.root() / "first", repo.root() / "missing"}), Error);
    REQUIRE(read_file(repo.metadata() / "index") == original);
}
