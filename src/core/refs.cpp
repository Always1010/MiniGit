#include "minigit/refs.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/index.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace minigit {
namespace {
bool present(const std::filesystem::path& path) {
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) return false;
    if (ec) throw Error("检查引用失败: " + ec.message());
    if (!std::filesystem::is_regular_file(status)) throw Error("引用必须是普通文件");
    return true;
}
ObjectId parse_id(std::string_view text) {
    if (text.size() != 65 || text.back() != '\n') throw Error("引用对象编号格式非法");
    return ObjectId::from_hex(text.substr(0, 64));
}
int open_ref_directory(const std::filesystem::path& path) {
    // Pin each directory inode, with no symlink following.
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw Error("打开引用根目录失败");
    try {
        for (const auto& part : std::filesystem::absolute(path).relative_path()) {
            const int next = ::openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) throw Error("打开引用目录失败");
            ::close(fd);
            fd = next;
        }
        return fd;
    } catch (...) { ::close(fd); throw; }
}
void delete_ref(const std::filesystem::path& path) {
    const int fd = open_ref_directory(path.parent_path());
    try {
        struct stat state{};
        if (::fstatat(fd, path.filename().c_str(), &state, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(state.st_mode))
            throw Error("待删除引用必须是普通文件");
        if (::unlinkat(fd, path.filename().c_str(), 0) != 0) throw Error("删除引用失败");
        if (::fsync(fd) != 0) throw Error("同步引用删除失败");
        ::close(fd);
    } catch (...) { ::close(fd); throw; }
}
void prune_empty_ref_directories(const Repository& repo, const std::filesystem::path& path) {
    const auto refs_root = repo.metadata() / "refs";
    auto directory = path.parent_path();
    // Keep the namespace directory (heads, tags, or another explicit namespace).
    while (directory != refs_root && directory.parent_path() != refs_root) {
        const auto parent = directory.parent_path();
        detail::reject_symlinks(directory);
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(directory, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) {
            directory = parent;
            continue;
        }
        if (ec || !std::filesystem::is_directory(status)) throw Error("待清理引用目录必须是目录且禁止软链接");
        const int fd = open_ref_directory(parent);
        try {
            struct stat state{};
            if (::fstatat(fd, directory.filename().c_str(), &state, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(state.st_mode))
                throw Error("待清理引用目录必须是目录且禁止软链接");
            if (::unlinkat(fd, directory.filename().c_str(), AT_REMOVEDIR) != 0) {
                if (errno == ENOTEMPTY || errno == EEXIST) { ::close(fd); return; }
                throw Error("清理空引用目录失败");
            }
            if (::fsync(fd) != 0) throw Error("同步引用目录清理失败");
            ::close(fd);
        } catch (...) { ::close(fd); throw; }
        directory = parent;
    }
}
void sync_existing_ref_parent(const Repository& repo, const std::filesystem::path& path) {
    const auto refs_root = repo.metadata() / "refs";
    auto directory = path.parent_path();
    for (;;) {
        detail::reject_symlinks(directory);
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(directory, ec);
        if (!ec && std::filesystem::is_directory(status)) { detail::sync_directory(directory); return; }
        const bool missing = ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found);
        if (!missing || directory == refs_root) throw Error("同步引用父目录失败");
        directory = directory.parent_path();
    }
}
void validate_published_commit(const Repository& repo, const ObjectId& id) {
    (void)read_snapshot(repo, read_commit(repo, id).tree);
}
}
void validate_ref_name(std::string_view name) {
    validate_repo_path(name);
    if (!name.starts_with("refs/") || name.size() <= 5 || name.back() == '.' ||
        name.find("..") != std::string_view::npos || name.find("@{") != std::string_view::npos ||
        name.find_first_of(" ~^:?*[\\") != std::string_view::npos)
        throw Error("非法引用名称");
    for (const char c : name) if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) == 127)
        throw Error("引用名称禁止控制字符");
    while (!name.empty()) {
        const auto slash = name.find('/');
        const auto part = name.substr(0, slash);
        if (part.empty() || part.front() == '.' || part.back() == '.' || part.ends_with(".lock")) throw Error("非法引用路径分量");
        if (slash == std::string_view::npos) break;
        name.remove_prefix(slash + 1);
        if (name.empty()) throw Error("引用名称禁止尾随斜线");
    }
}
std::optional<ObjectId> read_ref(const Repository& repo, std::string_view name) {
    validate_ref_name(name);
    const auto path = repo.metadata() / std::string(name);
    if (!present(path)) return std::nullopt;
    return parse_id(to_string(read_file(path)));
}
Head read_head(const Repository& repo) {
    const auto text = to_string(read_file(repo.metadata() / "HEAD"));
    if (text.starts_with("ref: ")) {
        if (text.empty() || text.back() != '\n') throw Error("HEAD 格式非法");
        const auto name = std::string_view(text).substr(5, text.size() - 6);
        validate_ref_name(name);
        if (!name.starts_with("refs/heads/")) throw Error("HEAD 必须指向分支");
        return {std::string(name), read_ref(repo, name)};
    }
    return {std::nullopt, parse_id(text)};
}
std::map<std::string, ObjectId> list_refs(const Repository& repo, std::string_view prefix) {
    if (prefix.empty()) prefix = "refs/";
    auto check = prefix;
    if (check.ends_with('/')) check.remove_suffix(1);
    if (check != "refs") validate_ref_name(check);
    const auto root = repo.metadata() / "refs";
    detail::require_directory(root);
    std::map<std::string, ObjectId> result;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, ec), end;
    if (ec) throw Error("遍历引用失败: " + ec.message());
    std::size_t count = 0;
    for (; it != end; it.increment(ec)) {
        if (ec) throw Error("遍历引用失败: " + ec.message());
        if (++count > 100000 || it.depth() >= 128) throw Error("引用数量或层级超过限制");
        detail::reject_symlinks(it->path());
        const auto status = it->symlink_status(ec);
        if (ec) throw Error("检查引用条目失败: " + ec.message());
        if (std::filesystem::is_directory(status)) { detail::require_directory(it->path()); continue; }
        if (!std::filesystem::is_regular_file(status)) throw Error("引用必须是普通文件");
        const auto name = it->path().lexically_relative(repo.metadata()).generic_string();
        validate_ref_name(name);
        if (name == check || (name.starts_with(check) && name.size() > check.size() && name[check.size()] == '/'))
            result.emplace(name, parse_id(to_string(read_file(it->path()))));
    }
    if (ec) throw Error("遍历引用失败: " + ec.message());
    return result;
}
void update_ref(const Repository& repo, std::string_view name, std::optional<ObjectId> expected, std::optional<ObjectId> desired) {
    validate_ref_name(name);
    // Reading and deleting legacy refs remain possible; new values must be
    // reachable by every bounded metadata traversal, including GC.
    if (desired) validate_repo_path_limits(name);
    const auto actual = read_ref(repo, name);
    if (actual != expected) throw Error("引用已变化，更新被拒绝");
    if (desired) validate_published_commit(repo, *desired);
    const auto path = repo.metadata() / std::string(name);
    if (desired == actual) {
        if (desired) {
            detail::ensure_directory(path.parent_path());
            detail::sync_directory(path.parent_path());
        } else {
            prune_empty_ref_directories(repo, path);
            sync_existing_ref_parent(repo, path);
        }
        return;
    }
    if (!desired) {
        delete_ref(path);
        prune_empty_ref_directories(repo, path);
        return;
    }
    detail::ensure_directory(path.parent_path());
    atomic_write(path, to_bytes(desired->hex() + "\n"));
}
void set_head(const Repository& repo, std::string_view target) {
    if (target.starts_with("refs/")) {
        validate_ref_name(target);
        validate_repo_path_limits(target);
        if (!target.starts_with("refs/heads/")) throw Error("HEAD 必须指向分支");
        if (const auto id = read_ref(repo, target)) validate_published_commit(repo, *id);
        atomic_write(repo.metadata() / "HEAD", to_bytes("ref: " + std::string(target) + "\n"));
    } else {
        const auto id = ObjectId::from_hex(target);
        validate_published_commit(repo, id);
        atomic_write(repo.metadata() / "HEAD", to_bytes(id.hex() + "\n"));
    }
}
ObjectId resolve_commit(const Repository& repo, std::string_view revision) {
    std::optional<ObjectId> id;
    if (revision == "HEAD") id = read_head(repo).id;
    else if (revision.size() == 64 && revision.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos)
        id = ObjectId::from_hex(revision);
    else if (revision.starts_with("refs/")) id = read_ref(repo, revision);
    else {
        const auto branch = read_ref(repo, "refs/heads/" + std::string(revision));
        const auto tag = read_ref(repo, "refs/tags/" + std::string(revision));
        if (branch && tag) throw Error("分支和标签名称存在歧义");
        id = branch ? branch : tag;
    }
    if (!id) throw Error("提交引用不存在或分支尚无提交");
    (void)read_commit(repo, *id);
    return *id;
}
}
