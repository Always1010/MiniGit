#pragma once
#include "minigit/repository.hpp"
namespace minigit {
// Acquires one RepoLock and moves one tracked regular file or directory prefix
// in one transaction. Lexical paths are relative to CWD or absolute. An existing
// destination directory appends the source basename once; the resulting root
// must be absent both in work and Index. Root/self/ancestor/descendant moves and
// selected conflicts are rejected. No force or destination overwrite is offered.
// Index mode/id and actual work bytes/full file permissions move independently;
// new directories use 0755. Source directory permissions are not transferred.
// Unknown/ignored descendants, empty directories and unsafe nodes are protected.
// HEAD/refs/logs/identity and unselected Index stages remain unchanged. Limits
// follow checkout path/Index and transaction budgets. Planning failure may retain
// immutable target/backup blobs, but never publishes work or metadata changes.
// Interrupted execution may leave a pending journal and requires recover.
void move_path(const Repository& repo, const std::filesystem::path& source,
               const std::filesystem::path& destination);
}
