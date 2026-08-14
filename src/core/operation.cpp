#include "minigit/operation.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/stash_stack.hpp"
#include "checkout_support.hpp"
#include <algorithm>
#include <map>
#include <utility>
namespace minigit {
namespace {
constexpr std::size_t max_work = 100000;
constexpr std::size_t max_raw_head = 4102;
constexpr std::size_t max_queue = 100000;
bool known_kind(OperationKind kind) {
    return kind == OperationKind::Merge || kind == OperationKind::CherryPick || kind == OperationKind::Revert ||
        kind == OperationKind::StashApply || kind == OperationKind::Rebase;
}
bool known_phase(OperationKind kind, OperationPhase phase) {
    return phase == OperationPhase::Conflicted || (kind == OperationKind::Rebase && phase == OperationPhase::Ready);
}
bool uses_v2(OperationKind kind) { return kind == OperationKind::StashApply || kind == OperationKind::Rebase; }
ObjectId reference_id(const Bytes& bytes) {
    if (bytes.size() != 65 || bytes.back() != '\n') throw Error("operation 引用格式非法");
    return ObjectId::from_hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), 64));
}
void add_size(std::size_t& size, std::size_t count) {
    if (count > max_file_size - size) throw Error("operation 数据超过 64 MiB 限制");
    size += count;
}
void validate_head(const Head& head) {
    if (!head.id) throw Error("operation HEAD 必须有提交");
    if (!head.symbolic) return;
    const auto& ref = *head.symbolic;
    if (!ref.starts_with("refs/heads/") || ref.size() > 4091 ||
        std::count(ref.begin(), ref.end(), '/') + 2 > 128)
        throw Error("operation HEAD 必须是有界分支引用");
    validate_ref_name(ref);
}
void validate_image(const NodeImage& image) {
    if (image.permissions > 07777) throw Error("operation 节点权限非法");
    switch (image.kind) {
    case NodeKind::Absent:
        if (image.blob || image.permissions) throw Error("operation 缺失节点不能带对象或权限");
        break;
    case NodeKind::File:
        if (!image.blob || !(image.permissions & 0400)) throw Error("operation 文件必须带 Blob 且所有者可读");
        break;
    case NodeKind::Directory:
        if (image.blob || (image.permissions & 0500) != 0500) throw Error("operation 目录必须由所有者读取和遍历");
        break;
    default: throw Error("operation 节点类型非法");
    }
}
std::size_t validate_state(const OperationState& state) {
    if (!known_kind(state.kind) || !known_phase(state.kind, state.phase))
        throw Error("operation kind 或 phase 非法");
    if ((state.kind == OperationKind::StashApply) != state.stash.has_value())
        throw Error("operation StashApply 必须具有专属上下文");
    if ((state.kind == OperationKind::Rebase) != state.rebase.has_value())
        throw Error("operation Rebase 必须具有专属上下文");
    if (state.stash) {
        if (state.stash->position >= 100000) throw Error("operation stash 位置超过限制");
        (void)reference_id(state.stash->raw_ref);
        if (state.message != (state.stash->pop ? "stash:pop" : "stash:apply"))
            throw Error("operation stash 消息与操作不一致");
    }
    validate_head(state.original_head);
    validate_head(state.current_head);
    if (state.rebase) {
        const auto& context = *state.rebase;
        if (state.current_head.symbolic) throw Error("operation Rebase 当前 HEAD 必须 detached");
        if (context.queue.empty() || context.queue.size() > max_queue || context.cursor >= context.queue.size())
            throw Error("operation Rebase 队列或 cursor 非法");
        if (context.queue.back() != *state.original_head.id || state.source_commit != context.queue[context.cursor])
            throw Error("operation Rebase 队列与原始 HEAD 或当前来源不一致");
        std::set<ObjectId> unique;
        for (const auto& source : context.queue)
            if (!unique.insert(source).second) throw Error("operation Rebase 队列来源重复");
        if (state.original_head.symbolic.has_value() != context.original_branch_ref.has_value())
            throw Error("operation Rebase 原始分支引用备份不一致");
        if (context.original_branch_ref && reference_id(*context.original_branch_ref) != *state.original_head.id)
            throw Error("operation Rebase 原始分支编号不一致");
        if (state.message != "rebase") throw Error("operation Rebase 消息必须为 rebase");
    } else if (state.original_head != state.current_head) throw Error("operation 原始和当前 HEAD 必须相同");
    if (state.original_head_bytes.size() > max_raw_head) throw Error("operation 原始 HEAD 字节过长");
    const auto raw = to_string(state.original_head_bytes);
    if (state.original_head.symbolic) {
        if (raw != "ref: " + *state.original_head.symbolic + "\n") throw Error("operation 原始 HEAD 字节与引用不一致");
    } else if (raw.size() != 65 || raw.back() != '\n' ||
               ObjectId::from_hex(std::string_view(raw).substr(0, 64)) != *state.original_head.id) {
        throw Error("operation 原始 detached HEAD 字节与编号不一致");
    }
    const auto& message = state.message;
    if (message.empty() || message.size() > max_file_size || message.find('\0') != std::string::npos ||
        std::all_of(message.begin(), message.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }))
        throw Error("operation 提交消息必须包含非空白内容且禁止 NUL");
    if (state.work.size() > max_work) throw Error("operation Work 超过 100000 条目限制");
    // Header, source ID, length/count fields, optional Index marker and checksum.
    std::size_t encoded = 10 + 32 + 4 + 4 + 4 + 1 + 32;
    for (const auto* head : {&state.original_head, &state.current_head}) {
        add_size(encoded, 34); // Symbolic and ID presence, fixed-size ID.
        if (head->symbolic) { add_size(encoded, 4); add_size(encoded, head->symbolic->size()); }
    }
    add_size(encoded, state.original_head_bytes.size());
    if (state.original_index_blob) add_size(encoded, 32);
    add_size(encoded, message.size());
    if (state.stash) add_size(encoded, 1 + 1 + 4 + 32 + 4 + 65);
    if (state.rebase) {
        add_size(encoded, 1 + 32 + 4 + 4 + 1);
        add_size(encoded, state.rebase->queue.size() * 32);
        if (state.rebase->original_branch_ref) add_size(encoded, 4 + 65);
    }
    std::size_t paths = 0;
    std::string_view previous;
    for (const auto& change : state.work) {
        checkout_support::validate_path(change.path);
        if (!previous.empty() && previous >= change.path) throw Error("operation Work 路径必须排序且唯一");
        previous = change.path;
        add_size(paths, change.path.size());
        add_size(encoded, 4);
        add_size(encoded, change.path.size());
        for (const auto* image : {&change.before, &change.after}) {
            validate_image(*image);
            add_size(encoded, 5);
            if (image->blob) add_size(encoded, 32);
        }
    }
    const auto find = [&](std::string_view path) {
        return std::lower_bound(state.work.begin(), state.work.end(), path,
            [](const auto& change, std::string_view key) { return change.path < key; });
    };
    for (const auto& change : state.work) {
        const auto prefix = change.path + "/";
        const auto child = find(prefix);
        if (child != state.work.end() && child->path.starts_with(prefix))
            for (const auto* image : {&change.before, &change.after})
                if (image->kind == NodeKind::Directory && (image->permissions & 0700) != 0700)
                    throw Error("含 operation 子项的目录必须具有所有者读写遍历权限");
        const auto path = std::string_view(change.path);
        for (auto slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
            const auto ancestor = path.substr(0, slash);
            const auto parent = find(ancestor);
            if (parent == state.work.end() || parent->path != ancestor) continue;
            if ((change.before.kind != NodeKind::Absent && parent->before.kind != NodeKind::Directory) ||
                (change.after.kind != NodeKind::Absent && parent->after.kind != NodeKind::Directory))
                throw Error("operation 节点两侧存在文件与目录冲突");
        }
    }
    return encoded;
}
void u32(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void id(Bytes& bytes, const ObjectId& value) { bytes.insert(bytes.end(), value.bytes().begin(), value.bytes().end()); }
void optional_id(Bytes& bytes, const std::optional<ObjectId>& value) {
    bytes.push_back(value ? 1 : 0);
    if (value) id(bytes, *value);
}
void text(Bytes& bytes, std::string_view value) {
    u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void head(Bytes& bytes, const Head& value) {
    bytes.push_back(value.symbolic ? 1 : 0);
    if (value.symbolic) text(bytes, *value.symbolic);
    optional_id(bytes, value.id);
}
void image(Bytes& bytes, const NodeImage& value) {
    bytes.push_back(static_cast<std::uint8_t>(value.kind));
    u32(bytes, value.permissions);
    if (value.blob) id(bytes, *value.blob);
}
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::span<const std::uint8_t> take(std::size_t size) {
        if (size > remaining()) throw Error("operation 数据被截断");
        const auto value = bytes_.subspan(position_, size);
        position_ += size;
        return value;
    }
    std::uint8_t byte() { return take(1)[0]; }
    bool presence() {
        const auto value = byte();
        if (value > 1) throw Error("operation 存在标志非法");
        return value != 0;
    }
    std::uint32_t number() {
        std::uint32_t value = 0;
        for (const auto byte : take(4)) value = (value << 8U) | byte;
        return value;
    }
    std::string text(std::size_t limit) {
        const auto length = number();
        if (length > limit) throw Error("operation 字段长度超过限制");
        return to_string(take(length));
    }
    std::optional<ObjectId> optional_id() { return presence() ? std::optional(ObjectId::from_bytes(take(32))) : std::nullopt; }
    Head head() {
        Head result;
        if (presence()) result.symbolic = text(4096);
        result.id = optional_id();
        return result;
    }
    NodeImage image() {
        NodeImage result;
        result.kind = static_cast<NodeKind>(byte());
        result.permissions = number();
        if (result.kind == NodeKind::File) result.blob = ObjectId::from_bytes(take(32));
        validate_image(result);
        return result;
    }
    std::size_t remaining() const { return bytes_.size() - position_; }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
};
std::optional<Bytes> raw_operation(const Repository& repo) {
    const auto path = repo.metadata() / "operation";
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) return std::nullopt;
    if (ec) throw Error("检查 operation 失败: " + ec.message());
    if (!std::filesystem::is_regular_file(status)) throw Error("operation 必须是普通文件");
    return read_file(path);
}
void validate_replacement(const Repository& repo, const OperationState& before, const OperationState& after) {
    if (before.kind != OperationKind::Rebase || after.kind != OperationKind::Rebase)
        throw Error("operation 不允许替换已存在状态");
    const auto& old = *before.rebase;
    const auto& next = *after.rebase;
    if (before.original_head != after.original_head || before.original_head_bytes != after.original_head_bytes ||
        before.original_index_blob != after.original_index_blob || old.onto != next.onto || old.queue != next.queue ||
        old.original_branch_ref != next.original_branch_ref)
        throw Error("operation Rebase 原始状态和队列不可改变");
    auto retained = after.work.begin();
    for (const auto& change : before.work) {
        while (retained != after.work.end() && retained->path < change.path) ++retained;
        if (retained == after.work.end() || retained->path != change.path || retained->before != change.before)
            throw Error("operation Rebase 旧 Work 路径或 before 不可改变");
    }
    if (before.phase == OperationPhase::Ready && after.phase == OperationPhase::Conflicted &&
        old.cursor == next.cursor && before.current_head == after.current_head) return;
    if (after.phase != OperationPhase::Ready || next.cursor != old.cursor + 1)
        throw Error("operation Rebase 必须冲突于当前来源或前进一个来源");
    const auto commit = read_commit(repo, *after.current_head.id);
    if (commit.parents.size() != 1 || commit.parents.front() != *before.current_head.id)
        throw Error("operation Rebase 新提交必须仅以旧 current HEAD 为父");
}
}
Bytes encode_operation(const OperationState& state) {
    const auto size = validate_state(state);
    auto bytes = to_bytes("MGOP");
    bytes.reserve(size);
    u32(bytes, uses_v2(state.kind) ? 2 : 1);
    bytes.push_back(static_cast<std::uint8_t>(state.kind));
    bytes.push_back(static_cast<std::uint8_t>(state.phase));
    head(bytes, state.original_head);
    head(bytes, state.current_head);
    u32(bytes, static_cast<std::uint32_t>(state.original_head_bytes.size()));
    bytes.insert(bytes.end(), state.original_head_bytes.begin(), state.original_head_bytes.end());
    optional_id(bytes, state.original_index_blob);
    id(bytes, state.source_commit);
    text(bytes, state.message);
    u32(bytes, static_cast<std::uint32_t>(state.work.size()));
    for (const auto& change : state.work) { text(bytes, change.path); image(bytes, change.before); image(bytes, change.after); }
    if (state.stash) {
        bytes.push_back(1);
        bytes.push_back(state.stash->pop ? 1 : 0);
        u32(bytes, state.stash->position);
        id(bytes, state.stash->stack_blob);
        u32(bytes, static_cast<std::uint32_t>(state.stash->raw_ref.size()));
        bytes.insert(bytes.end(), state.stash->raw_ref.begin(), state.stash->raw_ref.end());
    }
    if (state.rebase) {
        bytes.push_back(1);
        id(bytes, state.rebase->onto);
        u32(bytes, static_cast<std::uint32_t>(state.rebase->queue.size()));
        for (const auto& source : state.rebase->queue) id(bytes, source);
        u32(bytes, state.rebase->cursor);
        bytes.push_back(state.rebase->original_branch_ref ? 1 : 0);
        if (state.rebase->original_branch_ref) {
            u32(bytes, static_cast<std::uint32_t>(state.rebase->original_branch_ref->size()));
            bytes.insert(bytes.end(), state.rebase->original_branch_ref->begin(), state.rebase->original_branch_ref->end());
        }
    }
    const auto checksum = sha256(bytes);
    id(bytes, checksum);
    return bytes;
}
OperationState decode_operation(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 42 || bytes.size() > max_file_size) throw Error("operation 数据大小非法");
    const auto body = bytes.first(bytes.size() - 32);
    if (sha256(body) != ObjectId::from_bytes(bytes.last(32))) throw Error("operation 校验和不匹配");
    Reader reader(body);
    if (to_string(reader.take(4)) != "MGOP") throw Error("未知 operation 格式");
    const auto version = reader.number();
    if (version != 1 && version != 2) throw Error("未知 operation 版本");
    OperationState state;
    state.kind = static_cast<OperationKind>(reader.byte());
    state.phase = static_cast<OperationPhase>(reader.byte());
    if (!known_kind(state.kind) || !known_phase(state.kind, state.phase) ||
        (version == 2) != uses_v2(state.kind))
        throw Error("operation 版本、kind 或 phase 不匹配");
    state.original_head = reader.head();
    state.current_head = reader.head();
    const auto head_length = reader.number();
    if (head_length > max_raw_head) throw Error("operation 原始 HEAD 字节过长");
    const auto raw = reader.take(head_length);
    state.original_head_bytes.assign(raw.begin(), raw.end());
    state.original_index_blob = reader.optional_id();
    state.source_commit = ObjectId::from_bytes(reader.take(32));
    state.message = reader.text(max_file_size);
    const auto count = reader.number();
    if (count > max_work || count > reader.remaining() / 15) throw Error("operation Work 条目数量非法");
    state.work.reserve(count);
    std::size_t paths = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto path = reader.text(4096);
        checkout_support::validate_path(path);
        add_size(paths, path.size());
        if (!state.work.empty() && state.work.back().path >= path) throw Error("operation Work 路径必须排序且唯一");
        auto before = reader.image();
        auto after = reader.image();
        state.work.push_back({std::move(path), std::move(before), std::move(after)});
    }
    if (state.kind == OperationKind::StashApply) {
        if (!reader.presence()) throw Error("operation StashApply 上下文缺失");
        StashOperation context;
        context.pop = reader.presence();
        context.position = reader.number();
        context.stack_blob = ObjectId::from_bytes(reader.take(32));
        if (reader.number() != 65) throw Error("operation stash 引用长度非法");
        const auto raw_ref = reader.take(65);
        context.raw_ref.assign(raw_ref.begin(), raw_ref.end());
        state.stash = std::move(context);
    }
    if (state.kind == OperationKind::Rebase) {
        if (!reader.presence()) throw Error("operation Rebase 上下文缺失");
        RebaseOperation context;
        context.onto = ObjectId::from_bytes(reader.take(32));
        const auto queue_count = reader.number();
        // The remaining tail needs cursor and branch presence even without a ref.
        // Reject authenticated count bombs before queue's reserve/allocation.
        if (!queue_count || queue_count > max_queue || reader.remaining() < 5 || queue_count > (reader.remaining() - 5) / 32)
            throw Error("operation Rebase 队列数量非法");
        context.queue.reserve(queue_count);
        for (std::uint32_t i = 0; i < queue_count; ++i) context.queue.push_back(ObjectId::from_bytes(reader.take(32)));
        context.cursor = reader.number();
        if (reader.presence()) {
            if (reader.number() != 65) throw Error("operation Rebase 原始分支引用长度非法");
            const auto raw_ref = reader.take(65);
            context.original_branch_ref = Bytes(raw_ref.begin(), raw_ref.end());
        }
        state.rebase = std::move(context);
    }
    if (reader.remaining()) throw Error("operation 含多余数据");
    (void)validate_state(state);
    return state;
}
std::set<ObjectId> state_roots(const Repository& repo, const OperationState& state) {
    (void)validate_state(state);
    const ObjectStore store(repo.metadata() / "objects");
    std::map<ObjectId, ObjectType> checked;
    std::set<ObjectId> roots;
    std::size_t commit_bytes = 0;
    const auto keep = [&](const ObjectId& id, ObjectType type) {
        if (const auto found = checked.find(id); found != checked.end()) {
            if (found->second != type) throw Error("operation 同一编号具有不一致对象类型");
        } else {
            const auto object = store.read(id);
            if (object.type != type) throw Error("operation 根对象类型非法");
            if (state.rebase && type == ObjectType::Commit) add_size(commit_bytes, object.payload.size());
            checked.emplace(id, type);
        }
        roots.insert(id);
    };
    keep(*state.original_head.id, ObjectType::Commit);
    keep(*state.current_head.id, ObjectType::Commit);
    keep(state.source_commit, ObjectType::Commit);
    if (state.rebase) {
        keep(state.rebase->onto, ObjectType::Commit);
        for (const auto& source : state.rebase->queue) keep(source, ObjectType::Commit);
    }
    for (const auto& change : state.work)
        for (const auto* image : {&change.before, &change.after}) if (image->blob) keep(*image->blob, ObjectType::Blob);
    if (state.original_index_blob) {
        keep(*state.original_index_blob, ObjectType::Blob);
        const auto object = store.read(*state.original_index_blob);
        // decode_index's count is otherwise only bounded by encoded byte size.
        // Check the stricter stage limit before its reserve/count-driven loop.
        if (object.payload.size() >= 12) {
            std::uint32_t count = 0;
            for (const auto byte : std::span(object.payload).subspan(8, 4)) count = (count << 8U) | byte;
            if (count > 300000) throw Error("operation 原始索引超过 300000 阶段条目限制");
        }
        const auto index = decode_index(object.payload);
        validate_index_budget(index);
        for (const auto& entry : index) keep(entry.id, ObjectType::Blob);
    }
    if (state.stash) {
        keep(state.stash->stack_blob, ObjectType::Blob);
        const auto object = store.read(state.stash->stack_blob);
        const auto entries = decode_stash_stack(object.payload);
        if (state.stash->position >= entries.size() || entries[state.stash->position] != state.source_commit)
            throw Error("operation stash 选中位置与来源不一致");
        if (reference_id(state.stash->raw_ref) != entries.front()) throw Error("operation stash 引用与保存栈顶不一致");
        const auto stash = stash_roots(repo, entries);
        roots.insert(stash.begin(), stash.end());
    }
    return roots;
}
std::optional<OperationState> read_operation(const Repository& repo) {
    const auto bytes = raw_operation(repo);
    if (!bytes) return std::nullopt;
    auto state = decode_operation(*bytes);
    (void)state_roots(repo, state);
    return state;
}
void require_no_operation(const Repository& repo) {
    const auto path = repo.metadata() / "operation";
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) return;
    if (ec) throw Error("检查 operation 失败: " + ec.message());
    throw Error("存在未完成操作，请继续或中止");
}
void plan_operation_update(const Repository& repo, TransactionPlan& plan,
                           const std::optional<OperationState>& expected,
                           const std::optional<OperationState>& desired) {
    require_no_transaction(repo);
    if (std::any_of(plan.metadata.begin(), plan.metadata.end(), [](const auto& change) { return change.path == "operation"; }))
        throw Error("operation 已被计划中的其他操作修改");
    validate_transaction_budget(plan);
    auto before = raw_operation(repo);
    if (before) (void)state_roots(repo, decode_operation(*before));
    const auto expected_bytes = expected ? std::optional(encode_operation(*expected)) : std::nullopt;
    if (before != expected_bytes) throw Error("operation 与期待状态不一致");
    std::optional<Bytes> after;
    if (desired) { after = encode_operation(*desired); (void)state_roots(repo, *desired); }
    if (before == after) return;
    if (before && after) validate_replacement(repo, *expected, *desired);
    auto next = plan;
    next.metadata.push_back({"operation", std::move(before), std::move(after)});
    validate_transaction_budget(next);
    plan = std::move(next);
}
}
