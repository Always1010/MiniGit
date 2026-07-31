#include "checkout_support.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include <algorithm>
#include <utility>
namespace minigit::checkout_support {
namespace { constexpr std::size_t max_paths = 200000; }
void validate_path(std::string_view path) {
    validate_repo_path(path);
    if (path.size() > 4096 || std::count(path.begin(), path.end(), '/') + 1 > 128)
        throw Error("路径超过 4096 字节或 128 分量限制");
}
void BoundedPaths::add(const std::string& path) {
    if (paths.contains(path)) return;
    validate_path(path);
    if (paths.size() >= max_paths || path.size() > max_file_size - bytes)
        throw Error("路径数量或总字节超过限制");
    paths.insert(path);
    bytes += path.size();
}
std::vector<std::string> normalize_selectors(const Repository& repo,
                                           const std::vector<std::filesystem::path>& paths) {
    if (paths.empty() || paths.size() > max_paths) throw Error("路径参数为空或数量超过限制");
    std::size_t bytes = 0;
    std::vector<std::string> selectors;
    selectors.reserve(paths.size());
    const auto cwd = std::filesystem::current_path();
    for (const auto& path : paths) {
        const auto raw = path.generic_string();
        if (raw.empty() || raw.size() > 8192 || raw.size() > max_file_size - bytes ||
            raw.find('\0') != std::string::npos)
            throw Error("路径参数为空或超过字节限制");
        bytes += raw.size();
        const auto absolute = (path.is_absolute() ? path : cwd / path).lexically_normal();
        const auto relative = absolute.lexically_relative(repo.root());
        if (relative.empty()) throw Error("路径不在仓库内");
        auto selector = relative.generic_string();
        while (selector.size() > 1 && selector.back() == '/') selector.pop_back();
        if (selector == ".") selector.clear();
        else validate_path(selector);
        selectors.push_back(std::move(selector));
    }
    return selectors;
}
bool has_descendants(const std::set<std::string>& paths, const std::string& selector) {
    if (selector.empty()) return !paths.empty();
    const auto prefix = selector + "/";
    const auto child = paths.lower_bound(prefix);
    return child != paths.end() && child->starts_with(prefix);
}
std::set<std::string> select_known_paths(const BoundedPaths& candidates,
                                       const std::vector<std::string>& selectors) {
    for (const auto& selector : selectors)
        if (!candidates.paths.contains(selector) && !has_descendants(candidates.paths, selector))
            throw Error("路径未匹配已知文件: " + selector);
    const std::set<std::string> sorted(selectors.begin(), selectors.end());
    BoundedPaths selected;
    for (const auto& selector : sorted) {
        if (selector.empty()) return candidates.paths;
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
void validate_index_budget(const Index& index) {
    if (index.size() > 300000) throw Error("索引阶段条目超过 300000 限制");
    std::set<std::string> paths;
    std::size_t bytes = 44;
    for (const auto& entry : index) {
        validate_path(entry.path);
        if (paths.insert(entry.path).second && paths.size() > 100000)
            throw Error("索引不同路径超过 100000 限制");
        if (entry.path.size() > max_file_size - bytes || 41 > max_file_size - bytes - entry.path.size())
            throw Error("索引超过 64 MiB 限制");
        bytes += entry.path.size() + 41;
    }
}
void check_index_objects(const Repository& repo, const Index& index) {
    validate_index_budget(index);
    const ObjectStore store(repo.metadata() / "objects");
    std::set<ObjectId> checked;
    for (const auto& entry : index)
        if (checked.insert(entry.id).second && store.read(entry.id).type != ObjectType::Blob)
            throw Error("索引对象不是 blob");
}
}
