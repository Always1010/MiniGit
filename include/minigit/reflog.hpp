#pragma once
#include "minigit/refs.hpp"
#include "minigit/transaction.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace minigit {
struct ReflogEntry {
    std::optional<ObjectId> old_id;
    std::optional<ObjectId> new_id;
    std::string reason;
    bool operator==(const ReflogEntry&) const = default;
};
using Reflog = std::vector<ReflogEntry>;
// MGRL v1: at most 100000 entries, 8 MiB encoded; reason is single-line UTF-8,
// nonempty, at most 4096 bytes, and excludes ASCII controls and DEL.
Bytes encode_reflog(const Reflog& entries);
Reflog decode_reflog(std::span<const std::uint8_t> bytes);
// Caller holds RepoLock. ref is HEAD or a complete refs/heads/... name.
// Planning never writes disk; exceptions leave the supplied plan unchanged.
Reflog read_reflog(const Repository& repo, std::string_view ref);
void plan_append_reflog(const Repository& repo, TransactionPlan& plan,
                        std::string_view ref, const ReflogEntry& entry);
void plan_move_head(const Repository& repo, TransactionPlan& plan,
                    const Head& expected, const Head& desired, std::string_view reason);
}
