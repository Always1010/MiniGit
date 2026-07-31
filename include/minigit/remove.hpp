#pragma once
#include "minigit/repository.hpp"
#include <vector>
namespace minigit {
struct RemoveOptions {
    bool cached = false;
    bool force = false;
    bool recursive = false;
};
// Acquires one RepoLock and removes selected Index paths in one transaction.
// Paths are lexical exact/prefix selectors, relative to CWD or absolute. Every
// argument must match Index; directory prefixes require recursive. Normal remove
// requires Index == HEAD and work == Index or truly absent work. Cached requires
// Index == HEAD or Index == work, checking HEAD first without probing work.
// Force skips content safety and clears selected conflict stages; cached force
// never probes work. Unselected stages, HEAD, refs, logs and identity are kept.
// Work deletion protects unknown descendants and unsupported types; leaf removal
// retains empty parent directories. Limits match Restore's paths and Index.
void remove_paths(const Repository& repo, const std::vector<std::filesystem::path>& paths,
                  const RemoveOptions& options = {});
}
