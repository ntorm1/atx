#pragma once

// atx::engine::research::fields -- bounded whole-file reads, the path text a source record
// carries, streamed digests and copies, and the publish-last write of a manifest. Cold path helpers
// of the field builders; nothing here throws.

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

// The size and SHA-256 (lower-case hex) of a file's bytes.
struct FileDigest {
  u64 bytes{};
  std::string sha256;
};

// Streams `path` through SHA-256. Err(IoError) when it cannot be opened or read.
[[nodiscard]] core::Result<FileDigest> digest_file(const std::filesystem::path &path);

// Copies `from` to `to` (created exclusively: an existing `to` is never replaced) and returns the
// digest of the bytes copied, flushed and closed. Err(IoError) on any failure (a partial `to` may
// remain; no manifest names it).
[[nodiscard]] core::Result<FileDigest> copy_exclusive(const std::filesystem::path &from,
                                                      const std::filesystem::path &to);

// Publish-last (the Python builder's publish()): writes `bytes` to `.<name>.pending` beside `path`
// (created exclusively), flushes and fsyncs it, hard-links it to `path` (refused when `path`
// exists) and removes the pending name, so a reader sees no `path` or its complete synced bytes.
// Err(IoError) on any failure.
[[nodiscard]] core::Status publish_exclusive(const std::filesystem::path &path,
                                             std::string_view bytes);

} // namespace atx::engine::research::fields
