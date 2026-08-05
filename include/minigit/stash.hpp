#pragma once
#include "minigit/merge_snapshot.hpp"
#include "minigit/transaction.hpp"
#include <optional>
#include <string>
#include <vector>
namespace minigit {
struct StashPushOptions {
    bool include_untracked = false;
    std::optional<std::string> message;
};
struct StashListEntry {
    std::size_t position;
    ObjectId work;
    std::string message;
};
struct StashRestoreOptions {
    std::size_t position = 0;
    bool restore_index = false;
    bool pop = false;
};
enum class StashConflictLayer { Work, Staging, Both };
struct StashConflict {
    MergeConflict conflict;
    StashConflictLayer layer;
};
enum class StashRestoreOutcome { Applied, Conflicted, Aborted };
struct StashRestoreResult {
    StashRestoreOutcome outcome;
    std::vector<StashConflict> conflicts;
    bool dropped = false;
};
// Each business entry acquires RepoLock and refuses pending transactions.
// Push/drop/clear also reject active operations. Push requires non-unborn HEAD and a
// resolved typed Index, preserves independent H/Index/actual tracked Work, and
// optionally non-ignored untracked files. It atomically saves a stack entry and
// restores HEAD content/Index without moving HEAD or logs. No changes returns
// nullopt. Stored snapshots keep bytes/executable mode, not full permissions or
// empty directories; transaction rollback preserves actual node permissions.
std::optional<ObjectId> stash_push(const Repository& repo, const StashPushOptions& options = {},
                                   const TransactionHook& hook = {});
// Queries allow active operations and never inspect current Index or Work.
std::vector<StashListEntry> stash_list(const Repository& repo);
// Shows HEAD-at-save -> saved tracked Work plus saved untracked files; the
// independent saved Index is not displayed. Complete output is at most 64 MiB.
std::string stash_show(const Repository& repo, std::size_t position = 0, std::size_t context = 3);
void stash_drop(const Repository& repo, std::size_t position = 0, const TransactionHook& hook = {});
void stash_clear(const Repository& repo, const TransactionHook& hook = {});
// Restore requires a non-unborn HEAD and clean tracked Index/Work. Work always
// merges saved H -> W onto current HEAD; --index separately merges H -> I.
// Unknown nodes are protected. Conflicts retain the selected stack entry and
// save exact stack/Index/Work backups. Cross-layer D/F combinations that cannot
// preserve independent staging intent are rejected before publication.
StashRestoreResult stash_restore(const Repository& repo, const StashRestoreOptions& options = {},
                                 const TransactionHook& hook = {});
// Continue checks resolved typed Index and exact selected stack context without
// reading Work. Neither restore nor continue creates a Commit or moves HEAD.
// Pop removes the selected position only on complete success. Abort restores
// whole original raw Index/HEAD and affected actual Work, protecting new unknown
// descendants; it preserves the stash entry.
StashRestoreResult stash_continue(const Repository& repo, const TransactionHook& hook = {});
StashRestoreResult stash_abort(const Repository& repo, const TransactionHook& hook = {});
}
