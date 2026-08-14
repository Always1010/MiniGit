#include "minigit/index.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <utility>
namespace minigit {
namespace {
bool path_less(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
        [](char x, char y) { return static_cast<unsigned char>(x) < static_cast<unsigned char>(y); });
}
bool entry_less(const IndexEntry& a, const IndexEntry& b) {
    return a.path == b.path ? a.stage < b.stage : path_less(a.path, b.path);
}
void validate_index(const Index& entries) {
    std::map<std::string, unsigned> stages;
    for (const auto& entry : entries) {
        validate_repo_path(entry.path);
        if (entry.mode != "100644" && entry.mode != "100755") throw Error("非法索引文件模式");
        if (entry.stage > 3) throw Error("非法索引冲突阶段");
        auto& mask = stages[entry.path];
        const auto bit = 1U << entry.stage;
        if ((mask & bit) != 0) throw Error("索引路径和阶段重复");
        mask |= bit;
        if ((mask & 1U) != 0 && mask != 1U) throw Error("正常索引条目不能与冲突阶段并存");
    }
    // Conflict stages may describe directory/file conflicts; resolved files may not.
    for (const auto& [path, mask] : stages) {
        for (auto pos = path.find('/'); pos != std::string::npos; pos = path.find('/', pos + 1)) {
            const auto parent = stages.find(path.substr(0, pos));
            if (parent != stages.end() && (((parent->second | mask) & 1U) != 0))
                throw Error("索引文件与目录路径冲突");
        }
    }
}
void append_u32(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
}
void validate_index_budget(const Index& index) {
    if (index.size() > 300000) throw Error("索引阶段条目超过 300000 限制");
    std::set<std::string> paths;
    std::size_t bytes = 44;
    for (const auto& entry : index) {
        validate_repo_path_limits(entry.path);
        if (paths.insert(entry.path).second && paths.size() > 100000)
            throw Error("索引不同路径超过 100000 限制");
        if (entry.path.size() > max_file_size - bytes || 41 > max_file_size - bytes - entry.path.size())
            throw Error("索引超过 64 MiB 限制");
        bytes += entry.path.size() + 41;
    }
}
Index load_index(const Repository& repo) {
    const auto path = repo.metadata() / "index";
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && status.type() == std::filesystem::file_type::not_found)) return {};
    if (error) throw Error("检查索引失败: " + error.message());
    return decode_index(read_file(path));
}
Index decode_index(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > max_file_size) throw Error("索引超过 64 MiB 限制");
    if (bytes.size() < 44) throw Error("索引被截断");
    const std::span<const std::uint8_t> body(bytes.data(), bytes.size() - 32);
    if (sha256(body) != ObjectId::from_bytes(std::span<const std::uint8_t>(bytes).last(32)))
        throw Error("索引校验和不匹配");
    std::size_t position = 0;
    auto take = [&](std::size_t size) {
        if (size > body.size() - position) throw Error("索引条目被截断");
        const auto part = body.subspan(position, size);
        position += size;
        return part;
    };
    auto u32 = [&]() {
        std::uint32_t value = 0;
        for (const auto byte : take(4)) value = (value << 8U) | byte;
        return value;
    };
    if (to_string(take(4)) != "MGIX" || u32() != 1) throw Error("未知索引格式或版本");
    const auto count = u32();
    if (count > (body.size() - position) / 42) throw Error("索引条目数量非法");
    Index entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto length = u32();
        const auto name = to_string(take(length));
        const auto mode = u32();
        if (mode != 0100644U && mode != 0100755U) throw Error("非法索引文件模式");
        const auto stage = take(1)[0];
        const auto id = ObjectId::from_bytes(take(32));
        IndexEntry entry{name, mode == 0100644U ? "100644" : "100755", id, stage};
        if (!entries.empty() && !entry_less(entries.back(), entry)) throw Error("索引条目未排序或重复");
        entries.push_back(std::move(entry));
    }
    if (position != body.size()) throw Error("索引包含多余数据");
    validate_index(entries);
    return entries;
}
Bytes encode_index(Index entries) {
    validate_index_budget(entries);
    validate_index(entries);
    std::sort(entries.begin(), entries.end(), entry_less);
    // Validate the entire size before allocating or replacing the existing index.
    std::size_t size = 44;
    for (const auto& entry : entries) {
        if (entry.path.size() > max_file_size - size || 41 > max_file_size - size - entry.path.size())
            throw Error("索引超过 64 MiB 限制");
        size += 41 + entry.path.size();
    }
    Bytes bytes = to_bytes("MGIX");
    bytes.reserve(size);
    append_u32(bytes, 1);
    append_u32(bytes, static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        append_u32(bytes, static_cast<std::uint32_t>(entry.path.size()));
        bytes.insert(bytes.end(), entry.path.begin(), entry.path.end());
        append_u32(bytes, entry.mode == "100644" ? 0100644U : 0100755U);
        bytes.push_back(entry.stage);
        bytes.insert(bytes.end(), entry.id.bytes().begin(), entry.id.bytes().end());
    }
    const auto checksum = sha256(bytes);
    bytes.insert(bytes.end(), checksum.bytes().begin(), checksum.bytes().end());
    return bytes;
}
void save_index(const Repository& repo, Index entries) {
    atomic_write(repo.metadata() / "index", encode_index(std::move(entries)));
}
}
