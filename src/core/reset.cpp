#include "minigit/reset.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/worktree_update.hpp"
#include <algorithm>
#include <set>
#include <utility>
namespace minigit {
namespace {
void validate_path(std::string_view path) {
    validate_repo_path(path);
    if (path.size() > 4096 || std::count(path.begin(), path.end(), '/') + 1 > 128)
        throw Error("重置路径超过 4096 字节或 128 分量限制");
}
void validate_index_budget(const Index& index) {
    if (index.size() > 300000) throw Error("重置索引阶段条目超过 300000 限制");
    std::set<std::string> paths;
    std::size_t bytes = 44;
    for (const auto& entry : index) {
        validate_path(entry.path);
        if (paths.insert(entry.path).second && paths.size() > 100000)
            throw Error("重置索引不同路径超过 100000 限制");
        if (entry.path.size() > max_file_size - bytes || 41 > max_file_size - bytes - entry.path.size())
            throw Error("重置索引超过 64 MiB 限制");
        bytes += entry.path.size() + 41;
    }
}
void check_index(const Repository& repo, const Index& index, bool soft) {
    validate_index_budget(index);
    const ObjectStore store(repo.metadata() / "objects");
    std::set<ObjectId> checked;
    bool conflicts = false;
    for (const auto& entry : index) {
        if (checked.insert(entry.id).second && store.read(entry.id).type != ObjectType::Blob)
            throw Error("重置索引对象不是 blob");
        conflicts |= entry.stage != 0;
    }
    if (soft && conflicts) throw Error("索引存在未解决冲突，无法 soft 重置");
}
struct BoundedPaths {
    std::set<std::string> paths;
    std::size_t bytes = 0;
    void add(const std::string& path) {
        if (paths.contains(path)) return;
        validate_path(path);
        if (paths.size() >= 200000 || path.size() > max_file_size - bytes)
            throw Error("重置路径数量或总字节超过限制");
        paths.insert(path);
        bytes += path.size();
    }
};
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
        validate_index_budget(after_index);
        (void)encode_index(after_index);
    }
    BoundedPaths paths, owned;
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
