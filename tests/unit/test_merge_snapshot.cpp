#include <catch2/catch_test_macros.hpp>
#include "minigit/merge_snapshot.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
using namespace minigit;
namespace {
struct MergeFixture {
    test::TempDirectory temp;
    Repository repo;
    RepoLock lock;
    ObjectStore store;
    MergeFixture() : repo(Repository::init(temp.path())), lock(repo.metadata()), store(repo.metadata() / "objects") {
        save_index(repo, {});
        atomic_write(repo.metadata() / "identity", to_bytes("name=Test\nemail=a@b\n"));
        atomic_write_with_permissions(repo.root() / "sentinel", to_bytes("untouched Work"), 0640);
    }
    FileState file(std::string_view content, std::string mode = "100644") {
        return {std::move(mode), store.write(ObjectType::Blob, to_bytes(content))};
    }
    std::string content(const FileState& file) { return to_string(store.read(file.id).payload); }
};
std::map<std::string, Bytes> mutable_bytes(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata())) {
        const auto path = entry.path().lexically_relative(repo.metadata()).generic_string();
        if (!path.starts_with("objects/") && entry.is_regular_file()) result.emplace(path, read_file(entry.path()));
    }
    result.emplace("work:sentinel", read_file(repo.root() / "sentinel"));
    return result;
}
std::size_t object_count(const Repository& repo) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata() / "objects"))
        if (entry.is_regular_file()) ++count;
    return count;
}
void valid_result(const SnapshotMergeResult& result) {
    REQUIRE(decode_index(encode_index(result.index)) == result.index);
    Index work;
    for (const auto& [path, file] : result.work) work.push_back({path, file.mode, file.id});
    REQUIRE_NOTHROW(encode_index(work));
}
void originals(const SnapshotMergeResult& result, std::string_view path,
               const std::optional<FileState>& base, const std::optional<FileState>& ours,
               const std::optional<FileState>& theirs) {
    Index actual;
    for (const auto& entry : result.index) if (entry.path == path) actual.push_back(entry);
    Index expected;
    const std::array states{base, ours, theirs};
    for (unsigned side = 0; side < states.size(); ++side)
        if (states[side]) expected.push_back({std::string(path), states[side]->mode, states[side]->id, static_cast<std::uint8_t>(side + 1)});
    REQUIRE(actual == expected);
}
const MergeConflict& conflict_at(const SnapshotMergeResult& result, std::string_view path) {
    const auto found = std::find_if(result.conflicts.begin(), result.conflicts.end(), [&](const auto& conflict) { return conflict.path == path; });
    REQUIRE(found != result.conflicts.end());
    return *found;
}
std::string numbered(std::string_view prefix, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i) result += std::string(prefix) + std::to_string(i) + "\n";
    return result;
}
Snapshot many_paths(std::string_view prefix, std::size_t count, const FileState& file) {
    Snapshot result;
    for (std::size_t i = 0; i < count; ++i) {
        auto number = std::to_string(i);
        auto path = std::string(prefix) + std::string(6 - number.size(), '0') + number;
        result.emplace_hint(result.end(), std::move(path), file);
    }
    return result;
}
}
TEST_CASE("SnapshotMerge complete state choices preserve missing empty binary and unchanged long text without publication", "[merge_snapshot]") {
    MergeFixture fixture;
    const auto empty = fixture.file(""), old = fixture.file("old"), fresh = fixture.file("fresh");
    const auto binary = fixture.file(std::string("a\0b", 3), "100755");
    const auto many_lines = fixture.file(std::string(100001, '\n'));
    const Snapshot base{{"deleted", old}, {"empty", empty}, {"one", old}, {"two", old}, {"unchanged", many_lines}};
    const Snapshot ours{{"binary", binary}, {"empty", empty}, {"one", old}, {"two", fresh}, {"unchanged", many_lines}};
    const Snapshot theirs{{"binary", binary}, {"empty", empty}, {"one", fresh}, {"two", old}, {"unchanged", many_lines}};
    const auto before = mutable_bytes(fixture.repo);
    const auto count = object_count(fixture.repo);
    const auto result = merge_snapshots(fixture.repo, base, ours, theirs);
    const Snapshot expected{{"binary", binary}, {"empty", empty}, {"one", fresh}, {"two", fresh}, {"unchanged", many_lines}};
    REQUIRE(result.work == expected);
    REQUIRE(result.conflicts.empty());
    REQUIRE_FALSE(result.coarse);
    REQUIRE(result.index.size() == expected.size());
    REQUIRE(result.work.contains("empty"));
    REQUIRE_FALSE(result.work.contains("deleted"));
    valid_result(result);
    REQUIRE(object_count(fixture.repo) == count);
    REQUIRE(mutable_bytes(fixture.repo) == before);
    REQUIRE(read_file_data(fixture.repo.root() / "sentinel").permissions == 0640);
    const auto swapped = merge_snapshots(fixture.repo, base, theirs, ours);
    REQUIRE(swapped.work == expected);
    REQUIRE(swapped.index == result.index);
}
TEST_CASE("SnapshotMerge selects content and executable mode independently including clean merged text", "[merge_snapshot]") {
    MergeFixture fixture;
    const auto base_file = fixture.file("a\nb\nc");
    const FileState executable{"100755", base_file.id};
    const auto changed = fixture.file("a\nB\nc");
    SECTION("ours mode and theirs content") {
        const auto result = merge_snapshots(fixture.repo, {{"file", base_file}}, {{"file", executable}}, {{"file", changed}});
        REQUIRE((result.work.at("file") == FileState{"100755", changed.id}));
        REQUIRE(result.conflicts.empty());
        REQUIRE(result.index[0].stage == 0);
        valid_result(result);
    }
    SECTION("ours content and theirs mode") {
        const auto result = merge_snapshots(fixture.repo, {{"file", base_file}}, {{"file", changed}}, {{"file", executable}});
        REQUIRE((result.work.at("file") == FileState{"100755", changed.id}));
        REQUIRE(result.conflicts.empty());
        valid_result(result);
    }
    SECTION("independent text edits combine with executable mode and no final LF") {
        const auto o = fixture.file("A\nb\nc", "100755"), t = fixture.file("a\nB\nc");
        const auto before = mutable_bytes(fixture.repo);
        const auto result = merge_snapshots(fixture.repo, {{"file", base_file}}, {{"file", o}}, {{"file", t}});
        REQUIRE(fixture.content(result.work.at("file")) == "A\nB\nc");
        REQUIRE(result.work.at("file").mode == "100755");
        REQUIRE(result.conflicts.empty());
        REQUIRE_FALSE(result.coarse);
        REQUIRE((result.index == Index{{"file", "100755", result.work.at("file").id, 0}}));
        valid_result(result);
        REQUIRE(mutable_bytes(fixture.repo) == before);
    }
}
TEST_CASE("SnapshotMerge text conflicts retain all original modes and blobs while work contains exact markers and coarse ranges", "[merge_snapshot]") {
    MergeFixture fixture;
    FileState b, o, t;
    std::string expected;
    TextConflict range;
    bool coarse = false;
    SECTION("EOF LF removal against an added empty line") {
        b = fixture.file("prefix\na\n"); o = fixture.file("prefix\na", "100755"); t = fixture.file("prefix\na\n\n");
        const std::string block = "<<<<<<< ours\na\n=======\na\n\n>>>>>>> theirs\n";
        expected = "prefix\n" + block;
        range = {1, 1, 7, block.size()};
    }
    SECTION("coarse interiors preserve outside bytes and both sides") {
        const auto old = numbered("base-", 2001), left = numbered("ours-", 2001), right = numbered("theirs-", 2001);
        b = fixture.file("prefix\r\n" + old + "tail"); o = fixture.file("prefix\r\n" + left + "tail"); t = fixture.file("prefix\r\n" + right + "tail");
        const auto block = "<<<<<<< ours\n" + left + "=======\n" + right + ">>>>>>> theirs\n";
        expected = "prefix\r\n" + block + "tail";
        range = {1, 2001, 8, block.size()};
        coarse = true;
    }
    const auto before = mutable_bytes(fixture.repo);
    const auto result = merge_snapshots(fixture.repo, {{"file", b}}, {{"file", o}}, {{"file", t}});
    REQUIRE(fixture.content(result.work.at("file")) == expected);
    REQUIRE(result.coarse == coarse);
    originals(result, "file", b, o, t);
    const auto& conflict = conflict_at(result, "file");
    REQUIRE(conflict.kind == MergeConflictKind::Text);
    REQUIRE_FALSE(conflict.mode_conflict);
    REQUIRE((conflict.text == std::vector<TextConflict>{range}));
    REQUIRE(result.work.at("file").mode == o.mode);
    REQUIRE(result.work.at("file").id != b.id);
    REQUIRE(result.work.at("file").id != o.id);
    REQUIRE(result.work.at("file").id != t.id);
    valid_result(result);
    REQUIRE(mutable_bytes(fixture.repo) == before);
}
TEST_CASE("SnapshotMerge add add never confuses an empty new file with missing base and preserves mode conflicts", "[merge_snapshot]") {
    MergeFixture fixture;
    FileState o, t;
    std::string expected;
    bool mode_conflict = false;
    SECTION("ours empty addition") { o = fixture.file(""); t = fixture.file("theirs"); expected = "<<<<<<< ours\n=======\ntheirs\n>>>>>>> theirs\n"; }
    SECTION("theirs empty addition") { o = fixture.file("ours"); t = fixture.file(""); expected = "<<<<<<< ours\nours\n=======\n>>>>>>> theirs\n"; }
    SECTION("different text and modes carry both conflict facts") {
        o = fixture.file("ours\r\n", "100755"); t = fixture.file("theirs");
        expected = "<<<<<<< ours\nours\r\n=======\ntheirs\n>>>>>>> theirs\n";
        mode_conflict = true;
    }
    SECTION("same content and different mode is a Mode conflict without marker") {
        o = fixture.file("same", "100755"); t = FileState{"100644", o.id};
        const auto count = object_count(fixture.repo);
        const auto result = merge_snapshots(fixture.repo, {}, {{"file", o}}, {{"file", t}});
        originals(result, "file", {}, o, t);
        REQUIRE(result.work.at("file") == o);
        REQUIRE(conflict_at(result, "file").kind == MergeConflictKind::Mode);
        REQUIRE(conflict_at(result, "file").mode_conflict);
        REQUIRE(conflict_at(result, "file").text.empty());
        REQUIRE(object_count(fixture.repo) == count);
        valid_result(result);
        return;
    }
    SECTION("different additions must enforce the text input line limit") {
        o = fixture.file(std::string(100001, '\n')); t = fixture.file("other");
        const auto before = mutable_bytes(fixture.repo);
        const auto count = object_count(fixture.repo);
        try { (void)merge_snapshots(fixture.repo, {}, {{"file", o}}, {{"file", t}}); FAIL("oversized text addition accepted"); }
        catch (const Error& error) { REQUIRE(std::string(error.what()).find("100000 行限制") != std::string::npos); }
        REQUIRE(mutable_bytes(fixture.repo) == before);
        REQUIRE(object_count(fixture.repo) == count);
        return;
    }
    SECTION("whole file add add markers reject byte overflow before storing a result Blob") {
        {
            const Bytes long_line(max_file_size - 64, 'o');
            o = {"100644", fixture.store.write(ObjectType::Blob, long_line)};
        }
        t = fixture.file(std::string(100, 't'));
        const auto before = mutable_bytes(fixture.repo);
        const auto count = object_count(fixture.repo);
        try { (void)merge_snapshots(fixture.repo, {}, {{"file", o}}, {{"file", t}}); FAIL("oversized add add marker accepted"); }
        catch (const Error& error) { REQUIRE(std::string(error.what()).find("合并输出超过 64 MiB") != std::string::npos); }
        REQUIRE(mutable_bytes(fixture.repo) == before);
        REQUIRE(object_count(fixture.repo) == count);
        return;
    }
    const auto result = merge_snapshots(fixture.repo, {}, {{"file", o}}, {{"file", t}});
    originals(result, "file", {}, o, t);
    REQUIRE(fixture.content(result.work.at("file")) == expected);
    REQUIRE(result.work.at("file").mode == o.mode);
    const auto& conflict = conflict_at(result, "file");
    REQUIRE(conflict.kind == MergeConflictKind::AddAdd);
    REQUIRE(conflict.mode_conflict == mode_conflict);
    REQUIRE((conflict.text == std::vector<TextConflict>{{0, 0, 0, expected.size()}}));
    REQUIRE_FALSE(result.coarse);
    valid_result(result);
}
TEST_CASE("SnapshotMerge delete modify and binary conflicts keep original stages and surviving work without fake markers", "[merge_snapshot]") {
    MergeFixture fixture;
    const auto b = fixture.file("base"), mode_only = FileState{"100755", b.id};
    SECTION("delete against executable bit modification") {
        for (const bool ours_deletes : {false, true}) {
            const Snapshot o = ours_deletes ? Snapshot{} : Snapshot{{"file", mode_only}};
            const Snapshot t = ours_deletes ? Snapshot{{"file", mode_only}} : Snapshot{};
            const auto result = merge_snapshots(fixture.repo, {{"file", b}}, o, t);
            originals(result, "file", b, ours_deletes ? std::optional<FileState>{} : mode_only,
                      ours_deletes ? std::optional<FileState>{mode_only} : std::optional<FileState>{});
            REQUIRE(result.work.at("file") == mode_only);
            REQUIRE(conflict_at(result, "file").kind == MergeConflictKind::DeleteModify);
            REQUIRE(conflict_at(result, "file").text.empty());
            valid_result(result);
        }
    }
    SECTION("any original NUL makes genuinely diverged content binary") {
        const auto binary_base = fixture.file(std::string("b\0ase", 5));
        const auto o = fixture.file("ours", "100755"), t = fixture.file("theirs");
        const auto count = object_count(fixture.repo);
        const auto result = merge_snapshots(fixture.repo, {{"file", binary_base}}, {{"file", o}}, {{"file", t}});
        originals(result, "file", binary_base, o, t);
        REQUIRE(result.work.at("file") == o);
        REQUIRE(conflict_at(result, "file").kind == MergeConflictKind::Binary);
        REQUIRE(conflict_at(result, "file").text.empty());
        REQUIRE_FALSE(conflict_at(result, "file").mode_conflict);
        REQUIRE(object_count(fixture.repo) == count);
        valid_result(result);
    }
    SECTION("different binary additions preserve mode conflict independently") {
        const auto o = fixture.file(std::string("o\0", 2), "100755"), t = fixture.file("theirs");
        const auto result = merge_snapshots(fixture.repo, {}, {{"file", o}}, {{"file", t}});
        originals(result, "file", {}, o, t);
        REQUIRE(result.work.at("file") == o);
        REQUIRE(conflict_at(result, "file").kind == MergeConflictKind::Binary);
        REQUIRE(conflict_at(result, "file").mode_conflict);
        REQUIRE(conflict_at(result, "file").text.empty());
        valid_result(result);
    }
}
TEST_CASE("SnapshotMerge detects shortest directory file regions across nonadjacent keys and avoids false conflicts on clean replacements", "[merge_snapshot]") {
    MergeFixture fixture;
    const auto old = fixture.file("old"), changed = fixture.file("changed"), leaf = fixture.file("leaf"), other = fixture.file("other");
    Snapshot b, o, t, expected;
    bool directory_conflict = true;
    SECTION("ours directory and theirs modified file upgrades clean added leaves") {
        b = {{"p", old}}; o = {{"p/deep/x", leaf}, {"p/y", other}}; t = {{"p", changed}}; expected = o;
    }
    SECTION("ours file and theirs directory") {
        b = {{"p/x", old}}; o = {{"p", changed}}; t = {{"p/x", leaf}, {"p/y", other}}; expected = o;
    }
    SECTION("p-other intervenes lexically without hiding the p ancestor") {
        b = {{"p", old}, {"p-other", other}}; o = {{"p/x", leaf}, {"p-other", other}};
        t = {{"p", changed}, {"p-other", changed}}; expected = {{"p/x", leaf}, {"p-other", changed}};
    }
    SECTION("base file replaced by directory while theirs stays unchanged is clean") {
        b = {{"p", old}}; o = {{"p/x", leaf}}; t = b; expected = o; directory_conflict = false;
    }
    SECTION("base directory replaced by file while theirs stays unchanged is clean") {
        b = {{"p/x", old}}; o = {{"p", changed}}; t = b; expected = o; directory_conflict = false;
    }
    atomic_write(fixture.repo.root() / "p", to_bytes("unknown actual Work"));
    const auto before = mutable_bytes(fixture.repo);
    const auto count = object_count(fixture.repo);
    const auto result = merge_snapshots(fixture.repo, b, o, t);
    REQUIRE(result.work == expected);
    REQUIRE_FALSE(result.coarse);
    if (directory_conflict) {
        REQUIRE(result.conflicts.size() == 1);
        REQUIRE((result.conflicts[0] == MergeConflict{"p", MergeConflictKind::DirectoryFile, {}, false}));
        for (const auto& path : {std::string("p"), std::string("p/x"), std::string("p/deep/x"), std::string("p/y")}) {
            const auto bi = b.find(path), oi = o.find(path), ti = t.find(path);
            originals(result, path, bi == b.end() ? std::optional<FileState>{} : bi->second,
                      oi == o.end() ? std::optional<FileState>{} : oi->second,
                      ti == t.end() ? std::optional<FileState>{} : ti->second);
        }
        for (const auto& entry : result.index)
            if (entry.path == "p" || entry.path.starts_with("p/")) REQUIRE(entry.stage != 0);
    } else {
        REQUIRE(result.conflicts.empty());
        REQUIRE(result.index.size() == 1);
        REQUIRE(result.index[0].stage == 0);
    }
    valid_result(result);
    REQUIRE(object_count(fixture.repo) == count);
    REQUIRE(mutable_bytes(fixture.repo) == before);
    REQUIRE(to_string(read_file(fixture.repo.root() / "p")) == "unknown actual Work");
}
TEST_CASE("SnapshotMerge prevalidates every source schema and typed Blob before saving otherwise ready merged content", "[merge_snapshot]") {
    MergeFixture fixture;
    const auto b = fixture.file("a\nb\n"), o = fixture.file("A\nb\n"), t = fixture.file("a\nB\n");
    Snapshot base{{"a", b}}, ours{{"a", o}}, theirs{{"a", t}};
    SECTION("missing Blob in a later path") { theirs.emplace("z", FileState{"100644", ObjectId{}}); }
    SECTION("real Tree used as Blob in a later path") { theirs.emplace("z", FileState{"100644", fixture.store.write(ObjectType::Tree, encode_tree({}))}); }
    SECTION("mode error in a side that a shortcut could otherwise ignore") { theirs.emplace("z", FileState{"120000", o.id}); }
    SECTION("directory file collision inside one external input") { ours.emplace("a/child", o); }
    SECTION("unsafe metadata path") { base.emplace(".minigit/index", b); }
    SECTION("malformed UTF8 path") { theirs.emplace(std::string("\xff", 1), t); }
    SECTION("path byte limit") { theirs.emplace(std::string(4097, 'x'), t); }
    SECTION("path component limit") {
        std::string path = "x";
        for (unsigned i = 0; i < 128; ++i) path += "/x";
        theirs.emplace(path, t);
    }
    const auto before = mutable_bytes(fixture.repo);
    const auto count = object_count(fixture.repo);
    const auto base_before = base, ours_before = ours, theirs_before = theirs;
    REQUIRE_THROWS_AS(merge_snapshots(fixture.repo, base, ours, theirs), Error);
    REQUIRE(object_count(fixture.repo) == count);
    REQUIRE(mutable_bytes(fixture.repo) == before);
    REQUIRE(base == base_before);
    REQUIRE(ours == ours_before);
    REQUIRE(theirs == theirs_before);
}
TEST_CASE("SnapshotMerge bounds union and final distinct paths with shared immutable objects", "[merge_snapshot]") {
    MergeFixture fixture;
    const auto shared = fixture.file("shared");
    const auto before = mutable_bytes(fixture.repo);
    const auto count = object_count(fixture.repo);
    SECTION("three valid inputs can exceed the 200000 path union") {
        const auto base = many_paths("b", 100000, shared), ours = many_paths("o", 100000, shared);
        const Snapshot theirs{{"t", shared}};
        try { (void)merge_snapshots(fixture.repo, base, ours, theirs); FAIL("oversized union accepted"); }
        catch (const Error& error) { REQUIRE(std::string(error.what()).find("路径数量或总字节") != std::string::npos); }
    }
    SECTION("valid input counts can combine into a 100001 path result") {
        auto ours = many_paths("o", 100000, shared);
        const Snapshot theirs{{"t", shared}};
        try { (void)merge_snapshots(fixture.repo, {}, ours, theirs); FAIL("oversized final Index accepted"); }
        catch (const Error& error) { REQUIRE(std::string(error.what()).find("索引不同路径超过 100000") != std::string::npos); }
        ours.emplace("extra", shared);
        try { (void)merge_snapshots(fixture.repo, {}, ours, {}); FAIL("oversized input accepted"); }
        catch (const Error& error) { REQUIRE(std::string(error.what()).find("快照文件超过 100000") != std::string::npos); }
    }
    REQUIRE(object_count(fixture.repo) == count);
    REQUIRE(mutable_bytes(fixture.repo) == before);
}
