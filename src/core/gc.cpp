#include "minigit/gc.hpp"
#include "minigit/error.hpp"
#include "minigit/index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/reflog.hpp"
#include "minigit/stash_stack.hpp"
#include "checkout_support.hpp"
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>
namespace minigit {
namespace {
constexpr std::size_t max_objects = 100000, max_edges = 1000000;
struct Budget {
    std::size_t bytes = 0;
    void add(std::size_t count) {
        if (count > max_file_size - bytes) throw Error("GC 数据超过 64 MiB 限制");
        bytes += count;
    }
    void check(std::uintmax_t count) const {
        if (count > max_file_size - bytes) throw Error("GC 元数据读取超过 64 MiB 限制");
    }
};
struct Edge { ObjectId target; ObjectType type; };
struct Node {
    ObjectType type = ObjectType::Blob;
    std::vector<Edge> edges;
    std::filesystem::file_time_type modified;
};
using Graph = std::map<ObjectId, Node>;
bool lower_hex(std::string_view value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
std::filesystem::file_status inspect_status(const std::filesystem::path& path) {
    detail::reject_symlinks(path);
    std::error_code ec; const auto result = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return std::filesystem::file_status(std::filesystem::file_type::not_found);
    if (ec) throw Error("GC 检查节点失败: " + ec.message());
    return result;
}
Graph inventory(const Repository& repo, Budget& budget) {
    const auto root = repo.metadata() / "objects"; detail::require_directory(root);
    Graph graph; std::error_code ec; std::filesystem::directory_iterator directory(root, ec), end;
    if (ec) throw Error("GC 遍历对象目录失败: " + ec.message());
    std::size_t fanouts = 0;
    for (; directory != end; directory.increment(ec)) {
        if (ec) throw Error("GC 遍历对象目录失败: " + ec.message());
        const auto name = directory->path().filename().generic_string();
        if (++fanouts > 256 || !lower_hex(name, 2) || !std::filesystem::is_directory(inspect_status(directory->path())))
            throw Error("GC 对象扇出必须为小写两位十六进制目录");
        budget.add(name.size()); detail::require_directory(directory->path());
        std::filesystem::directory_iterator file(directory->path(), ec);
        if (ec) throw Error("GC 遍历对象扇出失败: " + ec.message());
        for (; file != end; file.increment(ec)) {
            if (ec) throw Error("GC 遍历对象扇出失败: " + ec.message());
            if (graph.size() >= max_objects) throw Error("GC 对象超过 100000 限制");
            const auto tail = file->path().filename().generic_string();
            if (!lower_hex(tail, 62) || !std::filesystem::is_regular_file(inspect_status(file->path())))
                throw Error("GC 对象必须为小写六十二位十六进制普通文件");
            budget.add(32 + 1 + 65); // Node ID/type and relative fanout/file path.
            const auto modified = std::filesystem::last_write_time(file->path(), ec);
            if (ec) throw Error("GC 读取对象修改时间失败: " + ec.message());
            graph.emplace(ObjectId::from_hex(name + tail), Node{ObjectType::Blob, {}, modified});
        }
        if (ec) throw Error("GC 遍历对象扇出失败: " + ec.message());
    }
    if (ec) throw Error("GC 遍历对象目录失败: " + ec.message());
    return graph;
}
void describe_graph(const Repository& repo, Graph& graph, Budget& budget) {
    const ObjectStore store(repo.metadata() / "objects"); std::size_t edges = 0;
    for (auto& [id, node] : graph) {
        // ObjectStore validates framing, hash and schema even for unreachable
        // data. Retain descriptions only; payload is released each iteration.
        const auto object = store.read(id); node.type = object.type;
        const auto edge = [&](const ObjectId& target, ObjectType type) {
            if (edges >= max_edges) throw Error("GC 对象图超过 1000000 条边限制");
            budget.add(32 + 1); ++edges; node.edges.push_back({target, type});
        };
        if (object.type == ObjectType::Commit) {
            const auto commit = decode_commit(object.payload);
            budget.add(commit.author.size()); budget.add(commit.committer.size()); budget.add(commit.message.size());
            edge(commit.tree, ObjectType::Tree);
            for (const auto& parent : commit.parents) edge(parent, ObjectType::Commit);
        } else if (object.type == ObjectType::Tree) {
            for (const auto& entry : decode_tree(object.payload)) {
                budget.add(entry.name.size()); budget.add(entry.mode.size());
                edge(entry.id, entry.mode == "40000" ? ObjectType::Tree : ObjectType::Blob);
            }
        }
    }
    for (const auto& [id, node] : graph) {
        (void)id;
        for (const auto& edge : node.edges) {
            const auto target = graph.find(edge.target);
            if (target == graph.end() || target->second.type != edge.type) throw Error("GC 对象图引用缺失或类型不符");
        }
    }
}
std::vector<ObjectId> deletion_order(const Graph& graph) {
    std::map<ObjectId, std::size_t> incoming;
    for (const auto& [id, node] : graph) { (void)node; incoming.emplace(id, 0); }
    for (const auto& [id, node] : graph) {
        (void)id;
        for (const auto& edge : node.edges) ++incoming.at(edge.target);
    }
    std::vector<ObjectId> ready, order;
    order.reserve(graph.size());
    for (const auto& [id, count] : incoming) if (count == 0) ready.push_back(id);
    while (!ready.empty()) {
        const auto id = ready.back(); ready.pop_back(); order.push_back(id);
        for (const auto& edge : graph.at(id).edges)
            if (--incoming.at(edge.target) == 0) ready.push_back(edge.target);
    }
    if (order.size() != graph.size()) throw Error("GC 对象图包含循环");
    return order;
}
class Roots {
public:
    Roots(const Repository& repo, const Graph& graph) : repo_(repo), graph_(graph) {}
    void keep(const ObjectId& id, std::optional<ObjectType> type = std::nullopt) {
        const auto found = graph_.find(id);
        if (found == graph_.end() || (type && found->second.type != *type)) throw Error("GC 根对象缺失或类型不符");
        roots.insert(id); // The complete inventory bounds distinct roots to 100000.
    }
    void path(const std::string& relative) {
        checkout_support::validate_path(relative);
        if (paths_.contains(relative)) return;
        budget_.add(relative.size()); paths_.insert(relative);
    }
    std::optional<Bytes> raw(const std::string& relative, bool required = false) {
        path(relative); const auto absolute = repo_.metadata() / relative; const auto state = inspect_status(absolute);
        if (state.type() == std::filesystem::file_type::not_found) {
            if (required) throw Error("GC 必需元数据不存在: " + relative);
            return std::nullopt;
        }
        if (!std::filesystem::is_regular_file(state)) throw Error("GC 元数据必须是普通文件: " + relative);
        std::error_code ec; const auto size = std::filesystem::file_size(absolute, ec);
        if (ec) throw Error("GC 读取元数据大小失败: " + ec.message());
        budget_.check(size);
        auto bytes = read_file(absolute); budget_.add(bytes.size()); return bytes;
    }
    std::set<ObjectId> roots;
private:
    const Repository& repo_;
    const Graph& graph_;
    Budget budget_;
    std::set<std::string> paths_;
};
ObjectId reference_id(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 65 || bytes.back() != '\n') throw Error("GC 引用对象编号格式非法");
    return ObjectId::from_hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), 64));
}
std::map<std::string, ObjectId> reference_roots(const Repository& repo, Roots& roots) {
    const auto directory = repo.metadata() / "refs"; roots.path("refs"); detail::require_directory(directory);
    std::map<std::string, ObjectId> refs; std::error_code ec;
    std::filesystem::recursive_directory_iterator entry(directory, ec), end;
    if (ec) throw Error("GC 遍历引用失败: " + ec.message());
    std::size_t count = 0;
    for (; entry != end; entry.increment(ec)) {
        if (ec) throw Error("GC 遍历引用失败: " + ec.message());
        if (++count > 100000) throw Error("GC 引用节点超过 100000 限制");
        const auto relative = entry->path().lexically_relative(repo.metadata()).generic_string(); roots.path(relative);
        validate_ref_name(relative); const auto state = inspect_status(entry->path());
        if (std::filesystem::is_directory(state)) { detail::require_directory(entry->path()); continue; }
        const auto id = reference_id(*roots.raw(relative, true)); roots.keep(id, ObjectType::Commit); refs.emplace(relative, id);
    }
    if (ec) throw Error("GC 遍历引用失败: " + ec.message());
    return refs;
}
void head_root(const Repository& repo, Roots& roots, const std::map<std::string, ObjectId>& refs) {
    const auto raw = roots.raw("HEAD", true); const auto& bytes = *raw;
    if (bytes.size() > 4102) throw Error("GC HEAD 超过路径长度限制");
    const auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("ref: ")) {
        if (text.back() != '\n') throw Error("GC HEAD 格式非法");
        const auto ref = std::string(text.substr(5, text.size() - 6));
        checkout_support::validate_path(ref); validate_ref_name(ref);
        if (!ref.starts_with("refs/heads/")) throw Error("GC HEAD 必须指向分支");
        if (const auto found = refs.find(ref); found != refs.end()) roots.keep(found->second, ObjectType::Commit);
        else {
            roots.path(ref);
            if (inspect_status(repo.metadata() / ref).type() != std::filesystem::file_type::not_found)
                throw Error("GC 未出生分支引用不能是目录或其它节点");
        }
    } else roots.keep(reference_id(bytes), ObjectType::Commit);
}
void log_roots(const Repository& repo, Roots& roots) {
    const auto directory = repo.metadata() / "logs"; roots.path("logs"); const auto root = inspect_status(directory);
    if (root.type() == std::filesystem::file_type::not_found) return;
    if (!std::filesystem::is_directory(root)) throw Error("GC logs 必须是目录");
    detail::require_directory(directory); std::error_code ec;
    std::filesystem::recursive_directory_iterator entry(directory, ec), end;
    if (ec) throw Error("GC 遍历日志失败: " + ec.message());
    std::size_t count = 0;
    for (; entry != end; entry.increment(ec)) {
        if (ec) throw Error("GC 遍历日志失败: " + ec.message());
        if (++count > 100000) throw Error("GC 日志节点超过 100000 限制");
        const auto relative = entry->path().lexically_relative(repo.metadata()).generic_string(); roots.path(relative);
        const auto state = inspect_status(entry->path()); const bool is_directory = std::filesystem::is_directory(state);
        if (relative == "logs/HEAD") { if (is_directory) throw Error("GC HEAD 日志必须是文件"); }
        else if (relative == "logs/refs" || relative == "logs/refs/heads") {
            if (!is_directory) throw Error("GC 日志命名空间必须是目录");
        } else if (relative.starts_with("logs/refs/heads/")) validate_ref_name(std::string_view(relative).substr(5));
        else throw Error("GC 日志路径不属于 HEAD 或 refs/heads");
        if (is_directory) { detail::require_directory(entry->path()); continue; }
        for (const auto& log : decode_reflog(*roots.raw(relative, true))) {
            if (log.old_id) roots.keep(*log.old_id, ObjectType::Commit);
            if (log.new_id) roots.keep(*log.new_id, ObjectType::Commit);
        }
    }
    if (ec) throw Error("GC 遍历日志失败: " + ec.message());
}
void other_roots(const Repository& repo, Roots& roots, const std::map<std::string, ObjectId>& refs) {
    if (const auto raw = roots.raw("index")) {
        if (raw->size() >= 12) {
            std::uint32_t count = 0;
            for (const auto byte : std::span(*raw).subspan(8, 4)) count = (count << 8U) | byte;
            if (count > 300000) throw Error("GC Index 超过 300000 stage 条目限制");
        }
        const auto index = decode_index(*raw); validate_index_budget(index);
        for (const auto& entry : index) roots.keep(entry.id, ObjectType::Blob);
    }
    const auto raw_stack = roots.raw("stashstack"); const auto stack = raw_stack ? decode_stash_stack(*raw_stack) : StashStack{};
    const auto stash_ref = refs.find("refs/stash");
    roots.path("refs/stash"); const auto stash_node = inspect_status(repo.metadata() / "refs/stash");
    const bool stash_present = stash_node.type() != std::filesystem::file_type::not_found;
    if ((stash_present && !std::filesystem::is_regular_file(stash_node)) || stash_present != (stash_ref != refs.end()))
        throw Error("GC stash 引用必须缺失或为普通引用文件");
    if (stack.empty() ? stash_ref != refs.end() : stash_ref == refs.end() || stash_ref->second != stack.front())
        throw Error("GC stash 栈与引用不一致");
    for (const auto& id : stash_roots(repo, stack)) roots.keep(id);
    if (const auto raw = roots.raw("operation"))
        for (const auto& id : state_roots(repo, decode_operation(*raw))) roots.keep(id);
}
}
GcResult collect_garbage(const Repository& repo, const GcOptions& options) {
    RepoLock lock(repo.metadata()); require_no_transaction(repo);
    const auto now = std::filesystem::file_time_type::clock::now();
    Budget graph_budget; auto graph = inventory(repo, graph_budget); describe_graph(repo, graph, graph_budget);
    const auto order = deletion_order(graph);
    Roots roots(repo, graph); const auto refs = reference_roots(repo, roots);
    head_root(repo, roots, refs); log_roots(repo, roots); other_roots(repo, roots, refs);
    const auto cutoff = now - std::chrono::hours(24);
    for (const auto& [id, node] : graph)
        if (node.modified > now || (!options.prune_now && node.modified >= cutoff)) roots.keep(id);
    std::set<ObjectId> reachable; std::vector<ObjectId> pending;
    const auto enqueue = [&](const ObjectId& id) { if (reachable.insert(id).second) pending.push_back(id); };
    for (const auto& id : roots.roots) enqueue(id);
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        for (const auto& edge : graph.at(id).edges) enqueue(edge.target);
    }
    // Every object, edge, metadata source and root has now passed preflight.
    GcResult result{reachable.size(), 0}; const ObjectStore store(repo.metadata() / "objects");
    // Referrers precede their children. A failed unlink leaves a complete graph
    // among remaining objects, so the next preflight can safely retry.
    for (const auto& id : order) {
        if (!reachable.contains(id)) { detail::remove_regular(store.object_path(id)); ++result.removed; }
    }
    return result;
}
}
