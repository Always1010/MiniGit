#pragma once
#include <filesystem>
namespace minigit {
class Repository {
public:
    static Repository init(const std::filesystem::path& path);
    static Repository discover(const std::filesystem::path& path = std::filesystem::current_path());
    const std::filesystem::path& root() const noexcept { return root_; }
    const std::filesystem::path& metadata() const noexcept { return metadata_; }
private:
    explicit Repository(std::filesystem::path root);
    std::filesystem::path root_;
    std::filesystem::path metadata_;
};
}
