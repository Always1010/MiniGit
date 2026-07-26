#include "minigit/bytes.hpp"
namespace minigit {
Bytes to_bytes(std::string_view text) { return Bytes(text.begin(), text.end()); }
std::string to_string(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
}
