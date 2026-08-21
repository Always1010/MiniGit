#include "transaction_metadata.hpp"
#include "minigit/error.hpp"
#include "minigit/index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/operation.hpp"
#include "minigit/refs.hpp"
#include "minigit/reflog.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/stash_stack.hpp"
namespace minigit::transaction_detail {
void validate_metadata_path(std::string_view path) {
    validate_repo_path_limits(path);
    if (path == "index" || path == "HEAD" || path == "refs/stash" || path == "logs/HEAD" || path == "operation" || path == "stashstack") return;
    if (path.starts_with("refs/heads/") || path.starts_with("refs/tags/")) { validate_ref_name(path); return; }
    if (path.starts_with("logs/refs/heads/")) { validate_ref_name(path.substr(5)); return; }
    throw Error("事务元数据路径不在白名单中");
}
void validate_metadata_bytes(const Repository& repo, const MetadataChange& change, const Bytes& bytes,
                             std::set<ObjectId>* roots) {
    if (change.path == "operation") {
        const auto semantic = state_roots(repo, decode_operation(bytes));
        if (roots) roots->insert(semantic.begin(), semantic.end());
        return;
    }
    if (change.path == "stashstack") {
        const auto semantic = stash_roots(repo, decode_stash_stack(bytes));
        if (roots) roots->insert(semantic.begin(), semantic.end());
        return;
    }
    const ObjectStore store(repo.metadata() / "objects");
    auto keep = [&](const ObjectId& id, ObjectType type) {
        const auto object = store.read(id);
        if (object.type != type) throw Error("事务元数据引用的对象类型非法");
        if (type == ObjectType::Commit) (void)read_snapshot(repo, decode_commit(object.payload).tree);
        if (roots) roots->insert(id);
    };
    if (change.path == "index") {
        for (const auto& entry : decode_index(bytes)) keep(entry.id, ObjectType::Blob);
        return;
    }
    if (change.path.starts_with("logs/")) {
        std::set<ObjectId> checked;
        for (const auto& entry : decode_reflog(bytes)) {
            for (const auto* id : {&entry.old_id, &entry.new_id}) if (*id && checked.insert(**id).second) {
                const auto object = store.read(**id);
                if (object.type != ObjectType::Commit) throw Error("reflog 必须引用 Commit");
                (void)decode_commit(object.payload);
                if (roots) roots->insert(**id);
            }
        }
        return;
    }
    const auto text = to_string(bytes);
    if (change.path == "HEAD" && text.starts_with("ref: ")) {
        if (text.empty() || text.back() != '\n') throw Error("事务 HEAD 格式非法");
        const auto ref = std::string_view(text).substr(5, text.size() - 6);
        validate_ref_name(ref);
        if (!ref.starts_with("refs/heads/")) throw Error("事务 HEAD 必须指向分支");
        if (const auto id = read_ref(repo, ref)) keep(*id, ObjectType::Commit);
        return;
    }
    if (text.size() != 65 || text.back() != '\n') throw Error("事务引用格式非法");
    const auto id = ObjectId::from_hex(std::string_view(text).substr(0, 64));
    if (change.path == "refs/stash") {
        const auto semantic = stash_roots(repo, {id});
        if (roots) roots->insert(semantic.begin(), semantic.end());
    }
    keep(id, ObjectType::Commit);
}
void validate_stash_pair(const TransactionPlan& plan) {
    const MetadataChange* stack = nullptr;
    const MetadataChange* ref = nullptr;
    for (const auto& change : plan.metadata) {
        if (change.path == "stashstack") stack = &change;
        if (change.path == "refs/stash") ref = &change;
    }
    if (!stack) return; // Preserve the existing independent ref primitive.
    if (!ref) throw Error("stashstack 事务必须同时记录 refs/stash");
    for (const bool after : {false, true}) {
        const auto& stack_bytes = after ? stack->after : stack->before;
        const auto& ref_bytes = after ? ref->after : ref->before;
        const auto entries = stack_bytes ? decode_stash_stack(*stack_bytes) : StashStack{};
        if (entries.empty()) {
            if (ref_bytes) throw Error("事务空 stash 栈不能带引用");
        } else {
            if (!ref_bytes) throw Error("事务 stash 栈缺少栈顶引用");
            const auto text = to_string(*ref_bytes);
            if (text.size() != 65 || text.back() != '\n' ||
                ObjectId::from_hex(std::string_view(text).substr(0, 64)) != entries.front())
                throw Error("事务 stash 栈顶与引用不一致");
        }
    }
}
}
