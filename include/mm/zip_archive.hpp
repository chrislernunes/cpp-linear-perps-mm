#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm {

struct ZipFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

// Reads stored and deflated entries. Does not invent missing members.
std::vector<ZipFile> unzip(const std::uint8_t* data, std::size_t size);
std::vector<ZipFile> unzip_file(const std::string& path);

}  // namespace mm
