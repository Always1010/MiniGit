#include "minigit/object_id.hpp"
#include "minigit/error.hpp"
#include <openssl/evp.h>
#include <algorithm>
namespace minigit {
ObjectId ObjectId::from_hex(std::string_view text) {
    if (text.size() != 64) throw Error("对象 ID 必须是 64 位十六进制");
    ObjectId id;
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        throw Error("对象 ID 含有非法十六进制字符");
    };
    for (std::size_t i = 0; i < 32; ++i)
        id.bytes_[i] = static_cast<std::uint8_t>((nibble(text[i * 2]) << 4U) | nibble(text[i * 2 + 1]));
    return id;
}
ObjectId ObjectId::from_bytes(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 32) throw Error("对象 ID 必须是 32 字节");
    ObjectId id;
    std::copy(bytes.begin(), bytes.end(), id.bytes_.begin());
    return id;
}
std::string ObjectId::hex() const {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t i = 0; i < 32; ++i) {
        result[i * 2] = digits[bytes_[i] >> 4U];
        result[i * 2 + 1] = digits[bytes_[i] & 15U];
    }
    return result;
}
ObjectId sha256(std::span<const std::uint8_t> bytes) {
    std::array<std::uint8_t, 32> digest{};
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32)
        throw Error("无法计算 SHA-256");
    return ObjectId::from_bytes(digest);
}
}
