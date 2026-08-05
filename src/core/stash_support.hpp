#pragma once
#include "minigit/stash_stack.hpp"
#include "minigit/snapshot.hpp"
namespace minigit::stash_support {
// Caller-held lock; no Work or mutable metadata reads/writes.
void validate_saved_tree(const Snapshot& snapshot);
Snapshot combined_work(const Snapshot& work, const Snapshot& untracked);
struct SavedEntry {
    StashEntry entry;
    Snapshot head, index, work, untracked;
};
SavedEntry load_saved_entry(const Repository& repo, ObjectId work);
}
