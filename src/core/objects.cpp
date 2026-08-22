#include "minigit/objects.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include <algorithm>
#include <charconv>
namespace minigit {
namespace {
bool name_less(std::string_view left, std::string_view right) {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
        [](char a, char b) { return static_cast<unsigned char>(a) < static_cast<unsigned char>(b); });
}
void validate_entry(const TreeEntry& entry) {
    const auto& name = entry.name;
    if (name.empty() || name == "." || name == ".." || name == ".minigit" || name == ".git" ||
        name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        name.find('\0') != std::string::npos || !valid_utf8(name)) throw Error("非法树条目名称");
    if (entry.mode != "100644" && entry.mode != "100755" && entry.mode != "40000")
        throw Error("非法树条目模式");
}
void validate_line(std::string_view value) {
    if (value.empty() || value.find_first_of("\r\n") != std::string_view::npos || value.find('\0') != std::string_view::npos)
        throw Error("作者和提交者必须是非空单行字段");
}
void ensure_size(std::size_t size) { if (size > max_file_size) throw Error("对象超过 64 MiB 限制"); }
}
std::string_view object_type_name(ObjectType type) {
    switch (type) {
    case ObjectType::Blob: return "blob";
    case ObjectType::Tree: return "tree";
    case ObjectType::Commit: return "commit";
    }
    throw Error("未知对象类型");
}
ObjectType parse_object_type(std::string_view text) {
    if (text == "blob") return ObjectType::Blob;
    if (text == "tree") return ObjectType::Tree;
    if (text == "commit") return ObjectType::Commit;
    throw Error("未知对象类型");
}
ObjectId hash_object(ObjectType type, std::span<const std::uint8_t> payload) {
    ensure_size(payload.size());
    auto framed = to_bytes(std::string(object_type_name(type)) + " " + std::to_string(payload.size()));
    framed.push_back(0);
    if (payload.size() > max_file_size - framed.size()) throw Error("对象超过 64 MiB 限制");
    framed.insert(framed.end(), payload.begin(), payload.end());
    return sha256(framed);
}
Bytes encode_tree(std::vector<TreeEntry> entries) {
    for (const auto& entry : entries) validate_entry(entry);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return name_less(a.name, b.name); });
    Bytes result;
    std::string_view previous;
    for (const auto& entry : entries) {
        if (!previous.empty() && previous == entry.name) throw Error("树条目名称重复");
        previous = entry.name;
        const auto header = to_bytes(entry.mode + " " + entry.name);
        if (header.size() + 33 > max_file_size - result.size()) throw Error("树对象超过 64 MiB 限制");
        result.insert(result.end(), header.begin(), header.end());
        result.push_back(0);
        result.insert(result.end(), entry.id.bytes().begin(), entry.id.bytes().end());
    }
    return result;
}
std::vector<TreeEntry> decode_tree(std::span<const std::uint8_t> bytes) {
    ensure_size(bytes.size());
    std::vector<TreeEntry> result;
    std::size_t position = 0;
    while (position < bytes.size()) {
        const auto start = position;
        while (position < bytes.size() && bytes[position] != 0) ++position;
        if (position == bytes.size() || bytes.size() - position - 1 < 32) throw Error("树条目被截断");
        const auto header = to_string(bytes.subspan(start, position - start));
        const auto separator = header.find(' ');
        if (separator == std::string::npos) throw Error("非法树条目头");
        TreeEntry entry{header.substr(separator + 1), header.substr(0, separator), ObjectId::from_bytes(bytes.subspan(position + 1, 32))};
        validate_entry(entry);
        if (!result.empty() && !name_less(result.back().name, entry.name)) throw Error("树条目未排序或名称重复");
        result.push_back(std::move(entry));
        position += 33;
    }
    return result;
}
Bytes encode_commit(const Commit& commit) {
    validate_line(commit.author);
    validate_line(commit.committer);
    if (commit.message.find('\0') != std::string::npos) throw Error("提交消息禁止 NUL");
    std::string text = "tree " + commit.tree.hex() + "\n";
    for (const auto& parent : commit.parents) {
        if (text.size() > max_file_size - 72) throw Error("提交对象超过 64 MiB 限制");
        text += "parent " + parent.hex() + "\n";
    }
    if (commit.author.size() > max_file_size || commit.committer.size() > max_file_size || commit.message.size() > max_file_size)
        throw Error("提交对象超过 64 MiB 限制");
    if (text.size() + commit.author.size() + commit.committer.size() + commit.message.size() + 20 > max_file_size)
        throw Error("提交对象超过 64 MiB 限制");
    text += "author " + commit.author + "\ncommitter " + commit.committer + "\n\n" + commit.message;
    return to_bytes(text);
}
Commit decode_commit(std::span<const std::uint8_t> bytes) {
    ensure_size(bytes.size());
    const auto text = to_string(bytes);
    if (text.find('\0') != std::string::npos) throw Error("提交对象禁止 NUL");
    const auto split = text.find("\n\n");
    if (split == std::string::npos) throw Error("提交对象缺少消息分隔符");
    auto header = std::string_view(text).substr(0, split);
    auto next_line = [&header]() {
        const auto end = header.find('\n');
        if (end == std::string_view::npos) {
            auto line = header;
            header = {};
            return line;
        }
        auto line = header.substr(0, end);
        header.remove_prefix(end + 1);
        return line;
    };
    auto line = next_line();
    if (!line.starts_with("tree ")) throw Error("提交对象缺少 tree 字段");
    Commit commit;
    commit.tree = ObjectId::from_hex(line.substr(5));
    line = next_line();
    while (line.starts_with("parent ")) {
        commit.parents.push_back(ObjectId::from_hex(line.substr(7)));
        line = next_line();
    }
    if (!line.starts_with("author ")) throw Error("提交对象缺少 author 字段或字段顺序非法");
    commit.author = std::string(line.substr(7));
    line = next_line();
    if (!line.starts_with("committer ") || !header.empty()) throw Error("提交对象字段缺失、重复或顺序非法");
    commit.committer = std::string(line.substr(10));
    validate_line(commit.author);
    validate_line(commit.committer);
    commit.message = text.substr(split + 2);
    return commit;
}
}
