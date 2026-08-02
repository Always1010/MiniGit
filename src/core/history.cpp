#include "minigit/history.hpp"
#include "minigit/refs.hpp"
#include "minigit/object_store.hpp"
#include "minigit/fs.hpp"
#include "minigit/error.hpp"
#include "minigit/transaction.hpp"
#include "minigit/reflog.hpp"
#include "minigit/operation.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <utility>
namespace minigit {
namespace {
void validate_key(std::string_view key) {
    if (key != "user.name" && key != "user.email") throw Error("仅支持 user.name 和 user.email");
}
void validate_identity(std::string_view key, std::string_view value) {
    validate_key(key);
    if (value.empty() || value.size() > 4096 || value.find_first_of("<>") != std::string_view::npos)
        throw Error("身份字段为空、过长或含尖括号");
    bool nonspace = false;
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 32 || c == 127) throw Error("身份字段必须是单行且禁止控制字符");
        if (c != ' ') nonspace = true;
    }
    if (!nonspace) throw Error("身份字段不能全为空白");
    if (key == "user.email") {
        const auto at = value.find('@');
        if (value.find(' ') != std::string_view::npos || at == std::string_view::npos || at == 0 ||
            at + 1 == value.size() || value.find('@', at + 1) != std::string_view::npos)
            throw Error("邮箱格式非法");
    }
}
using Identity = std::map<std::string, std::string>;
Identity load_identity(const Repository& repo) {
    const auto path = repo.metadata() / "identity";
    detail::reject_symlinks(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec) throw Error("检查身份配置失败: " + ec.message());
    const auto text = to_string(read_file(path));
    if (!text.starts_with("version=1\n") || text.size() > 8256) throw Error("身份配置版本或大小非法");
    Identity result;
    auto remaining = std::string_view(text).substr(10);
    std::string previous;
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        if (newline == std::string_view::npos) throw Error("身份配置缺少换行符");
        const auto line = remaining.substr(0, newline);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) throw Error("身份配置字段非法");
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        validate_identity(key, value);
        // Fixed order keeps this schema canonical: name first, then email.
        if (result.contains(std::string(key)) || (previous == "user.email")) throw Error("身份配置字段重复或顺序非法");
        previous = key;
        result.emplace(key, value);
        remaining.remove_prefix(newline + 1);
    }
    return result;
}
void validate_message(std::string_view message) {
    if (message.empty() || message.find('\0') != std::string_view::npos || message.size() > max_file_size ||
        std::all_of(message.begin(), message.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }))
        throw Error("提交消息必须包含非空白内容且禁止 NUL");
}
Identity required_identity(const Repository& repo) {
    auto identity = load_identity(repo);
    if (!identity.contains("user.name") || !identity.contains("user.email")) throw Error("请显式配置 user.name 和 user.email");
    return identity;
}
void validate_parent_count(const std::vector<ObjectId>& parents) {
    if (parents.size() > 100000) throw Error("提交父节点超过 100000 限制");
}
void validate_commit_frame(std::size_t parents, std::size_t author, std::size_t committer, std::size_t message) {
    // encode_commit: tree line (70), actor field syntax (20), parent lines (72).
    // ObjectStore framing: "commit " + decimal payload length + NUL.
    std::size_t payload = 90;
    for (const auto bytes : {parents * 72, author, committer, message}) {
        if (bytes > max_file_size - payload) throw Error("提交对象超过 64 MiB 限制");
        payload += bytes;
    }
    const auto frame_header = 8 + std::to_string(payload).size();
    if (payload > max_file_size - frame_header) throw Error("提交对象完整 frame 超过 64 MiB 限制");
}
Commit prepare_commit(const Identity& identity, const std::vector<ObjectId>& parents,
                      std::string_view message, std::optional<std::string_view> original_author) {
    validate_parent_count(parents);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto committer = identity.at("user.name") + " <" + identity.at("user.email") + "> " + std::to_string(seconds) + " +0000";
    const auto author = original_author.value_or(committer);
    validate_commit_frame(parents.size(), author.size(), committer.size(), message.size());
    Commit commit{ObjectId{}, parents, std::string(author), committer, std::string(message)};
    // The placeholder Tree ID has the same encoded width as the final ID.
    // Reuse the codec's actor contract, including preserved source-author bytes.
    (void)encode_commit(commit);
    return commit;
}
ObjectId store_final_commit(const Repository& repo, Commit commit, ObjectId tree) {
    commit.tree = tree;
    return ObjectStore(repo.metadata() / "objects").write(ObjectType::Commit, encode_commit(commit));
}
}
Snapshot head_snapshot(const Repository& repo) {
    const auto head = read_head(repo);
    return head.id ? read_snapshot(repo, read_commit(repo, *head.id).tree) : Snapshot{};
}
std::optional<std::string> identity_value(const Repository& repo, std::string_view key) {
    validate_key(key);
    const auto identity = load_identity(repo);
    const auto found = identity.find(std::string(key));
    if (found == identity.end()) return std::nullopt;
    return found->second;
}
ObjectId store_commit_snapshot(const Repository& repo, const Snapshot& snapshot,
                               const std::vector<ObjectId>& ordered_parents,
                               std::string_view message, std::optional<std::string_view> original_author) {
    validate_message(message);
    require_no_transaction(repo);
    validate_parent_count(ordered_parents);
    auto commit = prepare_commit(required_identity(repo), ordered_parents, message, original_author);
    std::set<ObjectId> checked;
    for (const auto& id : ordered_parents)
        if (checked.insert(id).second) (void)read_snapshot(repo, read_commit(repo, id).tree);
    const auto tree = write_snapshot(repo, snapshot);
    return store_final_commit(repo, std::move(commit), tree);
}
void configure_identity(const Repository& repo, std::string_view key, std::string_view value) {
    validate_identity(key, value);
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    auto identity = load_identity(repo);
    identity[std::string(key)] = value;
    std::string text = "version=1\n";
    for (const auto name : {"user.name", "user.email"}) {
        const auto found = identity.find(name);
        if (found != identity.end()) text += found->first + "=" + found->second + "\n";
    }
    atomic_write(repo.metadata() / "identity", to_bytes(text));
}
ObjectId create_commit(const Repository& repo, std::string_view message, bool allow_empty) {
    validate_message(message);
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    require_no_operation(repo);
    const auto identity = required_identity(repo);
    const auto head = read_head(repo);
    std::optional<Commit> parent;
    if (head.id) {
        parent = read_commit(repo, *head.id);
        (void)read_snapshot(repo, parent->tree);
    }
    const auto snapshot = index_snapshot(repo);
    if (!allow_empty && !head.id && snapshot.empty()) throw Error("初始索引为空，无法创建提交");
    const auto tree = write_snapshot(repo, snapshot);
    if (!allow_empty && parent && tree == parent->tree) throw Error("索引与 HEAD 相同，没有可提交的改动");
    const std::vector<ObjectId> parents = head.id ? std::vector<ObjectId>{*head.id} : std::vector<ObjectId>{};
    auto commit = prepare_commit(identity, parents, message, std::nullopt);
    const auto id = store_final_commit(repo, std::move(commit), tree);
    TransactionPlan plan;
    plan_move_head(repo, plan, head, {head.symbolic, id}, "commit");
    execute_transaction(repo, plan);
    return id;
}
}
