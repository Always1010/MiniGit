#pragma once
#include "minigit/objects.hpp"
#include "minigit/index.hpp"
#include <map>
namespace minigit {
struct FileState {
    std::string mode;
    ObjectId id;
    bool operator==(const FileState&) const = default;
};
using Snapshot = std::map<std::string, FileState>;
Commit read_commit(const Repository& repo, const ObjectId& id);
// Snapshot expansion is bounded to 128 tree levels, 100000 entries and 64 MiB
// of expanded path/tree data. Reused subtrees count separately at each prefix;
// each blob is checked once and released, under the per-object 64 MiB limit.
Snapshot read_snapshot(const Repository& repo, const ObjectId& tree);
// Caller must hold RepoLock; all paths, modes and blob objects are validated.
ObjectId write_snapshot(const Repository& repo, const Snapshot& snapshot);
Snapshot index_snapshot(const Repository& repo);
}
