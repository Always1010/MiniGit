#include "minigit/repository.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include "minigit/object_id.hpp"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace minigit {
namespace {
constexpr std::string_view config_text = "format=1\nhash=sha256\n";
constexpr std::string_view initial_head = "ref: refs/heads/main\n";
std::filesystem::path absolute_directory(const std::filesystem::path& path) {
    auto result = std::filesystem::absolute(path).lexically_normal();
    while (result.has_relative_path() && result.filename().empty()) result = result.parent_path();
    return result;
}
bool exists_no_follow(const std::filesystem::path& path) {
    struct stat state{};
    if (::lstat(path.c_str(), &state) == 0) return true;
    if (errno == ENOENT) return false;
    throw Error("检查仓库路径失败: " + std::string(std::strerror(errno)));
}
void validate_head(std::string_view head) {
    if (head.empty() || head.back() != '\n' || head.find('\0') != std::string_view::npos) throw Error("HEAD 格式非法");
    head.remove_suffix(1);
    if (head.starts_with("ref: ")) {
        auto ref = head.substr(5);
        if (!ref.starts_with("refs/heads/") || ref.size() == 11 || ref.back() == '/' || ref.back() == '.' ||
            ref.find("..") != std::string_view::npos || ref.find("//") != std::string_view::npos ||
            ref.find("@{") != std::string_view::npos || ref.find_first_of(" ~^:?*[\\\r\n\t") != std::string_view::npos)
            throw Error("HEAD 引用路径非法");
        std::size_t start = 0;
        while (start < ref.size()) {
            auto end = ref.find('/', start);
            if (end == std::string_view::npos) end = ref.size();
            auto component = ref.substr(start, end - start);
            if (component.empty() || component.front() == '.' || component.ends_with(".lock")) throw Error("HEAD 引用名称非法");
            start = end + 1;
        }
        for (char value : ref) if (static_cast<unsigned char>(value) < 32 || value == 127) throw Error("HEAD 含控制字符");
    } else (void)ObjectId::from_hex(head);
}
void validate_repository(const std::filesystem::path& metadata) {
    detail::require_directory(metadata);
    if (to_string(read_file(metadata / "config")) != config_text) throw Error("仓库配置损坏或格式版本不受支持");
    validate_head(to_string(read_file(metadata / "HEAD")));
    for (const auto& directory : {metadata / "objects", metadata / "refs", metadata / "refs" / "heads", metadata / "refs" / "tags"})
        detail::require_directory(directory);
    for (const auto& optional : {metadata / "index", metadata / "write.lock"}) {
        detail::reject_symlinks(optional);
        if (exists_no_follow(optional)) {
            struct stat state{};
            if (::lstat(optional.c_str(), &state) != 0 || !S_ISREG(state.st_mode)) throw Error("仓库元数据必须是普通文件");
        }
    }
}
}
Repository::Repository(std::filesystem::path root) : root_(std::move(root)), metadata_(root_ / ".minigit") {}
Repository Repository::init(const std::filesystem::path& path) {
    const auto original = std::filesystem::absolute(path);
    detail::reject_symlinks(original);
    detail::ensure_directory(original);
    auto root = absolute_directory(path);
    auto metadata = root / ".minigit";
    if (exists_no_follow(metadata)) { validate_repository(metadata); return Repository(root); }
    static std::atomic<unsigned long long> counter{0};
    std::filesystem::path temporary;
    bool made = false;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        temporary = root / (".minigit-init-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
        if (::mkdir(temporary.c_str(), 0755) == 0) { made = true; break; }
        if (errno != EEXIST) throw Error("创建仓库临时目录失败: " + std::string(std::strerror(errno)));
    }
    if (!made) throw Error("无法分配仓库临时目录");
    try {
        detail::ensure_directory(temporary / "objects");
        detail::ensure_directory(temporary / "refs" / "heads");
        detail::ensure_directory(temporary / "refs" / "tags");
        atomic_write(temporary / "config", to_bytes(config_text));
        atomic_write(temporary / "HEAD", to_bytes(initial_head));
        for (const auto& directory : {temporary / "objects", temporary / "refs" / "heads", temporary / "refs" / "tags", temporary / "refs", temporary})
            detail::sync_directory(directory);
        if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, metadata.c_str(), RENAME_NOREPLACE) != 0) {
            if (errno != EEXIST) throw Error("发布仓库失败: " + std::string(std::strerror(errno)));
            validate_repository(metadata);
            std::filesystem::remove_all(temporary);
        }
        detail::sync_directory(root);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(temporary, ignored);
        throw;
    }
    validate_repository(metadata);
    return Repository(root);
}
Repository Repository::discover(const std::filesystem::path& path) {
    const auto original = std::filesystem::absolute(path);
    detail::reject_symlinks(original);
    std::error_code ec;
    const auto status = std::filesystem::status(original, ec);
    if (ec || !std::filesystem::exists(status)) throw Error("发现仓库的起点不存在");
    auto root = absolute_directory(path);
    if (std::filesystem::is_regular_file(status)) root = root.parent_path();
    else if (!std::filesystem::is_directory(status)) throw Error("发现仓库的起点不是目录或普通文件");
    for (;;) {
        auto metadata = root / ".minigit";
        if (exists_no_follow(metadata)) { validate_repository(metadata); return Repository(root); }
        auto parent = root.parent_path();
        if (parent == root) break;
        root = std::move(parent);
    }
    throw Error("当前路径不在 MiniGit 仓库中");
}
}
