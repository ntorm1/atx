#pragma once

// Shared low-level helpers for the ATX binary container formats
// (surface_archive.hpp ATXVSA, surface_db.hpp ATXVDB): hardware-accelerated
// CRC-32C, alignment, and canonical symbol normalization. Moved verbatim from
// surface_archive.cpp so both formats share ONE bit-identical implementation.
//
// Thread-safety: all functions are pure / touch no shared mutable state.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace atx::vol::detail {

// Continue a CRC-32C (Castagnoli) over [p, p+n). `crc` is the running
// (un-finalized) state. Runtime-dispatched: SSE4.2 `_mm_crc32` when available,
// table-driven fallback otherwise — bit-identical outputs.
[[nodiscard]] std::uint32_t crc32c_update(std::uint32_t crc, const std::byte* p,
                                          std::size_t n) noexcept;

// One-shot CRC-32C with the standard init/final XOR applied.
[[nodiscard]] std::uint32_t crc32c(const std::byte* p, std::size_t n) noexcept;

[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t v, std::uint64_t a) noexcept {
  return (v + (a - 1u)) & ~(a - 1u);
}

// Canonical symbol: ASCII upper-cased, truncated to `max_len` bytes. Extracted
// verbatim from surface_archive.cpp — archive lookup keys and surface_db
// manifest keys MUST agree.
[[nodiscard]] std::string canonicalize_symbol(std::string_view s, std::size_t max_len = 32);

}  // namespace atx::vol::detail
