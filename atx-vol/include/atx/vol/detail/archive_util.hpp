#pragma once

// Shared low-level helpers for the ATX binary container formats
// (surface_archive.hpp ATXVSA, surface_db.hpp ATXVDB): hardware-accelerated
// CRC-32C, alignment, and canonical symbol normalization. Moved verbatim from
// surface_archive.cpp so both formats share ONE bit-identical implementation.
//
// Thread-safety: CRC/name helpers are pure; publication helpers are thread-safe.
//
// ATOMIC-PUBLISH DISCIPLINE, AND ITS ONE DOCUMENTED EXCEPTION. Every durable
// artifact this file's `reserve_unique_publish_temp_file`/
// `flush_and_publish_file` pair touches follows the same house rule: write a
// unique same-directory temp, flush it, then atomically rename it onto the
// destination -- a reader never observes a partially-written file. The
// exception is a SQLite-managed database file (Task D3's `catalog.sqlite`,
// research/catalog.hpp): SQLite owns that file's on-disk durability and
// crash-consistency itself, via its own WAL journal (`PRAGMA
// journal_mode=WAL`) -- there is no "write a temp copy of the live database
// and rename it in" equivalent for a file a running SQLite connection has
// open and is actively journaling against. Durability there is delegated
// entirely to SQLite's own mechanisms (the WAL journal plus `synchronous=
// NORMAL`), and a reader's equivalent of "validate before trusting" is
// SQLite's own integrity checking (`PRAGMA integrity_check`, or simply that
// every read goes through a real SQLite connection, which refuses to open a
// database it cannot parse) rather than this file's temp-then-rename
// primitives. Nothing in `research/catalog.cpp` calls into
// `reserve_unique_publish_temp_file`/`flush_and_publish_file` for this
// reason -- not an oversight, the documented exception.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "atx/core/error.hpp" // Result / Status (durable publish)

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

// Atomically reserve a unique, empty temporary file in `dst_path`'s directory.
// Callers overwrite the reserved file, close it, and pass it to
// flush_and_publish_file. Keeping the temp beside the destination is required
// for atomic rename. A failed reservation leaves no file behind.
[[nodiscard]] atx::core::Result<std::string>
reserve_unique_publish_temp_file(std::string_view dst_path);

// Durable atomic publish of a just-written temp file onto its destination — the
// shared primitive behind write_surface_archive_v2_file and the SurfaceDb
// write_manifest_file_atomic. The caller writes the payload to `tmp_path`
// and closes its stream; this then:
//   (1) fsync's `tmp_path` to stable storage BEFORE the rename, so a power loss
//       after the rename can never leave a correctly-named file with unflushed
//       content — which would have destroyed the prior good version (SE-P2-1); and
//   (2) renames `tmp_path` -> `dst_path` with bounded retry + exponential backoff,
//       since on Windows the rename fails while a reader holds `dst_path` open
//       without FILE_SHARE_DELETE (MSVC ifstream / mmap); a few backed-off retries
//       let a concurrent reader finish (SE-P2-2).
// Same-destination calls are serialized within this process. On POSIX the
// destination's parent directory is fsync'd after rename, making the directory
// entry durable as well as the file contents.
// On final failure the temp is PRESERVED (not deleted) so the freshly written
// bytes are recoverable, and IoError is returned.
[[nodiscard]] atx::core::Status flush_and_publish_file(std::string_view tmp_path,
                                                       std::string_view dst_path);

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
