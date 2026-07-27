#pragma once
#include "minigit/index.hpp"
#include <map>
#include <optional>
namespace minigit {
struct FileState {
    std::string mode;
    ObjectId id;
    bool operator==(const FileState&) const = default;
};
using Snapshot = std::map<std::string, FileState>;
// Read-only hashing; neither operation writes objects.
std::optional<FileState> read_worktree_entry(const Repository& repo, std::string_view path);
Snapshot scan_worktree(const Repository& repo);
// Acquires one RepoLock and atomically replaces the index after all paths succeed.
void add_paths(const Repository& repo, const std::vector<std::filesystem::path>& input, bool all = false);
}
