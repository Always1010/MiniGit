#pragma once
#include "minigit/merge_snapshot.hpp"
#include "minigit/refs.hpp"
#include "minigit/transaction.hpp"
#include <string>
#include <vector>
namespace minigit {
enum class RebaseAction { Start, Continue, Abort };
struct RebaseOptions {
    RebaseAction action = RebaseAction::Start;
    std::string revision; // Required only for Start; at most 4096 bytes.
};
enum class RebaseOutcome { Applied, UpToDate, Conflicted, Aborted };
struct RebaseResult {
    RebaseOutcome outcome = RebaseOutcome::Applied;
    Head head;
    std::vector<MergeConflict> conflicts;
};
// Acquires RepoLock and refuses pending short transactions. Start requires a
// non-unborn HEAD, clean tracked Index/Work and no operation. Complete ancestry
// is validated before up-to-date/fast-forward shortcuts. Divergence requires a
// unique merge base and a linear first-parent queue, validated before detaching.
// Each source becomes a preserved-empty single-parent commit with source author
// and message; original branch is pinned exactly until final attachment. Ready
// retries the pending source from a clean baseline; Conflicted continues from
// resolved Index, preserving unstaged Work. Abort restores exact original Index,
// HEAD and affected Work/full permissions, protecting unknown descendants.
// Each publication is one transaction; Hook is for durable-boundary tests.
RebaseResult rebase_repository(const Repository& repo, const RebaseOptions& options,
                               const TransactionHook& hook = {});
}
