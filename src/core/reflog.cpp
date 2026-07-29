#include "minigit/reflog.hpp"
#include "minigit/error.hpp"
#include "minigit/snapshot.hpp"
#include <algorithm>
#include <utility>
namespace minigit {
namespace {
constexpr std::size_t max_entries = 100000;
constexpr std::size_t max_log_size = 8U * 1024U * 1024U;
void validate_reason(std::string_view text) {
    if (text.empty() || text.size() > 4096) throw Error("reflog 原因为空或超过 4096 字节");
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 32 || lead == 127) throw Error("reflog 原因禁止控制字符");
        if (lead < 128) continue;
        unsigned count;
        std::uint32_t value, minimum;
        if (lead >= 0xc2U && lead <= 0xdfU) { count = 1; value = lead & 0x1fU; minimum = 0x80U; }
        else if (lead >= 0xe0U && lead <= 0xefU) { count = 2; value = lead & 0x0fU; minimum = 0x800U; }
        else if (lead >= 0xf0U && lead <= 0xf4U) { count = 3; value = lead & 7U; minimum = 0x10000U; }
        else throw Error("reflog 原因不是有效 UTF-8");
        if (count > text.size() - i) throw Error("reflog 原因 UTF-8 被截断");
        for (unsigned j = 0; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0U) != 0x80U) throw Error("reflog 原因不是有效 UTF-8");
            value = (value << 6U) | (next & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU))
            throw Error("reflog 原因不是有效 UTF-8");
    }
}
void u32(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void optional_id(Bytes& bytes, const std::optional<ObjectId>& id) {
    bytes.push_back(id ? 1 : 0);
    if (id) bytes.insert(bytes.end(), id->bytes().begin(), id->bytes().end());
}
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}
    std::span<const std::uint8_t> take(std::size_t size) {
        if (size > data_.size() - position_) throw Error("reflog 被截断");
        const auto part = data_.subspan(position_, size);
        position_ += size;
        return part;
    }
    std::uint32_t number() {
        std::uint32_t value = 0;
        for (const auto byte : take(4)) value = (value << 8U) | byte;
        return value;
    }
    std::optional<ObjectId> id() {
        const auto marker = take(1)[0];
        if (marker > 1) throw Error("reflog 对象存在标志非法");
        return marker ? std::optional(ObjectId::from_bytes(take(32))) : std::nullopt;
    }
    std::size_t remaining() const { return data_.size() - position_; }
