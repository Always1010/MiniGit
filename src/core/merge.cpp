#include "minigit/merge.hpp"
#include "checkout_support.hpp"
#include "operation_support.hpp"
#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "minigit/worktree_update.hpp"
#include <utility>
namespace minigit {
namespace {
Index snapshot_index(const Snapshot& snapshot) {
    Index index;
    index.reserve(snapshot.size());
    for (const auto& [path, file] : snapshot) index.push_back({path, file.mode, file.id});
    checkout_support::validate_index_budget(index);
    (void)encode_index(index);
    return index;
}
void validate_options(const MergeOptions& options) {
    switch (options.action) {
    case MergeAction::Start:
        if (options.revision.empty() || options.revision.size() > 4096 || options.revision.find('\0') != std::string::npos)
            throw Error("合并来源必须非空且不超过 4096 字节");
        if (options.message) operation_support::validate_message(*options.message);
        break;
    case MergeAction::Continue:
    case MergeAction::Abort:
        if (!options.revision.empty() || options.message) throw Error("继续或中止合并不接受来源和消息");
        break;
    default: throw Error("非法合并动作");
    }
}
MergeResult start_merge(const Repository& repo, const MergeOptions& options, const TransactionHook& hook) {
    require_no_operation(repo);
    const auto head = read_head(repo);
    const auto raw_head = operation_support::raw_metadata(repo, "HEAD");
    if (!raw_head) throw Error("合并 HEAD 不存在");
    const auto raw_index = operation_support::raw_metadata(repo, "index");
    const auto index = operation_support::checked_index(repo, raw_index);
    const auto ours = head.id ? read_snapshot(repo, read_commit(repo, *head.id).tree) : Snapshot{};
    if (operation_support::resolved_snapshot(index) != ours) throw Error("合并需要索引与 HEAD 完全一致");
    const auto source = resolve_commit(repo, options.revision);
    const auto theirs = read_snapshot(repo, read_commit(repo, source).tree);
    std::optional<ObjectId> base;
    if (head.id) base = find_merge_base(repo, *head.id, source);
    else (void)topological_order(load_commit_graph(repo, {source}));
    if (head.id && !base) throw Error("合并来源没有共同祖先");
    const auto observed = operation_support::clean_work(repo, ours);
    if (head.id && base == source) return {MergeOutcome::UpToDate, head, {}};
    const auto message = options.message ? *options.message : "Merge " + options.revision;
    operation_support::validate_message(message);
    const bool fast_forward = !head.id || base == head.id;
    SnapshotMergeResult merged;
    if (fast_forward) { merged.index = snapshot_index(theirs); merged.work = theirs; }
    else merged = merge_snapshots(repo, read_snapshot(repo, read_commit(repo, *base).tree), ours, theirs);
    checkout_support::validate_index_budget(merged.index);
    (void)encode_index(merged.index);
    checkout_support::BoundedPaths owned;
    const auto targets = operation_support::changed_targets(ours, merged.work, owned);
    TransactionPlan plan;
    if (merged.index != index) plan_index_update(repo, plan, merged.index);
    plan_worktree_update(repo, plan, targets, owned.paths);
    operation_support::check_work_before(plan, observed);
    Head desired = head;
    MergeOutcome outcome;
    if (!merged.conflicts.empty()) {
        OperationState state;
        state.original_head = state.current_head = head;
        state.original_head_bytes = *raw_head;
        state.source_commit = source;
        state.message = message;
        state.work = operation_support::conflict_work(repo, plan, merged, observed);
        (void)encode_operation(state);
        if (raw_index) state.original_index_blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, *raw_index);
        plan_operation_update(repo, plan, std::nullopt, state);
        outcome = MergeOutcome::Conflicted;
    } else {
        desired.id = fast_forward ? source : store_commit_snapshot(repo, merged.work, {*head.id, source}, message);
        plan_move_head(repo, plan, head, desired, fast_forward ? "merge:fast-forward" : "merge");
        outcome = fast_forward ? MergeOutcome::FastForward : MergeOutcome::Merged;
    }
    operation_support::check_original_metadata(repo, head, raw_head, raw_index);
    execute_transaction(repo, std::move(plan), hook);
    return {outcome, desired, std::move(merged.conflicts)};
}
MergeResult continue_merge(const Repository& repo, const TransactionHook& hook) {
    const auto state = operation_support::active_operation(repo, OperationKind::Merge);
    const auto raw_index = operation_support::raw_metadata(repo, "index");
    const auto index = operation_support::checked_index(repo, raw_index);
    const auto snapshot = operation_support::resolved_snapshot(index);
    const auto commit = store_commit_snapshot(repo, snapshot, {*state.original_head.id, state.source_commit}, state.message);
    const Head desired{state.current_head.symbolic, commit};
    TransactionPlan plan;
    plan_move_head(repo, plan, state.current_head, desired, "merge:continue");
    plan_operation_update(repo, plan, state, std::nullopt);
    if (operation_support::raw_metadata(repo, "index") != raw_index) throw Error("继续合并时索引发生变化");
    execute_transaction(repo, std::move(plan), hook);
    return {MergeOutcome::Merged, desired, {}};
}
MergeResult abort_merge(const Repository& repo, const TransactionHook& hook) {
    const auto state = operation_support::active_operation(repo, OperationKind::Merge);
    auto plan = operation_support::plan_abort_operation(repo, state);
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
