#pragma once
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
// Each business entry acquires RepoLock and refuses pending transactions.
// Mutators also reject active operations. Push requires non-unborn HEAD and a
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
}