private:
    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
};
void validate_branch(std::string_view ref) {
    validate_ref_name(ref);
    if (!ref.starts_with("refs/heads/")) throw Error("reflog 仅支持 HEAD 和分支引用");
}
std::string log_path(std::string_view ref) {
    if (ref != "HEAD") validate_branch(ref);
    const auto path = "logs/" + std::string(ref);
    if (path.size() > 4096 || std::count(path.begin(), path.end(), '/') + 1 > 128)
        throw Error("reflog 路径超过事务限制");
    return path;
}
std::optional<Bytes> read_metadata(const Repository& repo, std::string_view path) {
    const auto target = repo.metadata() / std::string(path);
    detail::reject_symlinks(target);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) return std::nullopt;
    if (ec) throw Error("检查 reflog 元数据失败: " + ec.message());
    return read_file(target);
}
MetadataChange* find_change(TransactionPlan& plan, std::string_view path) {
    MetadataChange* found = nullptr;
    for (auto& change : plan.metadata) if (change.path == path) {
        if (found) throw Error("reflog 计划存在重复元数据路径");
        found = &change;
    }
    return found;
}
std::optional<ObjectId> ref_id(const std::optional<Bytes>& bytes) {
    if (!bytes) return std::nullopt;
    const auto text = to_string(*bytes);
    if (text.size() != 65 || text.back() != '\n') throw Error("分支引用格式非法");
    return ObjectId::from_hex(std::string_view(text).substr(0, 64));
}
void append(const Repository& repo, TransactionPlan& plan, std::string_view ref, const ReflogEntry& entry) {
    const auto path = log_path(ref);
    validate_reason(entry.reason);
    const auto existing = find_change(plan, path);
    const auto bytes = existing ? existing->after : read_metadata(repo, path);
    auto entries = bytes ? decode_reflog(*bytes) : Reflog{};
    if (!entries.empty() && entries.back().new_id != entry.old_id) throw Error("reflog 历史与当前移动不连续");
    entries.push_back(entry);
    auto after = encode_reflog(entries);
    if (existing) existing->after = std::move(after);
    else plan.metadata.push_back({path, bytes, std::move(after)});
}
void change_metadata(const Repository& repo, TransactionPlan& plan, std::string_view path, std::optional<Bytes> after) {
    if (auto existing = find_change(plan, path)) existing->after = std::move(after);
    else plan.metadata.push_back({std::string(path), read_metadata(repo, path), std::move(after)});
}
}
Bytes encode_reflog(const Reflog& entries) {
    if (entries.size() > max_entries) throw Error("reflog 条目超过限制");
    std::size_t size = 44;
    for (const auto& entry : entries) {
        validate_reason(entry.reason);
        const auto length = 6 + (entry.old_id ? 32U : 0U) + (entry.new_id ? 32U : 0U) + entry.reason.size();
        if (length > max_log_size - size) throw Error("reflog 超过 8 MiB 限制");
        size += length;
    }
    auto bytes = to_bytes("MGRL");
    bytes.reserve(size);
    u32(bytes, 1);
    u32(bytes, static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        optional_id(bytes, entry.old_id);
        optional_id(bytes, entry.new_id);
        u32(bytes, static_cast<std::uint32_t>(entry.reason.size()));
        bytes.insert(bytes.end(), entry.reason.begin(), entry.reason.end());
    }
    const auto checksum = sha256(bytes);
    bytes.insert(bytes.end(), checksum.bytes().begin(), checksum.bytes().end());
    return bytes;
}
Reflog decode_reflog(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 44 || bytes.size() > max_log_size) throw Error("reflog 大小非法");
    const auto body = bytes.first(bytes.size() - 32);
    if (sha256(body) != ObjectId::from_bytes(bytes.last(32))) throw Error("reflog 校验和不匹配");
    Reader reader(body);
    if (to_string(reader.take(4)) != "MGRL" || reader.number() != 1) throw Error("未知 reflog 格式或版本");
    const auto count = reader.number();
    if (count > max_entries || count > reader.remaining() / 7) throw Error("reflog 条目数量非法");
    Reflog entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto old_id = reader.id();
        auto new_id = reader.id();
        const auto length = reader.number();
        if (length > 4096) throw Error("reflog 原因超过 4096 字节");
        auto reason = to_string(reader.take(length));
        validate_reason(reason);
        entries.push_back({old_id, new_id, std::move(reason)});
    }
    if (reader.remaining() != 0) throw Error("reflog 含多余数据");
    return entries;
}
Reflog read_reflog(const Repository& repo, std::string_view ref) {
    const auto bytes = read_metadata(repo, log_path(ref));
    return bytes ? decode_reflog(*bytes) : Reflog{};
}
void plan_append_reflog(const Repository& repo, TransactionPlan& plan, std::string_view ref, const ReflogEntry& entry) {
    auto next = plan;
    append(repo, next, ref, entry);
    plan = std::move(next);
}
void plan_move_head(const Repository& repo, TransactionPlan& plan, const Head& expected, const Head& desired, std::string_view reason) {
    validate_reason(reason);
    if (read_head(repo) != expected) throw Error("HEAD 已变化，移动规划被拒绝");
    if (desired.symbolic) (void)log_path(*desired.symbolic);
    else if (!desired.id) throw Error("detached HEAD 必须包含提交编号");
    if (desired.id) (void)read_snapshot(repo, read_commit(repo, *desired.id).tree);
    auto next = plan;
    if (const auto head_change = find_change(next, "HEAD"); head_change && head_change->after != read_metadata(repo, "HEAD"))
        throw Error("HEAD 已被计划中的其他操作修改");
    if (desired.symbolic && desired.symbolic == expected.symbolic) {
        const auto existing = find_change(next, *desired.symbolic);
        const auto bytes = existing ? existing->after : read_metadata(repo, *desired.symbolic);
        if (ref_id(bytes) != expected.id) throw Error("当前分支已被计划中的其他操作修改");
    }
    if (desired.symbolic != expected.symbolic && desired.symbolic) {
        const auto existing = find_change(next, *desired.symbolic);
        const auto bytes = existing ? existing->after : read_metadata(repo, *desired.symbolic);
        if (ref_id(bytes) != desired.id) throw Error("目标分支与 desired HEAD 不一致");
    }
    if (desired == expected) return;
    if (desired.symbolic && desired.symbolic == expected.symbolic) {
        change_metadata(repo, next, *desired.symbolic,
                        desired.id ? std::optional(to_bytes(desired.id->hex() + "\n")) : std::nullopt);
        append(repo, next, *desired.symbolic, {expected.id, desired.id, std::string(reason)});
    }
    if (desired.symbolic != expected.symbolic || !desired.symbolic) {
        const auto bytes = desired.symbolic ? to_bytes("ref: " + *desired.symbolic + "\n") : to_bytes(desired.id->hex() + "\n");
        change_metadata(repo, next, "HEAD", bytes);
    }
    append(repo, next, "HEAD", {expected.id, desired.id, std::string(reason)});
    plan = std::move(next);
}
}
