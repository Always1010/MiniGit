#include "minigit/tags.hpp"
#include "minigit/error.hpp"
#include "minigit/transaction.hpp"
#include "minigit/fs.hpp"
#include "minigit/history.hpp"
#include "minigit/refs.hpp"
namespace minigit {
namespace {
constexpr std::string_view tag_prefix = "refs/tags/";
std::string tag_ref(std::string_view name) {
    if (name == "HEAD" || name.starts_with("refs/") ||
        (name.size() == 64 && name.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos))
        throw Error("标签名称与修订标识冲突");
    const auto ref = std::string(tag_prefix) + std::string(name);
    validate_ref_name(ref);
    return ref;
}
}
std::map<std::string, ObjectId> list_tags(const Repository& repo) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    std::map<std::string, ObjectId> result;
    for (const auto& [ref, id] : list_refs(repo, tag_prefix)) {
        const auto name = ref.substr(tag_prefix.size());
        (void)tag_ref(name);
        (void)read_snapshot(repo, read_commit(repo, id).tree);
        result.emplace(name, id);
    }
    return result;
}
void create_tag(const Repository& repo, std::string_view name, std::string_view revision, bool force) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    const auto ref = tag_ref(name);
    const auto existing = read_ref(repo, ref);
    if (existing && !force) throw Error("标签已存在，使用 -f 覆盖");
    const auto target = resolve_commit(repo, revision);
    update_ref(repo, ref, existing, target);
}
void delete_tag(const Repository& repo, std::string_view name) {
    RepoLock lock(repo.metadata());
    require_no_transaction(repo);
    const auto ref = tag_ref(name);
    const auto existing = read_ref(repo, ref);
    if (!existing) throw Error("标签不存在");
    update_ref(repo, ref, existing, std::nullopt);
}
}
