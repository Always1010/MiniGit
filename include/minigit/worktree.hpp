#pragma once
#include "minigit/snapshot.hpp"
#include "minigit/fs.hpp"
#include <optional>
namespace minigit {
struct WorktreeFile {
    FileData data;
    FileState state;
};
// No implicit RepoLock. Content, permissions and derived state share one fd.
// Directories and absent paths are null; unsupported types and symlinks fail.
std::optional<WorktreeFile> read_worktree_file(const Repository& repo, std::string_view path);
// Read-only hashing; neither operation writes objects.
std::optional<FileState> read_worktree_entry(const Repository& repo, std::string_view path);
Snapshot scan_worktree(const Repository& repo);
// Acquires one RepoLock and atomically replaces the index after all paths succeed.
void add_paths(const Repository& repo, const std::vector<std::filesystem::path>& input, bool all = false);
}
