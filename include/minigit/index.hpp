#pragma once
#include "minigit/object_id.hpp"
#include "minigit/repository.hpp"
namespace minigit {
struct IndexEntry {
    std::string path;
    std::string mode;
    ObjectId id;
    std::uint8_t stage = 0;
    bool operator==(const IndexEntry&) const = default;
};
using Index = std::vector<IndexEntry>;
void validate_repo_path(std::string_view path);
Index load_index(const Repository& repo);
// The caller must hold RepoLock. The complete index is validated before writing.
void save_index(const Repository& repo, Index entries);
}
