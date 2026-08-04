#pragma once
#include "checkout_support.hpp"
#include "minigit/merge_snapshot.hpp"
#include "minigit/operation.hpp"
#include "minigit/worktree_update.hpp"
#include <map>
#include <optional>
#include <string>
#include <string_view>
// Caller-held RepoLock. Business rules, authors and parent selection stay in
// Merge/Replay; shared helpers never execute a transaction or acquire a lock.
namespace minigit::operation_support {
using WorkObservations = std::map<std::string, NodeImage>;
std::optional<Bytes> raw_metadata(const Repository& repo, std::string_view name);
Index checked_index(const Repository& repo, const std::optional<Bytes>& raw);
Snapshot resolved_snapshot(const Index& index);
void validate_message(std::string_view message);
WorkObservations clean_work(const Repository& repo, const Snapshot& snapshot);
void check_work_before(const TransactionPlan& plan, const WorkObservations& observed);
WorktreeTargets changed_targets(const Snapshot& before, const Snapshot& after,
                                checkout_support::BoundedPaths& owned);
void check_original_metadata(const Repository& repo, const Head& head,
                             const std::optional<Bytes>& raw_head, const std::optional<Bytes>& raw_index);
std::vector<WorkChange> conflict_work(const Repository& repo, const TransactionPlan& plan,
                                    const SnapshotMergeResult& merged, const WorkObservations& observed);
OperationState active_operation(const Repository& repo, OperationKind expected_kind);
// Fresh plan restores whole raw Index/HEAD and saved affected Work, protects
// unknown descendants, and deletes State. Late failures may leave backup Blobs;
// no mutable publication occurs. State must be the validated active operation.
TransactionPlan plan_abort_operation(const Repository& repo, const OperationState& state);
}
