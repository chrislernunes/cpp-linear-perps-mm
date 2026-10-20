#include "mm/zip_archive.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace mm {
namespace {

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

std::vector<std::uint8_t> inflate_raw(const std::uint8_t* data, std::size_t size, std::size_t expected) {
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(size);
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        throw std::runtime_error("inflateInit2 failed");
    std::vector<std::uint8_t> out(expected ? expected : std::max<std::size_t>(size * 4, 64));
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        if (stream.total_out >= out.size())
            out.resize(out.size() * 2 + 64);
        stream.next_out = out.data() + stream.total_out;
        stream.avail_out = static_cast<uInt>(out.size() - stream.total_out);
        rc = inflate(&stream, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&stream);
            throw std::runtime_error("zip deflate failed");
        }
    }
    out.resize(stream.total_out);
    inflateEnd(&stream);
    if (expected && out.size() != expected)
        throw std::runtime_error("zip uncompressed size mismatch");
    return out;
}

}  // namespace

std::vector<ZipFile> unzip(const std::uint8_t* data, std::size_t size) {
    if (size < 22)
        throw std::runtime_error("zip too small");
    std::size_t eocd = size;
    const std::size_t scan = std::min<std::size_t>(size, 22 + 65535);
    for (std::size_t i = 0; i + 22 <= scan; ++i) {
        const std::size_t pos = size - 22 - i;
        if (read_u32(data + pos) == 0x06054b50) {
            eocd = pos;
            break;
        }
    }
    if (eocd == size)
        throw std::runtime_error("zip end-of-central-directory not found");
    const std::uint16_t entries = read_u16(data + eocd + 10);
    const std::uint32_t cd_size = read_u32(data + eocd + 12);
    const std::uint32_t cd_offset = read_u32(data + eocd + 16);
    if (static_cast<std::size_t>(cd_offset) + cd_size > size)
        throw std::runtime_error("zip central directory out of range");
    std::vector<ZipFile> files;
    std::size_t cursor = cd_offset;
    for (std::uint16_t n = 0; n < entries; ++n) {
        if (cursor + 46 > size || read_u32(data + cursor) != 0x02014b50)
            throw std::runtime_error("bad zip central directory");
        const std::uint16_t method = read_u16(data + cursor + 10);
        const std::uint32_t crc = read_u32(data + cursor + 16);
        const std::uint32_t comp_size = read_u32(data + cursor + 20);
        const std::uint32_t uncomp_size = read_u32(data + cursor + 24);
        const std::uint16_t name_len = read_u16(data + cursor + 28);
        const std::uint16_t extra_len = read_u16(data + cursor + 30);
        const std::uint16_t comment_len = read_u16(data + cursor + 32);
        const std::uint32_t local_off = read_u32(data + cursor + 42);
        if (cursor + 46 + name_len > size)
            throw std::runtime_error("zip name out of range");
        ZipFile file;
        file.name.assign(reinterpret_cast<const char*>(data + cursor + 46), name_len);
        if (local_off + 30 > size || read_u32(data + local_off) != 0x04034b50)
            throw std::runtime_error("bad zip local header");
        const std::uint16_t local_name = read_u16(data + local_off + 26);
        const std::uint16_t local_extra = read_u16(data + local_off + 28);
        const std::size_t data_off = static_cast<std::size_t>(local_off) + 30 + local_name + local_extra;
        if (data_off + comp_size > size)
            throw std::runtime_error("zip payload out of range");
        if (!file.name.empty() && file.name.back() != '/') {
            if (method == 0) {
                if (comp_size != uncomp_size)
                    throw std::runtime_error("stored zip size mismatch");
                file.bytes.assign(data + data_off, data + data_off + comp_size);
            } else if (method == 8) {
                file.bytes = inflate_raw(data + data_off, comp_size, uncomp_size);
            } else {
                throw std::runtime_error("unsupported zip method " + std::to_string(method));
            }
            const auto actual = crc32(0L, file.bytes.data(), static_cast<uInt>(file.bytes.size()));
            if (actual != crc)
                throw std::runtime_error("zip crc mismatch for " + file.name);
            files.push_back(std::move(file));
        }
        cursor += 46 + name_len + extra_len + comment_len;
    }
    return files;
}

std::vector<ZipFile> unzip_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("unable to open zip " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return unzip(bytes.data(), bytes.size());
}

}  // namespace mm
