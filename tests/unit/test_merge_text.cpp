#include <catch2/catch_test_macros.hpp>
#include "minigit/merge_text.hpp"
#include "minigit/diff.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>
using namespace minigit;
namespace {
std::span<const std::uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
TextMergeResult merge(std::string_view base, std::string_view ours, std::string_view theirs) {
    return merge_text(bytes(base), bytes(ours), bytes(theirs));
}
void clean(std::string_view base, std::string_view ours, std::string_view theirs, std::string_view expected) {
    const auto result = merge(base, ours, theirs);
    REQUIRE(to_string(result.content) == expected);
    REQUIRE(result.conflicts.empty());
    REQUIRE_FALSE(result.coarse);
    const auto swapped = merge(base, theirs, ours);
    REQUIRE(swapped.content == result.content);
    REQUIRE(swapped.conflicts.empty());
    REQUIRE_FALSE(swapped.coarse);
}
std::string marker(std::string_view ours, std::string_view theirs) {
    std::string result = "<<<<<<< ours\n";
    result += ours;
    if (!ours.empty() && ours.back() != '\n') result += '\n';
    result += "=======\n";
    result += theirs;
    if (!theirs.empty() && theirs.back() != '\n') result += '\n';
    result += ">>>>>>> theirs\n";
    return result;
}
void one_conflict(const TextMergeResult& result, std::size_t base_begin, std::size_t base_count,
                  std::string_view prefix, std::string_view ours, std::string_view theirs,
                  std::string_view suffix) {
    const auto block = marker(ours, theirs);
    REQUIRE(to_string(result.content) == std::string(prefix) + block + std::string(suffix));
    REQUIRE(result.conflicts.size() == 1);
    REQUIRE((result.conflicts[0] == TextConflict{base_begin, base_count, prefix.size(), block.size()}));
    REQUIRE(to_string(std::span(result.content).subspan(result.conflicts[0].output_begin,
                                                       result.conflicts[0].output_count)) == block);
}
std::string numbered(std::string_view prefix, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i) result += std::string(prefix) + std::to_string(i) + "\n";
    return result;
}
}
TEST_CASE("MergeText identity choices validate all inputs and preserve arbitrary bytes line endings and missing LF", "[merge_text]") {
    const std::vector<std::string> texts{"", "\n", "a", "a\n", "first\r\nlast\r", "中文\n末行",
                                          std::string("\xff\x80\r\n\x01", 5)};
    for (const auto& text : texts) {
        clean("old\n", text, text, text);
        clean("old\n", "old\n", text, text);
        clean("old\n", text, "old\n", text);
        clean(text, text, text, text);
    }
    const std::string nul("a\0b", 3);
    SECTION("equal binary inputs still reject") { REQUIRE_THROWS_AS(merge(nul, nul, nul), Error); }
    SECTION("an otherwise unused side must reject NUL") {
        REQUIRE_THROWS_AS(merge(nul, "same", "same"), Error);
        REQUIRE_THROWS_AS(merge("same", nul, "same"), Error);
        REQUIRE_THROWS_AS(merge("same", "same", nul), Error);
    }
    SECTION("identity shortcuts still enforce every input line limit") {
        const std::string too_many(100001, '\n');
        REQUIRE_THROWS_AS(merge(too_many, too_many, too_many), Error);
        REQUIRE_THROWS_AS(merge(too_many, "same", "same"), Error);
        REQUIRE_THROWS_AS(merge("same", too_many, "same"), Error);
        REQUIRE_THROWS_AS(merge("same", "same", too_many), Error);
    }
}
TEST_CASE("MergeText combines independent insertions adjacent replacements and replacement boundary points", "[merge_text]") {
    clean("a\nb\nc\n", "x\na\nb\nc\n", "a\nb\nc\ny\n", "x\na\nb\nc\ny\n");
    clean("a\nb\nc\nd\n", "a\nB\nc\nd\n", "a\nb\nC\nd\n", "a\nB\nC\nd\n");
    clean("a\nb\nc\n", "a\nB\nc\n", "a\nx\nb\nc\n", "a\nx\nB\nc\n");
    clean("a\nb\nc\n", "a\nB\nc\n", "a\nb\nx\nc\n", "a\nB\nx\nc\n");
    clean("a\nb\nc\nd\n", "a\nc\nd\n", "a\nb\nd\n", "a\nd\n");
    clean("a\nb\nc\n", "a\nc\n", "a\nx\nb\nc\n", "a\nx\nc\n");
    clean("a\nb\nlast", "x\na\nb\nlast", "a\nB\nlast", "x\na\nB\nlast");
}
TEST_CASE("MergeText deduplicates equal point insertions and reports exact marker byte ranges for different insertions", "[merge_text]") {
    SECTION("shared insertion deduplicates while an unrelated change survives") {
        clean("a\nb\nc\n", "a\nx\nb\nc\n", "a\nx\nb\nC\n", "a\nx\nb\nC\n");
    }
    SECTION("empty base is a zero line conflict") {
        const auto result = merge("", "ours", "theirs\n");
        one_conflict(result, 0, 0, "", "ours", "theirs\n", "");
        REQUIRE_FALSE(result.coarse);
    }
    SECTION("outer bytes and CRLF stay exact") {
        const std::string prefix = "前\r\n";
        const auto result = merge(prefix + "tail", prefix + "left\r\ntail", prefix + "right\r\ntail");
        one_conflict(result, 1, 0, prefix, "left\r\n", "right\r\n", "tail");
        const auto swapped = merge(prefix + "tail", prefix + "right\r\ntail", prefix + "left\r\ntail");
        one_conflict(swapped, 1, 0, prefix, "right\r\n", "left\r\n", "tail");
    }
    SECTION("two separate conflicts retain their real offsets") {
        const auto result = merge("a\nb\nc\n", "x\na\nb\ny\nc\n", "X\na\nb\nY\nc\n");
        const auto first = marker("x\n", "X\n"), second = marker("y\n", "Y\n");
        REQUIRE(to_string(result.content) == first + "a\nb\n" + second + "c\n");
        REQUIRE((result.conflicts == std::vector<TextConflict>{{0, 0, 0, first.size()},
                                                               {2, 0, first.size() + 4, second.size()}}));
    }
}
TEST_CASE("MergeText replays whole overlapping ranges including internal insertions and transitive replacements", "[merge_text]") {
    SECTION("wide replacement and internal point include unchanged base lines on the other side") {
        const auto result = merge("prefix\na\nb\nc\nsuffix\n", "prefix\nwide\nsuffix\n",
                                  "prefix\na\ninside\nb\nc\nsuffix\n");
        one_conflict(result, 1, 3, "prefix\n", "wide\n", "a\ninside\nb\nc\n", "suffix\n");
    }
    SECTION("a spanning middle edit connects two otherwise separate edits") {
        const auto result = merge("prefix\na\nb\nc\nd\ne\nf\nsuffix\n",
                                  "prefix\nleft\nd\nright\nf\nsuffix\n",
                                  "prefix\na\nmiddle\nf\nsuffix\n");
        one_conflict(result, 1, 5, "prefix\n", "left\nd\nright\n", "a\nmiddle\n", "f\nsuffix\n");
    }
    SECTION("equal complete replay deduplicates independently of another conflicting range") {
        const auto result = merge("prefix\na\nb\nsuffix\nend\n",
                                  "prefix\nshared\nsuffix\nours-tail\n",
                                  "prefix\nshared\nsuffix\ntheirs-tail\n");
        one_conflict(result, 4, 1, "prefix\nshared\nsuffix\n", "ours-tail\n", "theirs-tail\n", "");
    }
    SECTION("deletion against modification replays the surviving whole range") {
        const auto result = merge("prefix\na\nb\nsuffix\n", "prefix\nsuffix\n", "prefix\na\nB\nsuffix\n");
        one_conflict(result, 1, 2, "prefix\n", "", "a\nB\n", "suffix\n");
    }
}
TEST_CASE("MergeText groups EOF LF removal with appended lines including empty lines and separates marker lines", "[merge_text]") {
    for (const auto& addition : {std::string("b\n"), std::string("\n"), std::string("b")}) {
        const auto result = merge("prefix\na\n", "prefix\na", "prefix\na\n" + addition);
        one_conflict(result, 1, 1, "prefix\n", "a", "a\n" + addition, "");
        const auto swapped = merge("prefix\na\n", "prefix\na\n" + addition, "prefix\na");
        one_conflict(swapped, 1, 1, "prefix\n", "a\n" + addition, "a", "");
    }
    SECTION("one-sided missing LF is preserved with an independent earlier edit") {
        clean("head\na\n", "head\na", "HEAD\na\n", "HEAD\na");
        clean("head\na", "head\nA", "HEAD\na", "HEAD\nA");
    }
    SECTION("two differing no-LF tails are displayed on separate marker lines") {
        const auto result = merge("prefix\nbase", "prefix\nours", "prefix\ntheirs");
        one_conflict(result, 1, 1, "prefix\n", "ours", "theirs", "");
    }
    SECTION("EOF handling uses the complete replay after an internal overlapping edit") {
        const auto result = merge("prefix\na\nb\nc\n", "prefix\nwide", "prefix\na\nB\nc\n\n");
        one_conflict(result, 1, 3, "prefix\n", "wide", "a\nB\nc\n\n", "");
    }
    SECTION("terminated EOF replacement and an EOF point combine cleanly") {
        clean("a\n", "A\n", "a\nb\n", "A\nb\n");
    }
}
TEST_CASE("MergeText enforces input byte and line bounds and incremental output size without an output line cap", "[merge_text]") {
    SECTION("100000 input lines are accepted") {
        const std::string maximum(100000, '\n');
        const auto result = merge(maximum, maximum, maximum);
        REQUIRE(to_string(result.content) == maximum);
        REQUIRE(result.conflicts.empty());
        REQUIRE_FALSE(result.coarse);
    }
    SECTION("independent clean insertions can produce more than 100000 output lines") {
        std::string base;
        base.reserve(199998);
        for (std::size_t i = 0; i < 99999; ++i) base += "b\n";
        const auto result = merge(base, "first\n" + base, base + "last\n");
        REQUIRE(to_string(result.content) == "first\n" + base + "last\n");
        REQUIRE(result.conflicts.empty());
        REQUIRE_FALSE(result.coarse);
        REQUIRE_THROWS_AS(split_lines(to_string(result.content)), Error);
    }
    SECTION("64 MiB is accepted and one extra input byte rejects even identity") {
        std::string text(max_file_size, 'x');
        {
            const auto result = merge(text, text, text);
            REQUIRE(result.content.size() == max_file_size);
            REQUIRE(result.content.front() == 'x');
            REQUIRE(result.content.back() == 'x');
            REQUIRE(result.conflicts.empty());
        }
        text += 'x';
        REQUIRE_THROWS_AS(merge(text, text, text), Error);
        REQUIRE_THROWS_AS(merge(text, "same", "same"), Error);
        REQUIRE_THROWS_AS(merge("same", text, "same"), Error);
        REQUIRE_THROWS_AS(merge("same", "same", text), Error);
    }
    SECTION("a few long conflicting lines overflow only when markers are appended") {
        const std::string ours = "head\n" + std::string(max_file_size / 2, 'o') + "\ntail\n";
        const std::string theirs = "head\n" + std::string(max_file_size / 2, 't') + "\ntail\n";
        REQUIRE_THROWS_AS(merge("head\nbase\ntail\n", ours, theirs), Error);
    }
}
TEST_CASE("MergeText coarse diffs preserve outer common bytes and both changed interiors in conflict", "[merge_text]") {
    const auto base_middle = numbered("base-", 2001), ours_middle = numbered("ours-", 2001), theirs_middle = numbered("theirs-", 2001);
    const std::string prefix = "common\r\n", suffix = "last-without-LF";
    SECTION("both diffs exceed their individual cell budget") {
        const auto result = merge(prefix + base_middle + suffix, prefix + ours_middle + suffix, prefix + theirs_middle + suffix);
        REQUIRE(result.coarse);
        one_conflict(result, 1, 2001, prefix, ours_middle, theirs_middle, suffix);
    }
    SECTION("one fallback still replays the narrow other-side change across the whole range") {
        const auto first_end = base_middle.find('\n') + 1;
        const auto theirs = "small-change\n" + base_middle.substr(first_end);
        const auto result = merge(prefix + base_middle + suffix, prefix + ours_middle + suffix, prefix + theirs + suffix);
        REQUIRE(result.coarse);
        one_conflict(result, 1, 2001, prefix, ours_middle, theirs, suffix);
    }
    SECTION("identity avoids fallback entirely") {
        const auto result = merge(base_middle, ours_middle, ours_middle);
        REQUIRE(result.content == to_bytes(ours_middle));
        REQUIRE(result.conflicts.empty());
        REQUIRE_FALSE(result.coarse);
    }
}
