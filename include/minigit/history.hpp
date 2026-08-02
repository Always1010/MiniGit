#pragma once
#include "minigit/snapshot.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace minigit {
Snapshot head_snapshot(const Repository& repo);
// Identity lives in a separate explicit versioned file, never repository config.
std::optional<std::string> identity_value(const Repository& repo, std::string_view key);
// Caller holds RepoLock; refuses a pending short transaction. Stores immutable
// Tree/Commit objects from explicit inputs without reading HEAD, Index or Work.
// Validates every distinct parent's complete snapshot, preserving parent order
// and duplicates (at most 100000). Actor/message/full frame checks precede Tree
// writes. original_author is borrowed only for this call; committer uses current
// configured identity. Empty snapshots and same-tree commits are permitted.
ObjectId store_commit_snapshot(const Repository& repo, const Snapshot& snapshot,
                               const std::vector<ObjectId>& ordered_parents,
                               std::string_view message,
                               std::optional<std::string_view> original_author = std::nullopt);
// These business operations acquire RepoLock themselves.
void configure_identity(const Repository& repo, std::string_view key, std::string_view value);
ObjectId create_commit(const Repository& repo, std::string_view message, bool allow_empty = false);
}
