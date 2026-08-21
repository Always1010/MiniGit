#pragma once
#include "minigit/fs.hpp"
#include "minigit/worktree_image.hpp"
#include "minigit/repository.hpp"
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace minigit {
struct MetadataChange {
    std::string path; // Relative to .minigit; only explicit metadata names allowed.
    std::optional<Bytes> before;
    std::optional<Bytes> after;
};
struct TransactionPlan {
    std::vector<WorkChange> work;
    std::vector<MetadataChange> metadata;
};
// Pure size checks: 100000 combined changes, 64 MiB combined path bytes,
// metadata before/after bytes, and encoded MGTR journal. Does not validate
// schemas or read filesystem/object data; before Blob IDs may be unsaved hashes.
void validate_transaction_budget(const TransactionPlan& plan);
// Test-only injection by API callers. Steps start at one; production callers omit.
using TransactionHook = std::function<void(std::size_t, std::string_view)>;
// All operations require caller-held RepoLock. Capture saves the actual worktree
// file as a backup blob; directories preserve permissions and absence is explicit.
NodeImage capture_work_node(const Repository& repo, std::string_view path);
// One checksummed journal transitions atomically from Prepared to Committed.
// Limits: 100000 changes, 128 path components, 4096 path bytes, 64 MiB journal.
// Total path bytes and metadata image bytes are each capped at 64 MiB. Files
// require owner-read; directories require owner-read/execute, and owner-write
// when containing planned descendants. Ownership is not captured or restored.
// Metadata ancestor file conversions are rejected; logs use the MGRL codec.
// HEAD must exist in both images so repository discovery can enter recovery.
void execute_transaction(const Repository& repo, TransactionPlan plan, const TransactionHook& hook = {});
// Prepared restores before; Committed restores after. Repeated recovery is safe.
// Recovery republishes files; ownership and modification times are not preserved.
void recover_transaction(const Repository& repo, const TransactionHook& hook = {});
void require_no_transaction(const Repository& repo);
// Includes backup blobs, metadata payload blobs and known semantic object roots.
std::set<ObjectId> transaction_roots(const Repository& repo);
}
