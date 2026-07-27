#include <catch2/catch_test_macros.hpp>
#include "minigit/diff.hpp"
#include "minigit/bytes.hpp"
#include "minigit/error.hpp"
#include "minigit/objects.hpp"
#include <algorithm>
#include <utility>
using namespace minigit;
namespace {
void append_line(std::string& output, const Line& line) {
    output += line.bytes;
    if (line.terminated) output += '\n';
}
std::string apply(const std::vector<Line>& before, const std::vector<Line>& after, const EditScript& script) {
    std::string output;
    std::size_t old_cursor = 0;
    for (const auto& edit : script.edits) {
        REQUIRE(edit.old_begin >= old_cursor);
        REQUIRE(edit.old_begin + edit.old_count <= before.size());
        REQUIRE(edit.new_begin + edit.new_count <= after.size());
        while (old_cursor < edit.old_begin) append_line(output, before[old_cursor++]);
        for (std::size_t i = 0; i < edit.new_count; ++i) append_line(output, after[edit.new_begin + i]);
        old_cursor += edit.old_count;
    }
    while (old_cursor < before.size()) append_line(output, before[old_cursor++]);
    return output;
}
std::string numbered(std::string_view prefix, std::size_t count) {
    std::string output;
    for (std::size_t i = 0; i < count; ++i) output += std::string(prefix) + std::to_string(i) + "\n";
    return output;
}
std::size_t occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0, pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) { ++count; pos += needle.size(); }
    return count;
}
}
TEST_CASE("Diff scripts reconstruct exact bytes across line endings insertions deletions and repetition", "[diff]") {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"", ""}, {"", "\n"}, {"\n", ""}, {"a\n", "a"}, {"a", "a\n"},
        {"first\r\nlast\r\n", "first\r\ninsert\r\nlast\r\n"},
        {"head\nremove\nstay\ntail\n", "head\nstay\ninsert\ntail\n"},
        {"repeat\nrepeat\nx\nrepeat\n", "repeat\nx\nrepeat\nrepeat\n"},
        {"a\nb\n", "b\na\n"}, {"before", "after"}, {"中文\n", "中文\n末行"}
    };
    for (const auto& [before, after] : cases) {
        const auto old_lines = split_lines(before), new_lines = split_lines(after);
        const auto script = diff_lines(old_lines, new_lines);
        REQUIRE(apply(old_lines, new_lines, script) == after);
        REQUIRE_FALSE(script.coarse);
        REQUIRE(diff_lines(old_lines, old_lines).edits.empty());
    }
    REQUIRE(split_lines("").empty());
    REQUIRE(split_lines("\n").size() == 1);
    const auto crlf = split_lines("a\r\nlast");
    REQUIRE(crlf[0].bytes == "a\r");
    REQUIRE(crlf[0].terminated);
    REQUIRE_FALSE(crlf[1].terminated);
    const auto tie = diff_lines(split_lines("a\nb\n"), split_lines("b\na\n"));
    REQUIRE((tie.edits.front() == Edit{0, 1, 0, 0}));
}
TEST_CASE("Diff falls back within the unmatched interior and preserves shared prefix and suffix", "[diff]") {
    const auto old_interior = numbered("old-", 2001), new_interior = numbered("new-", 2001);
    SECTION("whole replacement") {
        const auto before = split_lines(old_interior), after = split_lines(new_interior);
        const auto script = diff_lines(before, after);
        REQUIRE(script.coarse);
        REQUIRE((script.edits == std::vector<Edit>{{0, 2001, 0, 2001}}));
        REQUIRE(apply(before, after, script) == new_interior);
    }
    SECTION("interior replacement") {
        const auto old_text = "shared prefix\n" + old_interior + "shared suffix\n";
        const auto new_text = "shared prefix\n" + new_interior + "shared suffix\n";
        const auto before = split_lines(old_text), after = split_lines(new_text);
        const auto script = diff_lines(before, after);
        REQUIRE(script.coarse);
        REQUIRE((script.edits == std::vector<Edit>{{1, 2001, 1, 2001}}));
        REQUIRE(apply(before, after, script) == new_text);
    }
    SECTION("large shared regions leave a small exact interior") {
        const auto shared = numbered("shared-", 2001);
        const auto old_text = shared + "old\n" + shared;
        const auto new_text = shared + "new\n" + shared;
        const auto before = split_lines(old_text), after = split_lines(new_text);
        const auto script = diff_lines(before, after);
        REQUIRE_FALSE(script.coarse);
        REQUIRE((script.edits == std::vector<Edit>{{2001, 1, 2001, 1}}));
        REQUIRE(apply(before, after, script) == new_text);
    }
    SECTION("exact cell budget includes boundary cells") {
        const auto old_text = numbered("old-", 1999), new_text = numbered("new-", 1999);
        const auto before = split_lines(old_text), after = split_lines(new_text);
        const auto script = diff_lines(before, after);
        REQUIRE_FALSE(script.coarse);
        REQUIRE(apply(before, after, script) == new_text);
    }
}
TEST_CASE("Diff hunks merge touching context windows and retain correct zero ranges", "[diff]") {
    for (const auto gap : {5U, 6U, 7U}) {
        const auto next = 3 + gap;
        const EditScript script{{{2, 1, 2, 1}, {next, 1, next, 1}}, false};
        const auto hunks = make_hunks(script, next + 5, next + 5);
        REQUIRE(hunks.size() == (gap <= 6 ? 1 : 2));
        std::size_t removals = 0, additions = 0;
        for (const auto& hunk : hunks) {
            REQUIRE(hunk.old_begin + hunk.old_count <= next + 5);
            REQUIRE(hunk.new_begin + hunk.new_count <= next + 5);
            for (const auto& line : hunk.lines) {
                if (line.kind == DiffLine::Kind::Remove) ++removals;
                if (line.kind == DiffLine::Kind::Add) ++additions;
            }
        }
        REQUIRE(removals == 2);
        REQUIRE(additions == 2);
    }
    const auto added = make_hunks({{{0, 0, 0, 2}}, false}, 0, 2, 0);
    REQUIRE(added[0].old_count == 0);
    REQUIRE(added[0].new_count == 2);
    REQUIRE(added[0].lines[0].kind == DiffLine::Kind::Add);
    const auto deleted = make_hunks({{{0, 2, 0, 0}}, false}, 2, 0, 0);
    REQUIRE(deleted[0].old_count == 2);
    REQUIRE(deleted[0].new_count == 0);
    const auto shifted = make_hunks({{{0, 0, 0, 2}, {4, 1, 6, 0}}, false}, 8, 9, 0);
    REQUIRE(shifted.size() == 2);
    REQUIRE(shifted[1].old_begin == 4);
    REQUIRE(shifted[1].new_begin == 6);
}
TEST_CASE("Diff formatting distinguishes binary empty missing and mode only files", "[diff]") {
    const Bytes empty, text = to_bytes("data\n"), binary{0, 255, 10};
    const DiffFileView empty_file{"100644", empty}, plain{"100644", text}, executable{"100755", text}, binary_file{"100644", binary};
    SECTION("empty addition and deletion retain metadata") {
        const auto added = format_file_diff("empty", std::nullopt, empty_file);
        REQUIRE(added.find("new file mode 100644\n") != std::string::npos);
        REQUIRE(added.find("index " + std::string(64, '0') + ".." + hash_object(ObjectType::Blob, empty).hex()) != std::string::npos);
        REQUIRE(added.find("--- /dev/null\n+++ b/empty\n") != std::string::npos);
        REQUIRE(added.find("@@") == std::string::npos);
        const auto deleted = format_file_diff("empty", empty_file, std::nullopt);
        REQUIRE(deleted.find("deleted file mode 100644\n") != std::string::npos);
        REQUIRE(deleted.find("--- a/empty\n+++ /dev/null\n") != std::string::npos);
        REQUIRE(format_file_diff("empty", empty_file, empty_file).empty());
        REQUIRE(format_file_diff("empty", std::nullopt, std::nullopt).empty());
    }
    SECTION("mode only") {
        const auto output = format_file_diff("file", plain, executable);
        REQUIRE(output.find("old mode 100644\nnew mode 100755\n") != std::string::npos);
        REQUIRE(output.find("--- ") == std::string::npos);
        REQUIRE(output.find("@@") == std::string::npos);
    }
    SECTION("NUL chooses binary summary") {
        const auto output = format_file_diff("binary", plain, binary_file);
        REQUIRE(output.find("Binary files a/binary and b/binary differ\n") != std::string::npos);
        REQUIRE(output.find("@@") == std::string::npos);
        REQUIRE(output.find("--- ") == std::string::npos);
        REQUIRE(format_file_diff("binary", std::nullopt, binary_file).find("Binary files /dev/null and b/binary differ") != std::string::npos);
    }
}
TEST_CASE("Diff formatting quotes complete paths and emits exact unified ranges and final newline markers", "[diff]") {
    const auto before_bytes = to_bytes("head\nold"), after_bytes = to_bytes("head\nnew");
    const DiffFileView before{"100644", before_bytes}, after{"100644", after_bytes};
    const auto output = format_file_diff("中文 file\t\n", before, after);
    REQUIRE(output.starts_with("diff --minigit \"a/中文 file\\t\\n\" \"b/中文 file\\t\\n\"\n"));
    REQUIRE(output.find("@@ -1,2 +1,2 @@\n head\n-old\n\\ No newline at end of file\n+new\n\\ No newline at end of file\n") != std::string::npos);
    REQUIRE(occurrences(output, "\\ No newline at end of file\n") == 2);
    const auto line = to_bytes("line\n");
    const DiffFileView one{"100644", line};
    REQUIRE(format_file_diff("new", std::nullopt, one, 0).find("@@ -0,0 +1,1 @@\n+line\n") != std::string::npos);
    REQUIRE(format_file_diff("gone", one, std::nullopt, 0).find("@@ -1,1 +0,0 @@\n-line\n") != std::string::npos);
    const auto old_text = to_bytes("a\nlast"), new_text = to_bytes("b\nlast");
    const auto context = format_file_diff("context", DiffFileView{"100644", old_text}, DiffFileView{"100644", new_text});
    REQUIRE(context.find(" last\n\\ No newline at end of file\n") != std::string::npos);
    REQUIRE(occurrences(context, "\\ No newline at end of file\n") == 1);
}
TEST_CASE("Diff rejects invalid line scripts paths modes and line limits", "[diff]") {
    REQUIRE_THROWS_AS(split_lines(std::string(100001, '\n')), Error);
    REQUIRE(split_lines(std::string(100000, '\n')).size() == 100000);
    const std::vector<Line> embedded_lf{{"a\nb", true}}, unterminated_middle{{"a", false}, {"b", true}}, phantom_empty{{"", false}};
    REQUIRE_THROWS_AS(diff_lines(embedded_lf, {}), Error);
    REQUIRE_THROWS_AS(diff_lines(unterminated_middle, {}), Error);
    REQUIRE_THROWS_AS(diff_lines(phantom_empty, {}), Error);
    const std::vector<Line> excessive(100001, Line{"a", true});
    REQUIRE_THROWS_AS(diff_lines(excessive, {}), Error);
    for (const auto& script : std::vector<EditScript>{
        {{{3, 1, 3, 1}}, false}, {{{0, 4, 0, 1}}, false}, {{{0, 0, 0, 0}}, false},
        {{{1, 1, 0, 1}}, false}, {{{1, 1, 1, 1}, {0, 1, 0, 1}}, false},
        {{{0, 2, 0, 1}, {1, 1, 1, 1}}, false}, {{{0, 1, 0, 2}}, false}
    }) REQUIRE_THROWS_AS(make_hunks(script, 3, 3), Error);
    REQUIRE_THROWS_AS(make_hunks({}, 1, 2), Error);
    REQUIRE_THROWS_AS(make_hunks({}, 0, 0, 100001), Error);
    const Bytes bytes;
    const DiffFileView valid{"100644", bytes}, invalid{"120000", bytes};
    for (const auto path : {"", "../file", "/file", "a/.minigit/file", "a\\b"})
        REQUIRE_THROWS_AS(format_file_diff(path, valid, valid), Error);
    REQUIRE_THROWS_AS(format_file_diff(std::string(4097, 'a'), valid, valid), Error);
    REQUIRE_THROWS_AS(format_file_diff("file", valid, invalid), Error);
    REQUIRE_THROWS_AS(format_file_diff("file", valid, valid, 100001), Error);
}
