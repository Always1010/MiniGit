#pragma once
#include "minigit/object_id.hpp"
#include "minigit/repository.hpp"
#include "minigit/repo_path.hpp"
namespace minigit {
struct IndexEntry {
    std::string path;
    std::string mode;
    ObjectId id;
    std::uint8_t stage = 0;
    bool operator==(const IndexEntry&) const = default;
};
using Index = std::vector<IndexEntry>;
// New Index images: 100000 paths, 300000 stages, 64 MiB encoded.
void validate_index_budget(const Index& index);
Index load_index(const Repository& repo);
// Codec is independent of repository access and validates the complete index.
Bytes encode_index(Index entries);
// Decoding retains format-valid legacy paths; encode/save enforce new limits.
Index decode_index(std::span<const std::uint8_t> bytes);
// The caller must hold RepoLock. The complete index is validated before writing.
void save_index(const Repository& repo, Index entries);
}
