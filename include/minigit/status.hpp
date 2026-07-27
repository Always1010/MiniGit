#pragma once
#include "minigit/refs.hpp"
#include <string>
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
}
