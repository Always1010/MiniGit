#include "minigit/merge_text.hpp"
#include "minigit/diff.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
namespace minigit {
namespace {
std::string_view text_view(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void validate_input(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > max_file_size) throw Error("合并文本超过 64 MiB 限制");
    if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end()) throw Error("合并文本包含 NUL");
}
bool equal_bytes(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end());
}
struct SideEdit {
    Edit edit;
    unsigned side;
};
bool before(const Edit& left, const Edit& right) {
    if (left.old_begin != right.old_begin) return left.old_begin < right.old_begin;
    return left.old_count == 0 && right.old_count != 0;
}
std::vector<SideEdit> order_edits(const std::array<EditScript, 2>& scripts) {
    std::vector<SideEdit> result;
    result.reserve(scripts[0].edits.size() + scripts[1].edits.size());
    std::array<std::size_t, 2> next{};
    while (next[0] != scripts[0].edits.size() || next[1] != scripts[1].edits.size()) {
        const unsigned side = next[0] == scripts[0].edits.size() ? 1 :
            next[1] == scripts[1].edits.size() ? 0 :
            before(scripts[1].edits[next[1]], scripts[0].edits[next[0]]) ? 1 : 0;
        result.push_back({scripts[side].edits[next[side]++], side});
    }
    return result;
}
std::vector<Line> replay(std::span<const Line> base, std::span<const Line> branch,
                         std::span<const SideEdit> edits, unsigned side,
                         std::size_t lo, std::size_t hi) {
    std::vector<Line> result;
    std::size_t cursor = lo;
    for (const auto& tagged : edits) {
        if (tagged.side != side) continue;
        const auto& edit = tagged.edit;
        result.insert(result.end(), base.begin() + cursor, base.begin() + edit.old_begin);
        result.insert(result.end(), branch.begin() + edit.new_begin,
                      branch.begin() + edit.new_begin + edit.new_count);
        cursor = edit.old_begin + edit.old_count;
    }
    result.insert(result.end(), base.begin() + cursor, base.begin() + hi);
    return result;
}
class Output {
public:
    explicit Output(Bytes& bytes) : bytes_(bytes) {}
    void append(std::string_view text) {
        if (text.size() > max_file_size - bytes_.size()) throw Error("合并输出超过 64 MiB 限制");
        if (!text.empty()) {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(text.data());
            bytes_.insert(bytes_.end(), begin, begin + text.size());
        }
    }
    void clean(std::span<const Line> lines) {
        for (const auto& line : lines) {
            // A no-LF line can only end the complete output. The EOF grouping
            // below handles the interacting insertion before anything is emitted.
            if (unterminated_) throw Error("合并结果在末行之前缺少 LF");
            append(line.bytes);
            if (line.terminated) append("\n");
            unterminated_ = !line.terminated;
        }
    }
    void conflict(std::span<const Line> ours, std::span<const Line> theirs) {
        if (unterminated_) throw Error("合并冲突标记之前缺少 LF");
        append("<<<<<<< ours\n");
        display(ours);
        append("=======\n");
        display(theirs);
        append(">>>>>>> theirs\n");
    }
private:
    void display(std::span<const Line> lines) {
        for (const auto& line : lines) {
            append(line.bytes);
            if (line.terminated) append("\n");
        }
        if (!lines.empty() && !lines.back().terminated) append("\n");
    }
    Bytes& bytes_;
    bool unterminated_ = false;
};
}
TextMergeResult merge_text(std::span<const std::uint8_t> base,
                           std::span<const std::uint8_t> ours,
                           std::span<const std::uint8_t> theirs) {
    for (const auto input : {base, ours, theirs}) validate_input(input);
    const auto base_lines = split_lines(text_view(base));
    const std::array branches{split_lines(text_view(ours)), split_lines(text_view(theirs))};
    if (equal_bytes(ours, theirs)) return {Bytes(ours.begin(), ours.end()), {}, false};
    if (equal_bytes(base, ours)) return {Bytes(theirs.begin(), theirs.end()), {}, false};
    if (equal_bytes(base, theirs)) return {Bytes(ours.begin(), ours.end()), {}, false};
    // Each call destroys its DP matrix before the next one starts.
    std::array<EditScript, 2> scripts;
    scripts[0] = diff_lines(base_lines, branches[0]);
    scripts[1] = diff_lines(base_lines, branches[1]);
    const auto edits = order_edits(scripts);
    TextMergeResult result;
    result.coarse = scripts[0].coarse || scripts[1].coarse;
    Output output(result.content);
    std::size_t cursor = 0, begin = 0;
    while (begin < edits.size()) {
        const auto lo = edits[begin].edit.old_begin;
        auto hi = lo + edits[begin].edit.old_count;
        auto end = begin + 1;
        if (lo == hi) {
            while (end < edits.size() && edits[end].edit.old_begin == lo && edits[end].edit.old_count == 0) ++end;
        } else {
            // Strict intersection admits internal points and a transitive chain
            // of replacements; adjacent ranges and either boundary point stay out.
            while (end < edits.size() && edits[end].edit.old_begin < hi) {
                hi = std::max(hi, edits[end].edit.old_begin + edits[end].edit.old_count);
                ++end;
            }
        }
        auto members = std::span(edits).subspan(begin, end - begin);
        std::array replayed{replay(base_lines, branches[0], members, 0, lo, hi),
                            replay(base_lines, branches[1], members, 1, lo, hi)};
        if (lo != hi && hi == base_lines.size()) {
            std::array<bool, 2> eof_insert{};
            auto eof_end = end;
            while (eof_end < edits.size() && edits[eof_end].edit.old_begin == hi && edits[eof_end].edit.old_count == 0) {
                eof_insert[edits[eof_end].side] = edits[eof_end].edit.new_count != 0;
                ++eof_end;
            }
            const auto no_lf = [&](unsigned side) { return !replayed[side].empty() && !replayed[side].back().terminated; };
            if ((no_lf(0) && eof_insert[1]) || (no_lf(1) && eof_insert[0])) {
                // A new empty LF line is still an insertion. Absorbing it before
                // emission prevents both glued lines and cancellation of changes.
                end = eof_end;
                members = std::span(edits).subspan(begin, end - begin);
                replayed[0] = replay(base_lines, branches[0], members, 0, lo, hi);
                replayed[1] = replay(base_lines, branches[1], members, 1, lo, hi);
            }
        }
        output.clean(std::span(base_lines).subspan(cursor, lo - cursor));
        std::array<bool, 2> changed{};
        for (const auto& edit : members) changed[edit.side] = true;
        if (!changed[1] || replayed[0] == replayed[1]) output.clean(replayed[0]);
        else if (!changed[0]) output.clean(replayed[1]);
        else {
            const auto offset = result.content.size();
            output.conflict(replayed[0], replayed[1]);
            result.conflicts.push_back({lo, hi - lo, offset, result.content.size() - offset});
        }
        cursor = hi;
        begin = end;
    }
    output.clean(std::span(base_lines).subspan(cursor));
    return result;
}
}
