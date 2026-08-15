#include "minigit/object_store.hpp"
#include "minigit/error.hpp"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <charconv>
namespace minigit {
namespace {
std::size_t compressed_object_limit() {
    return static_cast<std::size_t>(::compressBound(static_cast<uLong>(max_file_size)));
}
}
ObjectStore::ObjectStore(std::filesystem::path objects_directory) : objects_directory_(std::move(objects_directory)) {
    detail::require_directory(objects_directory_);
}
std::filesystem::path ObjectStore::object_path(const ObjectId& id) const {
    auto hex = id.hex();
    return objects_directory_ / hex.substr(0, 2) / hex.substr(2);
}
ObjectId ObjectStore::write(ObjectType type, std::span<const std::uint8_t> payload) {
    const auto id = hash_object(type, payload);
    if (type == ObjectType::Tree) (void)decode_tree(payload);
    else if (type == ObjectType::Commit) (void)decode_commit(payload);
    const auto path = object_path(id);
    detail::ensure_directory(path.parent_path());
    detail::reject_symlinks(path);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        const auto existing = read(id);
        if (existing.type != type || existing.payload.size() != payload.size() || !std::equal(existing.payload.begin(), existing.payload.end(), payload.begin()))
            throw Error("已有对象与预期内容不符");
        // A previous rename may have succeeded before directory fsync failed.
        detail::sync_directory(path.parent_path());
        detail::sync_directory(objects_directory_);
        return id;
    }
    if (ec) throw Error("检查对象失败: " + ec.message());
    auto framed = to_bytes(std::string(object_type_name(type)) + " " + std::to_string(payload.size()));
    framed.push_back(0);
    framed.insert(framed.end(), payload.begin(), payload.end());
    uLongf length = ::compressBound(static_cast<uLong>(framed.size()));
    Bytes compressed(static_cast<std::size_t>(length));
    if (::compress2(compressed.data(), &length, framed.data(), static_cast<uLong>(framed.size()), Z_DEFAULT_COMPRESSION) != Z_OK)
        throw Error("压缩对象失败");
    compressed.resize(static_cast<std::size_t>(length));
    atomic_write(path, compressed, compressed_object_limit());
    detail::sync_directory(objects_directory_);
    return id;
}
Object ObjectStore::read(const ObjectId& id) const {
    auto compressed = read_file(object_path(id), compressed_object_limit());
    if (compressed.empty()) throw Error("压缩对象为空");
    z_stream stream{};
    stream.next_in = compressed.data();
    stream.avail_in = static_cast<uInt>(compressed.size());
    if (::inflateInit(&stream) != Z_OK) throw Error("初始化解压失败");
    struct End { z_stream* stream; ~End() { ::inflateEnd(stream); } } end{&stream};
    Bytes framed;
    std::array<std::uint8_t, 16384> buffer{};
    for (;;) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        const auto before = stream.total_in;
        int status = ::inflate(&stream, Z_NO_FLUSH);
        std::size_t count = buffer.size() - stream.avail_out;
        if (count > max_file_size - framed.size()) throw Error("解压对象超过 64 MiB 限制");
        framed.insert(framed.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
        if (status == Z_STREAM_END) { if (stream.avail_in != 0) throw Error("压缩对象包含尾部数据"); break; }
        if (status != Z_OK || (count == 0 && stream.total_in == before)) throw Error("对象压缩数据损坏或截断");
    }
    if (sha256(framed) != id) throw Error("对象哈希校验失败");
    const auto zero = std::find(framed.begin(), framed.end(), 0);
    if (zero == framed.end()) throw Error("对象缺少头部终止符");
    const auto header_size = static_cast<std::size_t>(zero - framed.begin());
    const auto header = to_string(std::span(framed).first(header_size));
    const auto space = header.find(' ');
    if (space == std::string::npos) throw Error("对象头格式非法");
    auto type = parse_object_type(std::string_view(header).substr(0, space));
    const auto size_text = std::string_view(header).substr(space + 1);
    if (size_text.empty() || (size_text.size() > 1 && size_text[0] == '0')) throw Error("对象长度格式非法");
    std::size_t size = 0;
    const auto parsed = std::from_chars(size_text.data(), size_text.data() + size_text.size(), size);
    if (parsed.ec != std::errc{} || parsed.ptr != size_text.data() + size_text.size() || size != framed.size() - header_size - 1)
        throw Error("对象长度校验失败");
    Object object{type, Bytes(zero + 1, framed.end())};
    if (type == ObjectType::Tree) (void)decode_tree(object.payload);
    else if (type == ObjectType::Commit) (void)decode_commit(object.payload);
    return object;
}
}
