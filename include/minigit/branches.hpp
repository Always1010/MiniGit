#pragma once
#include "minigit/transaction.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace minigit {
struct BranchInfo {
    std::string name;
    std::optional<ObjectId> id;
    bool current = false;
    bool operator==(const BranchInfo&) const = default;
};
// Converts a validated short name to refs/heads/... within reflog path limits.
std::string branch_ref(std::string_view name);
// Includes the current unborn branch. Complete snapshots are checked once per
// unique commit ID; combined short-name bytes are limited to 64 MiB.
std::vector<BranchInfo> list_branches(const Repository& repo);
// Caller holds RepoLock. Pure planning; exceptions leave plan unchanged.
void plan_create_branch(const Repository& repo, TransactionPlan& plan,
                        std::string_view name, ObjectId target, bool force = false);
// Business operations acquire RepoLock and publish through a transaction.
void create_branch(const Repository& repo, std::string_view name,
                   std::string_view revision = "HEAD", bool force = false);
void delete_branch(const Repository& repo, std::string_view name, bool force = false);
}
