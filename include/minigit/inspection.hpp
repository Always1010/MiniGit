#pragma once
#include "minigit/snapshot.hpp"
#include <string>
#include <vector>
namespace minigit {
struct DiffOptions {
    bool staged = false;
    std::vector<std::string> revisions{};
    std::size_t context = 3;
};
// Caller holds RepoLock. Validates both snapshots and blob objects, including
// unchanged files. Combined snapshots: 200000 paths, 64 MiB of path bytes;
// each path is at most 4096 bytes. Complete output is at most 64 MiB.
std::string diff_snapshots(const Repository& repo, const Snapshot& before,
                            const Snapshot& after, std::size_t context = 3);
// Acquires RepoLock. Default: index -> tracked worktree. Staged: HEAD/revision
// -> index. One revision: revision -> worktree over revision/index path union.
// Two revisions: snapshot -> snapshot, independently of the current index.
std::string repository_diff(const Repository& repo, const DiffOptions& options = {});
// Acquires RepoLock. Commit displays use the first-parent patch; full object IDs
// also accept raw blobs and trees. revision:path and :[stage:]path return blobs.
// Specifications are limited to 8193 bytes; complete output is at most 64 MiB.
Bytes show_object(const Repository& repo, std::string_view spec = "HEAD", std::size_t context = 3);
}
