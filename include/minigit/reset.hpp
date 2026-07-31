#pragma once
#include "minigit/repository.hpp"
#include <optional>
#include <string>
namespace minigit {
enum class ResetMode { Soft, Mixed, Hard };
struct ResetOptions {
    ResetMode mode = ResetMode::Mixed;
    std::optional<std::string> revision;
};
// Acquires one RepoLock. All modes validate the complete current Index and
// target snapshot. Soft preserves Index/work and rejects conflicts; Mixed
// replaces Index and preserves work; Hard also restores tracked work, with
// default 0644/0755 permissions and protection for unknown files/directories.
// Preserves the current symbolic branch or detached state. HEAD/ref, reflogs,
// Index and work publish in one transaction. Missing revision defaults to HEAD;
// a default unborn HEAD means an empty target, while explicit HEAD rejects it.
// Index: 100000 distinct paths, 300000 stages, 64 MiB encoding. Revision: 4096
// bytes. Hard path union: 200000 paths / 64 MiB, 4096 bytes / 128 components
// per path; shared planner and transaction budgets also apply before capture.
void reset_repository(const Repository& repo, const ResetOptions& options = {});
}
