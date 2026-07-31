#include "minigit/remove.hpp"
#include "checkout_support.hpp"
#include "minigit/error.hpp"
#include "minigit/refs.hpp"
#include "minigit/worktree.hpp"
#include "minigit/worktree_update.hpp"
#include <map>
#include <set>
#include <utility>
namespace minigit {
namespace {
std::optional<FileState> state_at(const Snapshot& snapshot, const std::string& path) {
    const auto found = snapshot.find(path);
    return found == snapshot.end() ? std::nullopt : std::optional(found->second);
}
// A null read_worktree_entry also describes directories and blocked children.
// Walk every ancestor without following links to distinguish those from absence.
NodeImage observe_work(const Repository& repo, const std::string& path) {
    auto current = repo.root();
    const std::filesystem::path relative(path);
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        current /= *part;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && status.type() == std::filesystem::file_type::not_found)) return {};
        if (error) throw Error("检查删除路径失败: " + error.message());
        if (std::filesystem::is_symlink(status)) throw Error("删除工作区路径禁止软链接: " + path);
        auto next = part;
        const bool last = ++next == relative.end();
        if (std::filesystem::is_directory(status)) {
            if (last) return {NodeKind::Directory, std::nullopt, static_cast<std::uint32_t>(status.permissions()) & 07777};
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) throw Error("删除工作区路径包含不支持的节点: " + path);
        if (!last) throw Error("删除路径的父目录已被文件替换: " + path);
        const auto file = read_worktree_file(repo, path);
        if (!file) throw Error("删除检查时工作区节点已变化: " + path);
        return {NodeKind::File, file->state.id, file->data.permissions};
    }
    throw Error("删除工作区路径为空");
}
FileState image_state(const NodeImage& image) {
    return {(image.permissions & 0111) == 0 ? "100644" : "100755", *image.blob};
}
void check_observations(const TransactionPlan& plan, const std::map<std::string, NodeImage>& observed) {
    std::map<std::string, const NodeImage*> before;
    for (const auto& change : plan.work) before.emplace(change.path, &change.before);
    for (const auto& [path, image] : observed) {
        const auto found = before.find(path);
        if (found == before.end()) {
            if (image.kind == NodeKind::Absent) continue;
        } else if (*found->second == image) continue;
        throw Error("删除规划时工作区节点已变化: " + path);
    }
}
}
void remove_paths(const Repository& repo, const std::vector<std::filesystem::path>& paths,
                  const RemoveOptions& options) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    const auto selectors = checkout_support::normalize_selectors(repo, paths);
    const auto index = load_index(repo);
    checkout_support::check_index_objects(repo, index);
    const auto head = read_head(repo);
    const auto a = head.id ? read_snapshot(repo, read_commit(repo, *head.id).tree) : Snapshot{};
    checkout_support::BoundedPaths known;
    Snapshot b;
    std::set<std::string> conflicts;
    for (const auto& entry : index) {
        known.add(entry.path);
        if (entry.stage == 0) b.emplace_hint(b.end(), entry.path, FileState{entry.mode, entry.id});
        else conflicts.insert(entry.path);
    }
    const auto selected = checkout_support::select_known_paths(known, selectors);
    if (!options.recursive)
        for (const auto& selector : selectors)
            if (selector.empty() || checkout_support::has_descendants(known.paths, selector))
                throw Error("删除目录前缀需要 -r: " + selector);
    Index after;
    after.reserve(index.size());
    for (const auto& entry : index) if (!selected.contains(entry.path)) after.push_back(entry);
    checkout_support::validate_index_budget(after);
    (void)encode_index(after);
    std::map<std::string, NodeImage> observed;
    if (!options.force) {
        for (const auto& path : selected) {
            if (conflicts.contains(path)) throw Error("删除选中的索引路径存在未解决冲突: " + path);
            const auto staged = state_at(b, path);
            const bool matches_head = staged == state_at(a, path);
            if (options.cached && matches_head) continue;
            if (!options.cached && !matches_head) throw Error("删除将丢弃已暂存修改: " + path);
            const auto image = observe_work(repo, path);
            const bool matches_work = image.kind == NodeKind::File && staged == image_state(image);
            if (!matches_work && (options.cached || image.kind != NodeKind::Absent))
                throw Error("删除将丢弃未保存的工作区修改: " + path);
            if (!options.cached) observed.emplace_hint(observed.end(), path, image);
        }
    }
    TransactionPlan plan;
    plan_index_update(repo, plan, after);
    if (!options.cached) {
        WorktreeTargets targets;
        for (const auto& path : selected) targets.emplace_hint(targets.end(), path, WorktreeTarget{});
        plan_worktree_update(repo, plan, targets, selected);
        if (!options.force) check_observations(plan, observed);
    }
    execute_transaction(repo, std::move(plan));
}
}
