#pragma once
#include "minigit/repository.hpp"
#include <optional>
#include <string>
#include <vector>
namespace minigit {
struct RestoreOptions {
    std::optional<std::string> source;
    bool staged = false;
    bool worktree = false;
};
// Acquires one RepoLock and publishes selected Index/worktree changes in one
// transaction, leaving HEAD and reflogs unchanged. Neither flag means worktree.
// Paths are exact or directory prefixes, relative to the current directory or
// absolute, normalized lexically without inspecting working files. Every input
// must match a source or Index path; no glob syntax is interpreted.
// Default sources: Index for worktree only, HEAD when staged is requested.
// Explicit source requires a complete commit snapshot. Only default Index ->
// worktree rejects selected conflicts; unselected Index stages are preserved.
// Source revisions allow 4096 bytes. Paths allow 200000 inputs / 64 MiB total
// (8192 bytes per raw input), normalized paths 4096 bytes / 128 components.
// Candidate/selected/owned sets each allow 200000 paths / 64 MiB of key bytes.
// Index allows 100000 distinct paths / 300000 stage entries / 64 MiB encoding.
// Worktree restoration uses default 0644/0755 permissions; leaf deletion may
// retain empty parent directories. Existing unknown files/directories are protected.
void restore_paths(const Repository& repo, const std::vector<std::filesystem::path>& paths,
                   const RestoreOptions& options = {});
}
