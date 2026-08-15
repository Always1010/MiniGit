#pragma once
#include "minigit/bytes.hpp"
#include <filesystem>
namespace minigit {
inline constexpr std::size_t max_file_size = 64U * 1024U * 1024U;
struct FileData {
    Bytes bytes;
    std::uint32_t permissions = 0; // st_mode & 07777, including special bits.
};
// Reads one opened regular-file inode. Before/after metadata must agree; this
// does not compare-and-swap the path, which may be replaced independently.
// Ordinary files use 64 MiB. ObjectStore supplies a bounded compressed-container
// limit; its uncompressed object framing remains limited to 64 MiB.
FileData read_file_data(const std::filesystem::path& path, std::size_t size_limit = max_file_size);
Bytes read_file(const std::filesystem::path& path, std::size_t size_limit = max_file_size);
// Default creation permissions remain 0644 subject to the process umask.
void atomic_write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                  std::size_t size_limit = max_file_size);
// Exact permissions (0..07777) are applied and verified on the temporary inode.
void atomic_write_with_permissions(const std::filesystem::path& path,
                                   std::span<const std::uint8_t> bytes, std::uint32_t permissions);
// Permanent write.lock inode: never unlink this file. Lock acquisition blocks.
class RepoLock {
public:
    explicit RepoLock(const std::filesystem::path& metadata_directory);
    ~RepoLock();
    RepoLock(const RepoLock&) = delete;
    RepoLock& operator=(const RepoLock&) = delete;
    RepoLock(RepoLock&&) = delete;
    RepoLock& operator=(RepoLock&&) = delete;
private:
    int fd_ = -1;
};
namespace detail {
void reject_symlinks(const std::filesystem::path& path);
void ensure_directory(const std::filesystem::path& path);
void require_directory(const std::filesystem::path& path);
void sync_directory(const std::filesystem::path& path);
// Pinned-directory operations for transaction publication; no symlink following.
void remove_regular(const std::filesystem::path& path);
void remove_empty_directory(const std::filesystem::path& path);
// exclusive rejects even an existing directory rather than changing its mode.
void create_directory_exact(const std::filesystem::path& path, std::uint32_t permissions, bool exclusive = false);
void chmod_directory(const std::filesystem::path& path, std::uint32_t permissions);
// Source must be a prepared regular file or directory on the same device.
// Directory publication requires an absent destination; file replaces only a file.
void rename_staged(const std::filesystem::path& source, const std::filesystem::path& target);
std::uint64_t directory_device(const std::filesystem::path& path);
std::uint64_t node_device(const std::filesystem::path& path);
}
}
