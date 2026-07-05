#pragma once

// Fitted-surface archive (ATSVSA v2) — an on-disk binary container that packs
// many fitted `VolSurface`s into one file (or one byte buffer) with O(1)
// symbol lookup.
//
// Ported from the C17 `ats-vol` library (ats_vol_surface_archive.h/.c). The
// on-disk shape mirrors the C format:
//
//   header (464 B) -> lookup table -> directory -> 4096-aligned surface blobs
//
// The lookup table is an open-addressed hash table (symbol_hash -> slot); a
// caller resolves one symbol without scanning the archive. The directory is
// sorted by canonical symbol for deterministic layout and whole-archive
// iteration. Integrity is layered: a CRC-32C over the header (with its own
// checksum field zeroed), a CRC-32C over the metadata block (lookup ‖
// directory), and a CRC-32C over each surface blob.
//
// ── Port scope (READ THIS) ───────────────────────────────────────────────
// The C v2 blob additionally serialized a yield/forward/dividend curve set, a
// TLV profile image, and per-side American-correction caches. None of those
// types are ported into atx-vol yet — `VolSurface` carries only the fitted
// slices (eSSVI / raw-SVI), the uid, the fit timestamp, and the calibration
// diagnostics. This archive therefore serializes exactly that surface state.
// The header/lookup/directory/blob-header framing, the magic strings, the
// endian + pointer-bits guards, the sizeof-based schema hash, and the CRC-32C
// integrity chain are ported faithfully; a byte layout section with no backing
// atx-vol type (curves/profile/corrections) is simply not emitted. See
// surface_archive.cpp for the byte offsets.
//
// ── Schema hash ──────────────────────────────────────────────────────────
// The header stores a compile-time fingerprint folded from the `sizeof` of the
// on-disk records and the serialized slice structs. A reader built against a
// different struct shape (a "pre-format" or drifted archive) recomputes a
// different hash and refuses the file with a ParseError, rather than silently
// mis-interpreting bytes.
//
// ── Endianness ───────────────────────────────────────────────────────────
// Records are written in host byte order and the header stamps `endian = 1`
// (little). A reader rejects any file whose `endian`/`pointer_bits` do not
// match. Like the C, only little-endian LP64 hosts are supported; the format
// is not byte-portable across endianness.
//
// ── Thread safety ────────────────────────────────────────────────────────
// A parsed `SurfaceArchive` is immutable after `open`. `count`, `header`,
// `directory`, `find`, `map_symbol`, and `map_all` are all `const` and touch
// no shared mutable state, so any number of threads may query one `const`
// archive concurrently (mirrors the C's "many readers" contract and
// test_surface_archive_parallel.c). Each `map_symbol` returns a fresh,
// independently-owned `VolSurface` copy — there is no aliasing between the
// archive's bytes and the returned surface.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "atx/vol/types.hpp"
#include "atx/vol/vol_surface.hpp"

namespace atx::vol {

// ── On-wire format constants ─────────────────────────────────────────────

inline constexpr std::uint16_t kArchiveMajor = 2;
inline constexpr std::uint16_t kArchiveMinor = 0;

// Maximum canonical-symbol length stored inline in the lookup slot / directory
// entry. Longer symbols are truncated on canonicalization (matches the C).
inline constexpr std::size_t kArchiveSymbolMax = 32;

// Surface blobs start on this file alignment; slice/array chunks inside a blob
// start on `kArchiveArrayAlign`.
inline constexpr std::uint32_t kArchiveBlobAlign = 4096;
inline constexpr std::uint32_t kArchiveArrayAlign = 64;

// Lookup-slot occupancy flags.
inline constexpr std::uint16_t kArchiveSlotEmpty = 0;
inline constexpr std::uint16_t kArchiveSlotOccupied = 1;

// ── On-disk records (POD, little-endian, fixed layout) ───────────────────
//
// Every field is fixed-width and the structs are trivially copyable and
// standard-layout, so (de)serialization is a bounds-checked `std::memcpy` to /
// from the byte buffer — no reinterpret_cast of misaligned data, no UB. Sizes
// are pinned by static_assert; any accidental layout drift is a compile error
// (and would also change the schema hash, so an old reader would reject it).

// File header. Lives at offset 0. `header_crc32c` covers the whole header with
// that one field zeroed; `metadata_crc32c` covers the lookup ‖ directory span.
struct ArchiveHeader {
  char magic[8]{};                          // "ATSVSA02", no NUL
  std::uint16_t major{};                    // kArchiveMajor
  std::uint16_t minor{};
  std::uint16_t header_size{};              // sizeof(ArchiveHeader)
  std::uint16_t endian{};                   // 1 = little
  std::uint16_t pointer_bits{};             // 64
  std::uint16_t alignment_log2{};           // 12 -> 4096 blob alignment
  std::uint32_t flags{};

