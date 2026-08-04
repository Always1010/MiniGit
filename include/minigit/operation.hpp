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
enum class OperationKind : std::uint8_t { Merge = 1, CherryPick = 2, Revert = 3, StashApply = 4 };
enum class OperationPhase : std::uint8_t { Conflicted = 1 };
struct StashOperation {
    bool pop = false;
    std::uint32_t position = 0;
    ObjectId stack_blob; // Blob containing the complete exact selected MGST bytes.
    Bytes raw_ref; // Exact 65-byte refs/stash image at selection.
    bool operator==(const StashOperation&) const = default;
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
};
// Pure canonical codec: Merge/CherryPick/Revert without context use MGOP v1
// unchanged. StashApply requires context and uses MGOP v2; both versions support
// only Conflicted and equal non-unborn Heads. Stash messages are stash:apply/pop.
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
std::set<ObjectId> state_roots(const Repository& repo, const OperationState& state);
// Caller holds RepoLock; refuses pending short transactions and preplanned
// operation changes. expected binds canonical State; actual raw bytes form CAS
// before. Supports create/delete/validated identical noop, rejects replacement.
// Full combined transaction budgets apply; no writes and strong plan guarantee.
void plan_operation_update(const Repository& repo, TransactionPlan& plan,
                           const std::optional<OperationState>& expected,
                           const std::optional<OperationState>& desired);
}
