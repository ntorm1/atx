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

#include "atx/core/error.hpp" // Status (durable publish)

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

// Durable atomic publish of a just-written temp file onto its destination — the
// shared primitive behind write_surface_archive_v2_file / write_surface_archive_file
// (v1) / write_manifest_file_atomic. The caller writes the payload to `tmp_path`
// and closes its stream; this then:
//   (1) fsync's `tmp_path` to stable storage BEFORE the rename, so a power loss
//       after the rename can never leave a correctly-named file with unflushed
//       content — which would have destroyed the prior good version (SE-P2-1); and
//   (2) renames `tmp_path` -> `dst_path` with bounded retry + exponential backoff,
//       since on Windows the rename fails while a reader holds `dst_path` open
//       without FILE_SHARE_DELETE (MSVC ifstream / mmap); a few backed-off retries
//       let a concurrent reader finish (SE-P2-2).
// On final failure the temp is PRESERVED (not deleted) so the freshly written
// bytes are recoverable, and IoError is returned.
[[nodiscard]] atx::core::Status flush_and_publish_file(std::string_view tmp_path,
                                                       std::string_view dst_path);

}  // namespace atx::vol::detail
