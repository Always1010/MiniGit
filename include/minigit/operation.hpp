#pragma once
#include "minigit/bytes.hpp"
#include "minigit/refs.hpp"
#include "minigit/transaction.hpp"
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>
namespace minigit {
enum class OperationKind : std::uint8_t { Merge = 1, CherryPick = 2, Revert = 3, StashApply = 4, Rebase = 5 };
enum class OperationPhase : std::uint8_t { Conflicted = 1, Ready = 2 };
struct StashOperation {
    bool pop = false;
    std::uint32_t position = 0;
    ObjectId stack_blob; // Blob containing the complete exact selected MGST bytes.
    Bytes raw_ref; // Exact 65-byte refs/stash image at selection.
    bool operator==(const StashOperation&) const = default;
};
struct RebaseOperation {
    ObjectId onto;
    std::vector<ObjectId> queue; // Unique source commits, oldest first; last is original HEAD.
    std::uint32_t cursor = 0; // Ready: not applied; Conflicted: awaiting resolved Index.
    std::optional<Bytes> original_branch_ref; // Exact 65-byte original symbolic ref image.
    bool operator==(const RebaseOperation&) const = default;
};
struct OperationState {
    OperationKind kind = OperationKind::Merge;
    OperationPhase phase = OperationPhase::Conflicted;
    Head original_head;
    Head current_head;
    Bytes original_head_bytes;
    std::optional<ObjectId> original_index_blob;
    ObjectId source_commit;
    std::string message;
    std::vector<WorkChange> work; // Original before and last published after.
    std::optional<StashOperation> stash;
    std::optional<RebaseOperation> rebase;
};
// Pure canonical codec: Merge/CherryPick/Revert without context use MGOP v1
// unchanged. StashApply retains its exact MGOP v2 context, Conflicted phase and
// equal non-unborn Heads. Rebase uses its own v2 context, Ready/Conflicted and a
// non-unborn detached current HEAD. Its unique queue has 1..100000 sources and
// cursor < size; message is rebase. Stash messages are stash:apply/pop.
// At most 64 MiB encoded, 100000 sorted unique paths, 4096 bytes/128 components
// per work path. Both image topologies and full permissions follow Transaction.
Bytes encode_operation(const OperationState& state);
OperationState decode_operation(std::span<const std::uint8_t> bytes);
// Caller holds RepoLock. No Work or live HEAD inspection, writes or pending
// transaction guard: typed semantic validation is also used during recovery.
// Missing metadata returns null; existing empty, unsafe or corrupt data fails.
std::optional<OperationState> read_operation(const Repository& repo);
// Rejects any existing operation node without reading its contents or Work.
void require_no_operation(const Repository& repo);
// Commit roots are typed/schema checked, without Tree or parent traversal.
// Includes Work File blobs, raw Index backup blob and every decoded stage blob.
// StashApply also keeps the saved MGST Blob and all entries' typed W/H/I/U roots,
// validating selected position/source and exact saved ref; no live stack/ref read.
// Rebase keeps onto and every queued Commit, with at most 64 MiB of distinct
// Commit payloads including common Head/source roots; no live branch/Tree/DAG read.
std::set<ObjectId> state_roots(const Repository& repo, const OperationState& state);
// Caller holds RepoLock; refuses pending short transactions and preplanned
// operation changes. expected binds canonical State; actual raw bytes form CAS
// before. Supports create/delete/validated identical noop. Only Rebase permits
// replacement: Ready->Conflicted retains cursor/current; either phase->Ready
// advances one source and requires a single parent equal to old current HEAD.
// Original/context fields and all prior Work keys/before images remain fixed.
// Full combined transaction budgets apply; no writes and strong plan guarantee.
void plan_operation_update(const Repository& repo, TransactionPlan& plan,
                           const std::optional<OperationState>& expected,
                           const std::optional<OperationState>& desired);
}
