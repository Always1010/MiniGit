#include "minigit/fs.hpp"
#include "minigit/error.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <optional>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
namespace minigit {
namespace {
[[noreturn]] void fail(std::string_view action) { throw Error(std::string(action) + ": " + std::strerror(errno)); }
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
int open_directory(const std::filesystem::path& path) {
    auto absolute = std::filesystem::absolute(path);
    Fd fd(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.value < 0) fail("打开根目录失败");
    for (const auto& component : absolute.relative_path()) {
        if (component.empty() || component == ".") continue;
        int next = ::openat(fd.value, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) fail("打开目录失败（禁止软链接）");
        ::close(fd.value);
        fd.value = next;
    }
    int result = fd.value;
    fd.value = -1;
    return result;
}
void require_regular(int fd) {
    struct stat state{};
    if (::fstat(fd, &state) != 0) fail("检查文件失败");
    if (!S_ISREG(state.st_mode)) throw Error("元数据必须是普通文件");
}
void reject_target(int parent, const std::filesystem::path& name) {
    struct stat state{};
    if (::fstatat(parent, name.c_str(), &state, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(state.st_mode)) throw Error("目标必须是普通文件，禁止软链接");
    } else if (errno != ENOENT) fail("检查目标文件失败");
}
}
namespace detail {
void reject_symlinks(const std::filesystem::path& path) {
    auto absolute = std::filesystem::absolute(path);
    std::filesystem::path current = absolute.root_path();
    for (const auto& part : absolute.relative_path()) {
        current /= part;
        struct stat state{};
        if (::lstat(current.c_str(), &state) != 0) {
            if (errno == ENOENT) continue;
            fail("检查路径失败");
        }
        if (S_ISLNK(state.st_mode)) throw Error("仓库路径禁止软链接: " + current.string());
    }
}
void require_directory(const std::filesystem::path& path) { Fd fd(open_directory(path)); }
void ensure_directory(const std::filesystem::path& path) {
    auto absolute = std::filesystem::absolute(path);
    Fd fd(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.value < 0) fail("打开根目录失败");
    for (const auto& component : absolute.relative_path()) {
        if (component.empty() || component == ".") continue;
        if (::mkdirat(fd.value, component.c_str(), 0755) != 0 && errno != EEXIST)
            fail("创建目录失败");
        // Synchronize existing entries too: they may survive an interrupted attempt.
        if (::fsync(fd.value) != 0) fail("同步目录的父目录失败");
        int next = ::openat(fd.value, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) fail("打开目录失败（禁止软链接）");
        ::close(fd.value);
        fd.value = next;
    }
}
void sync_directory(const std::filesystem::path& path) {
    Fd fd(open_directory(path));
    if (::fsync(fd.value) != 0) fail("同步目录失败");
}
namespace {
std::filesystem::path parent_of(const std::filesystem::path& path) {
    return path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
}
void validate_name(const std::filesystem::path& name) {
    if (name.empty() || name == "." || name == "..") throw Error("非法事务文件名");
}
void set_directory_permissions(int fd, std::uint32_t permissions) {
    if (permissions > 07777) throw Error("目录权限超过 07777 范围");
    if (::fchmod(fd, static_cast<mode_t>(permissions)) != 0) fail("设置目录权限失败");
    struct stat state{};
    if (::fstat(fd, &state) != 0) fail("检查目录权限失败");
    if (static_cast<std::uint32_t>(state.st_mode & 07777) != permissions)
        throw Error("目录权限未被精确应用");
    if (::fsync(fd) != 0) fail("同步目录权限失败");
}
void remove_entry(const std::filesystem::path& path, bool directory) {
    const auto name = path.filename();
    validate_name(name);
    Fd parent(open_directory(parent_of(path)));
    struct stat state{};
    if (::fstatat(parent.value, name.c_str(), &state, AT_SYMLINK_NOFOLLOW) != 0) fail("检查待删除节点失败");
    if (directory ? !S_ISDIR(state.st_mode) : !S_ISREG(state.st_mode))
        throw Error("待删除节点类型不符，禁止软链接");
    if (::unlinkat(parent.value, name.c_str(), directory ? AT_REMOVEDIR : 0) != 0) fail("删除事务节点失败");
    if (::fsync(parent.value) != 0) fail("同步事务节点删除失败");
}
}
void remove_regular(const std::filesystem::path& path) { remove_entry(path, false); }
void remove_empty_directory(const std::filesystem::path& path) { remove_entry(path, true); }
void create_directory_exact(const std::filesystem::path& path, std::uint32_t permissions, bool exclusive) {
    if (permissions > 07777) throw Error("目录权限超过 07777 范围");
    const auto name = path.filename();
    validate_name(name);
    Fd parent(open_directory(parent_of(path)));
    if (::mkdirat(parent.value, name.c_str(), 0700) != 0 && (exclusive || errno != EEXIST)) fail("创建事务目录失败");
    // mkdir is affected by umask, so pin even a mode-0000 inode before opening
    // it for fsync. /proc/self/fd refers to this inode, not a replaceable name.
    Fd pinned(::openat(parent.value, name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (pinned.value < 0) fail("固定事务目录失败");
    const auto pinned_path = "/proc/self/fd/" + std::to_string(pinned.value);
    if (::chmod(pinned_path.c_str(), 0700) != 0) fail("设置暂存目录访问权限失败");
    Fd fd(::open(pinned_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.value < 0) fail("打开事务目录失败");
    set_directory_permissions(fd.value, permissions);
    if (::fsync(parent.value) != 0) fail("同步事务目录创建失败");
}
void chmod_directory(const std::filesystem::path& path, std::uint32_t permissions) {
    if (permissions > 07777) throw Error("目录权限超过 07777 范围");
    const auto name = path.filename();
    validate_name(name);
    Fd parent(open_directory(parent_of(path)));
    Fd pinned(::openat(parent.value, name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (pinned.value < 0) fail("固定待修改目录失败");
    const auto pinned_path = "/proc/self/fd/" + std::to_string(pinned.value);
    int raw = ::open(pinned_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (raw < 0 && errno == EACCES) {
        if (::chmod(pinned_path.c_str(), 0700) != 0) fail("恢复目录访问权限失败");
        raw = ::open(pinned_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    if (raw < 0) fail("打开待修改目录失败");
    Fd fd(raw);
    set_directory_permissions(fd.value, permissions);
}
std::uint64_t directory_device(const std::filesystem::path& path) {
    Fd fd(open_directory(path));
    struct stat state{};
    if (::fstat(fd.value, &state) != 0) fail("检查目录设备失败");
    return static_cast<std::uint64_t>(state.st_dev);
}
std::uint64_t node_device(const std::filesystem::path& path) {
    const auto name = path.filename();
    validate_name(name);
    Fd parent(open_directory(parent_of(path)));
    Fd fd(::openat(parent.value, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (fd.value < 0) fail("固定事务节点设备失败");
    struct stat state{};
    if (::fstat(fd.value, &state) != 0) fail("检查事务节点设备失败");
    if (!S_ISREG(state.st_mode) && !S_ISDIR(state.st_mode)) throw Error("设备检查节点必须是普通文件或目录");
    return static_cast<std::uint64_t>(state.st_dev);
}
void rename_staged(const std::filesystem::path& source, const std::filesystem::path& target) {
    const auto source_name = source.filename(), target_name = target.filename();
    validate_name(source_name);
    validate_name(target_name);
    Fd source_parent(open_directory(parent_of(source))), target_parent(open_directory(parent_of(target)));
    struct stat source_state{}, target_parent_state{};
    if (::fstatat(source_parent.value, source_name.c_str(), &source_state, AT_SYMLINK_NOFOLLOW) != 0)
        fail("检查事务源节点失败");
    if (::fstat(target_parent.value, &target_parent_state) != 0) fail("检查事务目标设备失败");
    if (source_state.st_dev != target_parent_state.st_dev) throw Error("事务发布不支持跨文件系统");
    if (S_ISREG(source_state.st_mode)) {
        reject_target(target_parent.value, target_name);
        if (::renameat(source_parent.value, source_name.c_str(), target_parent.value, target_name.c_str()) != 0)
            fail("发布事务文件失败");
    } else if (S_ISDIR(source_state.st_mode)) {
        if (::syscall(SYS_renameat2, source_parent.value, source_name.c_str(), target_parent.value,
                      target_name.c_str(), RENAME_NOREPLACE) != 0) fail("发布事务目录失败");
    } else throw Error("事务源节点必须是普通文件或目录，禁止软链接");
    if (::fsync(target_parent.value) != 0) fail("同步事务目标父目录失败");
    if (::fsync(source_parent.value) != 0) fail("同步事务源父目录失败");
}
}
FileData read_file_data(const std::filesystem::path& path) {
    Fd parent(open_directory(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path()));
    Fd fd(::openat(parent.value, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (fd.value < 0) fail("读取文件失败（禁止软链接）");
    require_regular(fd.value);
    struct stat state{};
    if (::fstat(fd.value, &state) != 0) fail("检查文件失败");
    if (state.st_size < 0 || static_cast<std::uintmax_t>(state.st_size) > max_file_size)
        throw Error("文件超过 64 MiB 限制");
    Bytes result;
    result.reserve(static_cast<std::size_t>(state.st_size));
    std::array<std::uint8_t, 16384> buffer{};
    for (;;) {
        ssize_t n = ::read(fd.value, buffer.data(), buffer.size());
        if (n < 0) { if (errno == EINTR) continue; fail("读取文件失败"); }
        if (n == 0) break;
        const auto count = static_cast<std::size_t>(n);
        if (count > max_file_size - result.size()) throw Error("文件超过 64 MiB 限制");
        result.insert(result.end(), buffer.begin(), buffer.begin() + n);
    }
    struct stat after{};
    if (::fstat(fd.value, &after) != 0) fail("读取后检查文件失败");
    if (state.st_dev != after.st_dev || state.st_ino != after.st_ino || state.st_size != after.st_size ||
        state.st_mode != after.st_mode ||
        state.st_mtim.tv_sec != after.st_mtim.tv_sec || state.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        state.st_ctim.tv_sec != after.st_ctim.tv_sec || state.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        result.size() != static_cast<std::uintmax_t>(state.st_size))
        throw Error("读取期间文件内容或权限发生变化");
    return {std::move(result), static_cast<std::uint32_t>(state.st_mode & 07777)};
}
Bytes read_file(const std::filesystem::path& path) {
    auto data = read_file_data(path);
    return std::move(data.bytes);
}
namespace {
void atomic_write_impl(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                       std::optional<std::uint32_t> permissions) {
    if (permissions && *permissions > 07777) throw Error("文件权限超过 07777 范围");
    if (bytes.size() > max_file_size) throw Error("文件超过 64 MiB 限制");
    Fd parent(open_directory(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path()));
    const auto name = path.filename();
    if (name.empty() || name == "." || name == "..") throw Error("非法文件名");
    reject_target(parent.value, name);
    static std::atomic<unsigned long long> counter{0};
    std::string temporary;
    int raw = -1;
    for (unsigned attempts = 0; attempts < 128; ++attempts) {
        temporary = ".minigit-tmp-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1));
        raw = ::openat(parent.value, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (raw >= 0) break;
        if (errno != EEXIST) fail("创建临时文件失败");
    }
    if (raw < 0) throw Error("无法分配唯一临时文件");
    Fd fd(raw);
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            ssize_t n = ::write(fd.value, bytes.data() + offset, bytes.size() - offset);
            if (n < 0) { if (errno == EINTR) continue; fail("写入文件失败"); }
            if (n == 0) throw Error("写入未取得进展");
            offset += static_cast<std::size_t>(n);
        }
        if (permissions) {
            if (::fchmod(fd.value, static_cast<mode_t>(*permissions)) != 0) fail("设置文件权限失败");
            struct stat state{};
            if (::fstat(fd.value, &state) != 0) fail("检查文件权限失败");
            if (static_cast<std::uint32_t>(state.st_mode & 07777) != *permissions)
                throw Error("文件权限未被精确应用");
        }
        if (::fsync(fd.value) != 0) fail("同步文件失败");
        int closing = fd.value;
        fd.value = -1;
        if (::close(closing) != 0) fail("关闭文件失败");
        reject_target(parent.value, name);
        if (::renameat(parent.value, temporary.c_str(), parent.value, name.c_str()) != 0) fail("替换文件失败");
        if (::fsync(parent.value) != 0) fail("同步父目录失败");
    } catch (...) {
        ::unlinkat(parent.value, temporary.c_str(), 0);
        throw;
    }
}
}
void atomic_write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    atomic_write_impl(path, bytes, std::nullopt);
}
void atomic_write_with_permissions(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                   std::uint32_t permissions) {
    atomic_write_impl(path, bytes, permissions);
}
RepoLock::RepoLock(const std::filesystem::path& metadata_directory) {
    Fd parent(open_directory(metadata_directory));
    Fd fd(::openat(parent.value, "write.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0644));
    if (fd.value < 0) fail("打开仓库锁失败");
    require_regular(fd.value);
    while (::flock(fd.value, LOCK_EX) != 0) { if (errno != EINTR) fail("获取仓库锁失败"); }
    fd_ = fd.value;
    fd.value = -1;
}
RepoLock::~RepoLock() { if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); } }
}
