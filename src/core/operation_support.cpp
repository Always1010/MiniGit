#include "operation_support.hpp"
#include "binary_codec.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include "minigit/worktree.hpp"
#include <algorithm>
#include <set>
#include <utility>
namespace minigit::operation_support {
namespace {
std::optional<FileState> state_at(const Snapshot& snapshot, const std::string& path) {
    const auto found = snapshot.find(path);
    return found == snapshot.end() ? std::nullopt : std::optional(found->second);
}
void require_untouched_parents(const Repository& repo, const WorkChange& change,
                               const std::set<std::string>& saved) {
    if (change.before.kind == NodeKind::Absent) return;
    const auto path = std::string_view(change.path);
    for (auto slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
        const auto parent = std::string(path.substr(0, slash));
        if (saved.contains(parent)) continue;
        const auto absolute = repo.root() / parent;
        detail::reject_symlinks(absolute);
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(absolute, ec);
        if (ec || !std::filesystem::is_directory(status)) throw Error("中止操作的区域外父目录已变化: " + parent);
    }
}
}
std::optional<Bytes> raw_metadata(const Repository& repo, std::string_view name) {
    const auto path = repo.metadata() / name;
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) return std::nullopt;
    if (ec) throw Error("检查操作元数据失败: " + ec.message());
    if (!std::filesystem::is_regular_file(status)) throw Error("操作元数据必须是普通文件");
    return read_file(path);
}
Index checked_index(const Repository& repo, const std::optional<Bytes>& raw) {
    if (!raw) return {};
    // Bound the codec's count-driven allocation before decoding all stages.
    if (raw->size() >= 12) {
        detail::ByteReader count_field{std::span(*raw).subspan(8, 4), "索引计数被截断"};
        const auto count = count_field.u32();
        if (count > 300000) throw Error("操作索引超过 300000 stage 条目限制");
    }
    auto index = decode_index(*raw);
    checkout_support::check_index_objects(repo, index);
    return index;
}
Snapshot resolved_snapshot(const Index& index) {
    Snapshot snapshot;
    for (const auto& entry : index) {
        if (entry.stage != 0) throw Error("操作索引存在未解决冲突");
        snapshot.emplace_hint(snapshot.end(), entry.path, FileState{entry.mode, entry.id});
    }
    return snapshot;
}
void validate_message(std::string_view message) {
    if (message.empty() || message.size() > max_file_size || message.find('\0') != std::string_view::npos ||
        std::all_of(message.begin(), message.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }))
        throw Error("操作消息必须包含非空白内容且禁止 NUL");
}
WorkObservations clean_work(const Repository& repo, const Snapshot& snapshot) {
    WorkObservations observed;
    for (const auto& [path, state] : snapshot) {
        const auto file = read_worktree_file(repo, path);
        if (!file || file->state != state) throw Error("操作需要洁净的 tracked 工作区: " + path);
        observed.emplace_hint(observed.end(), path, NodeImage{NodeKind::File, file->state.id, file->data.permissions});
    }
    return observed;
}
void check_work_before(const TransactionPlan& plan, const WorkObservations& observed) {
    for (const auto& change : plan.work) {
        const auto found = observed.find(change.path);
        if (found != observed.end() && found->second != change.before)
            throw Error("操作规划期间 tracked 工作区发生变化: " + change.path);
    }
}
WorktreeTargets changed_targets(const Snapshot& before, const Snapshot& after,
                                checkout_support::BoundedPaths& owned) {
    checkout_support::BoundedPaths paths;
    for (const auto& [path, state] : before) { (void)state; paths.add(path); owned.add(path); }
    for (const auto& [path, state] : after) { (void)state; paths.add(path); }
    WorktreeTargets targets;
    for (const auto& path : paths.paths)
        if (state_at(before, path) != state_at(after, path))
            targets.emplace_hint(targets.end(), path, WorktreeTarget{state_at(after, path), std::nullopt});
    return targets;
}
void check_original_metadata(const Repository& repo, const Head& head,
                             const std::optional<Bytes>& raw_head, const std::optional<Bytes>& raw_index) {
    if (read_head(repo) != head || raw_metadata(repo, "HEAD") != raw_head || raw_metadata(repo, "index") != raw_index)
        throw Error("操作规划期间 HEAD 或索引发生变化");
}
// Save unchanged conflict nodes without publishing them or granting directory ownership.
std::vector<WorkChange> conflict_work(const Repository& repo, const TransactionPlan& plan,
                                    const SnapshotMergeResult& merged,
                                    const WorkObservations& observed) {
    std::map<std::string, WorkChange> changes;
    checkout_support::BoundedPaths keys;
    std::set<std::string> required;
    const auto add_key = [&](const std::string& path) {
        keys.add(path);
        if (keys.paths.size() > 100000) throw Error("操作受影响路径超过 100000 条目限制");
    };
    for (const auto& change : plan.work) { add_key(change.path); changes.emplace(change.path, change); }
    for (const auto& entry : merged.index) if (entry.stage != 0) { add_key(entry.path); required.insert(entry.path); }
    std::set<std::string> regions;
    for (const auto& conflict : merged.conflicts)
        if (conflict.kind == MergeConflictKind::DirectoryFile) {
            add_key(conflict.path); regions.insert(conflict.path); required.insert(conflict.path);
        }
    // At most 128 ancestors per bounded conflicted path. Only real directories
    // inside a DF region are saved; unrelated parents and unknown children stay out.
    for (const auto& entry : merged.index) {
        if (entry.stage == 0) continue;
        const auto path = std::string_view(entry.path);
        for (auto slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
            const auto ancestor = std::string(path.substr(0, slash));
            if (regions.contains(ancestor)) {
                add_key(ancestor);
                for (auto inner = path.find('/', slash + 1); inner != std::string_view::npos; inner = path.find('/', inner + 1))
                    add_key(std::string(path.substr(0, inner)));
                break;
            }
        }
    }
    for (const auto& path : keys.paths) {
        if (changes.contains(path)) continue;
        const auto image = capture_work_node(repo, path);
        const auto prior = observed.find(path);
        if (prior != observed.end() && prior->second != image)
            throw Error("保存操作冲突时 tracked 工作区发生变化: " + path);
        // Intermediate DF ancestors that are not directories need no record.
        if (required.contains(path) || image.kind == NodeKind::Directory) changes.emplace(path, WorkChange{path, image, image});
    }
    std::vector<WorkChange> work;
    work.reserve(changes.size());
    for (auto& [path, change] : changes) { (void)path; work.push_back(std::move(change)); }
    return work;
}
OperationState active_operation(const Repository& repo, OperationKind expected_kind) {
    const auto state = read_operation(repo);
    if (!state || state->kind != expected_kind) throw Error("没有正在进行的匹配 operation");
    if (read_head(repo) != state->current_head) throw Error("操作期间 HEAD 已变化");
    return *state;
}
TransactionPlan plan_abort_operation(const Repository& repo, const OperationState& state) {
    const auto raw_index = raw_metadata(repo, "index");
    const auto index = checked_index(repo, raw_index);
    std::optional<Bytes> original_index;
    if (state.original_index_blob) original_index = ObjectStore(repo.metadata() / "objects").read(*state.original_index_blob).payload;
    checkout_support::BoundedPaths owned;
    std::set<std::string> saved;
    WorktreeTargets targets;
    for (const auto& change : state.work) {
        owned.add(change.path);
        saved.insert(change.path);
        WorktreeTarget target;
        if (change.before.kind == NodeKind::File) {
            target.file = FileState{(change.before.permissions & 0111) ? "100755" : "100644", *change.before.blob};
            target.permissions = change.before.permissions;
        } else if (change.before.kind == NodeKind::Directory) {
            target.directory = true;
            target.permissions = change.before.permissions;
        }
        targets.emplace_hint(targets.end(), change.path, target);
    }
    for (const auto& entry : index) owned.add(entry.path);
    for (const auto& change : state.work) {
        require_untouched_parents(repo, change, saved);
        if (change.before.kind == NodeKind::Directory) continue;
        const auto prefix = change.path + "/";
        const auto begin = std::lower_bound(index.begin(), index.end(), prefix,
            [](const auto& entry, const std::string& path) { return entry.path < path; });
        for (auto it = begin; it != index.end() && it->path.starts_with(prefix); ++it)
            if (!targets.contains(it->path)) targets.emplace(it->path, WorktreeTarget{});
    }
    TransactionPlan plan;
    plan_move_head(repo, plan, state.current_head, state.original_head, "operation:abort");
    if (raw_index != original_index) plan.metadata.push_back({"index", raw_index, original_index});
    const auto raw_head = raw_metadata(repo, "HEAD");
    if (raw_head != std::optional(state.original_head_bytes)) {
        const auto head_change = std::find_if(plan.metadata.begin(), plan.metadata.end(),
            [](const auto& change) { return change.path == "HEAD"; });
        if (head_change != plan.metadata.end()) head_change->after = state.original_head_bytes;
        else plan.metadata.push_back({"HEAD", raw_head, state.original_head_bytes});
    }
    plan_worktree_update(repo, plan, targets, owned.paths);
    plan_operation_update(repo, plan, state, std::nullopt);
    check_original_metadata(repo, state.current_head, raw_head, raw_index);
    return plan;
}
}
