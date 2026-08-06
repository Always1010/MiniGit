#include "minigit/rebase.hpp"
#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "operation_support.hpp"
#include <algorithm>
#include <map>
#include <utility>
namespace minigit {
namespace {
void validate_options(const RebaseOptions& options) {
    switch (options.action) {
    case RebaseAction::Start:
        if (options.revision.empty() || options.revision.size() > 4096 || options.revision.find('\0') != std::string::npos)
            throw Error("rebase 来源必须非空且不超过 4096 字节");
        break;
    case RebaseAction::Continue:
    case RebaseAction::Abort:
        if (!options.revision.empty()) throw Error("rebase 继续或中止不接受来源");
        break;
    default: throw Error("非法 rebase 动作");
    }
}
Snapshot own_snapshot(const Repository& repo, const ObjectId& id) { return read_snapshot(repo, read_commit(repo, id).tree); }
void check_branch(const Repository& repo, const OperationState& state) {
    if (state.original_head.symbolic && operation_support::raw_metadata(repo, *state.original_head.symbolic) != state.rebase->original_branch_ref)
        throw Error("rebase 原始分支精确引用已变化");
}
void pin_branch(const Repository& repo, TransactionPlan& plan, const OperationState& state) {
    check_branch(repo, state);
    if (!state.original_head.symbolic) return;
    if (std::any_of(plan.metadata.begin(), plan.metadata.end(), [&](const auto& change) { return change.path == *state.original_head.symbolic; }))
        throw Error("rebase 原始分支已经被其他计划修改");
    auto next = plan;
    next.metadata.push_back({*state.original_head.symbolic, state.rebase->original_branch_ref, state.rebase->original_branch_ref});
    validate_transaction_budget(next); plan = std::move(next);
}
// State's before is the first affected image, not this step's current baseline.
// A saved original File/Absent ancestor proves that a newly affected descendant
// was originally absent even when earlier replay/resolution has created it.
std::vector<WorkChange> accumulate_work(const Repository& repo, const std::vector<WorkChange>& prior,
                                      const std::vector<WorkChange>& fresh) {
    std::map<std::string, WorkChange> changes;
    checkout_support::BoundedPaths keys;
    const auto add = [&](const std::string& path) {
        keys.add(path);
        if (keys.paths.size() > 100000) throw Error("rebase 累计受影响路径超过 100000 条目限制");
    };
    for (const auto& change : prior) { add(change.path); changes.emplace(change.path, change); }
    for (const auto& change : fresh) {
        add(change.path);
        if (const auto old = changes.find(change.path); old != changes.end()) { old->second.after = change.after; continue; }
        auto first = change.before;
        for (auto slash = change.path.find('/'); slash != std::string::npos; slash = change.path.find('/', slash + 1)) {
            const auto ancestor = changes.find(change.path.substr(0, slash));
            if (ancestor != changes.end() && ancestor->second.before.kind != NodeKind::Directory) { first = {}; break; }
        }
        changes.emplace(change.path, WorkChange{change.path, first, change.after});
    }
    // A user may have resolved a saved File into a Directory. Publishing a
    // child must refresh that existing ancestor's last-after image, while its
    // original before remains fixed. Do not expand the saved restoration scope.
    std::set<std::string> refreshed;
    for (const auto& change : fresh) {
        if (change.after.kind == NodeKind::Absent) continue;
        for (auto slash = change.path.find('/'); slash != std::string::npos; slash = change.path.find('/', slash + 1)) {
            const auto path = change.path.substr(0, slash); const auto ancestor = changes.find(path);
            if (ancestor == changes.end() || !refreshed.insert(path).second) continue;
            const auto planned = std::lower_bound(fresh.begin(), fresh.end(), path,
                [](const auto& entry, const auto& key) { return entry.path < key; });
            if (planned != fresh.end() && planned->path == path) {
                if (planned->after.kind != NodeKind::Directory) throw Error("rebase 本步 Work 拓扑不一致");
            } else {
                const auto image = capture_work_node(repo, path);
                if (image.kind != NodeKind::Directory) throw Error("rebase 累计 Work 的实际父目录已变化");
                ancestor->second.after = image;
            }
        }
    }
    for (auto& [path, change] : changes)
        for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
            const auto ancestor = changes.find(path.substr(0, slash));
            if (ancestor != changes.end() && ancestor->second.after.kind != NodeKind::Directory) { change.after = {}; break; }
        }
    std::vector<WorkChange> result; result.reserve(changes.size());
    for (auto& [path, change] : changes) { (void)path; result.push_back(std::move(change)); }
    return result;
}
void plan_checkout(const Repository& repo, TransactionPlan& plan, const Snapshot& ours, const SnapshotMergeResult& merged,
                   const Index& index, const operation_support::WorkObservations& observed) {
    checkout_support::validate_index_budget(merged.index); (void)encode_index(merged.index);
    checkout_support::BoundedPaths owned;
    const auto targets = operation_support::changed_targets(ours, merged.work, owned);
    if (merged.index != index) plan_index_update(repo, plan, merged.index);
    plan_worktree_update(repo, plan, targets, owned.paths);
    operation_support::check_work_before(plan, observed);
}
void finish(const Repository& repo, TransactionPlan& plan, const OperationState& state, const ObjectId& commit) {
    check_branch(repo, state);
    const Head desired{state.original_head.symbolic, commit};
    if (desired.symbolic) {
        const auto bytes = commit == *state.original_head.id ? *state.rebase->original_branch_ref : to_bytes(commit.hex() + "\n");
        plan.metadata.push_back({*desired.symbolic, state.rebase->original_branch_ref, bytes});
        if (commit != *state.original_head.id)
            plan_append_reflog(repo, plan, *desired.symbolic, {*state.original_head.id, commit, "rebase:finish"});
    }
    plan_move_head(repo, plan, state.current_head, desired, "rebase:finish");
    plan_operation_update(repo, plan, state, std::nullopt);
}
RebaseResult run_steps(const Repository& repo, OperationState state, const TransactionHook& hook) {
    while (true) {
        check_branch(repo, state);
        if (read_head(repo) != state.current_head) throw Error("rebase 当前 HEAD 已变化");
        const auto raw_head = operation_support::raw_metadata(repo, "HEAD");
        const auto raw_index = operation_support::raw_metadata(repo, "index");
        const auto index = operation_support::checked_index(repo, raw_index);
        const auto snapshot = operation_support::resolved_snapshot(index);
        const auto source = read_commit(repo, state.source_commit);
        if (source.parents.size() > 1) throw Error("rebase 初版不支持多父 source 提交");
        operation_support::validate_message(source.message);
        TransactionPlan plan;
        Snapshot result;
        auto next = state;
        if (state.phase == OperationPhase::Ready) {
            const auto ours = own_snapshot(repo, *state.current_head.id);
            if (snapshot != ours) throw Error("rebase Ready 需要 Index 与当前 HEAD 一致");
            const auto observed = operation_support::clean_work(repo, ours);
            const auto base = source.parents.empty() ? Snapshot{} : own_snapshot(repo, source.parents.front());
            auto merged = merge_snapshots(repo, base, ours, read_snapshot(repo, source.tree));
            plan_checkout(repo, plan, ours, merged, index, observed);
            if (!merged.conflicts.empty()) {
                next.phase = OperationPhase::Conflicted;
                next.work = accumulate_work(repo, state.work, operation_support::conflict_work(repo, plan, merged, observed));
                plan_operation_update(repo, plan, state, next); pin_branch(repo, plan, state);
                operation_support::check_original_metadata(repo, state.current_head, raw_head, raw_index);
                execute_transaction(repo, std::move(plan), hook);
                return {RebaseOutcome::Conflicted, state.current_head, std::move(merged.conflicts)};
            }
            next.work = accumulate_work(repo, state.work, plan.work);
            result = std::move(merged.work);
        } else result = snapshot; // Conflicted resolution must never read Work.
        const auto commit = store_commit_snapshot(repo, result, {*state.current_head.id}, source.message, source.author);
        const bool last = state.rebase->cursor + 1 == state.rebase->queue.size();
        if (last) finish(repo, plan, state, commit);
        else {
            next.phase = OperationPhase::Ready; next.current_head = {std::nullopt, commit};
            ++next.rebase->cursor; next.source_commit = next.rebase->queue[next.rebase->cursor];
            plan_move_head(repo, plan, state.current_head, next.current_head, "rebase:step");
            plan_operation_update(repo, plan, state, next); pin_branch(repo, plan, state);
        }
        operation_support::check_original_metadata(repo, state.current_head, raw_head, raw_index);
        execute_transaction(repo, std::move(plan), hook);
        if (last) return {RebaseOutcome::Applied, {state.original_head.symbolic, commit}, {}};
        state = std::move(next);
    }
}
RebaseResult start(const Repository& repo, const RebaseOptions& options, const TransactionHook& hook) {
    require_no_operation(repo);
    const auto head = read_head(repo);
    if (!head.id) throw Error("rebase 需要已经有提交的 HEAD");
    const auto raw_head = operation_support::raw_metadata(repo, "HEAD");
    if (!raw_head) throw Error("rebase HEAD 不存在");
    const auto raw_index = operation_support::raw_metadata(repo, "index");
    const auto index = operation_support::checked_index(repo, raw_index);
    const auto ours = own_snapshot(repo, *head.id);
    if (operation_support::resolved_snapshot(index) != ours) throw Error("rebase 需要 Index 与 HEAD 一致");
    const auto onto = resolve_commit(repo, options.revision);
    const auto base = find_merge_base(repo, *head.id, onto);
    if (!base) throw Error("rebase 两历史没有共同祖先");
    const auto theirs = own_snapshot(repo, onto);
    const auto observed = operation_support::clean_work(repo, ours);
    if (*base == onto) return {RebaseOutcome::UpToDate, head, {}};
    if (*base == *head.id) {
        SnapshotMergeResult merged;
        merged.work = theirs;
        for (const auto& [path, file] : theirs) merged.index.push_back({path, file.mode, file.id});
        TransactionPlan plan; plan_checkout(repo, plan, ours, merged, index, observed);
        const Head desired{head.symbolic, onto}; plan_move_head(repo, plan, head, desired, "rebase:fast-forward");
        operation_support::check_original_metadata(repo, head, raw_head, raw_index);
        execute_transaction(repo, std::move(plan), hook); return {RebaseOutcome::Applied, desired, {}};
    }
    std::vector<ObjectId> queue;
    for (auto source = *head.id; source != *base;) {
        if (queue.size() >= 100000) throw Error("rebase 队列超过 100000 来源限制");
        const auto commit = read_commit(repo, source);
        if (commit.parents.size() != 1) throw Error("rebase 来源必须沿单父 first-parent 链到达共同祖先");
        operation_support::validate_message(commit.message);
        (void)read_snapshot(repo, commit.tree);
        queue.push_back(source); source = commit.parents.front();
    }
    (void)own_snapshot(repo, *base); std::reverse(queue.begin(), queue.end());
    OperationState state;
    state.kind = OperationKind::Rebase; state.phase = OperationPhase::Ready;
    state.original_head = head; state.current_head = {std::nullopt, onto};
    state.original_head_bytes = *raw_head; state.source_commit = queue.front(); state.message = "rebase";
    std::optional<Bytes> raw_branch;
    if (head.symbolic) {
        raw_branch = operation_support::raw_metadata(repo, *head.symbolic);
        if (!raw_branch || raw_branch->size() != 65 || raw_branch->back() != '\n' ||
            ObjectId::from_hex(std::string_view(reinterpret_cast<const char*>(raw_branch->data()), 64)) != *head.id)
            throw Error("rebase 原始分支引用不一致");
    }
    state.rebase = RebaseOperation{onto, std::move(queue), 0, std::move(raw_branch)};
    SnapshotMergeResult checkout; checkout.work = theirs;
    for (const auto& [path, file] : theirs) checkout.index.push_back({path, file.mode, file.id});
    TransactionPlan plan; plan_checkout(repo, plan, ours, checkout, index, observed);
    state.work = accumulate_work(repo, {}, plan.work);
    (void)encode_operation(state);
    if (raw_index) state.original_index_blob = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, *raw_index);
    plan_move_head(repo, plan, head, state.current_head, "rebase:start");
    plan_operation_update(repo, plan, std::nullopt, state); pin_branch(repo, plan, state);
    operation_support::check_original_metadata(repo, head, raw_head, raw_index);
    execute_transaction(repo, std::move(plan), hook);
    return run_steps(repo, std::move(state), hook);
}
}
RebaseResult rebase_repository(const Repository& repo, const RebaseOptions& options, const TransactionHook& hook) {
    validate_options(options); RepoLock lock(repo.metadata()); require_no_transaction(repo);
    switch (options.action) {
    case RebaseAction::Start: return start(repo, options, hook);
    case RebaseAction::Continue: return run_steps(repo, operation_support::active_operation(repo, OperationKind::Rebase), hook);
    case RebaseAction::Abort: {
        const auto state = operation_support::active_operation(repo, OperationKind::Rebase); check_branch(repo, state);
        auto plan = operation_support::plan_abort_operation(repo, state); pin_branch(repo, plan, state);
        execute_transaction(repo, std::move(plan), hook); return {RebaseOutcome::Aborted, state.original_head, {}};
    }
    default: throw Error("非法 rebase 动作");
    }
}
}
