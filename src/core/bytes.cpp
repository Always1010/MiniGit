#include "minigit/bytes.hpp"
namespace minigit {
Bytes to_bytes(std::string_view text) { return Bytes(text.begin(), text.end()); }
std::string to_string(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
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
