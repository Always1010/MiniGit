#pragma once
#include "minigit/objects.hpp"
#include "minigit/worktree.hpp"
#include <optional>
namespace minigit {
Commit read_commit(const Repository& repo, const ObjectId& id);
// Snapshot expansion is bounded to 128 tree levels, 100000 entries and 64 MiB
// of expanded path/tree data. Reused subtrees count separately at each prefix;
// each blob is checked once and released, under the per-object 64 MiB limit.
Snapshot read_snapshot(const Repository& repo, const ObjectId& tree);
// Caller must hold RepoLock; all paths, modes and blob objects are validated.
ObjectId write_snapshot(const Repository& repo, const Snapshot& snapshot);
Snapshot index_snapshot(const Repository& repo);
Snapshot head_snapshot(const Repository& repo);
// Identity lives in a separate explicit versioned file, never repository config.
std::optional<std::string> identity_value(const Repository& repo, std::string_view key);
// These business operations acquire RepoLock themselves.
void configure_identity(const Repository& repo, std::string_view key, std::string_view value);
ObjectId create_commit(const Repository& repo, std::string_view message, bool allow_empty = false);
}
