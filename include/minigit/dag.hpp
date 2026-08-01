#pragma once
#include "minigit/history.hpp"
#include <map>
#include <optional>
namespace minigit {
using CommitGraph = std::map<ObjectId, Commit>;
// Caller holds RepoLock. Loads complete ancestry without expanding trees.
// Bounds: 100000 distinct commits, 1000000 parent edges, and 64 MiB of decoded
// identity/message data plus parent IDs. Duplicate parent edges are preserved.
CommitGraph load_commit_graph(const Repository& repo, const std::vector<ObjectId>& roots);
// Child before parent. Available-node ties use ascending ObjectId byte order.
// Rejects incomplete ancestry and cycles, including manually assembled graphs.
std::vector<ObjectId> topological_order(const CommitGraph& graph);
// Caller holds RepoLock. Validates the complete ancestry of both tips before
// choosing their single lowest common ancestor; does not expand Trees. Inherits
// load_commit_graph bounds. No common ancestor returns nullopt; multiple lowest
// ancestors (crisscross history) are rejected instead of choosing arbitrarily.
std::optional<ObjectId> find_merge_base(const Repository& repo, const ObjectId& left,
                                       const ObjectId& right);
struct HistoryEntry {
    ObjectId id;
    Commit commit;
};
// Acquires RepoLock. Unborn HEAD is empty. maximum limits output only; complete
// ancestry is loaded and validated even when maximum is zero.
std::vector<HistoryEntry> log_history(const Repository& repo, std::string_view revision = "HEAD",
                                      std::optional<std::size_t> maximum = std::nullopt);
}
