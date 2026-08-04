#include "minigit/stash_stack.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include "minigit/objects.hpp"
#include <algorithm>
#include <map>
#include <utility>
namespace minigit {
namespace {
constexpr std::size_t max_entries = 100000;
std::size_t encoded_size(const StashStack& stack) {
    if (stack.size() > max_entries || stack.size() > (max_file_size - 44) / 32)
        throw Error("stash 栈超过数量或 64 MiB 限制");
    return 44 + 32 * stack.size();
}
void number(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::uint32_t number(std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 0;
    for (const auto byte : bytes) value = (value << 8U) | byte;
    return value;
}
class Entries {
public:
    explicit Entries(const Repository& repo) : store_(repo.metadata() / "objects") {}
    const Commit& commit(ObjectId id) {
        if (const auto found = commits_.find(id); found != commits_.end()) return found->second;
        auto object = store_.read(id);
        if (object.type != ObjectType::Commit) throw Error("stash 条目必须引用 Commit");
        if (object.payload.size() > max_file_size - decoded_)
            throw Error("stash Commit 解码数据超过 64 MiB 限制");
        decoded_ += object.payload.size();
        auto value = decode_commit(object.payload);
        return commits_.emplace(id, std::move(value)).first->second;
    }
    StashEntry entry(ObjectId id) {
        const auto& work = commit(id);
        if (work.parents.size() != 2 && work.parents.size() != 3)
            throw Error("stash WorkCommit 必须有两个或三个父提交");
        StashEntry result{id, work.parents[0], work.parents[1], std::nullopt};
        (void)commit(result.head);
        const auto& index = commit(result.index);
        if (index.parents.size() != 1 || index.parents.front() != result.head)
            throw Error("stash IndexCommit 必须以 H 为唯一父提交");
        if (work.parents.size() == 3) {
            result.untracked = work.parents[2];
            if (!commit(*result.untracked).parents.empty()) throw Error("stash UntrackedCommit 必须是根提交");
        }
        return result;
    }
private:
    ObjectStore store_;
    std::map<ObjectId, Commit> commits_;
    std::size_t decoded_ = 0;
};
std::optional<Bytes> raw(const Repository& repo, std::string_view name) {
    const auto path = repo.metadata() / name;
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) return std::nullopt;
    if (ec) throw Error("检查 stash 元数据失败: " + ec.message());
    if (!std::filesystem::is_regular_file(status)) throw Error("stash 元数据必须是普通文件");
    return read_file(path);
}
ObjectId ref_id(const Bytes& bytes) {
    const auto text = to_string(bytes);
    if (text.size() != 65 || text.back() != '\n') throw Error("refs/stash 格式非法");
    return ObjectId::from_hex(std::string_view(text).substr(0, 64));
}
}
Bytes encode_stash_stack(const StashStack& stack) {
    const auto size = encoded_size(stack);
    auto bytes = to_bytes("MGST");
    bytes.reserve(size);
    number(bytes, 1);
    number(bytes, static_cast<std::uint32_t>(stack.size()));
    for (const auto& id : stack) bytes.insert(bytes.end(), id.bytes().begin(), id.bytes().end());
    const auto checksum = sha256(bytes);
    bytes.insert(bytes.end(), checksum.bytes().begin(), checksum.bytes().end());
    return bytes;
}
StashStack decode_stash_stack(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 44 || bytes.size() > max_file_size) throw Error("stash 栈数据大小非法");
    const auto body = bytes.first(bytes.size() - 32);
    if (sha256(body) != ObjectId::from_bytes(bytes.last(32))) throw Error("stash 栈校验和不匹配");
    if (to_string(body.first(4)) != "MGST" || number(body.subspan(4, 4)) != 1)
        throw Error("未知 stash 栈格式或版本");
    const auto count = number(body.subspan(8, 4));
    if (count > max_entries || body.size() - 12 != static_cast<std::size_t>(count) * 32)
        throw Error("stash 栈条目数量或长度非法");
    StashStack stack;
    stack.reserve(count);
    for (std::size_t i = 0; i < count; ++i) stack.push_back(ObjectId::from_bytes(body.subspan(12 + 32 * i, 32)));
    return stack;
}
StashEntry read_stash_entry(const Repository& repo, ObjectId work) { return Entries(repo).entry(work); }
std::set<ObjectId> stash_roots(const Repository& repo, const StashStack& stack) {
    (void)encoded_size(stack);
    Entries entries(repo);
    std::set<ObjectId> roots;
    for (const auto& id : stack) {
        const auto entry = entries.entry(id);
        roots.insert(entry.work);
        roots.insert(entry.head);
        roots.insert(entry.index);
        if (entry.untracked) roots.insert(*entry.untracked);
    }
    return roots;
}
StashStackState read_stash_stack(const Repository& repo) {
    StashStackState state;
    state.raw_stack = raw(repo, "stashstack");
    state.raw_ref = raw(repo, "refs/stash");
    if (state.raw_stack) state.entries = decode_stash_stack(*state.raw_stack);
    if (state.entries.empty()) {
        if (state.raw_ref) throw Error("空 stash 栈不能带 refs/stash");
    } else if (!state.raw_ref || ref_id(*state.raw_ref) != state.entries.front()) {
        throw Error("stash 栈顶与 refs/stash 不一致");
    }
    (void)stash_roots(repo, state.entries);
    return state;
}
void plan_stash_stack_update(const Repository& repo, TransactionPlan& plan,
                             const StashStackState& expected, const StashStack& desired) {
    require_no_transaction(repo);
    for (const auto& change : plan.metadata)
        if (change.path == "stashstack" || change.path == "refs/stash")
            throw Error("stash 栈或引用已被计划中的其他操作修改");
    validate_transaction_budget(plan);
    const auto actual = read_stash_stack(repo);
    if (actual.entries != expected.entries || actual.raw_stack != expected.raw_stack || actual.raw_ref != expected.raw_ref)
        throw Error("stash 栈与期待状态不一致");
    (void)stash_roots(repo, desired);
    if (desired == actual.entries) return;
    std::optional<Bytes> after_stack, after_ref;
    if (!desired.empty()) {
        after_stack = encode_stash_stack(desired);
        after_ref = !actual.entries.empty() && actual.entries.front() == desired.front() ?
            actual.raw_ref : std::optional(to_bytes(desired.front().hex() + "\n"));
    }
    auto next = plan;
    next.metadata.push_back({"stashstack", actual.raw_stack, std::move(after_stack)});
    next.metadata.push_back({"refs/stash", actual.raw_ref, std::move(after_ref)});
    validate_transaction_budget(next);
    plan = std::move(next);
}
}