  std::uint64_t file_size{};
  std::uint64_t created_ts_ns{};
  std::uint64_t schema_hash{};              // sizeof-based layout fingerprint
  std::uint64_t writer_version_hash{};      // informational; 0 if unset

  std::uint32_t surface_count{};
  std::uint32_t lookup_slot_count{};        // power of two
  std::uint64_t lookup_offset{};
  std::uint64_t directory_offset{};
  std::uint64_t data_offset{};

  std::uint32_t index_slot_size{};          // sizeof(ArchiveIndexSlot)
  std::uint32_t dir_entry_size{};           // sizeof(ArchiveDirEntry)
  std::uint32_t surface_blob_header_size{}; // sizeof(SurfaceBlobHeader)
  std::uint32_t reserved0{};

  std::uint32_t header_crc32c{};            // over header bytes, this field = 0
  std::uint32_t metadata_crc32c{};          // over (lookup ‖ directory) bytes

  std::uint8_t reserved[352]{};             // pad tail; covered by header_crc
};
static_assert(sizeof(ArchiveHeader) == 464, "ArchiveHeader layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveHeader>);
static_assert(std::is_standard_layout_v<ArchiveHeader>);

// Open-addressed lookup slot.
struct ArchiveIndexSlot {
  std::uint64_t symbol_hash{};
  std::uint64_t surface_offset{};
  std::uint64_t surface_size{};
  std::uint32_t surface_crc32c{};           // CRC-32C over the whole blob
  std::uint32_t uid{};
  std::uint16_t symbol_len{};
  std::uint16_t flags{};                    // kArchiveSlot*
  char symbol[32]{};                        // canonical, not NUL-terminated
  std::uint8_t reserved[60]{};
};
static_assert(sizeof(ArchiveIndexSlot) == 128, "ArchiveIndexSlot layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveIndexSlot>);
static_assert(std::is_standard_layout_v<ArchiveIndexSlot>);

// Directory entry (one per surface, sorted by canonical symbol).
struct ArchiveDirEntry {
  std::uint64_t surface_offset{};
  std::uint64_t surface_size{};
  std::uint64_t symbol_hash{};
  std::uint32_t uid{};
  std::uint32_t reserved0{};
  std::uint16_t symbol_len{};
  std::uint16_t param{};                    // Parametrization
  std::uint16_t n_slices{};
  std::uint16_t flags{};
  char symbol[32]{};
  std::uint8_t reserved[16]{};
};
static_assert(sizeof(ArchiveDirEntry) == 88, "ArchiveDirEntry layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveDirEntry>);
static_assert(std::is_standard_layout_v<ArchiveDirEntry>);

// Per-surface blob header. Lives at the start of each blob; the payload
// (symbol bytes + slice array) follows on aligned offsets. `payload_crc32c`
// covers [blob_header_size, blob_size); the enclosing slot's `surface_crc32c`
// covers the whole blob.
struct SurfaceBlobHeader {
  char magic[8]{};                          // "ATSVSFC2"
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint16_t param{};                    // Parametrization
  std::uint16_t flags{};
  std::uint32_t uid{};
  std::uint16_t n_slices{};
  std::uint16_t reserved0{};
  std::uint64_t slices_offset{};
  std::uint64_t slices_size{};
  std::uint64_t symbol_offset{};
  std::uint64_t symbol_size{};
  std::uint32_t blob_header_size{};         // sizeof(SurfaceBlobHeader)
  std::uint32_t payload_crc32c{};
  std::uint64_t blob_size{};
  std::int64_t fit_ts_ns{};
  double rmse_vol{};
  double max_residual_vol{};
  std::uint32_t n_quotes_used{};
  std::uint32_t n_quotes_dropped{};
  std::uint8_t reserved[24]{};
};
static_assert(sizeof(SurfaceBlobHeader) == 128, "SurfaceBlobHeader layout drift");
static_assert(std::is_trivially_copyable_v<SurfaceBlobHeader>);
static_assert(std::is_standard_layout_v<SurfaceBlobHeader>);

// ── Writer inputs ────────────────────────────────────────────────────────

// Knobs for `write_surface_archive`. Defaults match the C writer's defaults.
struct SurfaceArchiveWriteOpts {
  std::uint32_t flags{0};
  // Lookup hash-table load factor (percent), in (0, 100]. Lower values reserve
  // slack for future in-place appends. The slot count is rounded up to a power
  // of two large enough for the item count at this load factor (min 8).
  std::uint32_t lookup_load_pct{70};
  std::uint32_t blob_alignment{kArchiveBlobAlign};
  std::uint32_t array_alignment{kArchiveArrayAlign};
  // Stamp for `ArchiveHeader::created_ts_ns`. 0 => fill from the system clock.
  std::int64_t created_ts_ns{0};
};

// One (symbol, surface) entry to archive. Non-owning: `surface` must outlive
// the write call, and `symbol` need only be valid for its duration.
struct SurfaceArchiveItem {
  std::string_view symbol{};
  const VolSurface* surface{nullptr};
};

// ── Writer ───────────────────────────────────────────────────────────────

// Serialize `items` into an in-memory archive byte buffer.
//
// Symbols are canonicalized (ASCII upper-cased, truncated to kArchiveSymbolMax)
// before hashing and storage; two items whose canonical symbols collide is a
// duplicate.
//
// Errors: InvalidArgument (empty item list, null/empty surface, empty symbol,
// zero slices, bad load factor); ParseError (a parametrization with no
// serializable slices — Wing/C8/CStar16M); AlreadyExists (duplicate canonical
// symbol).
[[nodiscard]] Result<std::vector<std::byte>>
write_surface_archive(std::span<const SurfaceArchiveItem> items,
                      const SurfaceArchiveWriteOpts& opts = {});

// As above, but persist to `path` (written to `path` + ".tmp" then renamed for
// atomic replacement). Adds IoError on any filesystem failure.
[[nodiscard]] Status
write_surface_archive_file(std::string_view path,
                           std::span<const SurfaceArchiveItem> items,
                           const SurfaceArchiveWriteOpts& opts = {});

// ── Reader ───────────────────────────────────────────────────────────────

// An opened, validated archive. Owns its bytes (the in-memory analogue of the
// C's mmap region); all query methods are `const` and thread-safe to call
// concurrently. Rule of Zero: movable, copyable, trivially destructible.
class SurfaceArchive {
 public:
  // Take ownership of `bytes` and validate the full framing (magic, version,
  // endian, sizes, schema hash, header CRC, metadata CRC, directory bounds).
  //
  // Errors: ParseError (bad magic / version / endian / sizes / schema hash /
  // truncated / header or metadata checksum mismatch).
  [[nodiscard]] static Result<SurfaceArchive> open(std::vector<std::byte> bytes);

  // Read `path` fully into a buffer and `open` it. Adds IoError / NotFound on
  // filesystem failure.
  [[nodiscard]] static Result<SurfaceArchive> open_file(std::string_view path);

  [[nodiscard]] std::uint32_t count() const noexcept { return header_.surface_count; }
  [[nodiscard]] const ArchiveHeader& header() const noexcept { return header_; }
  [[nodiscard]] std::span<const ArchiveDirEntry> directory() const noexcept {
    return directory_;
  }

  // Resolve `symbol` (case-insensitive) to its directory entry fabricated from
  // the lookup slot (offsets/size/uid/hash/symbol). NotFound if absent.
  [[nodiscard]] Result<ArchiveDirEntry> find(std::string_view symbol) const;

  // Resolve `symbol` and reconstruct its `VolSurface` (an owned copy parsed via
  // set_slice_essvi / set_slice_svi). NotFound if absent; ParseError on a bad
  // blob (magic / bounds / CRC mismatch).
  [[nodiscard]] Result<VolSurface> map_symbol(std::string_view symbol) const;

  // Reconstruct every surface, in directory order. ParseError if any blob fails
  // to parse or verify.
  [[nodiscard]] Result<std::vector<VolSurface>> map_all() const;

  // Reconstruct every surface into caller storage, in directory order. Each
  // written slot holds the reconstructed surface. Returns the number written.
  // OutOfRange if `out.size() < count()` (the CAPACITY guard); ParseError on a
  // bad blob.
  [[nodiscard]] Result<std::size_t>
  map_all_into(std::span<std::optional<VolSurface>> out) const;

 private:
  SurfaceArchive() = default;

  [[nodiscard]] const ArchiveIndexSlot* find_slot(std::string_view symbol) const noexcept;
  [[nodiscard]] Result<VolSurface> reconstruct(std::uint64_t offset,
                                               std::uint64_t size,
                                               std::uint32_t expected_crc) const;

  std::vector<std::byte> buffer_{};        // owns the raw archive bytes
  ArchiveHeader header_{};                 // parsed copy
  std::vector<ArchiveIndexSlot> lookup_{}; // parsed copy of the lookup table
  std::vector<ArchiveDirEntry> directory_{};
};

}  // namespace atx::vol
