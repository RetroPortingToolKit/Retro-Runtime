#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace retro::runner {

// Lowercase hex SHA-256 of a file's bytes; empty on any I/O error.
std::string file_sha256_hex(const std::filesystem::path& path);

// Lowercase hex SHA-256 of `size` bytes in memory.
std::string sha256_hex(const void* data, std::size_t size);

} // namespace retro::runner
