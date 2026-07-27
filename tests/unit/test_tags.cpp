#include <catch2/catch_test_macros.hpp>
#include "minigit/tags.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "temp_directory.hpp"
#include <map>
#include <vector>
using namespace minigit;
namespace {
ObjectId tag_fixture_commit(const Repository& repo, std::string_view content) {
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto blob = store.write(ObjectType::Blob, to_bytes(content));
    const auto tree = store.write(ObjectType::Tree, encode_tree({{"file", "100644", blob}}));
    return store.write(ObjectType::Commit, encode_commit({tree, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", std::string(content)}));
}
std::map<std::string, Bytes> tag_metadata_contents(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        if (entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(repo.metadata()).generic_string(), read_file(entry.path()));
    }
    return result;
}
std::map<std::string, Bytes> tag_object_contents(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects")) {
        if (entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(repo.metadata() / "objects").generic_string(), read_file(entry.path()));
    }
    return result;
}
}
TEST_CASE("Tag creates lists forces and deletes lightweight commit refs without changing HEAD index or objects", "[tags]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto first = tag_fixture_commit(repo, "first");
    const auto second = tag_fixture_commit(repo, "second");
    {
        RepoLock lock(repo.metadata());
        update_ref(repo, "refs/heads/main", std::nullopt, first);
        const auto state = read_snapshot(repo, read_commit(repo, second).tree).at("file");
        save_index(repo, {{"file", state.mode, state.id}});
    }
    const auto head = read_file(repo.metadata() / "HEAD");
    const auto main = read_file(repo.metadata() / "refs/heads/main");
    const auto index = read_file(repo.metadata() / "index");
    const auto objects = tag_object_contents(repo);
    REQUIRE(list_tags(repo).empty());
    create_tag(repo, "z-default");
    create_tag(repo, "a-explicit", second.hex());
    create_tag(repo, "中文", "refs/tags/z-default");
    create_tag(repo, "-leading", "refs/heads/main");
    const std::map<std::string, ObjectId> expected{{"-leading", first}, {"a-explicit", second}, {"z-default", first}, {"中文", first}};
    REQUIRE(list_tags(repo) == expected);
    create_tag(repo, "z-default", second.hex(), true);
    REQUIRE(list_tags(repo).at("z-default") == second);
    create_tag(repo, "z-default", second.hex(), true);
    for (const auto& [name, id] : expected) {
        (void)id;
        delete_tag(repo, name);
    }
    REQUIRE(list_tags(repo).empty());
    REQUIRE(read_file(repo.metadata() / "HEAD") == head);
    REQUIRE(read_file(repo.metadata() / "refs/heads/main") == main);
    REQUIRE(read_file(repo.metadata() / "index") == index);
    REQUIRE(tag_object_contents(repo) == objects);
}
TEST_CASE("Tag validates names duplicates unborn targets and nested ref reuse before changing metadata", "[tags]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto target = tag_fixture_commit(repo, "target");
    SECTION("invalid and reserved short names are rejected by create and delete") {
        const std::vector<std::string> invalid{"", "HEAD", "refs/tags/x", "/x", "x/", "x//y", ".x", "x.lock", "x..y", "x y", "x~", "x^", "x?", "x*", "x[", "x:", "x@{y", "x\\y", "x\ny", "x/../y", std::string(64, 'a'), std::string(64, 'A'), std::string(64, '0'), std::string("x\0y", 3), std::string("\xc0\xaf", 2)};
        const auto before = tag_metadata_contents(repo);
        for (const auto& name : invalid) {
            REQUIRE_THROWS_AS(create_tag(repo, name, target.hex()), Error);
            REQUIRE_THROWS_AS(delete_tag(repo, name), Error);
            REQUIRE(tag_metadata_contents(repo) == before);
        }
    }
    SECTION("unborn missing revisions and absent tags leave metadata unchanged") {
        const auto before = tag_metadata_contents(repo);
        REQUIRE_THROWS_AS(create_tag(repo, "unborn"), Error);
        REQUIRE_THROWS_AS(create_tag(repo, "missing", "missing"), Error);
        REQUIRE_THROWS_AS(create_tag(repo, "missing", ObjectId{}.hex()), Error);
        REQUIRE_THROWS_AS(delete_tag(repo, "missing"), Error);
        REQUIRE(tag_metadata_contents(repo) == before);
    }
    SECTION("duplicates fail even when the requested target is unchanged") {
        create_tag(repo, "release", target.hex());
        const auto before = tag_metadata_contents(repo);
        REQUIRE_THROWS_AS(create_tag(repo, "release", target.hex()), Error);
        REQUIRE_THROWS_AS(create_tag(repo, "release", "missing", true), Error);
        REQUIRE(tag_metadata_contents(repo) == before);
        REQUIRE(list_tags(repo).at("release") == target);
    }
    SECTION("deleting the last nested child permits reusing its parent as a tag") {
        create_tag(repo, "release/deep/child", target.hex());
        delete_tag(repo, "release/deep/child");
        REQUIRE_FALSE(std::filesystem::exists(repo.metadata() / "refs/tags/release"));
        REQUIRE(std::filesystem::is_directory(repo.metadata() / "refs/tags"));
        create_tag(repo, "release", target.hex());
        REQUIRE(list_tags(repo) == std::map<std::string, ObjectId>{{"release", target}});
    }
}
TEST_CASE("Tag rejects corrupt incomplete or ambiguous targets and accepts explicit full refs", "[tags]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    const auto first = tag_fixture_commit(repo, "first");
    const auto second = tag_fixture_commit(repo, "second");
    SECTION("malformed persisted reference rejects listing and creation") {
        atomic_write(repo.metadata() / "refs/tags/bad", to_bytes("invalid\n"));
        const auto before = tag_metadata_contents(repo);
        REQUIRE_THROWS_AS(list_tags(repo), Error);
        REQUIRE_THROWS_AS(create_tag(repo, "copy", "refs/tags/bad"), Error);
        REQUIRE(tag_metadata_contents(repo) == before);
    }
    SECTION("missing or noncommit targets reject listing and creation") {
        ObjectId blob;
        {
            RepoLock lock(repo.metadata());
            blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes("blob"));
        }
        for (const auto invalid : {ObjectId{}, blob}) {
            atomic_write(repo.metadata() / "refs/tags/bad", to_bytes(invalid.hex() + "\n"));
            const auto before = tag_metadata_contents(repo);
            REQUIRE_THROWS_AS(list_tags(repo), Error);
            REQUIRE_THROWS_AS(create_tag(repo, "copy", "refs/tags/bad"), Error);
            REQUIRE_THROWS_AS(create_tag(repo, "copy", invalid.hex()), Error);
            REQUIRE(tag_metadata_contents(repo) == before);
        }
    }
    SECTION("a valid commit with an unreadable snapshot cannot be listed or published") {
        ObjectId incomplete;
        {
            RepoLock lock(repo.metadata());
            incomplete = ObjectStore(repo.metadata() / "objects").write(ObjectType::Commit, encode_commit({ObjectId{}, {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "missing tree"}));
        }
        atomic_write(repo.metadata() / "refs/tags/bad", to_bytes(incomplete.hex() + "\n"));
        const auto before = tag_metadata_contents(repo);
        REQUIRE_THROWS_AS(list_tags(repo), Error);
        REQUIRE_THROWS_AS(create_tag(repo, "copy", "refs/tags/bad"), Error);
        REQUIRE_THROWS_AS(create_tag(repo, "copy", incomplete.hex()), Error);
        REQUIRE(tag_metadata_contents(repo) == before);
    }
    SECTION("an ambiguous short revision requires an explicit branch or tag ref") {
        {
            RepoLock lock(repo.metadata());
            update_ref(repo, "refs/heads/release", std::nullopt, first);
        }
        create_tag(repo, "release", second.hex());
        const auto before = tag_metadata_contents(repo);
        REQUIRE_THROWS_AS(create_tag(repo, "copy", "release"), Error);
        REQUIRE(tag_metadata_contents(repo) == before);
        create_tag(repo, "branch-copy", "refs/heads/release");
        create_tag(repo, "tag-copy", "refs/tags/release");
        REQUIRE(list_tags(repo).at("branch-copy") == first);
        REQUIRE(list_tags(repo).at("tag-copy") == second);
    }
}
