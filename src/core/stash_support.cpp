#include "stash_support.hpp"
#include "checkout_support.hpp"
#include "minigit/error.hpp"
namespace minigit::stash_support {
// Match write_snapshot's complete file/directory entry and path-data bounds
// without writing Trees. This also validates combined tracked/untracked layout.
void validate_saved_tree(const Snapshot& snapshot) {
    checkout_support::BoundedPaths nodes;
    std::size_t bytes = 0;
    const auto add = [&](const std::string& path, std::size_t name_size, std::size_t overhead) {
        if (nodes.paths.contains(path)) return;
        nodes.add(path);
        if (nodes.paths.size() > 100000) throw Error("stash 保存树超过 100000 条目限制");
        const auto size = path.size() + name_size + overhead;
        if (size > max_file_size - bytes) throw Error("stash 保存树超过 64 MiB 限制");
        bytes += size;
    };
    for (const auto& [path, file] : snapshot) {
        checkout_support::validate_path(path);
        if (file.mode != "100644" && file.mode != "100755") throw Error("stash 文件模式非法");
        const auto child = snapshot.lower_bound(path + "/");
        if (child != snapshot.end() && child->first.starts_with(path + "/")) throw Error("stash 保存树存在文件目录冲突");
        const auto last = path.rfind('/');
        add(path, last == std::string::npos ? path.size() : path.size() - last - 1, 40);
        std::size_t begin = 0;
        for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
            add(path.substr(0, slash), slash - begin, 39);
            begin = slash + 1;
        }
    }
}
Snapshot combined_work(const Snapshot& work, const Snapshot& untracked) {
    Snapshot combined;
    checkout_support::BoundedPaths paths;
    for (const auto* snapshot : {&work, &untracked})
        for (const auto& [path, file] : *snapshot) {
            paths.add(path);
            if (paths.paths.size() > 100000) throw Error("stash Work 与 U 超过 100000 文件限制");
            if (!combined.emplace(path, file).second) throw Error("stash Work 与 U 路径重复");
        }
    validate_saved_tree(combined);
    return combined;
}
SavedEntry load_saved_entry(const Repository& repo, ObjectId work) {
    const auto entry = read_stash_entry(repo, work);
    SavedEntry saved{entry, read_snapshot(repo, read_commit(repo, entry.head).tree),
        read_snapshot(repo, read_commit(repo, entry.index).tree), read_snapshot(repo, read_commit(repo, entry.work).tree),
        entry.untracked ? read_snapshot(repo, read_commit(repo, *entry.untracked).tree) : Snapshot{}};
    for (const auto* snapshot : {&saved.head, &saved.index, &saved.work, &saved.untracked}) validate_saved_tree(*snapshot);
    (void)combined_work(saved.work, saved.untracked);
    return saved;
}
}
