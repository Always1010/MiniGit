#pragma once
#include "minigit/transaction.hpp"
namespace minigit::transaction_detail {
// Whitelist, typed payload schemas and the paired stash stack/ref invariant.
void validate_metadata_path(std::string_view path);
void validate_metadata_bytes(const Repository& repo, const MetadataChange& change,
                             const Bytes& bytes, std::set<ObjectId>* roots = nullptr);
void validate_stash_pair(const TransactionPlan& plan);
}
