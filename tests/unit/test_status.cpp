#include <catch2/catch_test_macros.hpp>
#include "minigit/status.hpp"
#include "minigit/format.hpp"
#include "minigit/worktree.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include "temp_directory.hpp"
#include <map>
#include <sys/stat.h>
using namespace minigit;
namespace {
void status_file(const Repository& repo, std::string_view path, std::string_view content) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write(target, to_bytes(content));
}
void status_commit(const Repository& repo) {
    configure_identity(repo, "user.name", "状态测试");
    configure_identity(repo, "user.email", "status@example.test");
    add_paths(repo, {}, true);
    (void)create_commit(repo, "baseline");
}
std::map<std::string, Bytes> metadata_contents(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        if (entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(repo.metadata()).generic_string(), read_file(entry.path()));
    }
    return result;
}
}
TEST_CASE("Status reports unborn untracked staged and deleted files and quotes paths", "[status]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto empty = collect_status(repo);
    REQUIRE(empty.head.symbolic == "refs/heads/main");
    REQUIRE_FALSE(empty.head.id.has_value());
    REQUIRE(empty.entries.empty());
    SECTION("untracked paths preserve UTF-8 whitespace and canonical order") {
        status_file(repo, "z.txt", "z");
        status_file(repo, "中文 文件\n.txt", "unicode");
        status_file(repo, "a.txt", "a");
        const std::vector<StatusEntry> expected{{"a.txt", '?', '?'}, {"z.txt", '?', '?'}, {"中文 文件\n.txt", '?', '?'}};
        REQUIRE(collect_status(repo).entries == expected);
    }
    SECTION("staged additions appear before untracked paths") {
        status_file(repo, "z", "staged");
        add_paths(repo, {repo.root() / "z"});
        status_file(repo, "a", "untracked");
        const std::vector<StatusEntry> expected{{"z", 'A', ' '}, {"a", '?', '?'}};
        REQUIRE(collect_status(repo).entries == expected);
        std::filesystem::remove(repo.root() / "z");
        REQUIRE(collect_status(repo).entries.front() == StatusEntry{"z", 'A', 'D'});
    }
    SECTION("path quoting uses exact control escapes and preserves UTF-8") {
        REQUIRE(quote_path("中文/file") == "中文/file");
        REQUIRE(quote_path("中文 文件") == "\"中文 文件\"");
        REQUIRE(quote_path("line\n\t\r\"\\") == "\"line\\n\\t\\r\\\"\\\\\"");
        const std::string controls{char(0), char(1), char(11), char(12), char(31), char(127), '7'};
        REQUIRE(quote_path(controls) == "\"\\000\\001\\013\\014\\037\\1777\"");
    }
}
TEST_CASE("Status compares staged and working content and executable modes independently", "[status]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    status_file(repo, "file", "baseline");
    status_commit(repo);
    REQUIRE(collect_status(repo).entries.empty());
    SECTION("staged and working edits produce MM") {
        status_file(repo, "file", "staged");
        add_paths(repo, {repo.root() / "file"});
        status_file(repo, "file", "working");
        REQUIRE(collect_status(repo).entries == std::vector<StatusEntry>{{"file", 'M', 'M'}});
    }
    SECTION("executable changes are visible before and after staging") {
        std::filesystem::permissions(repo.root() / "file", std::filesystem::perms::group_exec, std::filesystem::perm_options::add);
        REQUIRE(collect_status(repo).entries == std::vector<StatusEntry>{{"file", ' ', 'M'}});
        add_paths(repo, {repo.root() / "file"});
        REQUIRE(collect_status(repo).entries == std::vector<StatusEntry>{{"file", 'M', ' '}});
        std::filesystem::permissions(repo.root() / "file", std::filesystem::perms::group_exec, std::filesystem::perm_options::remove);
        REQUIRE(collect_status(repo).entries == std::vector<StatusEntry>{{"file", 'M', 'M'}});
    }
}
TEST_CASE("Status preserves staged deletion with an untracked replacement and file directory conversions", "[status]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    SECTION("a recreated staged deletion has two entries") {
        status_file(repo, "file", "baseline");
        status_commit(repo);
        std::filesystem::remove(repo.root() / "file");
        REQUIRE(collect_status(repo).entries == std::vector<StatusEntry>{{"file", ' ', 'D'}});
        add_paths(repo, {}, true);
        status_file(repo, "file", "replacement");
        REQUIRE(collect_status(repo).entries == std::vector<StatusEntry>{{"file", 'D', ' '}, {"file", '?', '?'}});
    }
    SECTION("a tracked file replaced by a directory is deleted") {
        status_file(repo, "item", "baseline");
        status_commit(repo);
        std::filesystem::remove(repo.root() / "item");
        status_file(repo, "item/child", "replacement");
        REQUIRE(collect_status(repo).entries == std::vector<StatusEntry>{{"item", ' ', 'D'}, {"item/child", '?', '?'}});
    }
    SECTION("a tracked child replaced by a file is deleted") {
        status_file(repo, "item/child", "baseline");
        status_commit(repo);
        std::filesystem::remove_all(repo.root() / "item");
        status_file(repo, "item", "replacement");
        REQUIRE(collect_status(repo).entries == std::vector<StatusEntry>{{"item/child", ' ', 'D'}, {"item", '?', '?'}});
    }
}
TEST_CASE("Status honors ignore rules and tracked exceptions without changing repository contents", "[status]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    status_file(repo, "logs/tracked", "baseline");
    status_commit(repo);
    status_file(repo, ".minigitignore", "logs/\n*.tmp\n");
    status_file(repo, "logs/tracked", "working only");
    status_file(repo, "logs/ignored", "ignored");
    status_file(repo, "ignored.tmp", "ignored");
    status_file(repo, "untracked", "must not be stored");
    const auto before = metadata_contents(repo);
    const auto report = collect_status(repo);
    REQUIRE(report.head == read_head(repo));
    REQUIRE(report.entries == std::vector<StatusEntry>{{"logs/tracked", ' ', 'M'}, {".minigitignore", '?', '?'}, {"untracked", '?', '?'}});
    REQUIRE(metadata_contents(repo) == before);
    REQUIRE_FALSE(std::filesystem::exists(ObjectStore(repo.metadata() / "objects").object_path(hash_object(ObjectType::Blob, to_bytes("working only")))));
}
TEST_CASE("Status reports every conflict mask and ignores conflict working edits", "[status]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    status_file(repo, ".minigitignore", "conflicts/\n");
    const std::vector<unsigned> masks{14, 12, 6, 10, 4, 8, 2};
    const std::vector<std::pair<char, char>> codes{{'U', 'U'}, {'A', 'A'}, {'U', 'D'}, {'D', 'U'}, {'A', 'U'}, {'U', 'A'}, {'D', 'D'}};
    Index index;
    std::vector<StatusEntry> expected;
    {
        RepoLock lock(repo.metadata());
        ObjectStore store(repo.metadata() / "objects");
        const auto blob = store.write(ObjectType::Blob, to_bytes("conflict stage"));
        for (std::size_t i = 0; i < masks.size(); ++i) {
            const auto path = "conflicts/" + std::to_string(i);
            status_file(repo, path, "working conflict resolution");
            for (std::uint8_t stage = 1; stage <= 3; ++stage)
                if ((masks[i] & (1U << stage)) != 0) index.push_back({path, "100644", blob, stage});
            expected.push_back({path, codes[i].first, codes[i].second});
        }
        save_index(repo, index);
    }
    expected.push_back({".minigitignore", '?', '?'});
    const auto before = metadata_contents(repo);
    REQUIRE(collect_status(repo).entries == expected);
    REQUIRE(scan_worktree(repo).size() == 8);
    REQUIRE(metadata_contents(repo) == before);
}
TEST_CASE("Status rejects corrupt index invalid object references and unsafe working types", "[status]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    status_file(repo, "file", "baseline");
    add_paths(repo, {}, true);
    SECTION("index checksum corruption") {
        auto bytes = read_file(repo.metadata() / "index");
        bytes.back() ^= 1;
        atomic_write(repo.metadata() / "index", bytes);
        REQUIRE_THROWS_AS(collect_status(repo), Error);
    }
    SECTION("normal index entry references a tree") {
        {
            RepoLock lock(repo.metadata());
            const auto tree = ObjectStore(repo.metadata() / "objects").write(ObjectType::Tree, encode_tree({}));
            save_index(repo, {{"file", "100644", tree}});
        }
        REQUIRE_THROWS_AS(collect_status(repo), Error);
    }
    SECTION("a later conflict stage references a tree") {
        {
            RepoLock lock(repo.metadata());
            ObjectStore store(repo.metadata() / "objects");
            const auto blob = store.write(ObjectType::Blob, to_bytes("stage"));
            const auto tree = store.write(ObjectType::Tree, encode_tree({}));
            save_index(repo, {{"file", "100644", blob, 1}, {"file", "100644", tree, 3}});
        }
        REQUIRE_THROWS_AS(collect_status(repo), Error);
    }
    SECTION("an index blob is missing") {
        {
            RepoLock lock(repo.metadata());
            save_index(repo, {{"file", "100644", ObjectId{}}});
        }
        REQUIRE_THROWS_AS(collect_status(repo), Error);
    }
    SECTION("HEAD resolves to a blob") {
        const auto blob = load_index(repo).front().id;
        atomic_write(repo.metadata() / "HEAD", to_bytes(blob.hex() + "\n"));
        REQUIRE_THROWS_AS(collect_status(repo), Error);
    }
    SECTION("a symlink is rejected even with an ignore rule") {
        status_file(repo, ".minigitignore", "link\n");
        std::filesystem::create_symlink(repo.root() / "file", repo.root() / "link");
        REQUIRE_THROWS_AS(collect_status(repo), Error);
    }
    SECTION("a FIFO is rejected without reading it") {
        REQUIRE(::mkfifo((repo.root() / "fifo").c_str(), 0600) == 0);
        REQUIRE_THROWS_AS(collect_status(repo), Error);
    }
}
