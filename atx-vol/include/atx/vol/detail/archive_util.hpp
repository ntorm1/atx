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
#include <utility>

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

// ── Whole-file slurp ────────────────────────────────────────────────────────
//
// ONE implementation, because there were two: `read_listed_definitions_file`
// and `read_listed_definitions_cached` carried the same eight lines verbatim,
// including both error strings (Wave E T7 review I2), so any change to the read
// had to be made twice or the two paths would silently diverge in cost.
//
// It is an `fread` of `file_size` bytes into a pre-sized buffer, NOT
// `std::string(istreambuf_iterator, istreambuf_iterator)`. The iterator form
// pulls one byte at a time through `sgetc`/`sbumpc` and grows the string
// geometrically; on a 730 MB definitions.tsv from page cache it measured
// ~197 MB/s, roughly an order of magnitude below what one sized read achieves.
//
// BYTE-IDENTICAL BY CONSTRUCTION to the form it replaces: the streams it
// replaces were already opened `std::ios::binary`, so there was no CRLF
// translation to lose, and `"rb"` reproduces exactly the same bytes.
//
// The size is a HINT, not a contract — the file may grow or shrink between the
// stat and the read. A short read that is not an error truncates to what was
// actually read; anything past the hinted size is drained in chunks. So the
// result is always "the file's bytes at EOF", the same postcondition the
// iterator form had.
//
// Returns a discriminated status rather than an error string so each caller can
// keep its OWN message: `read_listed_definitions_file`'s two strings are pinned
// by ~40 assertions in Wave E Task 4's tests and must not be reworded.
enum class FileReadStatus : std::uint8_t {
  Ok = 0,
  NotFound = 1, // cannot open (absent, permission, or a directory)
  IoError = 2,  // opened, then the read failed
};

[[nodiscard]] FileReadStatus read_whole_file(std::string_view path, std::string &out);

}  // namespace atx::vol::detail
