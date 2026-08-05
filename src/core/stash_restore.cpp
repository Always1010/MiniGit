#include "minigit/stash.hpp"
#include "minigit/stash_stack.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "operation_support.hpp"
#include "stash_support.hpp"
#include <algorithm>
#include <map>
#include <span>
#include <utility>
namespace minigit {
namespace {
using Group = std::span<const IndexEntry>;
Group group_at(const Index& index, const std::string& path) {
    const auto first = std::lower_bound(index.begin(), index.end(), path,
        [](const auto& entry, const auto& key) { return entry.path < key; });
    auto last = first;
    while (last != index.end() && last->path == path) ++last;
    return {first, last};
}
bool same_group(Group a, Group b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); }
class IndexGroups {
public:
    explicit IndexGroups(const Index& index) {
        for (auto it = index.begin(); it != index.end();) {
            const auto group = group_at(index, it->path);
            put(it->path, group);
            it += static_cast<Index::difference_type>(group.size());
        }
    }
    void put(const std::string& path, Group group) {
        if (const auto old = groups_.find(path); old != groups_.end()) erase(old);
        if (group.empty()) return;
        if (groups_.size() >= 100000 || group.size() > 300000 - count_) throw Error("stash 合成索引超过路径或阶段限制");
        std::size_t cost = 0;
        for (const auto& entry : group) {
            if (entry.path.size() > max_file_size - cost || 41 > max_file_size - cost - entry.path.size())
                throw Error("stash 合成索引超过 64 MiB 限制");
            cost += entry.path.size() + 41;
        }
        if (cost > max_file_size - bytes_) throw Error("stash 合成索引超过 64 MiB 限制");
        groups_.emplace(path, Index(group.begin(), group.end())); count_ += group.size(); bytes_ += cost;
    }
    void erase_region(const std::string& root) {
        if (const auto exact = groups_.find(root); exact != groups_.end()) erase(exact);
        const auto prefix = root + "/";
        auto child = groups_.lower_bound(prefix);
        while (child != groups_.end() && child->first.starts_with(prefix)) child = erase(child);
    }
    Index finish() const {
        Index result; result.reserve(count_);
        for (const auto& [path, group] : groups_) { (void)path; result.insert(result.end(), group.begin(), group.end()); }
        checkout_support::validate_index_budget(result); (void)encode_index(result);
        return result;
    }
private:
    std::map<std::string, Index>::iterator erase(std::map<std::string, Index>::iterator entry) {
        count_ -= entry->second.size();
        for (const auto& stage : entry->second) bytes_ -= stage.path.size() + 41;
        return groups_.erase(entry);
    }
    std::map<std::string, Index> groups_;
    std::size_t count_ = 0, bytes_ = 44;
};
bool overlaps_region(const std::set<std::string>& regions, const std::string& path) {
    if (regions.contains(path)) return true;
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
        if (regions.contains(path.substr(0, slash))) return true;
    const auto prefix = path + "/";
    const auto child = regions.lower_bound(prefix);
    return child != regions.end() && child->starts_with(prefix);
}
struct Composed { SnapshotMergeResult merged; std::vector<StashConflict> report; };
Composed compose(const Index& current, SnapshotMergeResult rw, const std::optional<SnapshotMergeResult>& ri) {
    IndexGroups groups(ri ? ri->index : current);
    checkout_support::BoundedPaths index_regions, both;
    if (ri) {
        for (const auto& conflict : ri->conflicts)
            if (conflict.kind == MergeConflictKind::DirectoryFile) index_regions.add(conflict.path);
        for (const auto& conflict : rw.conflicts) {
            if (conflict.kind == MergeConflictKind::DirectoryFile) throw Error("stash --index 不支持 Work 层目录文件冲突");
            if (overlaps_region(index_regions.paths, conflict.path)) throw Error("stash 两层冲突与 Index 目录文件区域交叉");
        }
    } else {
        // Remove every DF region before adding stages, so temporary counts do
        // not reject a legal final Index whose independent regions shrink.
        for (const auto& conflict : rw.conflicts)
            if (conflict.kind == MergeConflictKind::DirectoryFile) groups.erase_region(conflict.path);
    }
    Composed result;
    checkout_support::BoundedPaths report_paths;
    for (const auto& conflict : rw.conflicts) {
        auto layer = StashConflictLayer::Work;
        if (conflict.kind == MergeConflictKind::DirectoryFile) {
            const auto exact = group_at(rw.index, conflict.path);
            if (!exact.empty()) groups.put(conflict.path, exact);
            const auto prefix = conflict.path + "/";
            auto child = std::lower_bound(rw.index.begin(), rw.index.end(), prefix,
                [](const auto& entry, const auto& key) { return entry.path < key; });
            while (child != rw.index.end() && child->path.starts_with(prefix)) {
                const auto group = group_at(rw.index, child->path); groups.put(child->path, group);
                child += static_cast<Index::difference_type>(group.size());
            }
        } else {
            const auto incoming = group_at(rw.index, conflict.path);
            if (ri) {
                const auto indexed = group_at(ri->index, conflict.path);
                if (!indexed.empty() && indexed.front().stage != 0) {
                    if (!same_group(incoming, indexed)) throw Error("stash 两层同路径冲突具有不同原始 stages");
                    layer = StashConflictLayer::Both; both.add(conflict.path);
                }
            }
            groups.put(conflict.path, incoming);
        }
        report_paths.add(conflict.path); result.report.push_back({conflict, layer});
    }
    if (ri) for (const auto& conflict : ri->conflicts) {
        if (both.paths.contains(conflict.path)) continue;
        report_paths.add(conflict.path); result.report.push_back({conflict, StashConflictLayer::Staging});
    }
    std::sort(result.report.begin(), result.report.end(), [](const auto& a, const auto& b) { return a.conflict.path < b.conflict.path; });
    result.merged.index = groups.finish(); result.merged.work = std::move(rw.work);
    result.merged.coarse = rw.coarse || (ri && ri->coarse);
    for (const auto& conflict : result.report) result.merged.conflicts.push_back(conflict.conflict);
    return result;
}
// Preserve the stack primitive's logical-noop contract while pinning both exact
// metadata images in the final business transaction. All failures retain plan.
void pin_stack(const Repository& repo, TransactionPlan& plan, const StashStackState& expected) {
    auto next = plan;
    plan_stash_stack_update(repo, next, expected, expected.entries);
    next.metadata.push_back({"stashstack", expected.raw_stack, expected.raw_stack});
    next.metadata.push_back({"refs/stash", expected.raw_ref, expected.raw_ref});
    validate_transaction_budget(next); plan = std::move(next);
}
void drop_selected(const Repository& repo, TransactionPlan& plan, const StashStackState& expected, std::size_t position) {
    auto desired = expected.entries;
    desired.erase(desired.begin() + static_cast<StashStack::difference_type>(position));
    plan_stash_stack_update(repo, plan, expected, desired);
}
StashStackState saved_stack(const Repository& repo, const OperationState& state) {
    const auto object = ObjectStore(repo.metadata() / "objects").read(state.stash->stack_blob);
    StashStackState expected{decode_stash_stack(object.payload), object.payload, state.stash->raw_ref};
    const auto actual = read_stash_stack(repo);
    if (actual.entries != expected.entries || actual.raw_stack != expected.raw_stack || actual.raw_ref != expected.raw_ref)
        throw Error("stash 操作期间保存栈或引用发生变化");
    return expected;
}
// A regular tracked ancestor may make a leaf logically absent only when RW
// explicitly removes/converts it. Symlinks and unsupported nodes never qualify.
NodeImage absent_untracked(const Repository& repo, const std::string& path, const Snapshot& ours,
                           const WorktreeTargets& targets) {
    std::filesystem::path current = repo.root(); std::string prefix;
    const std::filesystem::path relative(path);
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        prefix += (prefix.empty() ? "" : "/") + part->generic_string(); current /= *part;
        std::error_code ec; const auto status = std::filesystem::symlink_status(current, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) break;
        if (ec || std::filesystem::is_symlink(status)) throw Error("stash U 路径不安全: " + path);
        auto next = part; ++next;
        if (std::filesystem::is_regular_file(status) && next != relative.end()) {
            const auto target = targets.find(prefix);
            if (!ours.contains(prefix) || target == targets.end() || target->second.file)
                throw Error("stash U 被未授权文件祖先遮挡: " + path);
            break;
        }
        if (next == relative.end() || !std::filesystem::is_directory(status)) throw Error("stash U 路径已有节点或类型不支持: " + path);
    }
    const auto image = capture_work_node(repo, path);
    if (image.kind != NodeKind::Absent) throw Error("stash U 路径必须真正缺失: " + path);
    return image;
}
}
StashRestoreResult stash_restore(const Repository& repo, const StashRestoreOptions& options, const TransactionHook& hook) {
    RepoLock lock(repo.metadata()); require_no_transaction(repo); require_no_operation(repo);
    const auto stack = read_stash_stack(repo);
    if (options.position >= stack.entries.size()) throw Error("stash 位置不存在");
    const auto head = read_head(repo);
    if (!head.id) throw Error("stash restore 需要已有提交的 HEAD");
    const auto raw_head = operation_support::raw_metadata(repo, "HEAD");
    if (!raw_head) throw Error("stash HEAD 不存在");
    const auto raw_index = operation_support::raw_metadata(repo, "index");
    const auto index = operation_support::checked_index(repo, raw_index);
    const auto ours = read_snapshot(repo, read_commit(repo, *head.id).tree);
    if (operation_support::resolved_snapshot(index) != ours) throw Error("stash restore 需要 Index 与 HEAD 一致");
    auto observed = operation_support::clean_work(repo, ours);
    const auto saved = stash_support::load_saved_entry(repo, stack.entries[options.position]);
    auto rw = merge_snapshots(repo, saved.head, ours, saved.work);
    std::optional<SnapshotMergeResult> ri;
    if (options.restore_index) {
        for (const auto& conflict : rw.conflicts)
            if (conflict.kind == MergeConflictKind::DirectoryFile) throw Error("stash --index 不支持 Work 层目录文件冲突");
        ri = merge_snapshots(repo, saved.head, ours, saved.index);
    }
    auto composed = compose(index, std::move(rw), ri);
    composed.merged.work = stash_support::combined_work(composed.merged.work, saved.untracked);
    checkout_support::BoundedPaths owned;
    const auto targets = operation_support::changed_targets(ours, composed.merged.work, owned);
    for (const auto& [path, file] : saved.untracked) { (void)file; observed.emplace(path, absent_untracked(repo, path, ours, targets)); }
    TransactionPlan plan;
    if (composed.merged.index != index) plan_index_update(repo, plan, composed.merged.index);
    plan_worktree_update(repo, plan, targets, owned.paths);
    operation_support::check_work_before(plan, observed);
    for (const auto& [path, file] : saved.untracked) {
        (void)file;
        const auto change = std::lower_bound(plan.work.begin(), plan.work.end(), path, [](const auto& entry, const auto& key) { return entry.path < key; });
        if (change == plan.work.end() || change->path != path || change->before.kind != NodeKind::Absent)
            throw Error("stash U 规划期间出现已有节点: " + path);
    }
    const bool conflicted = !composed.report.empty();
    if (conflicted) {
        OperationState state;
        state.kind = OperationKind::StashApply; state.original_head = state.current_head = head;
        state.original_head_bytes = *raw_head; state.source_commit = saved.entry.work;
        state.message = options.pop ? "stash:pop" : "stash:apply";
        state.work = operation_support::conflict_work(repo, plan, composed.merged, observed);
        ObjectStore store(repo.metadata() / "objects");
        state.stash = StashOperation{options.pop, static_cast<std::uint32_t>(options.position), store.write(ObjectType::Blob, *stack.raw_stack), *stack.raw_ref};
        if (raw_index) state.original_index_blob = store.write(ObjectType::Blob, *raw_index);
        plan_operation_update(repo, plan, std::nullopt, state);
        pin_stack(repo, plan, stack);
    } else if (options.pop) drop_selected(repo, plan, stack, options.position);
    else pin_stack(repo, plan, stack);
    operation_support::check_original_metadata(repo, head, raw_head, raw_index);
    execute_transaction(repo, std::move(plan), hook);
    return {conflicted ? StashRestoreOutcome::Conflicted : StashRestoreOutcome::Applied, std::move(composed.report), options.pop && !conflicted};
}
StashRestoreResult stash_continue(const Repository& repo, const TransactionHook& hook) {
    RepoLock lock(repo.metadata()); require_no_transaction(repo);
    const auto state = operation_support::active_operation(repo, OperationKind::StashApply);
    const auto stack = saved_stack(repo, state);
    const auto raw_head = operation_support::raw_metadata(repo, "HEAD");
    const auto raw_index = operation_support::raw_metadata(repo, "index");
    (void)operation_support::resolved_snapshot(operation_support::checked_index(repo, raw_index));
    TransactionPlan plan;
    plan_operation_update(repo, plan, state, std::nullopt);
    if (state.stash->pop) drop_selected(repo, plan, stack, state.stash->position);
    else pin_stack(repo, plan, stack);
    operation_support::check_original_metadata(repo, state.current_head, raw_head, raw_index);
    execute_transaction(repo, std::move(plan), hook);
    return {StashRestoreOutcome::Applied, {}, state.stash->pop};
}
StashRestoreResult stash_abort(const Repository& repo, const TransactionHook& hook) {
    RepoLock lock(repo.metadata()); require_no_transaction(repo);
    const auto state = operation_support::active_operation(repo, OperationKind::StashApply);
    const auto stack = saved_stack(repo, state);
    auto plan = operation_support::plan_abort_operation(repo, state);
    pin_stack(repo, plan, stack);
    execute_transaction(repo, std::move(plan), hook);
    return {StashRestoreOutcome::Aborted, {}, false};
}
}
