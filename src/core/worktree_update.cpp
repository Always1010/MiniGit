#include "minigit/worktree_update.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/worktree.hpp"
#include <algorithm>
#include <functional>
#include <utility>
namespace minigit {
namespace {
constexpr std::size_t max_inputs = 200000;
constexpr std::size_t max_changes = 100000;
void validate_path(std::string_view path) {
    validate_repo_path_limits(path);
}
template<class Paths, class Key>
void validate_paths(const Paths& paths, Key key) {
    if (paths.size() > max_inputs) throw Error("工作区更新输入路径超过限制");
    std::size_t bytes = 0;
    for (const auto& value : paths) {
        const auto& path = key(value);
        validate_path(path);
        if (path.size() > max_file_size - bytes) throw Error("工作区更新输入路径超过 64 MiB 限制");
        bytes += path.size();
    }
}
bool owned_directory(const std::set<std::string>& owned, const std::string& path) {
    if (owned.contains(path)) return true;
    const auto prefix = path + "/";
    const auto child = owned.lower_bound(prefix);
    return child != owned.end() && child->starts_with(prefix);
}
NodeImage inspect(const Repository& repo, const std::string& path) {
    if (const auto file = read_worktree_file(repo, path))
        return {NodeKind::File, file->state.id, file->data.permissions};
    const auto target = repo.root() / path;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec) throw Error("检查工作区更新路径失败: " + ec.message());
    if (!std::filesystem::is_directory(status)) throw Error("工作区更新节点类型不支持");
    return {NodeKind::Directory, std::nullopt, static_cast<std::uint32_t>(status.permissions()) & 07777};
}
void check_blobs(const Index& index, const ObjectStore& store, std::set<ObjectId>& checked) {
    for (const auto& entry : index)
        if (checked.insert(entry.id).second && store.read(entry.id).type != ObjectType::Blob)
            throw Error("索引更新引用的对象不是 blob");
}
std::optional<Bytes> index_bytes(const Repository& repo) {
    const auto path = repo.metadata() / "index";
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) return std::nullopt;
    if (ec) throw Error("检查索引更新失败: " + ec.message());
    return read_file(path);
}
}
void plan_worktree_update(const Repository& repo, TransactionPlan& plan,
                          const WorktreeTargets& targets, const std::set<std::string>& owned_paths) {
    require_no_transaction(repo);
    if (!plan.work.empty()) throw Error("工作区更新计划已经包含工作区改动");
    validate_paths(targets, [](const auto& entry) -> const std::string& { return entry.first; });
    validate_paths(owned_paths, [](const auto& path) -> const std::string& { return path; });
    const ObjectStore store(repo.metadata() / "objects");
    std::set<ObjectId> checked;
    std::map<std::string, NodeImage> desired;
    std::size_t desired_path_bytes = 0;
    auto set_desired = [&](const std::string& path, NodeImage image) {
        const auto found = desired.find(path);
        if (found != desired.end()) { found->second = std::move(image); return; }
        if (desired.size() >= max_inputs || path.size() > max_file_size - desired_path_bytes)
            throw Error("工作区更新展开目标路径超过数量或 64 MiB 限制");
        desired.emplace(path, std::move(image));
        desired_path_bytes += path.size();
    };
    for (const auto& [path, target] : targets) {
        if (target.directory && target.file) throw Error("工作区更新目标不能同时为文件和目录");
        if (target.directory) {
            const auto permissions = target.permissions.value_or(0755);
            if (permissions > 07777 || (permissions & 0500) != 0500)
                throw Error("工作区更新目录缺少所有者读取遍历权限或权限超限");
            set_desired(path, {NodeKind::Directory, std::nullopt, permissions});
            continue;
        }
        if (!target.file) {
            if (target.permissions) throw Error("删除目标不能带权限");
            set_desired(path, NodeImage{});
            continue;
        }
        const auto& file = *target.file;
        if (file.mode != "100644" && file.mode != "100755") throw Error("工作区更新文件模式非法");
        const auto permissions = target.permissions.value_or(file.mode == "100755" ? 0755U : 0644U);
        if (permissions > 07777 || (permissions & 0400) == 0 || ((permissions & 0111) != 0) != (file.mode == "100755"))
            throw Error("工作区更新权限与文件模式不一致");
        if (checked.insert(file.id).second && store.read(file.id).type != ObjectType::Blob)
            throw Error("工作区更新目标对象不是 blob");
        set_desired(path, NodeImage{NodeKind::File, file.id, permissions});
    }
    for (const auto& [path, target] : targets) if (target.file || target.directory) {
        for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
            const auto parent = path.substr(0, slash);
            const auto ancestor = targets.find(parent);
            if (ancestor != targets.end() && ancestor->second.file) throw Error("工作区更新目标存在文件与目录冲突");
            // Explicit Directory permissions survive automatic parent expansion.
            if (ancestor == targets.end() || !ancestor->second.directory)
                set_desired(parent, {NodeKind::Directory, std::nullopt, 0755});
        }
    }
    std::map<std::string, NodeImage> actual;
    std::size_t actual_path_bytes = 0;
    auto current = [&](const std::string& path) -> const NodeImage& {
        const auto found = actual.find(path);
        if (found != actual.end()) return found->second;
        if (actual.size() >= max_inputs || path.size() > max_file_size - actual_path_bytes)
            throw Error("工作区更新实际节点路径超过数量或 64 MiB 限制");
        const auto inserted = actual.emplace(path, inspect(repo, path));
        actual_path_bytes += path.size();
        return inserted.first->second;
    };
    auto preserve_directory_permissions = [&](const std::string& path) {
        auto& after = desired.at(path);
        const auto& before = current(path);
        const auto target = targets.find(path);
        const bool explicit_permissions = target != targets.end() && target->second.directory && target->second.permissions;
        if (before.kind == NodeKind::Directory && after.kind == NodeKind::Directory && !explicit_permissions)
            after.permissions = before.permissions;
    };
    // Visited directories and forced WorkChanges are distinct: a file noop must
    // not prevent a later directory traversal, and keep/delete intents differ.
    std::map<std::string, bool> inventoried;
    std::set<std::string> forced;
    std::size_t forced_path_bytes = 0;
    auto force = [&](const std::string& path) {
        if (forced.contains(path)) return;
        if (forced.size() >= max_changes || path.size() > max_file_size - forced_path_bytes)
            throw Error("工作区更新完整目录改动超过数量或 64 MiB 限制");
        forced.insert(path);
        forced_path_bytes += path.size();
    };
    std::function<void(const std::string&, bool)> inventory;
    inventory = [&](const std::string& directory, bool keep) {
        const auto visited = inventoried.find(directory);
        if (visited != inventoried.end() && visited->second == keep) return;
        inventoried.insert_or_assign(directory, keep);
        if (!owned_directory(owned_paths, directory)) throw Error("工作区更新不能修改未知目录: " + directory);
        force(directory);
        if ((current(directory).permissions & 0500) != 0500) throw Error("工作区更新原目录缺少读取遍历权限");
        std::error_code ec;
        const std::filesystem::directory_iterator children(repo.root() / directory, ec);
        if (ec) throw Error("遍历工作区更新目录失败: " + ec.message());
        for (const auto& child : children) {
            const auto path = directory + "/" + child.path().filename().string();
            validate_path(path);
            const auto& node = current(path);
            if (node.kind == NodeKind::File) {
                const auto target = targets.find(path);
                if (!owned_paths.contains(path)) throw Error("工作区更新目录包含未授权文件: " + path);
                if (!keep) {
                    if (target == targets.end() || target->second.file || target->second.directory)
                        throw Error("工作区更新目录包含未授权删除文件: " + path);
                    set_desired(path, {});
                } else if (!desired.contains(path)) set_desired(path, node);
                force(path);
            } else if (node.kind == NodeKind::Directory) {
                if (!keep) {
                    const auto after = desired.find(path);
                    if (after != desired.end() && after->second.kind != NodeKind::Absent)
                        throw Error("工作区更新删除目录不能保留子节点: " + path);
                    set_desired(path, {});
                } else if (!desired.contains(path)) set_desired(path, node);
                preserve_directory_permissions(path);
                inventory(path, desired.at(path).kind == NodeKind::Directory);
            } else throw Error("工作区更新遍历时节点已消失");
        }
    };
    // Snapshot the initial roots; recursive inventory may add descendants.
    std::vector<std::string> roots;
    roots.reserve(desired.size());
    for (const auto& [path, after] : desired) { (void)after; roots.push_back(path); }
    for (const auto& path : roots) {
        preserve_directory_permissions(path);
        const auto& after = desired.at(path);
        const auto& before = current(path);
        if (before.kind == NodeKind::Directory && before != after)
            inventory(path, after.kind == NodeKind::Directory);
    }
    std::vector<std::string> changed;
    std::size_t changed_path_bytes = 0;
    for (const auto& [path, after] : desired) {
        const auto& before = current(path);
        if (before == after && !forced.contains(path)) continue;
        if (before.kind == NodeKind::File && !owned_paths.contains(path)) throw Error("工作区更新不能覆盖未知文件: " + path);
        if (before.kind == NodeKind::File && after.kind == NodeKind::Directory) {
            const auto target = targets.find(path);
            if (target == targets.end() || target->second.file) throw Error("工作区更新文件转目录必须显式删除原文件: " + path);
        }
        if (before.kind == NodeKind::File && (before.permissions & 0400) == 0) throw Error("工作区更新原文件缺少所有者读取权限");
        for (const auto* image : {&before, &after})
            if (image->kind == NodeKind::Directory && (image->permissions & 0500) != 0500)
                throw Error("工作区更新目录缺少读取遍历权限");
        if (path.size() > max_file_size - changed_path_bytes) throw Error("工作区更新改动路径超过 64 MiB 限制");
        changed_path_bytes += path.size();
        changed.push_back(path);
        if (changed.size() > max_changes) throw Error("工作区更新改动超过限制");
    }
    auto next = plan;
    // The journal limit covers both kinds of changes. Check before capturing
    // immutable backups, as metadata may already have been added by the caller.
    if (next.metadata.size() > max_changes || changed.size() > max_changes - next.metadata.size())
        throw Error("工作区与元数据更新总数超过事务限制");
    next.work.reserve(changed.size());
    for (const auto& path : changed)
        next.work.push_back({path, actual.at(path), desired.at(path)});
    // Validate both final Work images, including forced noops, before saving any
    // backup. Transaction requires rwx on every Directory with a Work descendant,
    // even when that descendant will be Absent in this side of the journal.
    const std::set<std::string> work_paths(changed.begin(), changed.end());
    for (const auto& change : next.work) {
        const auto prefix = change.path + "/";
        const auto child = work_paths.lower_bound(prefix);
        if (child != work_paths.end() && child->starts_with(prefix))
            for (const auto* image : {&change.before, &change.after})
                if (image->kind == NodeKind::Directory && (image->permissions & 0700) != 0700)
                    throw Error("工作区更新含子项目录缺少所有者读写遍历权限");
        for (auto slash = change.path.find('/'); slash != std::string::npos; slash = change.path.find('/', slash + 1)) {
            const auto parent = change.path.substr(0, slash);
            if (!work_paths.contains(parent)) continue;
            for (const bool after : {false, true}) {
                const auto& image = after ? change.after : change.before;
                const auto& ancestor = after ? desired.at(parent) : actual.at(parent);
                if (image.kind != NodeKind::Absent && ancestor.kind != NodeKind::Directory)
                    throw Error("工作区更新节点存在文件与目录冲突");
            }
        }
    }
    validate_transaction_budget(next);
    for (auto& change : next.work) {
        auto before = capture_work_node(repo, change.path);
        if (before != change.before) throw Error("工作区更新捕获时节点已变化: " + change.path);
        change.before = std::move(before);
    }
    plan = std::move(next);
}
void plan_index_update(const Repository& repo, TransactionPlan& plan, const Index& index) {
    require_no_transaction(repo);
    for (const auto& change : plan.metadata) if (change.path == "index") throw Error("索引已被计划中的其他操作修改");
    const auto after = encode_index(index);
    const auto before = index_bytes(repo);
    const ObjectStore store(repo.metadata() / "objects");
    std::set<ObjectId> checked;
    check_blobs(index, store, checked);
    if (before) check_blobs(decode_index(*before), store, checked);
    if (before && *before == after) return;
    auto next = plan;
    next.metadata.push_back({"index", before, after});
    plan = std::move(next);
}
}
