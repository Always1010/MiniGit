#include "minigit/snapshot.hpp"
#include "minigit/object_store.hpp"
#include "minigit/error.hpp"
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
}
