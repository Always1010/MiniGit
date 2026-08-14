#pragma once
#include <cstddef>
#include <string_view>
namespace minigit {
inline constexpr std::size_t max_repo_path_bytes = 4096;
inline constexpr std::size_t max_repo_path_components = 128;
// Format validity only: also permits reading legacy paths for cleanup.
void validate_repo_path(std::string_view path);
// New stored paths and worktree planning additionally obey these shared limits.
void validate_repo_path_limits(std::string_view path);
}
