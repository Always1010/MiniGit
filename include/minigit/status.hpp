#pragma once
#include "minigit/refs.hpp"
#include <string>
#include <string_view>
#include <vector>
namespace minigit {
struct StatusEntry {
    std::string path;
    char staged = ' ';
    char unstaged = ' ';
    bool operator==(const StatusEntry&) const = default;
};
struct StatusReport {
    Head head;
    std::vector<StatusEntry> entries;
};
// Acquires one RepoLock for coherent metadata reads; hashing never writes objects.
StatusReport collect_status(const Repository& repo);
// Quotes whitespace and escapes control characters while preserving UTF-8 bytes.
std::string quote_path(std::string_view path);
}
