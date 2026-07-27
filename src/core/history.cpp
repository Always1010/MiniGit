#include "minigit/history.hpp"
#include "minigit/refs.hpp"
#include "minigit/object_store.hpp"
#include "minigit/error.hpp"
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
namespace minigit {
namespace {
constexpr std::size_t max_entries = 100000;
constexpr std::size_t max_depth = 128;
struct Budget {
    std::size_t bytes = 0;
    std::size_t entries = 0;
    std::set<ObjectId> blobs;
    void add(std::size_t size) {
        if (size > max_file_size - bytes) throw Error("快照展开数据超过 64 MiB 限制");
        bytes += size;
    }
    void entry() { if (++entries > max_entries) throw Error("快照条目超过限制"); }
    void blob(const ObjectStore& store, const ObjectId& id) {
        if (!blobs.insert(id).second) return;
        const auto object = store.read(id);
        if (object.type != ObjectType::Blob) throw Error("快照文件引用的对象不是 blob");
    }
};
void validate_mode(std::string_view mode) {
    if (mode != "100644" && mode != "100755") throw Error("快照文件模式非法");
}
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
struct TreeNode {
    std::map<std::string, std::unique_ptr<TreeNode>> directories;
    std::map<std::string, FileState> files;
};
}
Commit read_commit(const Repository& repo, const ObjectId& id) {
    const auto object = ObjectStore(repo.metadata() / "objects").read(id);
    if (object.type != ObjectType::Commit) throw Error("引用的对象不是 commit");
    return decode_commit(object.payload);
}
Snapshot read_snapshot(const Repository& repo, const ObjectId& tree) {
    const ObjectStore store(repo.metadata() / "objects");
    Snapshot result;
    Budget budget;
    std::function<void(const ObjectId&, const std::string&, std::size_t)> expand;
    expand = [&](const ObjectId& id, const std::string& prefix, std::size_t depth) {
        if (depth > max_depth) throw Error("树层级超过 128 限制");
        const auto object = store.read(id);
        if (object.type != ObjectType::Tree) throw Error("快照目录引用的对象不是 tree");
        budget.add(object.payload.size());
        for (const auto& entry : decode_tree(object.payload)) {
            budget.entry();
            const auto path = prefix + entry.name;
            validate_repo_path(path);
            budget.add(path.size());
            if (entry.mode == "40000") expand(entry.id, path + "/", depth + 1);
            else {
                validate_mode(entry.mode);
                budget.blob(store, entry.id);
                if (!result.emplace(path, FileState{entry.mode, entry.id}).second) throw Error("快照路径重复");
            }
        }
    };
    expand(tree, "", 1);
    return result;
}
ObjectId write_snapshot(const Repository& repo, const Snapshot& snapshot) {
    ObjectStore store(repo.metadata() / "objects");
    Budget budget;
    TreeNode root;
    // Finish validation before publishing any tree object.
    for (const auto& [path, state] : snapshot) {
        validate_repo_path(path);
        validate_mode(state.mode);
        budget.entry();
        budget.blob(store, state.id);
        TreeNode* node = &root;
        std::string_view remaining(path);
        std::size_t depth = 1;
        for (;;) {
            const auto slash = remaining.find('/');
            const auto name = std::string(remaining.substr(0, slash));
            if (slash == std::string_view::npos) {
                if (node->directories.contains(name)) throw Error("快照文件与目录冲突");
                budget.add(path.size() + name.size() + 40);
                node->files.emplace(name, state);
                break;
            }
            if (++depth > max_depth) throw Error("树层级超过 128 限制");
            if (node->files.contains(name)) throw Error("快照文件与目录冲突");
            auto& next = node->directories[name];
            if (!next) {
                budget.entry();
                const auto full_path_size = path.size() - remaining.size() + name.size();
                budget.add(full_path_size + name.size() + 39);
                next = std::make_unique<TreeNode>();
            }
            node = next.get();
            remaining.remove_prefix(slash + 1);
        }
    }
    std::function<ObjectId(const TreeNode&)> write;
    write = [&](const TreeNode& node) {
        std::vector<TreeEntry> entries;
        entries.reserve(node.files.size() + node.directories.size());
        for (const auto& [name, state] : node.files) entries.push_back({name, state.mode, state.id});
        for (const auto& [name, child] : node.directories) entries.push_back({name, "40000", write(*child)});
        return store.write(ObjectType::Tree, encode_tree(std::move(entries)));
    };
    return write(root);
}
Snapshot index_snapshot(const Repository& repo) {
    const ObjectStore store(repo.metadata() / "objects");
    Snapshot result;
    Budget budget;
    for (const auto& entry : load_index(repo)) {
        if (entry.stage != 0) throw Error("索引存在未解决的冲突");
        budget.entry();
        budget.add(entry.path.size() + 40);
        budget.blob(store, entry.id);
        result.emplace(entry.path, FileState{entry.mode, entry.id});
    }
    return result;
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
void configure_identity(const Repository& repo, std::string_view key, std::string_view value) {
    validate_identity(key, value);
    RepoLock lock(repo.metadata());
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
    if (message.empty() || message.find('\0') != std::string_view::npos || message.size() > max_file_size ||
        std::all_of(message.begin(), message.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }))
        throw Error("提交消息必须包含非空白内容且禁止 NUL");
    RepoLock lock(repo.metadata());
    const auto identity = load_identity(repo);
    if (!identity.contains("user.name") || !identity.contains("user.email")) throw Error("请显式配置 user.name 和 user.email");
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
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto author = identity.at("user.name") + " <" + identity.at("user.email") + "> " + std::to_string(seconds) + " +0000";
    Commit commit{tree, {}, author, author, std::string(message)};
    if (head.id) commit.parents.push_back(*head.id);
    const auto id = ObjectStore(repo.metadata() / "objects").write(ObjectType::Commit, encode_commit(commit));
    if (head.symbolic) update_ref(repo, *head.symbolic, head.id, id);
    else set_head(repo, id.hex());
    return id;
}
}
