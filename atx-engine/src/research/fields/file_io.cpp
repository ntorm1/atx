#include "atx/engine/research/fields/file_io.hpp"

#include <cstdio>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>

namespace atx::engine::research::fields {

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

} // namespace atx::engine::research::fields
