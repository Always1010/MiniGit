#pragma once
#include "minigit/object_id.hpp"
#include "minigit/repository.hpp"
#include <map>
#include <string>
#include <string_view>
namespace minigit {
// These operations acquire RepoLock themselves. Tags refer to complete commits.
std::map<std::string, ObjectId> list_tags(const Repository& repo);
void create_tag(const Repository& repo, std::string_view name,
                std::string_view revision = "HEAD", bool force = false);
void delete_tag(const Repository& repo, std::string_view name);
}
