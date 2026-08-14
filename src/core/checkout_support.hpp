#pragma once
#include "minigit/snapshot.hpp"
#include <set>
#include <string>
#include <string_view>
#include <vector>
// Private support for checkout-style commands. Policy stays in each business API.
namespace minigit::checkout_support {
void validate_path(std::string_view path);
struct BoundedPaths {
    std::set<std::string> paths;
    std::size_t bytes = 0;
    void add(const std::string& path);
};
// Pure lexical normalization; no worktree inspection or symlink traversal.
std::vector<std::string> normalize_selectors(const Repository& repo,
                                           const std::vector<std::filesystem::path>& paths);
bool has_descendants(const std::set<std::string>& paths, const std::string& selector);
// Checks every argument before combining duplicates and covered prefixes.
std::set<std::string> select_known_paths(const BoundedPaths& candidates,
                                       const std::vector<std::string>& selectors);
// Budget checks and deduplicated typed Blob validation of every stage, no writes.
void check_index_objects(const Repository& repo, const Index& index);
}
