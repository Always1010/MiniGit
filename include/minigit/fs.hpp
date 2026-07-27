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
FileData read_file_data(const std::filesystem::path& path);
Bytes read_file(const std::filesystem::path& path);
// Default creation permissions remain 0644 subject to the process umask.
void atomic_write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
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
}
}
