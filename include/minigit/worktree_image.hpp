#pragma once
#include "minigit/object_id.hpp"
#include <cstdint>
#include <optional>
#include <string>
namespace minigit {
// Actual worktree images shared by short transactions and paused operations.
// Files carry a Blob ID; directories carry permissions; absence is explicit.
enum class NodeKind { Absent, File, Directory };
struct NodeImage {
    NodeKind kind = NodeKind::Absent;
    std::optional<ObjectId> blob;
    std::uint32_t permissions = 0;
    bool operator==(const NodeImage&) const = default;
};
struct WorkChange {
    std::string path;
    NodeImage before;
    NodeImage after;
};
}
