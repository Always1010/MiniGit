#pragma once
#include <string>
#include <string_view>
namespace minigit {
// Quotes whitespace and escapes control characters while preserving UTF-8 bytes.
std::string quote_path(std::string_view path);
}
