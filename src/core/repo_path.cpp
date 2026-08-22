#include "minigit/repo_path.hpp"
#include "minigit/bytes.hpp"
#include "minigit/error.hpp"
#include <algorithm>
namespace minigit {
void validate_repo_path(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos || !valid_utf8(path)) throw Error("非法仓库相对路径");
    while (true) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (part.empty() || part == "." || part == ".." || part == ".git" || part == ".minigit")
            throw Error("非法仓库路径分量");
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
}
void validate_repo_path_limits(std::string_view path) {
    validate_repo_path(path);
    if (path.size() > max_repo_path_bytes ||
        static_cast<std::size_t>(std::count(path.begin(), path.end(), '/')) + 1 > max_repo_path_components)
        throw Error("路径超过 4096 字节或 128 分量限制");
}
}
