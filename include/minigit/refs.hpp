#pragma once
#include "minigit/repository.hpp"
#include "minigit/object_id.hpp"
#include <map>
#include <optional>
namespace minigit {
struct Head {
    std::optional<std::string> symbolic;
    std::optional<ObjectId> id;
    bool operator==(const Head&) const = default;
};
void validate_ref_name(std::string_view name);
std::optional<ObjectId> read_ref(const Repository& repo, std::string_view name);
Head read_head(const Repository& repo);
std::map<std::string, ObjectId> list_refs(const Repository& repo, std::string_view prefix);
// Mutations require the caller to hold RepoLock; expected=nullopt means unborn.
// New ref values obey repo_path limits; deletion can remove a legacy deep ref.
void update_ref(const Repository& repo, std::string_view name,
                std::optional<ObjectId> expected, std::optional<ObjectId> desired);
void set_head(const Repository& repo, std::string_view target);
ObjectId resolve_commit(const Repository& repo, std::string_view revision);
}
