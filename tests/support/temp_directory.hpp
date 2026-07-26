#pragma once
#include <filesystem>
#include <stdexcept>
#include <cstdlib>
#include <vector>
namespace minigit::test {
class TempDirectory {
public:
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "minigit-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        if (!created) throw std::runtime_error("无法创建测试临时目录");
        path_ = created;
    }
    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};
}
