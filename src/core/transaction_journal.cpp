#include "transaction_journal.hpp"
#include "minigit/error.hpp"
#include <utility>
namespace minigit::transaction_detail {
namespace {
constexpr std::size_t max_changes = 100000;
constexpr std::size_t max_path = 4096;
void append_bytes(Bytes& output, std::span<const std::uint8_t> bytes) {
    if (bytes.size() > max_file_size - output.size()) throw Error("事务 journal 超过 64 MiB 限制");
    output.insert(output.end(), bytes.begin(), bytes.end());
}
void u32(Bytes& output, std::uint32_t value) {
    Bytes bytes;
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    append_bytes(output, bytes);
}
void byte(Bytes& output, std::uint8_t value) { append_bytes(output, std::span<const std::uint8_t>(&value, 1)); }
void string(Bytes& output, std::string_view value) {
    u32(output, static_cast<std::uint32_t>(value.size()));
    append_bytes(output, to_bytes(value));
}
void optional_id(Bytes& output, const std::optional<ObjectId>& id) {
    byte(output, id ? 1 : 0);
    if (id) append_bytes(output, id->bytes());
}
void image(Bytes& output, const NodeImage& value) {
    byte(output, static_cast<std::uint8_t>(value.kind));
    u32(output, value.permissions);
    optional_id(output, value.blob);
}
} // namespace
Bytes encode_journal(const Journal& journal) {
    Bytes output = to_bytes("MGTR");
    u32(output, 1);
    byte(output, static_cast<std::uint8_t>(journal.state));
    u32(output, static_cast<std::uint32_t>(journal.plan.work.size()));
    u32(output, static_cast<std::uint32_t>(journal.metadata.size()));
    for (const auto& change : journal.plan.work) {
        string(output, change.path);
        image(output, change.before);
        image(output, change.after);
    }
    for (const auto& change : journal.metadata) {
        string(output, change.path);
        optional_id(output, change.before);
        optional_id(output, change.after);
    }
    const auto checksum = sha256(output);
    append_bytes(output, checksum.bytes());
    return output;
}
namespace {
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > bytes_.size() - position_) throw Error("事务 journal 被截断");
        const auto result = bytes_.subspan(position_, count);
        position_ += count;
        return result;
    }
    std::uint8_t byte() { return take(1)[0]; }
    std::uint32_t u32() {
        std::uint32_t result = 0;
        for (auto value : take(4)) result = (result << 8) | value;
        return result;
    }
    std::string string() {
        const auto count = u32();
        if (count > max_path) throw Error("事务 journal 路径过长");
        return to_string(take(count));
    }
    std::optional<ObjectId> id() {
        const auto flag = byte();
        if (flag > 1) throw Error("事务对象存在标志非法");
        return flag ? std::optional(ObjectId::from_bytes(take(32))) : std::nullopt;
    }
    NodeImage image() {
        const auto kind = byte();
        if (kind > 2) throw Error("事务节点类型非法");
        const auto permissions = u32();
        return {static_cast<NodeKind>(kind), id(), permissions};
    }
    bool done() const { return position_ == bytes_.size(); }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
};
} // namespace
Journal decode_journal(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > max_file_size) throw Error("事务 journal 超过 64 MiB 限制");
    if (bytes.size() < 49) throw Error("事务 journal 被截断");
    const auto body = std::span<const std::uint8_t>(bytes).first(bytes.size() - 32);
    if (sha256(body) != ObjectId::from_bytes(std::span<const std::uint8_t>(bytes).last(32))) throw Error("事务 journal 校验和错误");
    Reader reader(body);
    if (to_string(reader.take(4)) != "MGTR" || reader.u32() != 1) throw Error("事务 journal 版本非法");
    const auto state = reader.byte();
    if (state > 1) throw Error("事务 journal 状态非法");
    Journal result;
    result.state = static_cast<State>(state);
    const auto work_count = reader.u32();
    const auto metadata_count = reader.u32();
    if (work_count > max_changes || metadata_count > max_changes - work_count) throw Error("事务 journal 改动数量非法");
    for (std::uint32_t i = 0; i < work_count; ++i) {
        auto path = reader.string();
        auto before = reader.image();
        auto after = reader.image();
        result.plan.work.push_back({std::move(path), before, after});
    }
    for (std::uint32_t i = 0; i < metadata_count; ++i) {
        auto path = reader.string();
        const auto before = reader.id();
        const auto after = reader.id();
        result.metadata.push_back({std::move(path), before, after});
    }
    if (!reader.done()) throw Error("事务 journal 包含多余数据");
    return result;
}
}
