#include "minigit/reset.hpp"
#include "checkout_support.hpp"
#include "minigit/error.hpp"
#include "minigit/reflog.hpp"
#include "minigit/worktree_update.hpp"
#include <algorithm>
#include <utility>
namespace minigit {
namespace {
void check_index(const Repository& repo, const Index& index, bool soft) {
    checkout_support::check_index_objects(repo, index);
    const bool conflicts = std::any_of(index.begin(), index.end(), [](const auto& entry) { return entry.stage != 0; });
    if (soft && conflicts) throw Error("索引存在未解决冲突，无法 soft 重置");
}
}
void reset_repository(const Repository& repo, const ResetOptions& options) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    std::string reason;
    switch (options.mode) {
    case ResetMode::Soft: reason = "reset:soft"; break;
    case ResetMode::Mixed: reason = "reset:mixed"; break;
    case ResetMode::Hard: reason = "reset:hard"; break;
    default: throw Error("非法重置模式");
    }
    if (options.revision && (options.revision->empty() || options.revision->size() > 4096))
        throw Error("重置来源为空或超过 4096 字节限制");
    const auto before_head = read_head(repo);
    const auto index = load_index(repo);
    check_index(repo, index, options.mode == ResetMode::Soft);
    const auto target_id = options.revision ? std::optional(resolve_commit(repo, *options.revision)) : before_head.id;
    const auto target = target_id ? read_snapshot(repo, read_commit(repo, *target_id).tree) : Snapshot{};
    Index after_index;
    if (options.mode != ResetMode::Soft) {
        after_index.reserve(target.size());
        for (const auto& [path, state] : target) after_index.push_back({path, state.mode, state.id});
        checkout_support::validate_index_budget(after_index);
        (void)encode_index(after_index);
    }
    checkout_support::BoundedPaths paths, owned;
    WorktreeTargets targets;
    if (options.mode == ResetMode::Hard) {
        const auto old = before_head.id ? read_snapshot(repo, read_commit(repo, *before_head.id).tree) : Snapshot{};
        for (const auto& [path, state] : old) { (void)state; paths.add(path); owned.add(path); }
        for (const auto& entry : index) { paths.add(entry.path); owned.add(entry.path); }
        for (const auto& [path, state] : target) { (void)state; paths.add(path); }
        for (const auto& path : paths.paths) {
            const auto found = target.find(path);
            targets.emplace_hint(targets.end(), path, WorktreeTarget{
                found == target.end() ? std::nullopt : std::optional(found->second), std::nullopt});
        }
    }
    TransactionPlan plan;
    plan_move_head(repo, plan, before_head, {before_head.symbolic, target_id}, reason);
    if (options.mode != ResetMode::Soft && after_index != index) plan_index_update(repo, plan, after_index);
    if (options.mode == ResetMode::Hard) plan_worktree_update(repo, plan, targets, owned.paths);
    if (!plan.work.empty() || !plan.metadata.empty()) execute_transaction(repo, std::move(plan));
}
}
