#include "minigit/stash.hpp"
#include "minigit/stash_stack.hpp"
#include "operation_support.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/inspection.hpp"
#include "minigit/object_store.hpp"
#include "minigit/worktree.hpp"
#include <map>
#include <utility>
namespace minigit {
namespace {
std::optional<FileState> state_at(const Snapshot& snapshot, const std::string& path) {
    const auto found = snapshot.find(path);
    return found == snapshot.end() ? std::nullopt : std::optional(found->second);
}
// Match write_snapshot's complete file/directory entry and path-data bounds
// without writing Trees. This also validates combined tracked/untracked layout.
void validate_saved_tree(const Snapshot& snapshot) {
    checkout_support::BoundedPaths nodes;
    std::size_t bytes = 0;
    const auto add = [&](const std::string& path, std::size_t name_size, std::size_t overhead) {
        if (nodes.paths.contains(path)) return;
        nodes.add(path);
        if (nodes.paths.size() > 100000) throw Error("stash 保存树超过 100000 条目限制");
        const auto size = path.size() + name_size + overhead;
        if (size > max_file_size - bytes) throw Error("stash 保存树超过 64 MiB 限制");
        bytes += size;
    };
    for (const auto& [path, file] : snapshot) {
        checkout_support::validate_path(path);
        if (file.mode != "100644" && file.mode != "100755") throw Error("stash 文件模式非法");
        const auto child = snapshot.lower_bound(path + "/");
        if (child != snapshot.end() && child->first.starts_with(path + "/")) throw Error("stash 保存树存在文件目录冲突");
        const auto last = path.rfind('/');
        add(path, last == std::string::npos ? path.size() : path.size() - last - 1, 40);
        std::size_t begin = 0;
        for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
            add(path.substr(0, slash), slash - begin, 39);
            begin = slash + 1;
        }
    }
}
Snapshot combined_work(const Snapshot& work, const Snapshot& untracked) {
    Snapshot combined;
    checkout_support::BoundedPaths paths;
    for (const auto* snapshot : {&work, &untracked})
        for (const auto& [path, file] : *snapshot) {
            paths.add(path);
            if (paths.paths.size() > 100000) throw Error("stash Work 与 U 超过 100000 文件限制");
            if (!combined.emplace(path, file).second) throw Error("stash Work 与 U 路径重复");
        }
    validate_saved_tree(combined);
    return combined;
}
NodeImage capture_saved_file(const Repository& repo, const std::string& path, Snapshot& snapshot,
                            const std::optional<FileState>& expected = std::nullopt) {
    if (const auto file = read_worktree_file(repo, path)) {
        if (expected && file->state != *expected) throw Error("扫描 stash 后工作区文件发生变化: " + path);
        if (snapshot.size() >= 100000) throw Error("stash 文件数超过 100000 限制");
        const auto id = ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, file->data.bytes);
        snapshot.emplace_hint(snapshot.end(), path, file->state);
        return {NodeKind::File, id, file->data.permissions};
    }
    if (expected) throw Error("扫描 stash 后工作区文件消失: " + path);
    const auto target = repo.root() / path;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec || !std::filesystem::is_directory(status)) throw Error("检查 stash tracked 节点失败: " + path);
    return {NodeKind::Directory, std::nullopt, static_cast<std::uint32_t>(status.permissions()) & 07777};
}
Index snapshot_index(const Snapshot& snapshot) {
    Index index;
    index.reserve(snapshot.size());
    for (const auto& [path, file] : snapshot) index.push_back({path, file.mode, file.id});
    checkout_support::validate_index_budget(index);
    (void)encode_index(index);
    return index;
}
ObjectId select_entry(const StashStackState& stack, std::size_t position) {
    if (position >= stack.entries.size()) throw Error("stash 位置不存在");
    return stack.entries[position];
}
}
std::optional<ObjectId> stash_push(const Repository& repo, const StashPushOptions& options, const TransactionHook& hook) {
    if (options.message) operation_support::validate_message(*options.message);
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    require_no_operation(repo);
    const auto stack = read_stash_stack(repo);
    const auto head = read_head(repo);
    if (!head.id) throw Error("stash push 需要已经有提交的 HEAD");
    const auto raw_head = operation_support::raw_metadata(repo, "HEAD");
    if (!raw_head) throw Error("stash HEAD 不存在");
    const auto raw_index = operation_support::raw_metadata(repo, "index");
    const auto index = operation_support::checked_index(repo, raw_index);
    const auto indexed = operation_support::resolved_snapshot(index);
    const auto h = read_snapshot(repo, read_commit(repo, *head.id).tree);
    const auto after_index = snapshot_index(h);
    validate_saved_tree(indexed);
    checkout_support::BoundedPaths tracked, owned;
    for (const auto* snapshot : {&h, &indexed})
        for (const auto& [path, file] : *snapshot) { (void)file; tracked.add(path); owned.add(path); }
    Snapshot work, untracked;
    operation_support::WorkObservations observed;
    for (const auto& path : tracked.paths)
        observed.emplace_hint(observed.end(), path, capture_saved_file(repo, path, work));
    if (options.include_untracked) {
        const auto scanned = scan_worktree(repo);
        for (const auto& [path, file] : scanned) {
            if (tracked.paths.contains(path)) continue;
            if (work.size() + untracked.size() >= 100000) throw Error("stash Work 与 U 超过 100000 文件限制");
            owned.add(path);
            observed.emplace(path, capture_saved_file(repo, path, untracked, file));
        }
    }
    const auto all_work = combined_work(work, untracked);
    if (indexed == h && work == h && untracked.empty()) {
        operation_support::check_original_metadata(repo, head, raw_head, raw_index);
        // Validate stack CAS even when a no-change push publishes nothing.
        TransactionPlan noop;
        plan_stash_stack_update(repo, noop, stack, stack.entries);
        return std::nullopt;
    }
    if (stack.entries.size() >= 100000) throw Error("stash 栈超过 100000 条目限制");
    const auto message = options.message ? *options.message : "WIP " + head.id->hex();
    operation_support::validate_message(message);
    WorktreeTargets targets;
    checkout_support::BoundedPaths cleanup;
    for (const auto& path : tracked.paths) cleanup.add(path);
    for (const auto& [path, file] : untracked) { (void)file; cleanup.add(path); }
    for (const auto& path : cleanup.paths)
        if (state_at(all_work, path) != state_at(h, path))
            targets.emplace_hint(targets.end(), path, WorktreeTarget{state_at(h, path), std::nullopt});
    TransactionPlan plan;
    if (after_index != index) plan_index_update(repo, plan, after_index);
    plan_worktree_update(repo, plan, targets, owned.paths);
    operation_support::check_work_before(plan, observed);
    const auto index_commit = store_commit_snapshot(repo, indexed, {*head.id}, message);
    std::vector<ObjectId> parents{*head.id, index_commit};
    if (!untracked.empty()) parents.push_back(store_commit_snapshot(repo, untracked, {}, message));
    const auto work_commit = store_commit_snapshot(repo, work, parents, message);
    auto after_stack = stack.entries;
    after_stack.insert(after_stack.begin(), work_commit);
    plan_stash_stack_update(repo, plan, stack, after_stack);
    operation_support::check_original_metadata(repo, head, raw_head, raw_index);
    execute_transaction(repo, std::move(plan), hook);
    return work_commit;
}
std::vector<StashListEntry> stash_list(const Repository& repo) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    const auto stack = read_stash_stack(repo);
    std::map<ObjectId, std::string> messages;
    std::vector<StashListEntry> result;
    result.reserve(stack.entries.size());
    std::size_t bytes = 0;
    for (const auto& work : stack.entries) {
        auto found = messages.find(work);
        if (found == messages.end()) found = messages.emplace(work, read_commit(repo, work).message).first;
        if (found->second.size() > max_file_size - bytes || 80 > max_file_size - bytes - found->second.size())
            throw Error("stash list 输出超过 64 MiB 限制");
        bytes += found->second.size() + 80;
        result.push_back({result.size(), work, found->second});
    }
    return result;
}
std::string stash_show(const Repository& repo, std::size_t position, std::size_t context) {
    if (context > 100000) throw Error("stash show 上下文超过限制");
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    const auto stack = read_stash_stack(repo);
    const auto entry = read_stash_entry(repo, select_entry(stack, position));
    const auto head = read_snapshot(repo, read_commit(repo, entry.head).tree);
    (void)read_snapshot(repo, read_commit(repo, entry.index).tree);
    const auto work = read_snapshot(repo, read_commit(repo, entry.work).tree);
    const auto untracked = entry.untracked ? read_snapshot(repo, read_commit(repo, *entry.untracked).tree) : Snapshot{};
    return diff_snapshots(repo, head, combined_work(work, untracked), context);
}
void stash_drop(const Repository& repo, std::size_t position, const TransactionHook& hook) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    require_no_operation(repo);
    const auto stack = read_stash_stack(repo);
    (void)select_entry(stack, position);
    auto desired = stack.entries;
    desired.erase(desired.begin() + static_cast<StashStack::difference_type>(position));
    TransactionPlan plan;
    plan_stash_stack_update(repo, plan, stack, desired);
    execute_transaction(repo, std::move(plan), hook);
}
void stash_clear(const Repository& repo, const TransactionHook& hook) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    require_no_operation(repo);
    const auto stack = read_stash_stack(repo);
    TransactionPlan plan;
    plan_stash_stack_update(repo, plan, stack, {});
    if (!plan.metadata.empty()) execute_transaction(repo, std::move(plan), hook);
}
}
