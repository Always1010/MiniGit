#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace minigit {
struct Line {
    std::string_view bytes; // Excludes LF; views borrow the input buffer.
    bool terminated = false;
    bool operator==(const Line&) const = default;
};
struct Edit {
    std::size_t old_begin = 0;
    std::size_t old_count = 0;
    std::size_t new_begin = 0;
    std::size_t new_count = 0;
    bool operator==(const Edit&) const = default;
};
struct EditScript {
    std::vector<Edit> edits;
    bool coarse = false;
};
// At most 100000 lines per side. LF belongs to the terminated flag; CR remains.
std::vector<Line> split_lines(std::string_view bytes);
// Removes shared prefix/suffix; exact line IDs bound comparison cost in the DP.
// At most 4000000 DP cells including boundaries; larger interiors are replaced.
// LCS ties prefer removal. Script coordinates always refer to the full inputs.
EditScript diff_lines(std::span<const Line> before, std::span<const Line> after);
struct DiffLine {
    enum class Kind { Context, Remove, Add };
    Kind kind;
    std::size_t index; // Context/Remove index the old input; Add indexes the new.
    bool operator==(const DiffLine&) const = default;
};
struct Hunk {
    std::size_t old_begin = 0;
    std::size_t old_count = 0;
    std::size_t new_begin = 0;
    std::size_t new_count = 0;
    std::vector<DiffLine> lines;
};
// Validates ranges, ordering and equal-length unchanged gaps; no input bytes are
// available here to validate their contents. Context is at most 100000.
std::vector<Hunk> make_hunks(const EditScript& script, std::size_t old_lines_count,
                             std::size_t new_lines_count, std::size_t context = 3);
struct DiffFileView {
    std::string_view mode;
    std::span<const std::uint8_t> content;
};
// Pure formatting: hashes content without reading or writing repository objects.
// Inputs and complete output are each limited to 64 MiB. Missing and empty differ.
// Repository-relative paths are limited to 4096 bytes before quoting.
std::string format_file_diff(std::string_view path, std::optional<DiffFileView> before,
                              std::optional<DiffFileView> after, std::size_t context = 3);
}
