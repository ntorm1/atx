#pragma once

// atx::tsdb::crc32 — CRC-32 (IEEE 802.3 / zlib polynomial 0xEDB88320), table
// generated at compile time. Byte-wise, so the digest is endianness-independent
// and stable across processes/platforms — the property the segment integrity
// check and content_hash require (atx-core hash_bytes disclaims cross-restart
// stability, so it is unsuitable here).

#include <array>

#include "atx/core/types.hpp"

namespace atx::tsdb {

namespace detail {

/// Build the 256-entry CRC-32 lookup table at compile time.
[[nodiscard]] constexpr std::array<atx::u32, 256> make_crc32_table() noexcept {
  std::array<atx::u32, 256> table{};
  for (atx::u32 n = 0; n < 256U; ++n) {
    atx::u32 c = n;
    for (atx::u32 k = 0; k < 8U; ++k) {
      c = ((c & 1U) != 0U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    table[n] = c;
  }
  return table;
}

inline constexpr std::array<atx::u32, 256> kCrc32Table = make_crc32_table();

} // namespace detail

/// CRC-32 of a byte buffer. `data` may be nullptr iff `len == 0`.
[[nodiscard]] inline atx::u32 crc32(const void *data, atx::usize len) noexcept {
  // SAFETY: byte cursor over the buffer; len bounds the read. No write, no alias.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *p = static_cast<const atx::u8 *>(data);
  atx::u32 c = 0xFFFFFFFFU;
  for (atx::usize i = 0; i < len; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)
    c = detail::kCrc32Table[(c ^ p[i]) & 0xFFU] ^ (c >> 8U);
  }
  return c ^ 0xFFFFFFFFU;
}

} // namespace atx::tsdb
