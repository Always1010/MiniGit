#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace minigit {
using Bytes = std::vector<std::uint8_t>;
Bytes to_bytes(std::string_view text);
std::string to_string(std::span<const std::uint8_t> bytes);
}
