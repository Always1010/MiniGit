#include "minigit/branches.hpp"
#include "minigit/dag.hpp"
#include "minigit/error.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "minigit/refs.hpp"
#include "minigit/snapshot.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <utility>
namespace minigit {
namespace {
constexpr std::string_view branch_prefix = "refs/heads/";
void validate_target(const Repository& repo, const ObjectId& id) {
    (void)read_snapshot(repo, read_commit(repo, id).tree);
}
std::optional<Bytes> raw_log(const Repository& repo, const std::string& ref) {
    (void)read_reflog(repo, ref);
    const auto path = repo.metadata() / ("logs/" + ref);
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) return std::nullopt;
    if (ec) throw Error("检查分支日志失败: " + ec.message());
    return read_file(path);
}
}
std::string branch_ref(std::string_view name) {
    if (name == "HEAD" || name.starts_with("refs/") ||
        (name.size() == 64 && name.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos))
        throw Error("分支名称与修订标识冲突");
    const auto ref = std::string(branch_prefix) + std::string(name);
    validate_ref_name(ref);
    const auto log = "logs/" + ref;
    if (log.size() > 4096 || std::count(log.begin(), log.end(), '/') + 1 > 128)
        throw Error("分支日志路径超过事务限制");
    return ref;
}
std::vector<BranchInfo> list_branches(const Repository& repo) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    const auto head = read_head(repo);
    std::map<std::string, BranchInfo> branches;
    std::set<ObjectId> checked;
    std::size_t name_bytes = 0;
    auto count_name = [&](std::string_view name) {
        if (name.size() > max_file_size - name_bytes) throw Error("分支名称总数据超过 64 MiB 限制");
        name_bytes += name.size();
    };
    if (head.id) { validate_target(repo, *head.id); checked.insert(*head.id); }
    for (const auto& [ref, id] : list_refs(repo, branch_prefix)) {
        const auto name = ref.substr(branch_prefix.size());
        (void)branch_ref(name);
        count_name(name);
        if (checked.insert(id).second) validate_target(repo, id);
        branches.emplace(name, BranchInfo{name, id, head.symbolic == ref});
    }
    if (head.symbolic && !head.id) {
        const auto name = head.symbolic->substr(branch_prefix.size());
        (void)branch_ref(name);
        count_name(name);
        branches.emplace(name, BranchInfo{name, std::nullopt, true});
    }
    std::vector<BranchInfo> result;
    result.reserve(branches.size());
    for (auto& [name, info] : branches) { (void)name; result.push_back(std::move(info)); }
    return result;
}
void plan_create_branch(const Repository& repo, TransactionPlan& plan, std::string_view name, ObjectId target, bool force) {
    require_no_transaction(repo);
    const auto ref = branch_ref(name);
    for (const auto& change : plan.metadata)
        if (change.path == ref) throw Error("分支引用已被计划中的其他操作修改");
    if (read_head(repo).symbolic == ref) throw Error("不能创建或覆盖当前分支，请使用提交或 reset");
    const auto existing = read_ref(repo, ref);
    if (existing && !force) throw Error("分支已存在，使用 -f 覆盖");
    validate_target(repo, target);
    if (existing == target) return;
    auto next = plan;
    const auto before = existing ? std::optional(read_file(repo.metadata() / ref)) : std::nullopt;
    next.metadata.push_back({ref, before, to_bytes(target.hex() + "\n")});
    plan_append_reflog(repo, next, ref, {existing, target, existing ? "branch:update" : "branch:create"});
    plan = std::move(next);
}
void create_branch(const Repository& repo, std::string_view name, std::string_view revision, bool force) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    require_no_operation(repo);
    (void)branch_ref(name);
    TransactionPlan plan;
    plan_create_branch(repo, plan, name, resolve_commit(repo, revision), force);
    if (!plan.metadata.empty()) execute_transaction(repo, std::move(plan));
}
void delete_branch(const Repository& repo, std::string_view name, bool force) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    require_no_operation(repo);
    const auto ref = branch_ref(name);
    const auto head = read_head(repo);
    if (head.symbolic == ref) throw Error("不能删除当前分支");
    const auto existing = read_ref(repo, ref);
    if (!existing) throw Error("分支不存在");
    validate_target(repo, *existing);
    if (!force) {
        if (!head.id) throw Error("HEAD 尚无提交，无法确认分支已经合入");
        const auto graph = load_commit_graph(repo, {*head.id});
        (void)topological_order(graph);
        if (!graph.contains(*existing)) throw Error("分支尚未合入当前 HEAD，使用 -D 强制删除");
    }
    TransactionPlan plan;
    plan.metadata.push_back({ref, read_file(repo.metadata() / ref), std::nullopt});
    if (const auto bytes = raw_log(repo, ref)) plan.metadata.push_back({"logs/" + ref, bytes, std::nullopt});
    execute_transaction(repo, std::move(plan));
}
}
