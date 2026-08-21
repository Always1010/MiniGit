#include "minigit/transaction.hpp"
#include "transaction_journal.hpp"
#include "transaction_metadata.hpp"
#include "minigit/error.hpp"
#include "minigit/repo_path.hpp"
#include "minigit/object_store.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
namespace minigit {
namespace {
constexpr std::size_t max_changes = 100000;
using transaction_detail::State;
using transaction_detail::StoredMetadata;
using transaction_detail::Journal;
using transaction_detail::validate_metadata_path;
using transaction_detail::validate_metadata_bytes;
using transaction_detail::validate_stash_pair;
struct Steps {
    const TransactionHook& hook;
    std::size_t count = 0;
    void notify(std::string_view label) { ++count; if (hook) hook(count, label); }
};
std::filesystem::path journal_path(const Repository& repo) { return repo.metadata() / "transaction"; }
std::filesystem::path staging_path(const Repository& repo) { return repo.metadata() / "transaction-staging"; }
std::optional<std::filesystem::file_status> node_status(const std::filesystem::path& path) {
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto value = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && value.type() == std::filesystem::file_type::not_found)) return std::nullopt;
    if (ec) throw Error("检查事务路径失败: " + ec.message());
    return value;
}
void validate_path(std::string_view path) {
    validate_repo_path_limits(path);
}
NodeImage inspect_node(const std::filesystem::path& root, std::string_view path, bool hash_file = true) {
    std::filesystem::path current = root;
    const std::filesystem::path relative{std::string(path)};
    for (auto it = relative.begin(); it != relative.end(); ++it) {
        current /= *it;
        std::error_code ec;
        const auto value = std::filesystem::symlink_status(current, ec);
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory ||
            (!ec && value.type() == std::filesystem::file_type::not_found)) return {};
        if (ec) throw Error("检查事务节点失败: " + ec.message());
        if (std::filesystem::is_symlink(value)) throw Error("事务路径禁止软链接");
        auto next = it;
        if (++next != relative.end()) {
            if (std::filesystem::is_regular_file(value)) return {};
            if (!std::filesystem::is_directory(value)) throw Error("事务路径包含不支持的文件类型");
            continue;
        }
        if (std::filesystem::is_directory(value)) {
            return {NodeKind::Directory, std::nullopt, static_cast<std::uint32_t>(value.permissions()) & 07777};
        }
        if (!std::filesystem::is_regular_file(value)) throw Error("事务节点必须是普通文件或目录");
        if (!hash_file) return {NodeKind::File, std::nullopt, static_cast<std::uint32_t>(value.permissions()) & 07777};
        auto data = read_file_data(current);
        return {NodeKind::File, hash_object(ObjectType::Blob, data.bytes), data.permissions};
    }
    throw Error("事务节点路径为空");
}
Bytes blob(const ObjectStore& store, const ObjectId& id) {
    auto object = store.read(id);
    if (object.type != ObjectType::Blob) throw Error("事务备份对象不是 blob");
    return std::move(object.payload);
}
void validate_image(const NodeImage& image, const ObjectStore& store, std::set<ObjectId>& checked) {
    if (image.permissions > 07777) throw Error("事务权限超出范围");
    if (image.kind == NodeKind::Absent) {
        if (image.blob || image.permissions != 0) throw Error("缺失节点不能带对象或权限");
    } else if (image.kind == NodeKind::File) {
        if (!image.blob || (image.permissions & 0400) == 0) throw Error("事务文件必须具有备份及所有者读取权限");
        if (checked.insert(*image.blob).second) (void)blob(store, *image.blob);
    } else if (image.kind == NodeKind::Directory) {
        if (image.blob || (image.permissions & 0500) != 0500) throw Error("事务目录必须可由所有者读取和遍历");
    } else throw Error("未知事务节点类型");
}
void add_budget(std::size_t& used, std::size_t count) {
    if (count > max_file_size - used) throw Error("事务数据超过 64 MiB 限制");
    used += count;
}
void validate_plan(const Repository& repo, const TransactionPlan& plan) {
    validate_transaction_budget(plan);
    const auto ordered = [](const auto& a, const auto& b) { return a.path < b.path; };
    if (!std::is_sorted(plan.work.begin(), plan.work.end(), ordered) || !std::is_sorted(plan.metadata.begin(), plan.metadata.end(), ordered))
        throw Error("事务 journal 路径顺序非法");
    const ObjectStore store(repo.metadata() / "objects");
    std::set<ObjectId> checked;
    std::map<std::string, const WorkChange*> work;
    std::set<std::string> metadata;
    std::size_t path_bytes = 0, metadata_bytes = 0;
    for (const auto& change : plan.work) {
        validate_path(change.path);
        add_budget(path_bytes, change.path.size());
        if (!work.emplace(change.path, &change).second) throw Error("事务工作路径重复");
        validate_image(change.before, store, checked);
        validate_image(change.after, store, checked);
    }
    for (const auto& [path, change] : work) {
        const auto prefix = path + "/";
        const auto child = work.lower_bound(prefix);
        if (child != work.end() && child->first.starts_with(prefix)) {
            for (const auto* image : {&change->before, &change->after})
                if (image->kind == NodeKind::Directory && (image->permissions & 0700) != 0700)
                    throw Error("含事务子项的目录必须具有所有者读写遍历权限");
        }
        for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
            const auto parent = work.find(path.substr(0, slash));
            if (parent == work.end()) continue;
            for (const bool after : {false, true}) {
                const auto& image = after ? change->after : change->before;
                const auto& ancestor = after ? parent->second->after : parent->second->before;
                if (image.kind != NodeKind::Absent && ancestor.kind != NodeKind::Directory) throw Error("事务目标节点存在文件与目录冲突");
            }
        }
    }
    for (const auto& change : plan.metadata) {
        validate_metadata_path(change.path);
        if (change.path == "HEAD" && (!change.before || !change.after)) throw Error("事务 HEAD 不能缺失或删除");
        add_budget(path_bytes, change.path.size());
        if (!metadata.insert(change.path).second) throw Error("事务元数据路径重复");
        for (const auto* bytes : {&change.before, &change.after}) if (*bytes) {
            add_budget(metadata_bytes, (**bytes).size());
            validate_metadata_bytes(repo, change, **bytes);
        }
    }
    validate_stash_pair(plan);
    // Metadata file/ancestor conversions need a separate directory schema.
    for (const auto& path : metadata) {
        const auto prefix = path + "/";
        const auto child = metadata.lower_bound(prefix);
        if (child != metadata.end() && child->starts_with(prefix)) throw Error("事务元数据文件路径不能互为祖先");
    }
}
Journal read_journal(const Repository& repo) {
    auto journal = transaction_detail::decode_journal(read_file(journal_path(repo)));
    const ObjectStore store(repo.metadata() / "objects");
    std::size_t metadata_bytes = 0;
    for (const auto& stored : journal.metadata) {
        MetadataChange change{stored.path, std::nullopt, std::nullopt};
        if (stored.before) {
            change.before = blob(store, *stored.before);
            add_budget(metadata_bytes, change.before->size());
        }
        if (stored.after) {
            change.after = blob(store, *stored.after);
            add_budget(metadata_bytes, change.after->size());
        }
        journal.plan.metadata.push_back(std::move(change));
    }
    validate_plan(repo, journal.plan);
    return journal;
}
std::optional<Bytes> metadata_current(const Repository& repo, const std::string& path) {
    const auto node = inspect_node(repo.metadata(), path, false);
    if (node.kind == NodeKind::Absent) return std::nullopt;
    if (node.kind != NodeKind::File) throw Error("事务元数据必须是普通文件");
    return read_file(repo.metadata() / path);
}
void inventory(const Repository& repo, const std::string& directory, const std::map<std::string, const WorkChange*>& work) {
    detail::require_directory(repo.root() / directory);
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(repo.root() / directory, ec), end;
    if (ec) throw Error("检查事务目录子项失败: " + ec.message());
    std::size_t count = 0;
    for (; it != end; it.increment(ec)) {
        if (ec) throw Error("检查事务目录子项失败: " + ec.message());
        if (++count > max_changes || it.depth() >= 128) throw Error("事务目录子项超过限制");
        detail::reject_symlinks(it->path());
        const auto path = it->path().lexically_relative(repo.root()).generic_string();
        if (!work.contains(path)) throw Error("事务目录包含未记录子项: " + path);
        const auto value = it->symlink_status(ec);
        if (ec || (!std::filesystem::is_regular_file(value) && !std::filesystem::is_directory(value))) throw Error("事务目录含不支持的子项");
        if (std::filesystem::is_directory(value)) detail::require_directory(it->path());
    }
    if (ec) throw Error("检查事务目录子项失败: " + ec.message());
}
void check_parent(const Repository& repo, const std::filesystem::path& root, const std::string& path,
                  const std::map<std::string, const WorkChange*>& work, bool worktree) {
    const auto device = detail::directory_device(repo.metadata());
    // Planned directories can already be mount points. Check their real device
    // before allowing topology changes to bypass a currently blocking file.
    for (auto ancestor = std::filesystem::path(path).parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path()) {
        if (inspect_node(root, ancestor.generic_string(), false).kind == NodeKind::Directory && detail::directory_device(root / ancestor) != device)
            throw Error("事务不能跨文件系统发布");
    }
    auto parent = std::filesystem::path(path).parent_path();
    while (!parent.empty()) {
        if (worktree) {
            const auto planned = work.find(parent.generic_string());
            if (planned != work.end()) {
                if (planned->second->before.kind == NodeKind::Directory || planned->second->after.kind == NodeKind::Directory) {
                    parent = parent.parent_path();
                    continue;
                }
            }
        }
        const auto node = inspect_node(root, parent.generic_string(), false);
        if (node.kind == NodeKind::File) throw Error("事务发布父路径被文件遮挡");
        if (node.kind == NodeKind::Directory) break;
        if (worktree) throw Error("事务缺失未记录父目录");
        parent = parent.parent_path();
    }
    const auto absolute = root / parent;
    if (detail::directory_device(absolute) != device) throw Error("事务不能跨文件系统发布");
    if (::faccessat(AT_FDCWD, absolute.c_str(), W_OK | X_OK, AT_EACCESS) != 0) throw Error("事务发布父目录不可写或不可遍历");
}
void preflight(const Repository& repo, const TransactionPlan& plan, bool recovery) {
    const auto device = detail::directory_device(repo.metadata());
    std::map<std::string, const WorkChange*> work;
    for (const auto& change : plan.work) work.emplace(change.path, &change);
    for (const auto& change : plan.work) {
        const auto current = inspect_node(repo.root(), change.path);
        if (current.kind == NodeKind::File && detail::node_device(repo.root() / change.path) != device) throw Error("事务不能跨文件系统发布");
        const bool intermediate_absent = current.kind == NodeKind::Absent && change.before.kind != change.after.kind;
        if (current != change.before && (!recovery || (current != change.after && !intermediate_absent)))
            throw Error("工作区已被事务之外的操作修改: " + change.path);
        if (current.kind == NodeKind::Directory) {
            if (detail::directory_device(repo.root() / change.path) != device) throw Error("事务不能跨文件系统发布");
            inventory(repo, change.path, work);
        }
        check_parent(repo, repo.root(), change.path, work, true);
    }
    for (const auto& change : plan.metadata) {
        const auto current = metadata_current(repo, change.path);
        if (current && detail::node_device(repo.metadata() / change.path) != device) throw Error("事务不能跨文件系统发布");
        if (current != change.before && (!recovery || current != change.after)) throw Error("元数据已被事务之外的操作修改: " + change.path);
        check_parent(repo, repo.metadata(), change.path, work, false);
    }
}
using StageImages = std::map<std::string, NodeImage>;
std::string stage_name(bool after, bool metadata, std::size_t index, NodeKind kind) {
    return std::string(after ? "a-" : "b-") + (metadata ? "m-" : "w-") + std::to_string(index) + (kind == NodeKind::Directory ? "-d" : "-f");
}
StageImages stage_images(const Journal& journal) {
    StageImages result;
    for (std::size_t i = 0; i < journal.plan.work.size(); ++i)
        for (const bool after : {false, true}) {
            const auto& value = after ? journal.plan.work[i].after : journal.plan.work[i].before;
            if (value.kind != NodeKind::Absent) result.emplace(stage_name(after, false, i, value.kind), value);
        }
    for (std::size_t i = 0; i < journal.metadata.size(); ++i)
        for (const bool after : {false, true}) {
            const auto& id = after ? journal.metadata[i].after : journal.metadata[i].before;
            if (id) result.emplace(stage_name(after, true, i, NodeKind::File), NodeImage{NodeKind::File, id, 0644});
        }
    return result;
}
bool canonical_stage_name(std::string_view name, NodeKind kind) {
    if (name.size() < 7 || (name[0] != 'a' && name[0] != 'b') || name[1] != '-' ||
        (name[2] != 'w' && name[2] != 'm') || name[3] != '-' || name[name.size() - 2] != '-') return false;
    const char suffix = kind == NodeKind::Directory ? 'd' : 'f';
    if (name.back() != suffix || (name[2] == 'm' && suffix != 'f')) return false;
    const auto number = name.substr(4, name.size() - 6);
    if (number.empty() || (number.size() > 1 && number[0] == '0')) return false;
    std::size_t value = 0;
    for (const char c : number) { if (c < '0' || c > '9') return false; value = value * 10 + static_cast<std::size_t>(c - '0'); if (value >= max_changes) return false; }
    return true;
}
std::vector<std::pair<std::filesystem::path, NodeKind>> staging_inventory(const Repository& repo, const StageImages* expected) {
    std::vector<std::pair<std::filesystem::path, NodeKind>> result;
    const auto root = staging_path(repo);
    const auto value = node_status(root);
    if (!value) return result;
    if (!std::filesystem::is_directory(*value) || (static_cast<std::uint32_t>(value->permissions()) & 07777) != 0700)
        throw Error("事务 staging 必须是权限为 0700 的目录");
    detail::require_directory(root);
    std::error_code ec;
    std::filesystem::directory_iterator it(root, ec), end;
    if (ec) throw Error("检查事务 staging 失败: " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) throw Error("检查事务 staging 失败: " + ec.message());
        if (result.size() >= max_changes * 2) throw Error("事务 staging 条目过多");
        const auto name = it->path().filename().string();
        detail::reject_symlinks(it->path());
        const auto node = inspect_node(root, name);
        if (!canonical_stage_name(name, node.kind)) throw Error("事务 staging 含未知条目");
        if (node.kind == NodeKind::Directory) {
            detail::require_directory(it->path());
            std::filesystem::directory_iterator children(it->path(), ec);
            if (ec || children != end) throw Error("事务 staging 目录必须为空");
        }
        if (expected) {
            const auto found = expected->find(name);
            if (found == expected->end() || found->second != node) throw Error("事务 staging 内容已被外部修改");
        }
        result.emplace_back(it->path(), node.kind);
    }
    if (ec) throw Error("检查事务 staging 失败: " + ec.message());
    return result;
}
void prepare_staging(const Repository& repo, const StageImages& images, Steps& steps) {
    if (!node_status(staging_path(repo))) { detail::create_directory_exact(staging_path(repo), 0700); steps.notify("staging.create"); }
    const ObjectStore store(repo.metadata() / "objects");
    for (const auto& [name, image] : images) {
        const auto target = staging_path(repo) / name;
        if (node_status(target)) continue;
        if (image.kind == NodeKind::File) atomic_write_with_permissions(target, blob(store, *image.blob), image.permissions);
        else detail::create_directory_exact(target, image.permissions);
        steps.notify("staging.publish:" + name);
    }
}
void cleanup(const Repository& repo, const StageImages* images, Steps& steps) {
    const auto entries = staging_inventory(repo, images);
    for (const auto& [path, kind] : entries) {
        if (kind == NodeKind::File) detail::remove_regular(path);
        else detail::remove_empty_directory(path);
        steps.notify("cleanup.staging:" + path.filename().string());
    }
    if (node_status(staging_path(repo))) { detail::remove_empty_directory(staging_path(repo)); steps.notify("cleanup.directory"); }
}
void ensure_metadata_parent(const Repository& repo, const std::string& path, Steps& steps) {
    auto current = repo.metadata();
    for (const auto& part : std::filesystem::path(path).parent_path()) {
        current /= part;
        if (!node_status(current)) { detail::create_directory_exact(current, 0755); steps.notify("metadata.mkdir:" + current.lexically_relative(repo.metadata()).generic_string()); }
        else detail::require_directory(current);
    }
}
void prune_metadata_parent(const Repository& repo, const std::string& path, Steps& steps) {
    std::filesystem::path boundary;
    if (path.starts_with("refs/heads/")) boundary = "refs/heads";
    else if (path.starts_with("refs/tags/")) boundary = "refs/tags";
    else if (path.starts_with("logs/refs/heads/")) boundary = "logs/refs/heads";
    else return;
    auto parent = std::filesystem::path(path).parent_path();
    while (parent != boundary && !parent.empty()) {
        const auto absolute = repo.metadata() / parent;
        if (node_status(absolute)) {
            detail::require_directory(absolute);
            std::error_code ec;
            const std::filesystem::directory_iterator entries(absolute, ec), end;
            if (ec) throw Error("检查事务元数据空目录失败: " + ec.message());
            if (entries != end) return;
            detail::remove_empty_directory(absolute);
            steps.notify("metadata.rmdir:" + parent.generic_string());
        }
        parent = parent.parent_path();
    }
}
void sync_parents(const std::filesystem::path& root, const std::string& path) {
    for (auto parent = std::filesystem::path(path).parent_path(); !parent.empty(); parent = parent.parent_path())
        if (inspect_node(root, parent.generic_string(), false).kind == NodeKind::Directory) detail::sync_directory(root / parent);
    detail::sync_directory(root);
}
void apply(const Repository& repo, const Journal& journal, bool after, Steps& steps, bool recovery = false) {
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < journal.plan.work.size(); ++i) order.push_back(i);
    auto depth = [&](std::size_t i) { return std::count(journal.plan.work[i].path.begin(), journal.plan.work[i].path.end(), '/'); };
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return depth(a) != depth(b) ? depth(a) > depth(b) : journal.plan.work[a].path > journal.plan.work[b].path; });
    auto target = [&](std::size_t i) -> const NodeImage& { return after ? journal.plan.work[i].after : journal.plan.work[i].before; };
    for (const auto i : order) {
        const auto& change = journal.plan.work[i];
        if (inspect_node(repo.root(), change.path).kind == NodeKind::File && target(i).kind != NodeKind::File) {
            detail::remove_regular(repo.root() / change.path); steps.notify("work.unlink:" + change.path);
        }
    }
    for (const auto i : order) {
        const auto& change = journal.plan.work[i];
        if (inspect_node(repo.root(), change.path).kind == NodeKind::Directory && target(i).kind != NodeKind::Directory) {
            detail::remove_empty_directory(repo.root() / change.path); steps.notify("work.rmdir:" + change.path);
        }
    }
    std::reverse(order.begin(), order.end());
    for (const auto i : order) {
        const auto& change = journal.plan.work[i];
        if (target(i).kind != NodeKind::Directory) continue;
        const auto current = inspect_node(repo.root(), change.path);
        if (current.kind == NodeKind::Absent) {
            // Moving a directory across parents requires write access to it on
            // Linux. A read-only leaf is instead created exclusively with its
            // exact mode; its journalled staging image remains for cleanup.
            if ((target(i).permissions & 0200) == 0)
                detail::create_directory_exact(repo.root() / change.path, target(i).permissions, true);
            else detail::rename_staged(staging_path(repo) / stage_name(after, false, i, NodeKind::Directory), repo.root() / change.path);
            steps.notify("work.mkdir:" + change.path);
        } else {
            detail::chmod_directory(repo.root() / change.path, target(i).permissions);
            sync_parents(repo.root(), change.path);
            steps.notify("work.chmod:" + change.path);
        }
    }
    for (const auto i : order) {
        const auto& change = journal.plan.work[i];
        if (target(i).kind != NodeKind::File) continue;
        if (!recovery && inspect_node(repo.root(), change.path) == target(i)) { sync_parents(repo.root(), change.path); continue; }
        detail::rename_staged(staging_path(repo) / stage_name(after, false, i, NodeKind::File), repo.root() / change.path);
        steps.notify("work.write:" + change.path);
    }
    for (const auto i : order) if (target(i).kind == NodeKind::Absent) sync_parents(repo.root(), journal.plan.work[i].path);
    for (std::size_t i = 0; i < journal.plan.metadata.size(); ++i) {
        const auto& change = journal.plan.metadata[i];
        const auto& bytes = after ? change.after : change.before;
        if (metadata_current(repo, change.path) == bytes && (!recovery || !bytes)) {
            if (!bytes) prune_metadata_parent(repo, change.path, steps);
            sync_parents(repo.metadata(), change.path);
            continue;
        }
        if (!bytes) detail::remove_regular(repo.metadata() / change.path);
        else {
            ensure_metadata_parent(repo, change.path, steps);
            detail::rename_staged(staging_path(repo) / stage_name(after, true, i, NodeKind::File), repo.metadata() / change.path);
        }
        steps.notify("metadata.write:" + change.path);
        if (!bytes) { prune_metadata_parent(repo, change.path, steps); sync_parents(repo.metadata(), change.path); }
    }
}
}
void validate_transaction_budget(const TransactionPlan& plan) {
    if (plan.work.size() > max_changes || plan.metadata.size() > max_changes - plan.work.size())
        throw Error("事务改动超过限制");
    std::size_t path_bytes = 0, metadata_bytes = 0;
    for (const auto& change : plan.work) add_budget(path_bytes, change.path.size());
    for (const auto& change : plan.metadata) {
        add_budget(path_bytes, change.path.size());
        for (const auto* bytes : {&change.before, &change.after})
            if (*bytes) add_budget(metadata_bytes, (**bytes).size());
    }
    // MGTR v1: magic/version/state/counts plus the final SHA-256 checksum.
    std::size_t journal_bytes = 49;
    auto add_journal = [&](std::size_t count) {
        if (count > max_file_size - journal_bytes) throw Error("事务 journal 超过 64 MiB 限制");
        journal_bytes += count;
    };
    for (const auto& change : plan.work) {
        add_journal(4); // Path length.
        add_journal(change.path.size());
        for (const auto* image : {&change.before, &change.after}) {
            add_journal(6); // Kind, permissions, and optional Blob presence.
            if (image->blob) add_journal(32);
        }
    }
    for (const auto& change : plan.metadata) {
        add_journal(4);
        add_journal(change.path.size());
        for (const auto* bytes : {&change.before, &change.after}) {
            add_journal(1); // The payload is stored separately as a Blob.
            if (*bytes) add_journal(32);
        }
    }
}
NodeImage capture_work_node(const Repository& repo, std::string_view path) {
    require_no_transaction(repo);
    validate_path(path);
    const auto current = inspect_node(repo.root(), path, false);
    if (current.kind != NodeKind::File) return current;
    auto data = read_file_data(repo.root() / std::string(path));
    const auto id = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, data.bytes);
    return {NodeKind::File, id, data.permissions};
}
void require_no_transaction(const Repository& repo) {
    if (node_status(journal_path(repo)) || node_status(staging_path(repo))) throw Error("存在未恢复事务，请先恢复");
}
void execute_transaction(const Repository& repo, TransactionPlan plan, const TransactionHook& hook) {
    require_no_transaction(repo);
    std::sort(plan.work.begin(), plan.work.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    std::sort(plan.metadata.begin(), plan.metadata.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    // Authenticate the complete plan and current before images.
    validate_plan(repo, plan);
    preflight(repo, plan, false);
    Journal journal;
    journal.plan = std::move(plan);
    ObjectStore store(repo.metadata() / "objects");
    for (const auto& change : journal.plan.metadata) {
        StoredMetadata stored{change.path, std::nullopt, std::nullopt};
        if (change.before) stored.before = store.write(ObjectType::Blob, *change.before);
        if (change.after) stored.after = store.write(ObjectType::Blob, *change.after);
        journal.metadata.push_back(std::move(stored));
    }
    // Encode both durable states before staging or changing live paths.
    const auto prepared = transaction_detail::encode_journal(journal);
    auto committed = journal;
    committed.state = State::Committed;
    const auto committed_bytes = transaction_detail::encode_journal(committed);
    const auto images = stage_images(journal);
    Steps steps{hook};
    prepare_staging(repo, images, steps);
    (void)staging_inventory(repo, &images);
    preflight(repo, journal.plan, false);
    // Prepared is the rollback point: recovery restores the actual before.
    atomic_write(journal_path(repo), prepared);
    steps.notify("journal.prepared");
    apply(repo, journal, true, steps);
    // Committed is the roll-forward point: recovery republishes after.
    atomic_write(journal_path(repo), committed_bytes);
    steps.notify("journal.committed");
    cleanup(repo, &images, steps);
    detail::remove_regular(journal_path(repo)); steps.notify("cleanup.journal");
}
void recover_transaction(const Repository& repo, const TransactionHook& hook) {
    Steps steps{hook};
    if (!node_status(journal_path(repo))) {
        cleanup(repo, nullptr, steps);
        return;
    }
    // Re-authenticate the journal, immutable payloads and current paths before
    // rebuilding staging. The durable state chooses the recovery direction.
    const auto journal = read_journal(repo);
    const auto images = stage_images(journal);
    (void)staging_inventory(repo, &images);
    preflight(repo, journal.plan, true);
    prepare_staging(repo, images, steps);
    (void)staging_inventory(repo, &images);
    preflight(repo, journal.plan, true);
    apply(repo, journal, journal.state == State::Committed, steps, true);
    cleanup(repo, &images, steps);
    detail::remove_regular(journal_path(repo)); steps.notify("cleanup.journal");
}
std::set<ObjectId> transaction_roots(const Repository& repo) {
    if (!node_status(journal_path(repo))) { require_no_transaction(repo); return {}; }
    const auto journal = read_journal(repo);
    std::set<ObjectId> roots;
    for (const auto& change : journal.plan.work)
        for (const auto* value : {&change.before, &change.after}) if (value->blob) roots.insert(*value->blob);
    for (std::size_t i = 0; i < journal.metadata.size(); ++i) {
        for (const auto* id : {&journal.metadata[i].before, &journal.metadata[i].after}) if (*id) roots.insert(**id);
        const auto& change = journal.plan.metadata[i];
        for (const auto* bytes : {&change.before, &change.after}) if (*bytes) validate_metadata_bytes(repo, change, **bytes, &roots);
    }
    return roots;
}
}
