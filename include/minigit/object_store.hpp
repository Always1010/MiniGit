#pragma once
#include "minigit/fs.hpp"
#include "minigit/objects.hpp"
namespace minigit {
// Mutations require the caller to hold RepoLock; this class does not acquire it.
class ObjectStore {
public:
    explicit ObjectStore(std::filesystem::path objects_directory);
    ObjectId write(ObjectType type, std::span<const std::uint8_t> payload);
    Object read(const ObjectId& id) const;
    std::filesystem::path object_path(const ObjectId& id) const;
private:
    std::filesystem::path objects_directory_;
};
}
