#pragma once
#include "minigit/bytes.hpp"
#include <cstddef>
#include <span>
#include <vector>
namespace minigit {
struct TextConflict {
    std::size_t base_begin = 0; // Base line coordinates, including zero-length insertions.
    std::size_t base_count = 0;
    std::size_t output_begin = 0; // Byte range from opening marker through closing LF.
    std::size_t output_count = 0;
    bool operator==(const TextConflict&) const = default;
};
struct TextMergeResult {
    Bytes content;
    std::vector<TextConflict> conflicts;
    bool coarse = false;
};
// Pure line-based three-way merge. Every input is checked before identity shortcuts:
// at most 64 MiB and 100000 lines, with no NUL. Other bytes and CRLF are preserved.
// Output is bounded to 64 MiB, but may have more than 100000 lines. Clean output
// preserves missing final LF; conflicts use fixed ours/theirs markers and add only
// the display LF needed before a marker. Coarse means an actual diff used fallback;
// each of the two sequential diff calls has its own 4000000-cell limit.
TextMergeResult merge_text(std::span<const std::uint8_t> base,
                           std::span<const std::uint8_t> ours,
                           std::span<const std::uint8_t> theirs);
}
