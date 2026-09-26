#pragma once

#include <filesystem>
#include <string>

namespace retro::runner {

// Lowercase hex SHA-256 of a file's bytes; empty on any I/O error.
std::string file_sha256_hex(const std::filesystem::path& path);

} // namespace retro::runner
