#pragma once
#include "minigit/bytes.hpp"
#include <array>
#include <compare>
namespace minigit {
class ObjectId {
public:
    ObjectId() = default;
    static ObjectId from_hex(std::string_view text);
    static ObjectId from_bytes(std::span<const std::uint8_t> bytes);
    std::string hex() const;
    const std::array<std::uint8_t, 32>& bytes() const noexcept { return bytes_; }
    auto operator<=>(const ObjectId&) const = default;
private:
    std::array<std::uint8_t, 32> bytes_{};
};
ObjectId sha256(std::span<const std::uint8_t> bytes);
}
