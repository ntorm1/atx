#pragma once

// atx::engine::research::fields -- bounded whole-file reads and the path text a source record
// carries. Cold path helpers of the field builders; nothing here throws.

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

// Lower-case hex of a SHA-256 digest.
[[nodiscard]] std::string hex_digest(const std::array<std::byte, 32> &digest);

// Every byte of `path` (binary). Err(IoError) when it cannot be opened or read,
// Err(InvalidArgument) when it is larger than `max_bytes`.
[[nodiscard]] core::Result<std::string> read_bounded(const std::filesystem::path &path,
                                                     u64 max_bytes);

// The absolute, normalised path text a source record names (symlinks resolved where the path
// exists).
[[nodiscard]] std::string record_path(const std::filesystem::path &path);

// Creates `path` for binary writing exclusively (C11 "x" mode: CREATE_NEW / O_EXCL, so an existing
// file is never truncated); nullptr when it exists or cannot be created. The caller closes it.
[[nodiscard]] std::FILE *open_exclusive(const std::filesystem::path &path) noexcept;

// Creates `path` exclusively and writes `bytes`, flushed and closed. Err(IoError) when it exists or
// cannot be written.
[[nodiscard]] core::Status write_exclusive(const std::filesystem::path &path,
                                           std::string_view bytes);

// (size, last write time) of a file: captured before a read and compared after it, as the Python
// builder's identity() check, so a file replaced during a build is refused. Err(IoError) when it
// cannot be stat'ed.
struct FileStamp {
  u64 bytes{};
  i64 write_time_ticks{};
  friend bool operator==(const FileStamp &, const FileStamp &) = default;
};
[[nodiscard]] core::Result<FileStamp> file_stamp(const std::filesystem::path &path);

} // namespace atx::engine::research::fields
