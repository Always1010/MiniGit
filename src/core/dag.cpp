#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/transaction.hpp"
#include "minigit/fs.hpp"
#include "minigit/refs.hpp"
#include <algorithm>
#include <set>
namespace minigit {
namespace {
constexpr std::size_t max_commits = 100000;
constexpr std::size_t max_parent_edges = 1000000;
struct GraphBudget {
    std::size_t edges = 0;
    std::size_t bytes = 0;
    void add_bytes(std::size_t count) {
        if (count > max_file_size - bytes) throw Error("提交图解码数据超过 64 MiB 限制");
        bytes += count;
    }
    void add(const Commit& commit) {
        if (commit.parents.size() > max_parent_edges - edges) throw Error("提交图父边超过限制");
        edges += commit.parents.size();
        add_bytes(commit.parents.size() * 32);
        add_bytes(commit.author.size());
        add_bytes(commit.committer.size());
        add_bytes(commit.message.size());
    }
};
}
CommitGraph load_commit_graph(const Repository& repo, const std::vector<ObjectId>& roots) {
    CommitGraph graph;
    GraphBudget budget;
    std::set<ObjectId> discovered;
    std::vector<ObjectId> pending;
    auto enqueue = [&](const ObjectId& id) {
        if (discovered.contains(id)) return;
        if (discovered.size() >= max_commits) throw Error("提交图节点超过限制");
        discovered.insert(id);
        pending.push_back(id);
    };
    for (const auto& id : roots) enqueue(id);
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        auto commit = read_commit(repo, id);
        budget.add(commit);
        for (const auto& parent : commit.parents) enqueue(parent);
        graph.emplace(id, std::move(commit));
    }
    return graph;
}
std::vector<ObjectId> topological_order(const CommitGraph& graph) {
    if (graph.size() > max_commits) throw Error("提交图节点超过限制");
    GraphBudget budget;
    std::map<ObjectId, std::size_t> children;
    for (const auto& [id, commit] : graph) { children.emplace(id, 0); budget.add(commit); }
    for (const auto& [id, commit] : graph) {
        (void)id;
        for (const auto& parent : commit.parents) {
            const auto found = children.find(parent);
            if (found == children.end()) throw Error("提交图缺少父节点");
            ++found->second;
        }
    }
    std::set<ObjectId> ready;
    for (const auto& [id, count] : children) if (count == 0) ready.insert(id);
    std::vector<ObjectId> result;
    result.reserve(graph.size());
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        result.push_back(id);
        for (const auto& parent : graph.at(id).parents) {
            auto& count = children.at(parent);
            if (--count == 0) ready.insert(parent);
        }
    }
    if (result.size() != graph.size()) throw Error("提交图包含环");
    return result;
}
std::optional<ObjectId> find_merge_base(const Repository& repo, const ObjectId& left,
                                       const ObjectId& right) {
    const auto graph = load_commit_graph(repo, {left, right});
    const auto order = topological_order(graph);
    struct Reach { unsigned mask = 0; bool common_descendant = false; };
    std::map<ObjectId, Reach> reach;
    for (const auto& [id, commit] : graph) { (void)commit; reach.emplace(id, Reach{}); }
    reach.at(left).mask |= 1;
    reach.at(right).mask |= 2;
    // A parent is visited after every child, including paths through non-first
    // parents. Duplicate edges only repeat idempotent OR operations.
    for (const auto& id : order)
        for (const auto& parent : graph.at(id).parents) reach.at(parent).mask |= reach.at(id).mask;
    // Only strict ancestors inherit this flag; a common node must not mark itself
    // as its own common descendant, or every candidate would be discarded.
    for (const auto& id : order) {
        const auto& current = reach.at(id);
        if (current.mask == 3 || current.common_descendant)
            for (const auto& parent : graph.at(id).parents) reach.at(parent).common_descendant = true;
    }
    std::optional<ObjectId> result;
    for (const auto& id : order) {
        const auto& current = reach.at(id);
        if (current.mask != 3 || current.common_descendant) continue;
        if (result) throw Error("提交历史存在多个最低共同祖先");
        result = id;
    }
    return result;
}
std::vector<HistoryEntry> log_history(const Repository& repo, std::string_view revision, std::optional<std::size_t> maximum) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    if (revision == "HEAD" && !read_head(repo).id) return {};
    auto graph = load_commit_graph(repo, {resolve_commit(repo, revision)});
    const auto order = topological_order(graph);
    const auto count = maximum ? std::min(*maximum, order.size()) : order.size();
    std::vector<HistoryEntry> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) result.push_back({order[i], std::move(graph.at(order[i]))});
    return result;
}
}
