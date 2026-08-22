#pragma once
#include "minigit/bytes.hpp"
#include "minigit/error.hpp"
namespace minigit::detail {
// Only byte mechanics are shared. Each format keeps its own version, field,
// presence-marker, count and total-size checks next to its codec.
inline void append_u32(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
class ByteReader {
public:
    ByteReader(std::span<const std::uint8_t> bytes, std::string_view truncated_error)
        : bytes_(bytes), truncated_error_(truncated_error) {}
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > remaining()) throw Error(std::string(truncated_error_));
        const auto value = bytes_.subspan(position_, count);
        position_ += count;
        return value;
    }
    std::uint8_t byte() { return take(1)[0]; }
    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (const auto byte : take(4)) value = (value << 8U) | byte;
        return value;
    }
    std::size_t remaining() const { return bytes_.size() - position_; }
private:
    std::span<const std::uint8_t> bytes_;
    std::string_view truncated_error_; // Callers supply a static format diagnostic.
    std::size_t position_ = 0;
};
}
