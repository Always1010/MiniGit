#include "minigit/merge.hpp"
#include "checkout_support.hpp"
#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "minigit/worktree.hpp"
#include "minigit/worktree_update.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <utility>
namespace minigit {
namespace {
std::optional<Bytes> raw_metadata(const Repository& repo, std::string_view name) {
    const auto path = repo.metadata() / name;
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) return std::nullopt;
    if (ec) throw Error("检查合并元数据失败: " + ec.message());
    if (!std::filesystem::is_regular_file(status)) throw Error("合并元数据必须是普通文件");
    return read_file(path);
}
Index checked_index(const Repository& repo, const std::optional<Bytes>& raw) {
    if (!raw) return {};
    // Bound the codec's count-driven allocation before decoding all stages.
    if (raw->size() >= 12) {
        std::uint32_t count = 0;
        for (std::size_t i = 8; i < 12; ++i) count = (count << 8U) | (*raw)[i];
        if (count > 300000) throw Error("合并索引超过 300000 stage 条目限制");
    }
    auto index = decode_index(*raw);
    checkout_support::check_index_objects(repo, index);
    return index;
}
Snapshot resolved_snapshot(const Index& index) {
    Snapshot snapshot;
    for (const auto& entry : index) {
        if (entry.stage != 0) throw Error("合并索引存在未解决冲突");
        snapshot.emplace_hint(snapshot.end(), entry.path, FileState{entry.mode, entry.id});
    }
    return snapshot;
}
Index snapshot_index(const Snapshot& snapshot) {
    Index index;
    index.reserve(snapshot.size());
    for (const auto& [path, file] : snapshot) index.push_back({path, file.mode, file.id});
    checkout_support::validate_index_budget(index);
    (void)encode_index(index);
    return index;
}
std::optional<FileState> state_at(const Snapshot& snapshot, const std::string& path) {
    const auto found = snapshot.find(path);
    return found == snapshot.end() ? std::nullopt : std::optional(found->second);
}
void validate_message(std::string_view message) {
    if (message.empty() || message.size() > max_file_size || message.find('\0') != std::string_view::npos ||
        std::all_of(message.begin(), message.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }))
        throw Error("合并消息必须包含非空白内容且禁止 NUL");
}
void validate_options(const MergeOptions& options) {
    switch (options.action) {
    case MergeAction::Start:
        if (options.revision.empty() || options.revision.size() > 4096 || options.revision.find('\0') != std::string::npos)
            throw Error("合并来源必须非空且不超过 4096 字节");
        if (options.message) validate_message(*options.message);
        break;
    case MergeAction::Continue:
    case MergeAction::Abort:
        if (!options.revision.empty() || options.message) throw Error("继续或中止合并不接受来源和消息");
        break;
    default: throw Error("非法合并动作");
    }
}
std::map<std::string, NodeImage> clean_work(const Repository& repo, const Snapshot& snapshot) {
    std::map<std::string, NodeImage> observed;
    for (const auto& [path, state] : snapshot) {
        const auto file = read_worktree_file(repo, path);
        if (!file || file->state != state) throw Error("合并需要洁净的 tracked 工作区: " + path);
        observed.emplace_hint(observed.end(), path, NodeImage{NodeKind::File, file->state.id, file->data.permissions});
    }
    return observed;
}
void check_work_before(const TransactionPlan& plan, const std::map<std::string, NodeImage>& observed) {
    for (const auto& change : plan.work) {
        const auto found = observed.find(change.path);
        if (found != observed.end() && found->second != change.before)
            throw Error("合并规划期间 tracked 工作区发生变化: " + change.path);
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
        throw Error("合并规划期间 HEAD 或索引发生变化");
}
// Include unchanged conflict nodes in the saved affected region, without
// pretending that Start publishes those nodes or granting directory ownership.
std::vector<WorkChange> conflict_work(const Repository& repo, const TransactionPlan& plan,
                                    const SnapshotMergeResult& merged,
                                    const std::map<std::string, NodeImage>& observed) {
    std::map<std::string, WorkChange> changes;
    checkout_support::BoundedPaths keys;
    std::set<std::string> required;
    const auto add_key = [&](const std::string& path) {
        keys.add(path);
        if (keys.paths.size() > 100000) throw Error("合并受影响路径超过 100000 条目限制");
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
            throw Error("保存合并冲突时 tracked 工作区发生变化: " + path);
        // Intermediate DF ancestors that are not directories need no record.
        if (required.contains(path) || image.kind == NodeKind::Directory) changes.emplace(path, WorkChange{path, image, image});
    }
    std::vector<WorkChange> work;
    work.reserve(changes.size());
    for (auto& [path, change] : changes) { (void)path; work.push_back(std::move(change)); }
    return work;
}
MergeResult start_merge(const Repository& repo, const MergeOptions& options, const TransactionHook& hook) {
    require_no_operation(repo);
    const auto head = read_head(repo);
    const auto raw_head = raw_metadata(repo, "HEAD");
    if (!raw_head) throw Error("合并 HEAD 不存在");
    const auto raw_index = raw_metadata(repo, "index");
    const auto index = checked_index(repo, raw_index);
    const auto ours = head.id ? read_snapshot(repo, read_commit(repo, *head.id).tree) : Snapshot{};
    if (resolved_snapshot(index) != ours) throw Error("合并需要索引与 HEAD 完全一致");
    const auto source = resolve_commit(repo, options.revision);
    const auto theirs = read_snapshot(repo, read_commit(repo, source).tree);
    std::optional<ObjectId> base;
    if (head.id) base = find_merge_base(repo, *head.id, source);
    else (void)topological_order(load_commit_graph(repo, {source}));
    if (head.id && !base) throw Error("合并来源没有共同祖先");
    const auto observed = clean_work(repo, ours);
    if (head.id && base == source) return {MergeOutcome::UpToDate, head, {}};
    const auto message = options.message ? *options.message : "Merge " + options.revision;
    validate_message(message);
    const bool fast_forward = !head.id || base == head.id;
    SnapshotMergeResult merged;
    if (fast_forward) { merged.index = snapshot_index(theirs); merged.work = theirs; }
    else merged = merge_snapshots(repo, read_snapshot(repo, read_commit(repo, *base).tree), ours, theirs);
    checkout_support::validate_index_budget(merged.index);
    (void)encode_index(merged.index);
    checkout_support::BoundedPaths owned;
    const auto targets = changed_targets(ours, merged.work, owned);
    TransactionPlan plan;
    if (merged.index != index) plan_index_update(repo, plan, merged.index);
    plan_worktree_update(repo, plan, targets, owned.paths);
    check_work_before(plan, observed);
    Head desired = head;
    MergeOutcome outcome;
    if (!merged.conflicts.empty()) {
        OperationState state;
        state.original_head = state.current_head = head;
        state.original_head_bytes = *raw_head;
        state.source_commit = source;
        state.message = message;
        state.work = conflict_work(repo, plan, merged, observed);
        (void)encode_operation(state);
        if (raw_index) state.original_index_blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, *raw_index);
        plan_operation_update(repo, plan, std::nullopt, state);
        outcome = MergeOutcome::Conflicted;
    } else {
        desired.id = fast_forward ? source : store_commit_snapshot(repo, merged.work, {*head.id, source}, message);
        plan_move_head(repo, plan, head, desired, fast_forward ? "merge:fast-forward" : "merge");
        outcome = fast_forward ? MergeOutcome::FastForward : MergeOutcome::Merged;
    }
    check_original_metadata(repo, head, raw_head, raw_index);
    execute_transaction(repo, std::move(plan), hook);
    return {outcome, desired, std::move(merged.conflicts)};
}
OperationState active_merge(const Repository& repo) {
    const auto state = read_operation(repo);
    if (!state || state->kind != OperationKind::Merge) throw Error("没有正在进行的 Merge operation");
    if (read_head(repo) != state->current_head) throw Error("合并期间 HEAD 已变化");
    return *state;
}
MergeResult continue_merge(const Repository& repo, const TransactionHook& hook) {
    const auto state = active_merge(repo);
    const auto raw_index = raw_metadata(repo, "index");
    const auto index = checked_index(repo, raw_index);
    const auto snapshot = resolved_snapshot(index);
    const auto commit = store_commit_snapshot(repo, snapshot, {*state.original_head.id, state.source_commit}, state.message);
    const Head desired{state.current_head.symbolic, commit};
    TransactionPlan plan;
    plan_move_head(repo, plan, state.current_head, desired, "merge:continue");
    plan_operation_update(repo, plan, state, std::nullopt);
    if (raw_metadata(repo, "index") != raw_index) throw Error("继续合并时索引发生变化");
    execute_transaction(repo, std::move(plan), hook);
    return {MergeOutcome::Merged, desired, {}};
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
        if (ec || !std::filesystem::is_directory(status)) throw Error("中止合并的区域外父目录已变化: " + parent);
    }
}
MergeResult abort_merge(const Repository& repo, const TransactionHook& hook) {
    const auto state = active_merge(repo);
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
    plan_move_head(repo, plan, state.current_head, state.original_head, "merge:abort");
    if (raw_index != original_index) plan.metadata.push_back({"index", raw_index, original_index});
    const auto raw_head = raw_metadata(repo, "HEAD");
    if (raw_head != std::optional(state.original_head_bytes))
        plan.metadata.push_back({"HEAD", raw_head, state.original_head_bytes});
    plan_worktree_update(repo, plan, targets, owned.paths);
    plan_operation_update(repo, plan, state, std::nullopt);
    check_original_metadata(repo, state.current_head, raw_head, raw_index);
    execute_transaction(repo, std::move(plan), hook);
    return {MergeOutcome::Aborted, state.original_head, {}};
}
}
MergeResult merge_repository(const Repository& repo, const MergeOptions& options, const TransactionHook& hook) {
    validate_options(options);
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    switch (options.action) {
    case MergeAction::Start: return start_merge(repo, options, hook);
    case MergeAction::Continue: return continue_merge(repo, hook);
    case MergeAction::Abort: return abort_merge(repo, hook);
    default: throw Error("非法合并动作");
    }
}
}
