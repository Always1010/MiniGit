#include "minigit/restore.hpp"
#include "checkout_support.hpp"
#include "minigit/error.hpp"
#include "minigit/refs.hpp"
#include "minigit/worktree_update.hpp"
#include <algorithm>
#include <set>
#include <utility>
namespace minigit {
namespace {
using checkout_support::BoundedPaths;
struct CheckedIndex {
    Snapshot source;
    BoundedPaths paths;
    std::set<std::string> conflicts;
};
CheckedIndex check_index(const Repository& repo, const Index& index) {
    checkout_support::check_index_objects(repo, index);
    CheckedIndex result;
    for (const auto& entry : index) {
        result.paths.add(entry.path);
        if (entry.stage == 0) result.source.emplace_hint(result.source.end(), entry.path, FileState{entry.mode, entry.id});
        else result.conflicts.insert(entry.path);
    }
    return result;
}
std::set<std::string> select_paths(const Snapshot& source, const BoundedPaths& index_paths,
                                 const std::vector<std::string>& selectors) {
    auto candidates = index_paths;
    for (const auto& [path, state] : source) { (void)state; candidates.add(path); }
    return checkout_support::select_known_paths(candidates, selectors);
}
}
void restore_paths(const Repository& repo, const std::vector<std::filesystem::path>& paths,
                   const RestoreOptions& options) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    if (options.source && (options.source->empty() || options.source->size() > 4096))
        throw Error("恢复来源为空或超过 4096 字节限制");
    const auto selectors = checkout_support::normalize_selectors(repo, paths);
    const auto index = load_index(repo);
    const auto checked = check_index(repo, index);
    const bool worktree = options.worktree || !options.staged;
    const bool index_source = !options.source && !options.staged;
    const auto source = index_source ? checked.source :
        read_snapshot(repo, read_commit(repo, resolve_commit(repo, options.source ? *options.source : "HEAD")).tree);
    const auto selected = select_paths(source, checked.paths, selectors);
    if (index_source)
        for (const auto& path : selected)
            if (checked.conflicts.contains(path)) throw Error("恢复选中的索引路径存在未解决冲突: " + path);
    Index next_index;
    if (options.staged) {
        next_index.reserve(index.size());
        for (const auto& entry : index) if (!selected.contains(entry.path)) next_index.push_back(entry);
        for (const auto& path : selected) {
            const auto found = source.find(path);
            if (found != source.end()) next_index.push_back({path, found->second.mode, found->second.id});
        }
        // Combined distinct-path/stage budgets and DF topology must pass before
        // any worktree inspection or immutable backup-object capture.
        checkout_support::validate_index_budget(next_index);
        (void)encode_index(next_index);
        std::sort(next_index.begin(), next_index.end(), [](const auto& left, const auto& right) {
            return left.path == right.path ? left.stage < right.stage : left.path < right.path;
        });
    }
    BoundedPaths owned;
    WorktreeTargets targets;
    if (worktree) {
        owned = checked.paths;
        const auto head = read_head(repo);
        if (head.id)
            for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, *head.id).tree)) {
                (void)state;
                owned.add(path);
            }
        for (const auto& path : selected) {
            const auto found = source.find(path);
            targets.emplace_hint(targets.end(), path, WorktreeTarget{
                found == source.end() ? std::nullopt : std::optional(found->second), std::nullopt});
        }
    }
    TransactionPlan plan;
    if (options.staged && next_index != index) plan_index_update(repo, plan, next_index);
    if (worktree) plan_worktree_update(repo, plan, targets, owned.paths);
    if (!plan.work.empty() || !plan.metadata.empty()) execute_transaction(repo, std::move(plan));
}
}
