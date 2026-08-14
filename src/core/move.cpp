#include "minigit/move.hpp"
#include "checkout_support.hpp"
#include "minigit/error.hpp"
#include "minigit/worktree.hpp"
#include "minigit/worktree_update.hpp"
#include <map>
#include <utility>
namespace minigit {
namespace {
struct PathInfo {
    NodeKind kind = NodeKind::Absent;
    std::uint32_t permissions = 0;
};
// Inspect topology before reading C. Ancestor files and unsafe nodes must never
// masquerade as absence; this does not read payloads or follow symlinks.
PathInfo inspect_path(const Repository& repo, const std::string& path) {
    auto current = repo.root();
    const std::filesystem::path relative(path);
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        current /= *part;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && status.type() == std::filesystem::file_type::not_found)) return {};
        if (error) throw Error("检查移动路径失败: " + error.message());
        if (std::filesystem::is_symlink(status)) throw Error("移动路径禁止软链接: " + path);
        auto next = part;
        const bool last = ++next == relative.end();
        if (std::filesystem::is_directory(status)) {
            if (last) return {NodeKind::Directory, static_cast<std::uint32_t>(status.permissions()) & 07777};
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) throw Error("移动路径包含不支持的节点: " + path);
        if (!last) throw Error("移动路径的父目录被文件遮挡: " + path);
        return {NodeKind::File, static_cast<std::uint32_t>(status.permissions()) & 07777};
    }
    throw Error("移动路径为空");
}
bool below(const std::string& path, const std::string& parent) {
    return path == parent || path.starts_with(parent + "/");
}
const WorkChange& planned_change(const std::map<std::string, const WorkChange*>& changes, const std::string& path) {
    const auto found = changes.find(path);
    if (found == changes.end()) throw Error("移动规划时工作区节点已变化: " + path);
    return *found->second;
}
}
void move_path(const Repository& repo, const std::filesystem::path& source,
               const std::filesystem::path& destination) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    const auto names = checkout_support::normalize_selectors(repo, {source, destination});
    const auto& from = names[0];
    auto to = names[1];
    if (from.empty() || to.empty()) throw Error("移动不能使用仓库根路径");
    const auto index = load_index(repo);
    checkout_support::check_index_objects(repo, index);
    checkout_support::BoundedPaths known;
    for (const auto& entry : index) known.add(entry.path);
    const auto selected = checkout_support::select_known_paths(known, {from});
    for (const auto& entry : index)
        if (selected.contains(entry.path) && entry.stage != 0) throw Error("移动选中的索引路径存在未解决冲突: " + entry.path);
    const bool directory = !known.paths.contains(from);
    const auto source_info = inspect_path(repo, from);
    if (source_info.kind != (directory ? NodeKind::Directory : NodeKind::File))
        throw Error("移动源工作区类型与索引不一致或源已缺失");
    if (inspect_path(repo, to).kind == NodeKind::Directory)
        to += "/" + std::filesystem::path(from).filename().generic_string();
    checkout_support::validate_path(to);
    if (below(from, to) || below(to, from)) throw Error("移动源与目标不能相同或互为祖先");
    if (inspect_path(repo, to).kind != NodeKind::Absent) throw Error("移动目标工作区节点已经存在");
    if (known.paths.contains(to) || checkout_support::has_descendants(known.paths, to))
        throw Error("移动目标索引路径已经存在");
    std::map<std::string, std::string> destinations;
    checkout_support::BoundedPaths work_paths;
    if (directory) { work_paths.add(from); work_paths.add(to); }
    Index after;
    after.reserve(index.size());
    for (const auto& entry : index) {
        if (!selected.contains(entry.path)) { after.push_back(entry); continue; }
        auto path = directory ? to + entry.path.substr(from.size()) : to;
        work_paths.add(entry.path);
        work_paths.add(path);
        destinations.emplace_hint(destinations.end(), entry.path, path);
        after.push_back({std::move(path), entry.mode, entry.id});
    }
    validate_index_budget(after);
    (void)encode_index(after);
    TransactionPlan plan;
    plan_index_update(repo, plan, after);
    // Before reading or storing C, reject impossible metadata/journal budgets.
    // The shared planner checks combined Work budgets later, without duplicating
    // its directory inventory here. Late rejection may retain immutable blobs.
    validate_transaction_budget(plan);
    std::map<std::string, NodeImage> observed;
    WorktreeTargets targets;
    if (directory) {
        observed.emplace(from, NodeImage{NodeKind::Directory, std::nullopt, source_info.permissions});
        targets.emplace(from, WorktreeTarget{});
    }
    for (const auto& [path, moved] : destinations) {
        NodeImage image;
        {
            const auto file = read_worktree_file(repo, path);
            if (!file) throw Error("移动源文件已经缺失或变成目录: " + path);
            if ((file->data.permissions & 0400) == 0) throw Error("移动源文件缺少所有者读取权限: " + path);
            image = {NodeKind::File, file->state.id, file->data.permissions};
        }
        // This stores C independently of staged B, then verifies the actual
        // inode contents/permissions still agree with the original observation.
        if (capture_work_node(repo, path) != image) throw Error("移动保存时源文件已变化: " + path);
        observed.emplace_hint(observed.end(), path, image);
        const FileState working{(image.permissions & 0111) == 0 ? "100644" : "100755", *image.blob};
        targets.emplace(path, WorktreeTarget{});
        targets.emplace(moved, WorktreeTarget{working, image.permissions});
    }
    plan_worktree_update(repo, plan, targets, selected);
    std::map<std::string, const WorkChange*> changes;
    for (const auto& change : plan.work) changes.emplace(change.path, &change);
    for (const auto& [path, image] : observed)
        if (planned_change(changes, path).before != image) throw Error("移动规划时源节点已变化: " + path);
    for (const auto& [path, moved] : destinations) {
        (void)path;
        if (planned_change(changes, moved).before != NodeImage{}) throw Error("移动规划时目标节点已经存在: " + moved);
    }
    if (directory) {
        const auto& root = planned_change(changes, to);
        if (root.before != NodeImage{} || root.after.kind != NodeKind::Directory)
            throw Error("移动规划时目标目录已经存在: " + to);
    }
    execute_transaction(repo, std::move(plan));
}
}
