#pragma once
#include "minigit/bytes.hpp"
#include <filesystem>
namespace minigit {
inline constexpr std::size_t max_file_size = 64U * 1024U * 1024U;
Bytes read_file(const std::filesystem::path& path);
void atomic_write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
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
