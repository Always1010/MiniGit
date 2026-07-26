#include "minigit/fs.hpp"
#include "minigit/error.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
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
}
Bytes read_file(const std::filesystem::path& path) {
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
    return result;
}
void atomic_write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
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
