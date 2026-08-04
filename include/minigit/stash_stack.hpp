#pragma once
#include "minigit/transaction.hpp"
#include <optional>
#include <set>
#include <span>
#include <vector>
namespace minigit {
using StashStack = std::vector<ObjectId>; // Newest first; duplicate IDs are valid.
struct StashStackState {
    StashStack entries;
    std::optional<Bytes> raw_stack;
    std::optional<Bytes> raw_ref;
};
struct StashEntry {
    ObjectId work;
    ObjectId head;
    ObjectId index;
    std::optional<ObjectId> untracked;
    bool operator==(const StashEntry&) const = default;
};
// Pure MGST v1 codec: BE version/count, fixed-width IDs and SHA-256 checksum.
// At most 100000 entries and 64 MiB; exact encoded size is 44 + 32 * count.
// An encoded empty stack is valid; missing metadata also means an empty stack.
Bytes encode_stash_stack(const StashStack& stack);
StashStack decode_stash_stack(std::span<const std::uint8_t> bytes);
// Caller holds RepoLock. Validates W parents=[H,I,(U)], I parents=[H], U root,
// and all four Commit types/schemas. No Tree, ancestor DAG or live metadata read.
StashEntry read_stash_entry(const Repository& repo, ObjectId work);
// All entries, not just top. Cached typed Commit reads share a 64 MiB decoded
// payload budget. Roots are W/H/I/U; no Tree/DAG/Index/Work/live stack/ref reads
// or pending guard, so this API is usable for transaction metadata images.
std::set<ObjectId> stash_roots(const Repository& repo, const StashStack& stack);
// Caller holds RepoLock. Safe ordinary-file reads; verifies the complete stack
// and refs/stash agrees with its top (both missing/empty require absent ref).
// Preserves exact optional bytes, including uppercase ref IDs and encoded empty.
StashStackState read_stash_stack(const Repository& repo);
// Caller holds RepoLock; refuses pending transactions and preplanned stack/ref
// changes. Exact expected raw bytes AND entries bind CAS. Desired empty removes
// both metadata files; identical logical noop preserves existing exact bytes.
// Validates all desired roots, appends to a copy, checks combined budgets, then
// assigns. No writes, self-lock, operation guard or transaction execution.
void plan_stash_stack_update(const Repository& repo, TransactionPlan& plan,
                             const StashStackState& expected, const StashStack& desired);
}
