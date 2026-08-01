#include "minigit/merge_snapshot.hpp"
#include "checkout_support.hpp"
#include "minigit/diff.hpp"
#include "minigit/error.hpp"
#include "minigit/object_store.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <utility>
namespace minigit {
namespace {
using State = std::optional<FileState>;
struct Choice { bool resolved; State file; };
State state_at(const Snapshot& snapshot, const std::string& path) {
    const auto found = snapshot.find(path);
    return found == snapshot.end() ? State{} : State{found->second};
}
Choice choose(const State& base, const State& ours, const State& theirs) {
    if (ours == theirs) return {true, ours};
    if (base == ours) return {true, theirs};
    if (base == theirs) return {true, ours};
    return {false, {}};
}
template<class T> std::optional<T> scalar(const std::optional<T>& base, const T& ours, const T& theirs) {
    if (ours == theirs) return ours;
    if (base && *base == ours) return theirs;
    if (base && *base == theirs) return ours;
    return {};
}
Index snapshot_index(const Snapshot& snapshot) {
    if (snapshot.size() > 100000) throw Error("快照文件超过 100000 限制");
    Index index;
    index.reserve(snapshot.size());
    std::size_t bytes = 44;
    for (const auto& [path, file] : snapshot) {
        checkout_support::validate_path(path);
        if (file.mode != "100644" && file.mode != "100755") throw Error("非法快照文件模式");
        if (path.size() > max_file_size - bytes || 41 > max_file_size - bytes - path.size())
            throw Error("快照结构超过 64 MiB 限制");
        bytes += path.size() + 41;
        index.push_back({path, file.mode, file.id});
    }
    checkout_support::validate_index_budget(index);
    (void)encode_index(index);
    return index;
}
const std::string* region_at(const std::set<std::string>& roots, const std::string& path) {
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        const auto root = roots.find(path.substr(0, slash));
        if (root != roots.end()) return &*root;
    }
    const auto root = roots.find(path);
    return root == roots.end() ? nullptr : &*root;
}
class IndexOutput {
public:
    explicit IndexOutput(Index& index) : index_(index) {}
    void append(const std::string& path, const FileState& file, std::uint8_t stage) {
        if (index_.empty() || index_.back().path != path) {
            if (paths_ == 100000) throw Error("索引不同路径超过 100000 限制");
            ++paths_;
        }
        if (index_.size() == 300000) throw Error("索引阶段条目超过 300000 限制");
        if (path.size() > max_file_size - bytes_ || 41 > max_file_size - bytes_ - path.size())
            throw Error("索引超过 64 MiB 限制");
        bytes_ += path.size() + 41;
        index_.push_back({path, file.mode, file.id, stage});
    }
    void originals(const std::string& path, const std::array<State, 3>& states) {
        for (unsigned side = 0; side < states.size(); ++side)
            if (states[side]) append(path, *states[side], static_cast<std::uint8_t>(side + 1));
    }
private:
    Index& index_;
    std::size_t paths_ = 0, bytes_ = 44;
};
Bytes read_blob(const ObjectStore& store, const ObjectId& id) {
    auto object = store.read(id);
    if (object.type != ObjectType::Blob) throw Error("合并快照对象不是 blob");
    return std::move(object.payload);
}
bool has_nul(std::span<const std::uint8_t> content) {
    return std::find(content.begin(), content.end(), 0) != content.end();
}
std::string_view text_view(std::span<const std::uint8_t> content) {
    if (content.empty()) return {};
    return {reinterpret_cast<const char*>(content.data()), content.size()};
}
Bytes add_add_marker(std::span<const std::uint8_t> ours, std::span<const std::uint8_t> theirs) {
    // Missing base is not an empty existing file: different additions always
    // conflict, even when one addition itself has zero bytes.
    (void)split_lines(text_view(ours));
    (void)split_lines(text_view(theirs));
    Bytes result;
    auto append = [&](std::string_view text) {
        if (text.size() > max_file_size - result.size()) throw Error("合并输出超过 64 MiB 限制");
        if (!text.empty()) {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(text.data());
            result.insert(result.end(), begin, begin + text.size());
        }
    };
    auto display = [&](std::span<const std::uint8_t> content) {
        append(text_view(content));
        if (!content.empty() && content.back() != '\n') append("\n");
    };
    append("<<<<<<< ours\n");
    display(ours);
    append("=======\n");
    display(theirs);
    append(">>>>>>> theirs\n");
    return result;
}
}
SnapshotMergeResult merge_snapshots(const Repository& repo, const Snapshot& base,
                                    const Snapshot& ours, const Snapshot& theirs) {
    const std::array<const Snapshot*, 3> inputs{&base, &ours, &theirs};
    // Every schema is checked before object reads, and every existing object
    // before a new immutable Blob can be saved. Payloads are released per read.
    for (const auto* input : inputs) (void)snapshot_index(*input);
    checkout_support::BoundedPaths paths;
    std::set<ObjectId> ids;
    for (const auto* input : inputs)
        for (const auto& [path, file] : *input) { paths.add(path); ids.insert(file.id); }
    ObjectStore store(repo.metadata() / "objects");
    for (const auto& id : ids)
        if (store.read(id).type != ObjectType::Blob) throw Error("合并快照对象不是 blob");
    checkout_support::BoundedPaths possible;
    for (const auto& path : paths.paths) {
        const auto b = state_at(base, path), o = state_at(ours, path), t = state_at(theirs, path);
        const auto choice = choose(b, o, t);
        if (choice.resolved ? choice.file.has_value() : b || o || t) possible.add(path);
    }
    checkout_support::BoundedPaths roots;
    for (const auto& path : possible.paths) {
        for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
            const auto prefix = path.substr(0, slash);
            if (possible.paths.contains(prefix)) { roots.add(prefix); break; }
        }
    }
    SnapshotMergeResult result;
    IndexOutput index(result.index);
    for (const auto& path : paths.paths) {
        const std::array states{state_at(base, path), state_at(ours, path), state_at(theirs, path)};
        const auto& b = states[0];
        const auto& o = states[1];
        const auto& t = states[2];
        if (const auto* root = region_at(roots.paths, path)) {
            index.originals(path, states);
            if (o) result.work.emplace_hint(result.work.end(), path, *o);
            if (path == *root) result.conflicts.push_back({path, MergeConflictKind::DirectoryFile, {}, false});
            continue;
        }
        const auto choice = choose(b, o, t);
        if (choice.resolved) {
            if (choice.file) {
                index.append(path, *choice.file, 0);
                result.work.emplace_hint(result.work.end(), path, *choice.file);
            }
            continue;
        }
        if (!o || !t) {
            index.originals(path, states);
            result.work.emplace_hint(result.work.end(), path, o ? *o : *t);
            result.conflicts.push_back({path, MergeConflictKind::DeleteModify, {}, false});
            continue;
        }
        const auto mode = scalar(b ? std::optional{b->mode} : std::nullopt, o->mode, t->mode);
        const bool mode_conflict = !mode;
        FileState working{mode ? *mode : o->mode, o->id};
        auto content = scalar(b ? std::optional{b->id} : std::nullopt, o->id, t->id);
        std::vector<TextConflict> text;
        auto kind = MergeConflictKind::Mode;
        bool content_conflict = false;
        if (content) working.id = *content;
        else {
            // At most the three payloads for this exact file are live here.
            const auto base_bytes = b ? read_blob(store, b->id) : Bytes{};
            const auto ours_bytes = read_blob(store, o->id);
            const auto theirs_bytes = read_blob(store, t->id);
            if (has_nul(base_bytes) || has_nul(ours_bytes) || has_nul(theirs_bytes)) {
                content_conflict = true;
                kind = MergeConflictKind::Binary;
            } else if (!b) {
                auto marked = add_add_marker(ours_bytes, theirs_bytes);
                text.push_back({0, 0, 0, marked.size()});
                working.id = store.write(ObjectType::Blob, marked);
                content_conflict = true;
                kind = MergeConflictKind::AddAdd;
            } else {
                auto merged = merge_text(base_bytes, ours_bytes, theirs_bytes);
                result.coarse = result.coarse || merged.coarse;
                content_conflict = !merged.conflicts.empty();
                text = std::move(merged.conflicts);
                working.id = store.write(ObjectType::Blob, merged.content);
                kind = content_conflict ? MergeConflictKind::Text : MergeConflictKind::Mode;
            }
        }
        result.work.emplace_hint(result.work.end(), path, working);
        if (content_conflict || mode_conflict) {
            index.originals(path, states);
            result.conflicts.push_back({path, kind, std::move(text), mode_conflict});
        } else index.append(path, working, 0);
    }
    checkout_support::validate_index_budget(result.index);
    (void)encode_index(result.index);
    (void)snapshot_index(result.work); // Also proves the published layout has no D/F.
    return result;
}
}
