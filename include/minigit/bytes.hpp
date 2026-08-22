#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace minigit {
using Bytes = std::vector<std::uint8_t>;
// Checks UTF-8 encoding only; field-specific controls and path rules stay with
// their validators. Overlong encodings, surrogates and out-of-range values fail.
bool valid_utf8(std::string_view text);
Bytes to_bytes(std::string_view text);
std::string to_string(std::span<const std::uint8_t> bytes);
}
