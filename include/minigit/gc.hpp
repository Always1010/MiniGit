#pragma once
#include "minigit/repository.hpp"
#include <cstddef>
namespace minigit {
struct GcOptions { bool prune_now = false; };
struct GcResult { std::size_t retained = 0, removed = 0; };
// Acquires RepoLock and refuses pending short transactions; active operations
// are allowed and retain their complete semantic roots. Validates every loose
// object, typed edge and root before deleting anything, including orphan data.
// Only lowercase 2/62-hex fanout/file layouts are accepted: at most 100000
// objects, 256 fanouts, 1000000 edges and 64 MiB of logical graph descriptions.
// This counts node IDs/types/paths, edge IDs/types, Commit actor/message bytes
// and Tree name/mode bytes; it is not an exact heap-usage bound.
// Ref/log traversal is bounded to 100000 entries each, paths to 4096 bytes/128
// components; root metadata paths and raw bytes share a separate 64 MiB limit.
// Default one-day grace treats fresh objects as extra roots with full closure.
// prune_now disables grace; future mtimes remain roots under both policies.
// Does not read Work, require identity, expire reflogs or remove empty fanouts.
// Deletion uses referrer-first topological order and individual pinned-directory
// unlink+fsync, preserving remaining graph closure after a syscall failure.
// It is not an atomic transaction.
GcResult collect_garbage(const Repository& repo, const GcOptions& options = {});
}
