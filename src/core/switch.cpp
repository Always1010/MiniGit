#include "minigit/switch.hpp"
#include "minigit/branches.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/worktree.hpp"
#include "minigit/worktree_update.hpp"
#include <set>
#include <utility>
namespace minigit {
namespace {
std::optional<FileState> state_at(const Snapshot& snapshot, const std::string& path) {
    const auto found = snapshot.find(path);
    return found == snapshot.end() ? std::nullopt : std::optional(found->second);
}
Snapshot commit_snapshot(const Repository& repo, const std::optional<ObjectId>& id) {
    return id ? read_snapshot(repo, read_commit(repo, *id).tree) : Snapshot{};
}
void validate_index_budget(const Index& index) {
    if (index.size() > 100000) throw Error("切换分支索引条目超过快照限制");
    std::size_t path_bytes = 0;
    for (const auto& entry : index) {
        if (entry.path.size() > max_file_size - path_bytes || 40 > max_file_size - path_bytes - entry.path.size())
            throw Error("切换分支索引快照数据超过 64 MiB 限制");
        path_bytes += entry.path.size() + 40;
    }
}
Snapshot checked_index(const Repository& repo, const Index& index) {
    validate_index_budget(index);
    const ObjectStore store(repo.metadata() / "objects");
    std::set<ObjectId> checked;
    Snapshot result;
    bool conflicts = false;
    for (const auto& entry : index) {
        if (checked.insert(entry.id).second && store.read(entry.id).type != ObjectType::Blob)
            throw Error("切换分支的索引对象不是 blob");
        if (entry.stage != 0) conflicts = true;
        else result.emplace_hint(result.end(), entry.path, FileState{entry.mode, entry.id});
    }
    if (conflicts) throw Error("索引存在未解决冲突，无法切换分支");
    return result;
}
}
Head switch_worktree(const Repository& repo, const SwitchOptions& options) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    if (options.detach && options.new_branch) throw Error("switch -c 与 --detach 不能同时使用");
    if (!options.detach && !options.new_branch && options.target.empty()) throw Error("switch 需要目标分支");
    const auto before_head = read_head(repo);
    Head desired;
    std::string reason;
    if (options.new_branch) {
        const auto ref = branch_ref(*options.new_branch);
        if (before_head.symbolic == ref || read_ref(repo, ref)) throw Error("新分支名称已经存在或属于当前分支");
        const bool unborn = options.target.empty() && !before_head.id;
        desired = {ref, unborn ? std::nullopt : std::optional(resolve_commit(repo, options.target.empty() ? "HEAD" : options.target))};
        reason = "switch:" + *options.new_branch;
    } else if (options.detach) {
        desired = {std::nullopt, resolve_commit(repo, options.target.empty() ? "HEAD" : options.target)};
        reason = "switch:detach";
    } else {
        const auto ref = branch_ref(options.target);
        const auto id = read_ref(repo, ref);
        if (!id && !(before_head.symbolic == ref && !before_head.id)) throw Error("目标分支不存在");
        desired = {ref, id};
        reason = "switch:" + options.target;
    }
    const auto a = commit_snapshot(repo, before_head.id);
    const auto d = commit_snapshot(repo, desired.id);
    const auto index = load_index(repo);
    const auto b = checked_index(repo, index);
    Snapshot next_index = b;
    WorktreeTargets targets;
    std::set<std::string> paths;
    std::set<std::string> owned;
    std::size_t path_bytes = 0;
    auto add_path = [&](const std::string& path) {
        if (paths.contains(path)) return;
        if (paths.size() >= 200000 || path.size() > max_file_size - path_bytes)
            throw Error("切换分支路径数量或总字节超过限制");
        paths.insert(path);
        path_bytes += path.size();
    };
    for (const auto& [path, state] : a) { (void)state; add_path(path); owned.insert(path); }
    for (const auto& [path, state] : b) { (void)state; add_path(path); owned.insert(path); }
    for (const auto& [path, state] : d) { (void)state; add_path(path); }
    for (const auto& path : paths) {
        const auto old = state_at(a, path);
        const auto staged = state_at(b, path);
        const auto target = state_at(d, path);
        if (old == target || staged == target) continue;
        if (staged != old)
            throw Error("切换分支将覆盖未提交改动或未跟踪文件: " + path);
        if (target) next_index[path] = *target;
        else next_index.erase(path);
        targets.emplace(path, WorktreeTarget{target, std::nullopt});
    }
    Index after_index;
    after_index.reserve(next_index.size());
    for (const auto& [path, state] : next_index) after_index.push_back({path, state.mode, state.id});
    // Validate the combined index before any planner can capture backup objects.
    validate_index_budget(after_index);
    (void)encode_index(after_index);
    // Delay worktree reads until the combined index passes all schema budgets.
    for (const auto& [path, target] : targets) {
        (void)target;
        if (read_worktree_entry(repo, path) != state_at(b, path))
            throw Error("切换分支将覆盖未提交改动或未跟踪文件: " + path);
    }
    TransactionPlan plan;
    if (options.new_branch && desired.id)
        plan_create_branch(repo, plan, *options.new_branch, *desired.id);
    plan_move_head(repo, plan, before_head, desired, reason);
    if (after_index != index) plan_index_update(repo, plan, after_index);
    plan_worktree_update(repo, plan, targets, owned);
    if (!plan.work.empty() || !plan.metadata.empty()) execute_transaction(repo, std::move(plan));
    return desired;
}
}
