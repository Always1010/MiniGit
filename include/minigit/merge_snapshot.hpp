#pragma once
#include "minigit/merge_text.hpp"
#include "minigit/snapshot.hpp"
#include <string>
#include <vector>
namespace minigit {
enum class MergeConflictKind { Text, Binary, AddAdd, DeleteModify, Mode, DirectoryFile };
struct MergeConflict {
    std::string path; // DirectoryFile uses the shortest disjoint conflict root.
    MergeConflictKind kind;
    std::vector<TextConflict> text;
    bool mode_conflict = false; // Also retained alongside content conflicts.
    bool operator==(const MergeConflict&) const = default;
};
struct SnapshotMergeResult {
    Index index; // Canonical path/stage order; conflicts keep original stages only.
    Snapshot work; // Complete result files, with no directory/file collisions.
    std::vector<MergeConflict> conflicts;
    bool coarse = false;
};
// Caller holds RepoLock. Validates all three schemas and typed Blobs before any
// immutable write. Each input and final result has at most 100000 distinct files;
// final Index permits 300000 stages and 64 MiB encoded. Union is at most 200000
// paths / 64 MiB key bytes. Snapshot limits count files, not future Tree entries.
// Reads content one path at a time, may save merged/marker Blobs, and never writes
// Work, Index, refs, logs or operation state. Late failure may leave unreachable
// immutable Blobs. D/F regions retain ours layout and original 1/2/3 stages.
SnapshotMergeResult merge_snapshots(const Repository& repo, const Snapshot& base,
                                    const Snapshot& ours, const Snapshot& theirs);
}
