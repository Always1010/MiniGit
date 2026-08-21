#pragma once
#include "minigit/transaction.hpp"
namespace minigit::transaction_detail {
enum class State : std::uint8_t { Prepared = 0, Committed = 1 };
struct StoredMetadata {
    std::string path;
    std::optional<ObjectId> before, after;
};
struct Journal {
    State state = State::Prepared;
    TransactionPlan plan; // Codec fills Work; coordinator loads metadata payloads.
    std::vector<StoredMetadata> metadata;
};
// MGTR v1 stores Work images and metadata Blob IDs, never live filesystem data.
// The coordinator validates the complete plan after loading immutable payloads.
Bytes encode_journal(const Journal& journal);
Journal decode_journal(std::span<const std::uint8_t> bytes);
}
