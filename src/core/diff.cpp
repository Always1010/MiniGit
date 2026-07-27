#include "minigit/diff.hpp"
#include "minigit/error.hpp"
#include "minigit/format.hpp"
#include "minigit/fs.hpp"
#include "minigit/index.hpp"
#include "minigit/objects.hpp"
#include <algorithm>
#include <limits>
#include <unordered_map>
namespace minigit {
namespace {
constexpr std::size_t max_lines = 100000;
constexpr std::size_t max_dp_cells = 4000000;
void validate_lines(std::span<const Line> lines) {
    if (lines.size() > max_lines) throw Error("文本超过 100000 行限制");
    std::size_t size = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (line.bytes.find('\n') != std::string_view::npos ||
            (!line.terminated && (i + 1 != lines.size() || line.bytes.empty())))
            throw Error("行内容或终止标志非法");
        if (line.bytes.size() > max_file_size - size) throw Error("文本超过 64 MiB 限制");
        size += line.bytes.size();
        if (line.terminated) {
            if (size == max_file_size) throw Error("文本超过 64 MiB 限制");
            ++size;
        }
    }
}
struct LineHash {
    std::size_t operator()(const Line& line) const noexcept {
        return std::hash<std::string_view>{}(line.bytes) ^ (line.terminated ? std::numeric_limits<std::size_t>::max() : 0);
    }
};
void validate_script(const EditScript& script, std::size_t old_count, std::size_t new_count, std::size_t context) {
    if (old_count > max_lines || new_count > max_lines || context > max_lines) throw Error("行数或上下文超过限制");
    std::size_t old_end = 0, new_end = 0;
    for (const auto& edit : script.edits) {
        if (edit.old_begin > old_count || edit.old_count > old_count - edit.old_begin ||
            edit.new_begin > new_count || edit.new_count > new_count - edit.new_begin ||
            (edit.old_count == 0 && edit.new_count == 0) ||
            edit.old_begin < old_end || edit.new_begin < new_end ||
            edit.old_begin - old_end != edit.new_begin - new_end)
            throw Error("编辑脚本范围、顺序或间隔非法");
        old_end = edit.old_begin + edit.old_count;
        new_end = edit.new_begin + edit.new_count;
    }
    if (old_count - old_end != new_count - new_end) throw Error("编辑脚本尾部间隔不匹配");
}
class Output {
public:
    void append(std::string_view text) {
        if (text.size() > max_file_size - value_.size()) throw Error("差异输出超过 64 MiB 限制");
        value_.append(text);
    }
    void line(std::string_view text) { append(text); append("\n"); }
    std::string finish() { return std::move(value_); }
private:
    std::string value_;
};
std::string_view text_view(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void validate_file(const std::optional<DiffFileView>& file) {
    if (!file) return;
    if (file->mode != "100644" && file->mode != "100755") throw Error("差异文件模式非法");
    if (file->content.size() > max_file_size) throw Error("差异文件超过 64 MiB 限制");
}
bool has_nul(std::span<const std::uint8_t> bytes) { return std::find(bytes.begin(), bytes.end(), 0) != bytes.end(); }
}
std::vector<Line> split_lines(std::string_view bytes) {
    if (bytes.size() > max_file_size) throw Error("文本超过 64 MiB 限制");
    std::vector<Line> result;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        if (result.size() == max_lines) throw Error("文本超过 100000 行限制");
        const auto end = bytes.find('\n', begin);
        if (end == std::string_view::npos) { result.push_back({bytes.substr(begin), false}); break; }
        result.push_back({bytes.substr(begin, end - begin), true});
        begin = end + 1;
    }
    return result;
}
EditScript diff_lines(std::span<const Line> before, std::span<const Line> after) {
    validate_lines(before);
    validate_lines(after);
    std::size_t prefix = 0;
    while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) ++prefix;
    std::size_t suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before[before.size() - suffix - 1] == after[after.size() - suffix - 1]) ++suffix;
    const auto old_lines = before.subspan(prefix, before.size() - prefix - suffix);
    const auto new_lines = after.subspan(prefix, after.size() - prefix - suffix);
    const auto n = old_lines.size(), m = new_lines.size();
    EditScript result;
    if (n == 0 || m == 0) {
        if (n != 0 || m != 0) result.edits.push_back({prefix, n, prefix, m});
        return result;
    }
    const auto rows = n + 1, columns = m + 1;
    if (rows > max_dp_cells / columns) {
        result.edits.push_back({prefix, n, prefix, m});
        result.coarse = true;
        return result;
    }
    // Intern exact bytes + LF state once, rather than comparing long lines per cell.
    std::unordered_map<Line, std::size_t, LineHash> ids;
    ids.reserve(n + m);
    auto intern = [&](std::span<const Line> lines) {
        std::vector<std::size_t> result_ids;
        result_ids.reserve(lines.size());
        for (const auto& line : lines) {
            const auto [found, inserted] = ids.emplace(line, ids.size());
            (void)inserted;
            result_ids.push_back(found->second);
        }
        return result_ids;
    };
    const auto old_ids = intern(old_lines), new_ids = intern(new_lines);
    std::vector<std::uint32_t> lengths(rows * columns, 0);
    auto cell = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return lengths[i * columns + j]; };
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t j = m; j-- > 0;)
            cell(i, j) = old_ids[i] == new_ids[j] ? 1 + cell(i + 1, j + 1) : std::max(cell(i + 1, j), cell(i, j + 1));
    std::optional<Edit> edit;
    auto flush = [&]() { if (edit) { result.edits.push_back(*edit); edit.reset(); } };
    std::size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && old_ids[i] == new_ids[j]) { flush(); ++i; ++j; continue; }
        if (!edit) edit = Edit{prefix + i, 0, prefix + j, 0};
        if (i < n && (j == m || cell(i + 1, j) >= cell(i, j + 1))) { ++edit->old_count; ++i; }
        else { ++edit->new_count; ++j; }
    }
    flush();
    return result;
}
std::vector<Hunk> make_hunks(const EditScript& script, std::size_t old_count, std::size_t new_count, std::size_t context) {
    validate_script(script, old_count, new_count, context);
    std::vector<Hunk> result;
    std::size_t first = 0;
    while (first < script.edits.size()) {
        const auto& initial = script.edits[first];
        const auto leading = std::min(context, initial.old_begin);
        Hunk hunk;
        hunk.old_begin = initial.old_begin - leading;
        hunk.new_begin = initial.new_begin - leading;
        std::size_t last = first;
        auto old_end = initial.old_begin + initial.old_count;
        auto new_end = initial.new_begin + initial.new_count;
        while (last + 1 < script.edits.size()) {
            const auto gap = script.edits[last + 1].old_begin - old_end;
            if (gap > context * 2) break;
            ++last;
            old_end = script.edits[last].old_begin + script.edits[last].old_count;
            new_end = script.edits[last].new_begin + script.edits[last].new_count;
        }
        const auto trailing = std::min(context, old_count - old_end);
        old_end += trailing;
        new_end += trailing;
        hunk.old_count = old_end - hunk.old_begin;
        hunk.new_count = new_end - hunk.new_begin;
        std::size_t cursor = hunk.old_begin;
        for (std::size_t e = first; e <= last; ++e) {
            const auto& edit = script.edits[e];
            while (cursor < edit.old_begin) hunk.lines.push_back({DiffLine::Kind::Context, cursor++});
            for (std::size_t k = 0; k < edit.old_count; ++k) hunk.lines.push_back({DiffLine::Kind::Remove, cursor++});
            for (std::size_t k = 0; k < edit.new_count; ++k) hunk.lines.push_back({DiffLine::Kind::Add, edit.new_begin + k});
        }
        while (cursor < old_end) hunk.lines.push_back({DiffLine::Kind::Context, cursor++});
        result.push_back(std::move(hunk));
        first = last + 1;
    }
    return result;
}
std::string format_file_diff(std::string_view path, std::optional<DiffFileView> before, std::optional<DiffFileView> after, std::size_t context) {
    if (path.size() > 4096) throw Error("差异路径超过 4096 字节限制");
    validate_repo_path(path);
    validate_file(before);
    validate_file(after);
    if (context > max_lines) throw Error("上下文超过 100000 行限制");
    if (!before && !after) return {};
    const auto old_bytes = before ? before->content : std::span<const std::uint8_t>{};
    const auto new_bytes = after ? after->content : std::span<const std::uint8_t>{};
    const bool equal_content = std::ranges::equal(old_bytes, new_bytes);
    if (before && after && before->mode == after->mode && equal_content) return {};
    const auto old_path = quote_path("a/" + std::string(path));
    const auto new_path = quote_path("b/" + std::string(path));
    const auto old_id = before ? hash_object(ObjectType::Blob, old_bytes).hex() : std::string(64, '0');
    const auto new_id = after ? hash_object(ObjectType::Blob, new_bytes).hex() : std::string(64, '0');
    Output output;
    output.line("diff --minigit " + old_path + " " + new_path);
    if (!before) output.line("new file mode " + std::string(after->mode));
    else if (!after) output.line("deleted file mode " + std::string(before->mode));
    else if (before->mode != after->mode) {
        output.line("old mode " + std::string(before->mode));
        output.line("new mode " + std::string(after->mode));
    }
    output.line("index " + old_id + ".." + new_id);
    if (before && after && equal_content) return output.finish();
    const auto old_label = before ? old_path : std::string("/dev/null");
    const auto new_label = after ? new_path : std::string("/dev/null");
    if (has_nul(old_bytes) || has_nul(new_bytes)) {
        output.line("Binary files " + old_label + " and " + new_label + " differ");
        return output.finish();
    }
    const auto old_lines = split_lines(text_view(old_bytes));
    const auto new_lines = split_lines(text_view(new_bytes));
    output.line("--- " + old_label);
    output.line("+++ " + new_label);
    for (const auto& hunk : make_hunks(diff_lines(old_lines, new_lines), old_lines.size(), new_lines.size(), context)) {
        const auto old_start = hunk.old_count == 0 ? hunk.old_begin : hunk.old_begin + 1;
        const auto new_start = hunk.new_count == 0 ? hunk.new_begin : hunk.new_begin + 1;
        output.line("@@ -" + std::to_string(old_start) + "," + std::to_string(hunk.old_count) + " +" + std::to_string(new_start) + "," + std::to_string(hunk.new_count) + " @@");
        for (const auto& entry : hunk.lines) {
            const auto& line = entry.kind == DiffLine::Kind::Add ? new_lines[entry.index] : old_lines[entry.index];
            const char marker = entry.kind == DiffLine::Kind::Add ? '+' : entry.kind == DiffLine::Kind::Remove ? '-' : ' ';
            output.append(std::string_view(&marker, 1));
            output.line(line.bytes);
            if (!line.terminated) output.line("\\ No newline at end of file");
        }
    }
    return output.finish();
}
}
