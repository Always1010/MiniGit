#pragma once
#include "minigit/refs.hpp"
#include <optional>
#include <string>
namespace minigit {
struct SwitchOptions {
    std::optional<std::string> new_branch;
    bool detach = false;
    // Empty defaults to HEAD only with detach or new_branch.
    std::string target;
};
// Acquires one RepoLock. Preserves staged/working changes using the four-way
// safety matrix; branch creation, HEAD, Index and work publish in one transaction.
Head switch_worktree(const Repository& repo, const SwitchOptions& options);
}
