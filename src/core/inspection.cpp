#include "minigit/inspection.hpp"
#include "minigit/diff.hpp"
#include "minigit/error.hpp"
#include "minigit/transaction.hpp"
#include "minigit/fs.hpp"
#include "minigit/format.hpp"
#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/worktree.hpp"
#include <set>
#include <utility>
namespace minigit {
namespace {
constexpr std::size_t max_snapshot_paths = 200000;
void validate_context(std::size_t context) {
    if (context > 100000) throw Error("上下文超过 100000 行限制");
}
void validate_snapshots(const Snapshot& before, const Snapshot& after) {
    if (before.size() > max_snapshot_paths || after.size() > max_snapshot_paths - before.size())
        throw Error("差异快照路径超过 200000 条限制");
    std::size_t bytes = 0;
    for (const auto* snapshot : {&before, &after}) {
        for (const auto& [path, state] : *snapshot) {
            if (path.size() > 4096) throw Error("差异路径超过 4096 字节限制");
            validate_repo_path(path);
            if (state.mode != "100644" && state.mode != "100755") throw Error("差异文件模式非法");
            if (path.size() > max_file_size - bytes) throw Error("差异快照路径数据超过 64 MiB 限制");
            bytes += path.size();
            const auto prefix = path + "/";
            const auto child = snapshot->lower_bound(prefix);
            if (child != snapshot->end() && child->first.starts_with(prefix)) throw Error("差异快照文件与目录冲突");
        }
    }
}
Bytes read_blob(const ObjectStore& store, const ObjectId& id) {
    auto object = store.read(id);
    if (object.type != ObjectType::Blob) throw Error("差异文件引用的对象不是 blob");
    return std::move(object.payload);
}
const FileState* find(const Snapshot& snapshot, const std::string& path) {
    const auto found = snapshot.find(path);
    return found == snapshot.end() ? nullptr : &found->second;
}
std::set<std::string> paths(const Snapshot& before, const Snapshot& after) {
    std::set<std::string> result;
    for (const auto* snapshot : {&before, &after})
        for (const auto& [path, state] : *snapshot) { (void)state; result.insert(path); }
    return result;
}
void append(std::string& output, std::string addition) {
    if (addition.size() > max_file_size - output.size()) throw Error("差异总输出超过 64 MiB 限制");
    output += addition;
}
Snapshot revision_snapshot(const Repository& repo, std::string_view revision) {
    return read_snapshot(repo, read_commit(repo, resolve_commit(repo, revision)).tree);
}
void validate_show_path(std::string_view path) {
    if (path.size() > 4096) throw Error("查看路径超过 4096 字节限制");
    validate_repo_path(path);
}
Bytes show_path(const Repository& repo, std::string_view revision, std::string_view path) {
    const ObjectStore store(repo.metadata() / "objects");
    if (!revision.empty()) {
        validate_show_path(path);
        const auto snapshot = revision_snapshot(repo, revision);
        const auto entry = snapshot.find(std::string(path));
        if (entry == snapshot.end()) throw Error("提交中不存在指定文件");
        return read_blob(store, entry->second.id);
    }
    std::uint8_t stage = 0;
    if (path.size() >= 2 && path[0] >= '0' && path[0] <= '3' && path[1] == ':') {
        stage = static_cast<std::uint8_t>(path[0] - '0');
        path.remove_prefix(2);
    }
    validate_show_path(path);
    for (const auto& entry : load_index(repo))
        if (entry.path == path && entry.stage == stage) return read_blob(store, entry.id);
    throw Error("索引中不存在指定路径和阶段");
}
Bytes show_commit(const Repository& repo, const ObjectId& id, const Commit& commit, std::size_t context) {
    const auto after = read_snapshot(repo, commit.tree);
    const auto before = commit.parents.empty() ? Snapshot{} : read_snapshot(repo, read_commit(repo, commit.parents[0]).tree);
    auto output = format_commit(id, commit);
    append(output, diff_snapshots(repo, before, after, context));
    return to_bytes(output);
}
Bytes show_tree(std::span<const std::uint8_t> bytes) {
    std::string output;
    for (const auto& entry : decode_tree(bytes)) {
        append(output, entry.mode + " " + entry.id.hex() + " " + quote_path(entry.name) + "\n");
    }
    return to_bytes(output);
}
std::string worktree_diff(const Repository& repo, const Snapshot& before, const Snapshot& index, std::size_t context) {
    validate_snapshots(before, index);
    const ObjectStore store(repo.metadata() / "objects");
    std::string output;
    for (const auto& path : paths(before, index)) {
        Bytes old_bytes;
        std::optional<DiffFileView> old_view, new_view;
        if (const auto state = find(before, path)) {
            old_bytes = read_blob(store, state->id);
            old_view = DiffFileView{state->mode, old_bytes};
        }
        const auto file = read_worktree_file(repo, path);
        if (file) new_view = DiffFileView{file->state.mode, file->data.bytes};
        append(output, format_file_diff(path, old_view, new_view, context));
    }
    return output;
}
}
std::string diff_snapshots(const Repository& repo, const Snapshot& before, const Snapshot& after, std::size_t context) {
    validate_context(context);
    validate_snapshots(before, after);
    const ObjectStore store(repo.metadata() / "objects");
    std::string output;
    for (const auto& path : paths(before, after)) {
        const auto old_state = find(before, path), new_state = find(after, path);
        Bytes old_bytes, new_bytes;
        std::optional<DiffFileView> old_view, new_view;
        if (old_state) {
            old_bytes = read_blob(store, old_state->id);
            old_view = DiffFileView{old_state->mode, old_bytes};
        }
        if (new_state) {
            if (old_state && old_state->id == new_state->id) new_view = DiffFileView{new_state->mode, old_bytes};
            else {
                new_bytes = read_blob(store, new_state->id);
                new_view = DiffFileView{new_state->mode, new_bytes};
            }
        }
        append(output, format_file_diff(path, old_view, new_view, context));
    }
    return output;
}
std::string repository_diff(const Repository& repo, const DiffOptions& options) {
    validate_context(options.context);
    if (options.revisions.size() > 2 || (options.staged && options.revisions.size() > 1)) throw Error("差异请求版本数量非法");
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    if (options.revisions.size() == 2) {
        const auto before = revision_snapshot(repo, options.revisions[0]);
        const auto after = revision_snapshot(repo, options.revisions[1]);
        return diff_snapshots(repo, before, after, options.context);
    }
    const auto index = index_snapshot(repo);
    if (options.staged) {
        const auto before = options.revisions.empty() ? head_snapshot(repo) : revision_snapshot(repo, options.revisions[0]);
        return diff_snapshots(repo, before, index, options.context);
    }
    if (options.revisions.empty()) return worktree_diff(repo, index, index, options.context);
    return worktree_diff(repo, revision_snapshot(repo, options.revisions[0]), index, options.context);
}
Bytes show_object(const Repository& repo, std::string_view spec, std::size_t context) {
    if (spec.size() > 8193) throw Error("查看对象描述超过 8193 字节限制");
    validate_context(context);
    RepoLock lock(repo.metadata());
    const auto colon = spec.find(':');
    if (colon != std::string_view::npos) {
        require_no_transaction(repo);
        return show_path(repo, spec.substr(0, colon), spec.substr(colon + 1));
    }
    if (spec.size() == 64 && spec.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos) {
        const auto id = ObjectId::from_hex(spec);
        auto object = ObjectStore(repo.metadata() / "objects").read(id);
        if (object.type == ObjectType::Blob) return std::move(object.payload);
        if (object.type == ObjectType::Tree) return show_tree(object.payload);
        require_no_transaction(repo);
        return show_commit(repo, id, decode_commit(object.payload), context);
    }
    require_no_transaction(repo);
    const auto id = resolve_commit(repo, spec);
    return show_commit(repo, id, read_commit(repo, id), context);
}
}
