#pragma once
#include "minigit/snapshot.hpp"
#include "minigit/transaction.hpp"
#include <map>
#include <optional>
#include <set>
namespace minigit {
struct WorktreeTarget {
    std::optional<FileState> file;
    std::optional<std::uint32_t> permissions;
    bool operator==(const WorktreeTarget&) const = default;
};
using WorktreeTargets = std::map<std::string, WorktreeTarget>;
// Caller holds RepoLock. A null file deletes; an empty blob remains a file.
// plan.work must be empty. Protected files and empty directories are preserved.
// Targets and owned paths each allow 200000 paths / 64 MiB of path bytes. The
// expanded desired-node map and actual-node cache each obey those same limits,
// incrementally before inserting or inspecting a new path.
// Work and preplanned metadata together obey all transaction size budgets;
// checked before capturing any new worktree backup objects.
// Existing unchanged parent directories are omitted; leaf deletion keeps empty
// parent directories. Planning may write immutable backup blobs, never metadata
// or worktree files, and exceptions leave the supplied plan unchanged.
void plan_worktree_update(const Repository& repo, TransactionPlan& plan,
                          const WorktreeTargets& targets, const std::set<std::string>& owned_paths);
// Caller holds RepoLock. Supports all index stages, validates blob objects, and
// preserves exact before bytes. An existing index change in plan is rejected.
void plan_index_update(const Repository& repo, TransactionPlan& plan, const Index& index);
}
