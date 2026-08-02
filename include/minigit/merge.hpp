#pragma once
#include "minigit/merge_snapshot.hpp"
#include "minigit/refs.hpp"
#include "minigit/transaction.hpp"
#include <optional>
#include <string>
#include <vector>
namespace minigit {
enum class MergeAction { Start, Continue, Abort };
struct MergeOptions {
    MergeAction action = MergeAction::Start;
    std::string revision; // Required only for Start; at most 4096 bytes.
    std::optional<std::string> message; // Start only; otherwise use saved message.
};
enum class MergeOutcome { UpToDate, FastForward, Merged, Conflicted, Aborted };
struct MergeResult {
    MergeOutcome outcome = MergeOutcome::UpToDate;
    Head head;
    std::vector<MergeConflict> conflicts;
};
// Acquires one RepoLock and refuses pending short transactions. Start requires
// clean tracked Index/Work and no long operation. One source, unique merge base;
// unborn/fast-forward supported, unrelated or multiple lowest bases rejected.
// Continue commits a fully resolved Index; Abort restores saved actual Work,
// whole raw Index (including absence) and exact raw HEAD, protecting unknowns.
// Each publication is one transaction. Hook is for durable-boundary tests only.
MergeResult merge_repository(const Repository& repo, const MergeOptions& options,
                             const TransactionHook& hook = {});
}
