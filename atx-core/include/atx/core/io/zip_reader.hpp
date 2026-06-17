#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core::io {

class ZipEntryReader {
public:
  // Open `zip_path`; select the first entry whose name CONTAINS `entry_name_substr`
  // (empty => the first non-directory entry). Err(IoError) if the zip cannot be
  // opened; Err(NotFound) if no matching entry; Err(ParseError) on a corrupt central
  // directory or a failed iterator init.
  [[nodiscard]] static atx::core::Result<ZipEntryReader>
  open(std::string_view zip_path, std::string_view entry_name_substr = {});

  ZipEntryReader(ZipEntryReader &&) noexcept;
  ZipEntryReader &operator=(ZipEntryReader &&) noexcept;
  ZipEntryReader(const ZipEntryReader &) = delete;
  ZipEntryReader &operator=(const ZipEntryReader &) = delete;
  ~ZipEntryReader();

  // Fill `dst` with the next decompressed bytes. Returns the count written
  // (0 == end of entry). Err(ParseError) on an inflate/CRC error.
  [[nodiscard]] atx::core::Result<atx::usize> read(std::span<char> dst);

  [[nodiscard]] atx::u64 uncompressed_size() const noexcept;
  [[nodiscard]] std::string_view entry_name() const noexcept;

private:
  struct Impl; // owns mz_zip_archive + iter state (miniz confined here)
  explicit ZipEntryReader(std::unique_ptr<Impl> p) noexcept;
  std::unique_ptr<Impl> p_;
};

} // namespace atx::core::io
