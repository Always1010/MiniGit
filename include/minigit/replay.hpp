#pragma once
#include "minigit/merge_snapshot.hpp"
#include "minigit/refs.hpp"
#include "minigit/transaction.hpp"
#include <optional>
#include <string>
#include <vector>
namespace minigit {
enum class ReplayAction { Start, Continue, Abort };
struct ReplayOptions {
    ReplayAction action = ReplayAction::Start;
    std::string revision; // Required only for Start; at most 4096 bytes.
    std::optional<std::string> message; // Start only; overrides the default.
};
enum class ReplayOutcome { Applied, Conflicted, Aborted };
struct ReplayResult {
    ReplayOutcome outcome = ReplayOutcome::Applied;
    Head head;
    std::vector<MergeConflict> conflicts;
};
// Each entry acquires RepoLock and refuses pending short transactions. Start
// requires a non-unborn HEAD, clean tracked Index/Work, and no operation. One
// source with zero or one parent; unrelated histories are allowed. Clean and
// resolved results always create a commit with the original HEAD as sole parent,
// including empty changes. Cherry-pick preserves the source author and defaults
// to its message; revert uses the current author and "Revert <full source ID>".
// Continue/Abort require the matching operation kind. Continue preserves Work;
// Abort restores saved affected Work/full permissions and exact raw Index/HEAD.
// Each publication uses one transaction. Hook is for durable-boundary tests.
ReplayResult cherry_pick_repository(const Repository& repo, const ReplayOptions& options,
                                   const TransactionHook& hook = {});
ReplayResult revert_repository(const Repository& repo, const ReplayOptions& options,
                              const TransactionHook& hook = {});
}
