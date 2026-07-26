#pragma once
#include "minigit/object_id.hpp"
namespace minigit {
enum class ObjectType { Blob, Tree, Commit };
std::string_view object_type_name(ObjectType type);
ObjectType parse_object_type(std::string_view text);
ObjectId hash_object(ObjectType type, std::span<const std::uint8_t> payload);
struct Object { ObjectType type; Bytes payload; };
struct TreeEntry { std::string name; std::string mode; ObjectId id; };
Bytes encode_tree(std::vector<TreeEntry> entries);
std::vector<TreeEntry> decode_tree(std::span<const std::uint8_t> bytes);
struct Commit {
    ObjectId tree;
    std::vector<ObjectId> parents;
    std::string author;
    std::string committer;
    std::string message;
};
Bytes encode_commit(const Commit& commit);
Commit decode_commit(std::span<const std::uint8_t> bytes);
}
