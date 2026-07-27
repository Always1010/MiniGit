#pragma once
#include "minigit/snapshot.hpp"
#include <optional>
namespace minigit {
Snapshot head_snapshot(const Repository& repo);
// Identity lives in a separate explicit versioned file, never repository config.
std::optional<std::string> identity_value(const Repository& repo, std::string_view key);
// These business operations acquire RepoLock themselves.
void configure_identity(const Repository& repo, std::string_view key, std::string_view value);
ObjectId create_commit(const Repository& repo, std::string_view message, bool allow_empty = false);
}
