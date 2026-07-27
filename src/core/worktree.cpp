#include "minigit/worktree.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include <algorithm>
#include <cerrno>
#include <fnmatch.h>
#include <set>
#include <sys/stat.h>
namespace minigit {
namespace {
struct WorkingFile { std::string mode; Bytes bytes; };
struct IgnoreRule { std::string pattern; bool basename; bool directory; };
bool below(std::string_view path, std::string_view prefix) {
    return prefix.empty() || path == prefix || (path.starts_with(prefix) && path.size() > prefix.size() && path[prefix.size()] == '/');
}
std::optional<std::filesystem::file_status> inspect(const Repository& repo, std::string_view path) {
    std::filesystem::path current = repo.root();
    const std::filesystem::path relative{std::string(path)};
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        current /= *part;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory ||
            (!error && status.type() == std::filesystem::file_type::not_found)) return std::nullopt;
        if (error) throw Error("检查工作区路径失败: " + error.message());
        if (std::filesystem::is_symlink(status)) throw Error("工作区禁止软链接: " + current.string());
        auto next = part;
        if (++next != relative.end() && !std::filesystem::is_directory(status)) {
            if (!std::filesystem::is_regular_file(status)) throw Error("工作区包含不支持的文件类型");
            return std::nullopt;
        }
        if (next == relative.end()) return status;
    }
    return std::filesystem::status(repo.root());
}
WorkingFile read_working_file(const Repository& repo, const std::string& path, const std::filesystem::file_status& status) {
    if (!std::filesystem::is_regular_file(status)) throw Error("工作区条目必须是普通文件: " + path);
    constexpr auto execute = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
    return {(status.permissions() & execute) == std::filesystem::perms::none ? "100644" : "100755", read_file(repo.root() / path)};
}
std::vector<IgnoreRule> read_ignore(const Repository& repo) {
    const auto status = inspect(repo, ".minigitignore");
    if (!status) return {};
    if (!std::filesystem::is_regular_file(*status)) throw Error(".minigitignore 必须是普通文件");
    const auto text = to_string(read_file(repo.root() / ".minigitignore"));
    std::vector<IgnoreRule> rules;
    std::string_view remaining(text);
    while (!remaining.empty()) {
        const auto end = remaining.find('\n');
        auto line = remaining.substr(0, end);
        if (end == std::string_view::npos) remaining = {};
        else remaining.remove_prefix(end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() || line.starts_with('#')) continue;
        if (line.starts_with('!') || line.find("**") != std::string_view::npos)
            throw Error(".minigitignore 不支持 ! 或 ** 规则");
        const bool anchored = line.starts_with('/');
        if (anchored) line.remove_prefix(1);
        const bool directory = line.ends_with('/');
        if (directory) line.remove_suffix(1);
        validate_repo_path(line);
        const bool basename = !anchored && line.find('/') == std::string_view::npos;
        // Only * and ? are glob operators; brackets retain their literal meaning.
        std::string pattern;
        for (const char c : line) {
            if (c == '[' || c == ']') pattern.push_back('\\');
            pattern.push_back(c);
        }
        rules.push_back({std::move(pattern), basename, directory});
    }
    return rules;
}
bool ignored(std::string_view path, bool directory, const std::vector<IgnoreRule>& rules) {
    for (const auto& rule : rules) {
        std::size_t start = 0;
        for (;;) {
            const auto slash = path.find('/', start);
            const bool is_directory = slash != std::string_view::npos || directory;
            const auto end = slash == std::string_view::npos ? path.size() : slash;
            const auto candidate = rule.basename ? path.substr(start, end - start) : path.substr(0, end);
            if ((!rule.directory || is_directory) && ::fnmatch(rule.pattern.c_str(), std::string(candidate).c_str(), FNM_PATHNAME) == 0)
                return true;
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
    }
    return false;
}
bool tracked_below(const std::set<std::string>& tracked, const std::string& directory) {
    const auto first = tracked.lower_bound(directory + "/");
    return first != tracked.end() && below(*first, directory);
}
template<class Visitor>
void walk(const Repository& repo, const std::string& directory, const std::vector<IgnoreRule>& rules,
          const std::set<std::string>& tracked, Visitor& visitor) {
    std::error_code error;
    std::filesystem::directory_iterator children(repo.root() / directory, error);
    if (error) throw Error("遍历工作区目录失败: " + error.message());
    for (const auto& child : children) {
        const auto name = child.path().filename().string();
        if (name == ".git" || name == ".minigit") continue;
        const auto path = directory.empty() ? name : directory + "/" + name;
        validate_repo_path(path);
        const auto status = child.symlink_status(error);
        if (error) throw Error("检查工作区文件失败: " + error.message());
        if (std::filesystem::is_symlink(status)) throw Error("工作区禁止软链接: " + path);
        const bool is_directory = std::filesystem::is_directory(status);
        if (!is_directory && !std::filesystem::is_regular_file(status)) throw Error("工作区包含不支持的文件类型: " + path);
        if (ignored(path, is_directory, rules) && !tracked.contains(path) && (!is_directory || !tracked_below(tracked, path))) continue;
        if (is_directory) walk(repo, path, rules, tracked, visitor);
        else visitor(path, status);
    }
}
std::set<std::string> tracked_paths(const Index& index) {
    std::set<std::string> result;
    for (const auto& entry : index) result.insert(entry.path);
    return result;
}
std::string resolve_input(const Repository& repo, const std::filesystem::path& input) {
    const auto absolute = std::filesystem::absolute(input);
    // Check the original spelling first: link/../file must not hide a symlink.
    bool non_directory_ancestor = false;
    std::filesystem::path current = absolute.root_path();
    for (const auto& part : absolute.relative_path()) {
        if (part == "..") {
            struct stat parent{};
            if (::lstat(current.c_str(), &parent) != 0 || !S_ISDIR(parent.st_mode))
                throw Error(".. 前的路径必须是现有目录: " + current.string());
        }
        current /= part;
        struct stat state{};
        if (::lstat(current.c_str(), &state) == 0) {
            if (S_ISLNK(state.st_mode)) throw Error("仓库路径禁止软链接: " + current.string());
        } else if (errno == ENOTDIR) non_directory_ancestor = true;
        else if (errno != ENOENT) throw Error("检查输入路径失败: " + current.string());
    }
    if (!non_directory_ancestor) detail::reject_symlinks(absolute);
    const auto normalized = absolute.lexically_normal();
    const auto relative = normalized.lexically_relative(repo.root());
    if (relative.empty() || relative.is_absolute()) throw Error("路径不在仓库内");
    if (relative == ".") return {};
    const auto path = relative.generic_string();
    validate_repo_path(path);
    return path;
}
}
std::optional<FileState> read_worktree_entry(const Repository& repo, std::string_view path) {
    validate_repo_path(path);
    const auto status = inspect(repo, path);
    if (!status || std::filesystem::is_directory(*status)) return std::nullopt;
    const auto file = read_working_file(repo, std::string(path), *status);
    return FileState{file.mode, hash_object(ObjectType::Blob, file.bytes)};
}
Snapshot scan_worktree(const Repository& repo) {
    const auto tracked = tracked_paths(load_index(repo));
    const auto rules = read_ignore(repo);
    Snapshot result;
    auto visitor = [&](const std::string& path, const std::filesystem::file_status& status) {
        const auto file = read_working_file(repo, path, status);
        result.emplace(path, FileState{file.mode, hash_object(ObjectType::Blob, file.bytes)});
    };
    walk(repo, "", rules, tracked, visitor);
    return result;
}
void add_paths(const Repository& repo, const std::vector<std::filesystem::path>& input, bool all) {
    if (input.empty() && !all) throw Error("add 需要指定路径或 -A");
    RepoLock lock(repo.metadata());
    auto index = load_index(repo);
    const auto tracked = tracked_paths(index);
    const auto rules = read_ignore(repo);
    std::set<std::string> selected;
    // Normalize every argument before writing objects; failed selections never change the index.
    if (all) selected.insert("");
    for (const auto& argument : input) selected.insert(resolve_input(repo, argument));
    ObjectStore store(repo.metadata() / "objects");
    Snapshot files;
    auto visitor = [&](const std::string& path, const std::filesystem::file_status& status) {
        if (files.contains(path)) return;
        // Keep only IDs after each file is stored, rather than retaining all payloads.
        const auto file = read_working_file(repo, path, status);
        files.emplace(path, FileState{file.mode, store.write(ObjectType::Blob, file.bytes)});
    };
    for (const auto& path : selected) {
        const auto status = path.empty() ? std::optional(std::filesystem::status(repo.root())) : inspect(repo, path);
        if (!status) {
            if (!tracked.contains(path) && !tracked_below(tracked, path)) throw Error("指定路径不存在且未被跟踪: " + path);
            continue;
        }
        const bool directory = std::filesystem::is_directory(*status);
        if (!path.empty() && ignored(path, directory, rules) && !tracked.contains(path) && (!directory || !tracked_below(tracked, path)))
            throw Error("指定路径被 .minigitignore 忽略: " + path);
        if (directory) walk(repo, path, rules, tracked, visitor);
        else visitor(path, *status);
    }
    // Remove selected old entries, including deletions and every conflict stage.
    std::erase_if(index, [&](const IndexEntry& entry) {
        for (const auto& path : selected) if (below(entry.path, path)) return true;
        for (const auto& [path, file] : files) {
            (void)file;
            if (below(path, entry.path) || below(entry.path, path)) return true;
        }
        return false;
    });
    for (const auto& [path, file] : files) index.push_back({path, file.mode, file.id, 0});
    save_index(repo, std::move(index));
}
}
