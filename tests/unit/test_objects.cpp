#include <catch2/catch_test_macros.hpp>
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "temp_directory.hpp"
#include <zlib.h>
#include <array>
using namespace minigit;
namespace {
Bytes compress_fixture(std::span<const std::uint8_t> bytes) {
    uLongf size = compressBound(static_cast<uLong>(bytes.size()));
    Bytes result(size);
    REQUIRE(compress2(result.data(), &size, bytes.data(), static_cast<uLong>(bytes.size()), Z_DEFAULT_COMPRESSION) == Z_OK);
    result.resize(size);
    return result;
}
}
TEST_CASE("SHA-256 uses the full 32-byte digest", "[objects]") {
    REQUIRE(sha256(to_bytes("abc")).hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(ObjectId::from_hex(std::string(64, 'A')).hex() == std::string(64, 'a'));
    REQUIRE_THROWS_AS(ObjectId::from_hex("abc"), Error);
    REQUIRE_THROWS_AS(ObjectId::from_hex(std::string(64, 'z')), Error);
    REQUIRE_THROWS_AS(ObjectId::from_bytes(Bytes(31)), Error);
}
TEST_CASE("Loose blobs round trip and reuse existing objects", "[objects]") {
    test::TempDirectory temp;
    auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    for (const auto& payload : {Bytes{}, Bytes{0, 255, 10, 128, 0}}) {
        auto id = store.write(ObjectType::Blob, payload);
        REQUIRE(id == hash_object(ObjectType::Blob, payload));
        auto stored = read_file(store.object_path(id));
        REQUIRE(store.write(ObjectType::Blob, payload) == id);
        REQUIRE(read_file(store.object_path(id)) == stored);
        auto object = store.read(id);
        REQUIRE(object.type == ObjectType::Blob);
        REQUIRE(object.payload == payload);
    }
}
TEST_CASE("Loose objects reject corruption and noncanonical headers", "[objects]") {
    test::TempDirectory temp;
    auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    const auto id = store.write(ObjectType::Blob, to_bytes("original"));
    SECTION("hash mismatch") {
        const auto other = store.write(ObjectType::Blob, to_bytes("changed"));
        atomic_write(store.object_path(id), read_file(store.object_path(other)));
        REQUIRE_THROWS_AS(store.read(id), Error);
        REQUIRE_THROWS_AS(store.write(ObjectType::Blob, to_bytes("original")), Error);
    }
    SECTION("bad compressed bytes") {
        atomic_write(store.object_path(id), Bytes{1, 2, 3});
        REQUIRE_THROWS_AS(store.read(id), Error);
    }
    SECTION("truncated zlib stream") {
        auto data = read_file(store.object_path(id));
        data.pop_back();
        atomic_write(store.object_path(id), data);
        REQUIRE_THROWS_AS(store.read(id), Error);
    }
    SECTION("compressed tail") {
        auto data = read_file(store.object_path(id));
        data.push_back(0);
        atomic_write(store.object_path(id), data);
        REQUIRE_THROWS_AS(store.read(id), Error);
    }
    SECTION("malformed headers with matching hashes") {
        for (const auto& header : {"blob 2", "blob 01", "blob -1", "blob 1x", "unknown 1", "blob "}) {
            auto framed = to_bytes(header);
            framed.push_back(0);
            framed.push_back('x');
            const auto corrupt_id = sha256(framed);
            detail::ensure_directory(store.object_path(corrupt_id).parent_path());
            atomic_write(store.object_path(corrupt_id), compress_fixture(framed));
            REQUIRE_THROWS_AS(store.read(corrupt_id), Error);
        }
    }
    SECTION("decompression bomb") {
        Bytes framed(max_file_size + 1, 0);
        atomic_write(store.object_path(id), compress_fixture(framed));
        REQUIRE_THROWS_AS(store.read(id), Error);
    }
}
TEST_CASE("Trees have strict names, modes and unsigned UTF-8 order", "[objects]") {
    const auto id = sha256(to_bytes("entry"));
    auto entries = decode_tree(encode_tree({{"中", "40000", id}, {"z", "100755", id}, {"a", "100644", id}}));
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].name == "a");
    REQUIRE(entries[1].name == "z");
    REQUIRE(entries[2].name == "中");
    REQUIRE(entries[2].id == id);
    REQUIRE(decode_tree({}).empty());
    REQUIRE_THROWS_AS(encode_tree({{"a", "100644", id}, {"a", "100755", id}}), Error);
    for (const auto& name : {"", ".", "..", ".minigit", ".git", "a/b", "a\\b"})
        REQUIRE_THROWS_AS(encode_tree({{name, "100644", id}}), Error);
    REQUIRE_THROWS_AS(encode_tree({{std::string("a\0b", 3), "100644", id}}), Error);
    REQUIRE_THROWS_AS(encode_tree({{std::string("\xc0\xaf", 2), "100644", id}}), Error);
    REQUIRE_THROWS_AS(encode_tree({{"a", "120000", id}}), Error);
    auto truncated = encode_tree({{"a", "100644", id}});
    truncated.pop_back();
    REQUIRE_THROWS_AS(decode_tree(truncated), Error);
    auto first = encode_tree({{"z", "100644", id}});
    auto second = encode_tree({{"a", "100644", id}});
    first.insert(first.end(), second.begin(), second.end());
    REQUIRE_THROWS_AS(decode_tree(first), Error);
}
TEST_CASE("Commits preserve parent order and multiline messages", "[objects]") {
    Commit commit{sha256(to_bytes("tree")), {sha256(to_bytes("parent1")), sha256(to_bytes("parent2"))},
                  "Writer <writer@example.test> 0 +0000", "Writer <writer@example.test> 0 +0000", "subject\n\nbody\n"};
    auto decoded = decode_commit(encode_commit(commit));
    REQUIRE(decoded.tree == commit.tree);
    REQUIRE(decoded.parents == commit.parents);
    REQUIRE(decoded.author == commit.author);
    REQUIRE(decoded.committer == commit.committer);
    REQUIRE(decoded.message == commit.message);
    auto serialized = to_string(encode_commit(commit));
    serialized.insert(serialized.find("author "), "tree " + commit.tree.hex() + "\n");
    REQUIRE_THROWS_AS(decode_commit(to_bytes(serialized)), Error);
    REQUIRE_THROWS_AS(decode_commit(to_bytes("tree invalid\n\nmessage")), Error);
    commit.author = "bad\ninjected";
    REQUIRE_THROWS_AS(encode_commit(commit), Error);
    commit.author = "valid";
    commit.message = std::string("bad\0message", 11);
    REQUIRE_THROWS_AS(encode_commit(commit), Error);
}
TEST_CASE("Object size budget includes the framing header", "[objects]") {
    Bytes too_large(max_file_size, 0);
    REQUIRE_THROWS_AS(hash_object(ObjectType::Blob, too_large), Error);
}

TEST_CASE("Maximum framed blob survives compression expansion without relaxing ordinary file limits", "[objects]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    RepoLock lock(repo.metadata());
    ObjectStore store(repo.metadata() / "objects");
    Bytes payload(max_file_size - 14); // "blob 67108850" plus NUL fills 64 MiB.
    std::uint32_t random = 1;
    for (auto& byte : payload) {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        byte = static_cast<std::uint8_t>(random);
    }
    const auto expected = hash_object(ObjectType::Blob, payload);
    REQUIRE(store.write(ObjectType::Blob, payload) == expected);
    REQUIRE(std::filesystem::file_size(store.object_path(expected)) > max_file_size);
    REQUIRE_THROWS_AS(read_file(store.object_path(expected)), Error);
    REQUIRE(store.read(expected).payload == payload);
    REQUIRE(store.write(ObjectType::Blob, payload) == expected);
    payload.push_back(0);
    REQUIRE_THROWS_AS(store.write(ObjectType::Blob, payload), Error);
}
