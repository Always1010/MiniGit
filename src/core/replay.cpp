#include "minigit/replay.hpp"
#include "operation_support.hpp"
#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/reflog.hpp"
#include <utility>
namespace minigit {
namespace {
void validate_options(const ReplayOptions& options) {
    switch (options.action) {
    case ReplayAction::Start:
        if (options.revision.empty() || options.revision.size() > 4096 || options.revision.find('\0') != std::string::npos)
            throw Error("重放来源必须非空且不超过 4096 字节");
        if (options.message) operation_support::validate_message(*options.message);
        break;
    case ReplayAction::Continue:
    case ReplayAction::Abort:
        if (!options.revision.empty() || options.message) throw Error("继续或中止重放不接受来源和消息");
        break;
    default: throw Error("非法重放动作");
    }
}
void require_single_source(const Commit& source) {
    if (source.parents.size() > 1) throw Error("重放初版不支持多父 source 提交");
}
std::string_view reason(OperationKind kind, bool continuing = false) {
    if (kind == OperationKind::CherryPick) return continuing ? "cherry-pick:continue" : "cherry-pick";
    return continuing ? "revert:continue" : "revert";
}
std::optional<std::string_view> original_author(OperationKind kind, const Commit& source) {
    return kind == OperationKind::CherryPick ? std::optional<std::string_view>(source.author) : std::nullopt;
}
ReplayResult start_replay(const Repository& repo, OperationKind kind, const ReplayOptions& options,
                          const TransactionHook& hook) {
    require_no_operation(repo);
    const auto head = read_head(repo);
    if (!head.id) throw Error("重放需要已经有提交的 HEAD");
    const auto raw_head = operation_support::raw_metadata(repo, "HEAD");
    if (!raw_head) throw Error("重放 HEAD 不存在");
    const auto raw_index = operation_support::raw_metadata(repo, "index");
    const auto index = operation_support::checked_index(repo, raw_index);
    const auto ours = read_snapshot(repo, read_commit(repo, *head.id).tree);
    if (operation_support::resolved_snapshot(index) != ours) throw Error("重放需要索引与 HEAD 完全一致");
    const auto source = resolve_commit(repo, options.revision);
    require_single_source(read_commit(repo, source));
    // This is an integrity check, not a merge-base or ancestry relationship test.
    const auto graph = load_commit_graph(repo, {*head.id, source});
    (void)topological_order(graph);
    const auto& source_commit = graph.at(source);
    const auto source_snapshot = read_snapshot(repo, source_commit.tree);
    const auto parent_snapshot = source_commit.parents.empty() ? Snapshot{} :
        read_snapshot(repo, graph.at(source_commit.parents.front()).tree);
    const auto message = options.message ? *options.message :
        kind == OperationKind::CherryPick ? source_commit.message : "Revert " + source.hex();
    operation_support::validate_message(message);
    const auto observed = operation_support::clean_work(repo, ours);
    auto merged = kind == OperationKind::CherryPick ? merge_snapshots(repo, parent_snapshot, ours, source_snapshot) :
        merge_snapshots(repo, source_snapshot, ours, parent_snapshot);
    checkout_support::validate_index_budget(merged.index);
    (void)encode_index(merged.index);
    checkout_support::BoundedPaths owned;
    const auto targets = operation_support::changed_targets(ours, merged.work, owned);
    TransactionPlan plan;
    if (merged.index != index) plan_index_update(repo, plan, merged.index);
    plan_worktree_update(repo, plan, targets, owned.paths);
    operation_support::check_work_before(plan, observed);
    Head desired = head;
    auto outcome = ReplayOutcome::Applied;
    if (!merged.conflicts.empty()) {
        OperationState state;
        state.kind = kind;
        state.original_head = state.current_head = head;
        state.original_head_bytes = *raw_head;
        state.source_commit = source;
        state.message = message;
        state.work = operation_support::conflict_work(repo, plan, merged, observed);
        (void)encode_operation(state);
        if (raw_index) state.original_index_blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, *raw_index);
        plan_operation_update(repo, plan, std::nullopt, state);
        outcome = ReplayOutcome::Conflicted;
    } else {
        // No empty-change shortcut: every clean replay creates a single-parent commit.
        desired.id = store_commit_snapshot(repo, merged.work, {*head.id}, message, original_author(kind, source_commit));
        plan_move_head(repo, plan, head, desired, reason(kind));
    }
    operation_support::check_original_metadata(repo, head, raw_head, raw_index);
    execute_transaction(repo, std::move(plan), hook);
    return {outcome, desired, std::move(merged.conflicts)};
}
ReplayResult continue_replay(const Repository& repo, OperationKind kind, const TransactionHook& hook) {
    const auto state = operation_support::active_operation(repo, kind);
    const auto raw_index = operation_support::raw_metadata(repo, "index");
    const auto index = operation_support::checked_index(repo, raw_index);
    const auto snapshot = operation_support::resolved_snapshot(index);
    const auto source = read_commit(repo, state.source_commit);
    require_single_source(source);
    const auto commit = store_commit_snapshot(repo, snapshot, {*state.original_head.id}, state.message, original_author(kind, source));
    const Head desired{state.current_head.symbolic, commit};
    TransactionPlan plan;
    plan_move_head(repo, plan, state.current_head, desired, reason(kind, true));
    plan_operation_update(repo, plan, state, std::nullopt);
    if (operation_support::raw_metadata(repo, "index") != raw_index) throw Error("继续重放时索引发生变化");
    execute_transaction(repo, std::move(plan), hook);
    return {ReplayOutcome::Applied, desired, {}};
}
ReplayResult replay_repository(const Repository& repo, OperationKind kind, const ReplayOptions& options,
                              const TransactionHook& hook) {
    validate_options(options);
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    switch (options.action) {
    case ReplayAction::Start: return start_replay(repo, kind, options, hook);
    case ReplayAction::Continue: return continue_replay(repo, kind, hook);
    case ReplayAction::Abort: {
        const auto state = operation_support::active_operation(repo, kind);
        auto plan = operation_support::plan_abort_operation(repo, state);
        execute_transaction(repo, std::move(plan), hook);
        return {ReplayOutcome::Aborted, state.original_head, {}};
    }
    default: throw Error("非法重放动作");
    }
}
}
ReplayResult cherry_pick_repository(const Repository& repo, const ReplayOptions& options, const TransactionHook& hook) {
    return replay_repository(repo, OperationKind::CherryPick, options, hook);
}
ReplayResult revert_repository(const Repository& repo, const ReplayOptions& options, const TransactionHook& hook) {
    return replay_repository(repo, OperationKind::Revert, options, hook);
}
}
