#include "minigit/repo_path.hpp"
#include "minigit/error.hpp"
#include <algorithm>
#include <cstdint>
namespace minigit {
namespace {
bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80U) continue;
        unsigned count;
        std::uint32_t cp, minimum;
        if (lead >= 0xc2U && lead <= 0xdfU) { count = 1; cp = lead & 0x1fU; minimum = 0x80U; }
        else if (lead >= 0xe0U && lead <= 0xefU) { count = 2; cp = lead & 0x0fU; minimum = 0x800U; }
        else if (lead >= 0xf0U && lead <= 0xf4U) { count = 3; cp = lead & 7U; minimum = 0x10000U; }
        else return false;
        if (count > text.size() - i) return false;
        for (unsigned j = 0; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0U) != 0x80U) return false;
            cp = (cp << 6U) | (next & 0x3fU);
        }
        if (cp < minimum || cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU)) return false;
    }
    return true;
}
}
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
