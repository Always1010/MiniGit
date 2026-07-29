#pragma once
#include "minigit/objects.hpp"
#include <string>
#include <string_view>
namespace minigit {
// Quotes whitespace and escapes control characters while preserving UTF-8 bytes.
std::string quote_path(std::string_view path);
// Preserves the log display format; complete output is limited to 64 MiB.
std::string format_commit(const ObjectId& id, const Commit& commit, bool oneline = false);
}
