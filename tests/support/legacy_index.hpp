#pragma once
#include "minigit/index.hpp"
#include <algorithm>
namespace minigit::test {
// Authenticated old-format fixture. New encoders intentionally reject its path.
inline Bytes legacy_index_entry(IndexEntry entry) {
    const auto path = entry.path;
    entry.path = "p";
    auto bytes = encode_index({entry});
    bytes.erase(bytes.begin() + 16);
    bytes.insert(bytes.begin() + 16, path.begin(), path.end());
    for (unsigned byte = 0; byte < 4; ++byte)
        bytes[12 + byte] = static_cast<std::uint8_t>(path.size() >> (24 - byte * 8));
    const auto checksum = sha256(std::span<const std::uint8_t>(bytes).first(bytes.size() - 32));
    std::copy(checksum.bytes().begin(), checksum.bytes().end(), bytes.end() - 32);
    return bytes;
}
}
