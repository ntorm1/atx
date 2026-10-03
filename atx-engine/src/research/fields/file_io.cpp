#include "atx/engine/research/fields/file_io.hpp"

#include <cstdio>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace atx::engine::research::fields {
namespace {

constexpr usize kStreamChunk = 1U << 20;

// Reads `in` to its end through SHA-256, copying every chunk to `out` when it is non-null.
[[nodiscard]] core::Result<FileDigest> stream_digest(std::ifstream &in, std::FILE *out,
                                                     const std::filesystem::path &path) {
  core::Sha256 sha;
  std::vector<char> buffer(kStreamChunk);
  FileDigest digest;
  while (in) { // bounded by the file's size: each pass consumes up to one chunk
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) {
      break;
    }
    const auto n = static_cast<usize>(got);
    ATX_TRY_VOID(sha.update(std::as_bytes(std::span<const char>(buffer.data(), n))));
    if (out != nullptr && std::fwrite(buffer.data(), 1U, n, out) != n) {
      return core::Err(core::ErrorCode::IoError,
                       "research fields: copy of " + path.string() + " failed to write");
    }
    digest.bytes += static_cast<u64>(n);
  }
  if (in.bad()) {
    return core::Err(core::ErrorCode::IoError, "research fields: cannot read " + path.string());
  }
  ATX_TRY(const auto hash, sha.finalize());
  digest.sha256 = hex_digest(hash);
  return core::Ok(std::move(digest));
}

// fsync of an open stream's file (its bytes reach the device before the publishing link).
[[nodiscard]] bool sync_file(std::FILE *file) noexcept {
#if defined(_WIN32)
  return _commit(_fileno(file)) == 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

} // namespace

std::string hex_digest(const std::array<std::byte, 32> &digest) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (usize i = 0; i < digest.size(); ++i) {
    const auto b = std::to_integer<unsigned>(digest[i]);
    out[2 * i] = kDigits[b >> 4U];
    out[2 * i + 1] = kDigits[b & 15U];
  }
  return out;
}

core::Result<std::string> read_bounded(const std::filesystem::path &path, u64 max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return core::Err(core::ErrorCode::IoError, "research fields: cannot stat " + path.string());
  }
  if (static_cast<u64>(size) > max_bytes) {
    return core::Err(core::ErrorCode::InvalidArgument, "research fields: " + path.string() +
                                                           " is larger than " +
                                                           std::to_string(max_bytes) + " bytes");
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "research fields: cannot open " + path.string());
  }
  std::string bytes(static_cast<usize>(size), '\0');
  if (!bytes.empty()) {
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<u64>(in.gcount()) != static_cast<u64>(bytes.size())) {
      return core::Err(core::ErrorCode::IoError, "research fields: short read of " + path.string());
    }
  }
  if (in.peek() != std::char_traits<char>::eof()) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: " + path.string() + " grew while read");
  }
  return core::Ok(std::move(bytes));
}

std::string record_path(const std::filesystem::path &path) {
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return canonical.string();
  }
  const auto absolute = std::filesystem::absolute(path, ec);
  return ec ? path.string() : absolute.lexically_normal().string();
}

std::FILE *open_exclusive(const std::filesystem::path &path) noexcept {
  std::FILE *file = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&file, path.c_str(), L"wbx") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wbx");
#endif
  return file;
}

core::Status write_exclusive(const std::filesystem::path &path, std::string_view bytes) {
  std::FILE *file = open_exclusive(path);
  if (file == nullptr) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: cannot create (exists or not writable) " + path.string());
  }
  const bool written = std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size();
  const bool flushed = std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!written || !flushed || !closed) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: write failed for " + path.string());
  }
  return core::Ok();
}

core::Result<FileStamp> file_stamp(const std::filesystem::path &path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return core::Err(core::ErrorCode::IoError, "research fields: cannot stat " + path.string());
  }
  const auto written = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return core::Err(core::ErrorCode::IoError, "research fields: cannot stat " + path.string());
  }
  return core::Ok(
      FileStamp{static_cast<u64>(size), static_cast<i64>(written.time_since_epoch().count())});
}

core::Result<FileDigest> digest_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "research fields: cannot open " + path.string());
  }
  return stream_digest(in, nullptr, path);
}

core::Result<FileDigest> copy_exclusive(const std::filesystem::path &from,
                                        const std::filesystem::path &to) {
  std::ifstream in(from, std::ios::binary);
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "research fields: cannot open " + from.string());
  }
  std::FILE *file = open_exclusive(to);
  if (file == nullptr) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: cannot create (exists or not writable) " + to.string());
  }
  auto copied = stream_digest(in, file, from);
  const bool flushed = std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!copied) {
    return core::Err(copied.error());
  }
  if (!flushed || !closed) {
    return core::Err(core::ErrorCode::IoError, "research fields: write failed for " + to.string());
  }
  return copied;
}

core::Status publish_exclusive(const std::filesystem::path &path, std::string_view bytes) {
  const std::filesystem::path pending =
      path.parent_path() / ("." + path.filename().string() + ".pending");
  std::FILE *file = open_exclusive(pending);
  if (file == nullptr) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: cannot create (exists or not writable) " + pending.string());
  }
  const bool written = std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size();
  const bool synced = std::fflush(file) == 0 && sync_file(file);
  const bool closed = std::fclose(file) == 0;
  if (!written || !synced || !closed) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: write failed for " + pending.string());
  }
  std::error_code ec;
  std::filesystem::create_hard_link(pending, path, ec);
  if (ec) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: cannot publish (exists or not linkable) " + path.string());
  }
  std::filesystem::remove(pending, ec);
  if (ec) {
    return core::Err(core::ErrorCode::IoError, "research fields: published " + path.string() +
                                                   " but cannot remove " + pending.string());
  }
  return core::Ok();
}

} // namespace atx::engine::research::fields
