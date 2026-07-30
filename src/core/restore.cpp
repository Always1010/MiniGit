#include "minigit/restore.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/worktree_update.hpp"
#include <algorithm>
#include <set>
#include <utility>
namespace minigit {
namespace {
constexpr std::size_t max_paths = 200000;
void validate_path(std::string_view path) {
    validate_repo_path(path);
    if (path.size() > 4096 || std::count(path.begin(), path.end(), '/') + 1 > 128)
        throw Error("恢复路径超过 4096 字节或 128 分量限制");
}
struct BoundedPaths {
    std::set<std::string> paths;
    std::size_t bytes = 0;
    void add(const std::string& path) {
        if (paths.contains(path)) return;
        validate_path(path);
        if (paths.size() >= max_paths || path.size() > max_file_size - bytes)
            throw Error("恢复路径数量或总字节超过限制");
        paths.insert(path);
        bytes += path.size();
    }
};
std::vector<std::string> normalize_selectors(const Repository& repo,
                                           const std::vector<std::filesystem::path>& paths) {
    if (paths.empty() || paths.size() > max_paths) throw Error("恢复路径参数为空或数量超过限制");
    std::size_t bytes = 0;
    std::vector<std::string> selectors;
    selectors.reserve(paths.size());
    const auto cwd = std::filesystem::current_path();
    for (const auto& path : paths) {
        const auto raw = path.generic_string();
        if (raw.empty() || raw.size() > 8192 || raw.size() > max_file_size - bytes ||
            raw.find('\0') != std::string::npos)
            throw Error("恢复路径参数为空或超过字节限制");
        bytes += raw.size();
        const auto absolute = (path.is_absolute() ? path : cwd / path).lexically_normal();
        const auto relative = absolute.lexically_relative(repo.root());
        if (relative.empty()) throw Error("恢复路径不在仓库内");
        auto selector = relative.generic_string();
        // A trailing slash is a directory selector, not an empty path component.
        while (selector.size() > 1 && selector.back() == '/') selector.pop_back();
        if (selector == ".") selector.clear();
        else validate_path(selector);
        selectors.push_back(std::move(selector));
    }
    return selectors;
}
void validate_index_budget(const Index& index) {
    if (index.size() > 300000) throw Error("恢复索引阶段条目超过 300000 限制");
    std::set<std::string> paths;
    std::size_t bytes = 44;
    for (const auto& entry : index) {
        validate_path(entry.path);
        if (paths.insert(entry.path).second && paths.size() > 100000)
            throw Error("恢复索引不同路径超过 100000 限制");
        if (entry.path.size() > max_file_size - bytes || 41 > max_file_size - bytes - entry.path.size())
            throw Error("恢复索引超过 64 MiB 限制");
        bytes += entry.path.size() + 41;
    }
}
struct CheckedIndex {
    Snapshot source;
    BoundedPaths paths;
    std::set<std::string> conflicts;
};
CheckedIndex check_index(const Repository& repo, const Index& index) {
    validate_index_budget(index);
    const ObjectStore store(repo.metadata() / "objects");
    std::set<ObjectId> checked;
    CheckedIndex result;
    for (const auto& entry : index) {
        if (checked.insert(entry.id).second && store.read(entry.id).type != ObjectType::Blob)
            throw Error("恢复索引对象不是 blob");
        result.paths.add(entry.path);
        if (entry.stage == 0) result.source.emplace_hint(result.source.end(), entry.path, FileState{entry.mode, entry.id});
        else result.conflicts.insert(entry.path);
    }
    return result;
}
bool matches(const std::set<std::string>& candidates, const std::string& selector) {
    if (selector.empty()) return !candidates.empty();
    if (candidates.contains(selector)) return true;
    const auto prefix = selector + "/";
    const auto child = candidates.lower_bound(prefix);
    return child != candidates.end() && child->starts_with(prefix);
}
std::set<std::string> select_paths(const Snapshot& source, const BoundedPaths& index_paths,
                                 const std::vector<std::string>& selectors) {
    auto candidates = index_paths;
    for (const auto& [path, state] : source) { (void)state; candidates.add(path); }
    // Validate every original argument before merging duplicates or ancestors.
    for (const auto& selector : selectors)
        if (!matches(candidates.paths, selector)) throw Error("恢复路径未匹配来源或索引: " + selector);
    const std::set<std::string> sorted(selectors.begin(), selectors.end());
    BoundedPaths selected;
    for (const auto& selector : sorted) {
        if (selector.empty()) return std::move(candidates.paths);
        bool covered = false;
        for (auto slash = selector.find('/'); slash != std::string::npos; slash = selector.find('/', slash + 1))
            if (sorted.contains(selector.substr(0, slash))) { covered = true; break; }
        if (covered) continue;
        if (candidates.paths.contains(selector)) selected.add(selector);
        const auto prefix = selector + "/";
        for (auto it = candidates.paths.lower_bound(prefix); it != candidates.paths.end() && it->starts_with(prefix); ++it)
            selected.add(*it);
    }
    return std::move(selected.paths);
}
}
void restore_paths(const Repository& repo, const std::vector<std::filesystem::path>& paths,
                   const RestoreOptions& options) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    if (options.source && (options.source->empty() || options.source->size() > 4096))
        throw Error("恢复来源为空或超过 4096 字节限制");
    const auto selectors = normalize_selectors(repo, paths);
    const auto index = load_index(repo);
    const auto checked = check_index(repo, index);
    const bool worktree = options.worktree || !options.staged;
    const bool index_source = !options.source && !options.staged;
    const auto source = index_source ? checked.source :
        read_snapshot(repo, read_commit(repo, resolve_commit(repo, options.source ? *options.source : "HEAD")).tree);
    const auto selected = select_paths(source, checked.paths, selectors);
    if (index_source)
        for (const auto& path : selected)
            if (checked.conflicts.contains(path)) throw Error("恢复选中的索引路径存在未解决冲突: " + path);
    Index next_index;
    if (options.staged) {
        next_index.reserve(index.size());
        for (const auto& entry : index) if (!selected.contains(entry.path)) next_index.push_back(entry);
        for (const auto& path : selected) {
            const auto found = source.find(path);
            if (found != source.end()) next_index.push_back({path, found->second.mode, found->second.id});
        }
        // Combined distinct-path/stage budgets and DF topology must pass before
        // any worktree inspection or immutable backup-object capture.
        validate_index_budget(next_index);
        (void)encode_index(next_index);
        std::sort(next_index.begin(), next_index.end(), [](const auto& left, const auto& right) {
            return left.path == right.path ? left.stage < right.stage : left.path < right.path;
        });
    }
    BoundedPaths owned;
    WorktreeTargets targets;
    if (worktree) {
        owned = checked.paths;
        const auto head = read_head(repo);
        if (head.id)
            for (const auto& [path, state] : read_snapshot(repo, read_commit(repo, *head.id).tree)) {
                (void)state;
                owned.add(path);
            }
        for (const auto& path : selected) {
            const auto found = source.find(path);
            targets.emplace_hint(targets.end(), path, WorktreeTarget{
                found == source.end() ? std::nullopt : std::optional(found->second), std::nullopt});
        }
    }
    TransactionPlan plan;
    if (options.staged && next_index != index) plan_index_update(repo, plan, next_index);
    if (worktree) plan_worktree_update(repo, plan, targets, owned.paths);
    if (!plan.work.empty() || !plan.metadata.empty()) execute_transaction(repo, std::move(plan));
}
}
