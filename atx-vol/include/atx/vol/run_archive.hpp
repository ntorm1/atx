#pragma once

// RunArchive (ATXRUN01) on-disk ABI — the binary result container for backtest
// outputs. Layout mirrors the ATXVSA2 surface archive (surface_archive.hpp):
//
//   RunArchiveHeader (256 B, offset 0)
//   section directory: RaSectionDescriptor[section_count] (sorted by name)
//   64-B-aligned sections; each section =
//     RaSectionHeader + RaColumnDescriptor[n_cols] + 8-B-aligned column arrays
//     (dict-str -> u32 code column + string table at aux offset; u8-enum ->
//      u8 code column + label table at aux offset)
//
// Struct discipline (mirrors ArchiveV2Header et al.): fields ordered by
// descending alignment so there is no internal padding, every struct is
// trivially copyable + standard layout, and sizeof plus each load-bearing
// offsetof is pinned by static_assert — this is an on-disk ABI, so a field
// reorder that preserved sizeof would still silently corrupt readers.

#include "atx/vol/run_archive_schema.hpp"
#include "atx/vol/types.hpp" // Result / Status

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace atx::vol {

// Sections start 64-B aligned; column arrays inside a section align to 8.
inline constexpr std::uint32_t kRaSectionAlign = 64;
inline constexpr std::uint32_t kRaColumnAlign = 8;

// Per-section record magic (RaSectionHeader::magic), no NUL.
inline constexpr char kRaSectionMagic[8] = {'A', 'T', 'X', 'R', 'S', 'C', '0', '1'};

// ── File header (offset 0). All fields naturally aligned (8-byte block first). ──
struct RunArchiveHeader {
  char magic[8]{};                     // "ATXRUN01", no NUL (kRaMagic)
  std::uint64_t file_size{};
  std::uint64_t created_ts_ns{};
  std::uint64_t schema_hash{};         // ra_schema_hash() at write time
  std::uint64_t writer_version_hash{}; // informational; 0 if unset
  std::uint64_t run_identity_hash{};   // identity of the producing run
  std::uint64_t section_dir_offset{};  // byte offset of RaSectionDescriptor[]
  std::uint64_t data_offset{};         // byte offset of the first section
  std::uint32_t section_count{};
  std::uint32_t header_crc32c{};   // over header bytes, this field = 0
  std::uint32_t metadata_crc32c{}; // over the section directory bytes
  std::uint32_t flags{};
  std::uint16_t major{}; // kRaMajor
  std::uint16_t minor{}; // kRaMinor
  std::uint16_t header_size{};  // sizeof(RunArchiveHeader)
  std::uint16_t endian{};       // 1 = little
  std::uint16_t pointer_bits{}; // 64
  std::uint16_t reserved_u16{};
  std::uint8_t reserved[164]{}; // pad to 256; covered by header_crc
};
static_assert(sizeof(RunArchiveHeader) == 256, "RunArchiveHeader layout drift");
static_assert(std::is_trivially_copyable_v<RunArchiveHeader>);
static_assert(std::is_standard_layout_v<RunArchiveHeader>);

// Section directory entry (one per section, sorted by name) — the jump table
// from the header to each section record. Byte offsets, never pointers.
struct RaSectionDescriptor {
  std::uint64_t section_offset{}; // byte offset (file-relative) of the section
  std::uint64_t section_size{};   // section extent (self-contained)
  std::uint64_t n_rows{};
  std::uint32_t n_cols{};
  std::uint32_t col_desc_offset{}; // section-relative offset of the descriptors
  // Content identity: a COPY of this section's payload_crc32c (also in the
  // section header). It lives in the directory — which metadata_crc32c covers —
  // so ANY section-payload rewrite (even one preserving byte length and offset)
  // changes metadata_crc32c and therefore the archive's content identity (the
  // F6 trick, see ArchiveV2DirEntry::payload_crc32c).
  std::uint32_t payload_crc32c{};
  RaSectionKind kind{};
  char name[32]{}; // canonical section name, not NUL-terminated
  std::uint8_t reserved[11]{}; // pad to 80
};
static_assert(sizeof(RaSectionDescriptor) == 80, "RaSectionDescriptor layout drift");
static_assert(std::is_trivially_copyable_v<RaSectionDescriptor>);
static_assert(std::is_standard_layout_v<RaSectionDescriptor>);

// Per-section record header (at each section's start, 64-B aligned).
struct RaSectionHeader {
  char magic[8]{};              // "ATXRSC01" (kRaSectionMagic)
  std::uint64_t section_size{}; // whole record incl. this header
  std::uint64_t n_rows{};
  std::uint32_t n_cols{};
  std::uint32_t col_desc_offset{}; // section-relative: RaColumnDescriptor[n_cols]
  std::uint32_t data_offset{};     // section-relative: first column array
  std::uint32_t payload_crc32c{};  // over section bytes with this field = 0
  std::uint32_t flags{};
  RaSectionKind kind{};
  std::uint8_t reserved[19]{}; // pad to 64
};
static_assert(sizeof(RaSectionHeader) == 64, "RaSectionHeader layout drift");
static_assert(std::is_trivially_copyable_v<RaSectionHeader>);
static_assert(std::is_standard_layout_v<RaSectionHeader>);

// Per-column descriptor. Registry columns AND dynamically appended columns
// (per-signal backtest series) are described identically; dict-str / u8-enum
// columns park their string/label table at the aux offset.
struct RaColumnDescriptor {
  std::uint64_t data_offset{}; // section-relative, kRaColumnAlign-aligned
  std::uint64_t data_size{};   // payload bytes (codes only for dict/enum)
  std::uint64_t aux_offset{};  // section-relative dict/label table; 0 if none
  std::uint64_t aux_size{};    // aux table bytes; 0 if none
  std::uint32_t aux_count{};   // number of dict/label entries; 0 if none
  RaDType dtype{};
  std::uint8_t reserved_u8{};
  std::uint16_t name_len{};
  char name[40]{}; // column name, not NUL-terminated
  char unit[16]{}; // informational unit tag, not NUL-terminated ("" = unpinned)
};
static_assert(sizeof(RaColumnDescriptor) == 96, "RaColumnDescriptor layout drift");
static_assert(std::is_trivially_copyable_v<RaColumnDescriptor>);
static_assert(std::is_standard_layout_v<RaColumnDescriptor>);

// Per-field offsets pinned explicitly (not just sizeof): on-disk ABI, so a
// field reorder that preserved sizeof would still silently corrupt readers.
static_assert(offsetof(RunArchiveHeader, file_size) == 8);
static_assert(offsetof(RunArchiveHeader, created_ts_ns) == 16);
static_assert(offsetof(RunArchiveHeader, schema_hash) == 24);
static_assert(offsetof(RunArchiveHeader, writer_version_hash) == 32);
static_assert(offsetof(RunArchiveHeader, run_identity_hash) == 40);
static_assert(offsetof(RunArchiveHeader, section_dir_offset) == 48);
static_assert(offsetof(RunArchiveHeader, data_offset) == 56);
static_assert(offsetof(RunArchiveHeader, section_count) == 64);
static_assert(offsetof(RunArchiveHeader, header_crc32c) == 68);
static_assert(offsetof(RunArchiveHeader, metadata_crc32c) == 72);
static_assert(offsetof(RunArchiveHeader, flags) == 76);
static_assert(offsetof(RunArchiveHeader, major) == 80);
static_assert(offsetof(RunArchiveHeader, minor) == 82);
static_assert(offsetof(RunArchiveHeader, header_size) == 84);
static_assert(offsetof(RunArchiveHeader, endian) == 86);
static_assert(offsetof(RunArchiveHeader, pointer_bits) == 88);
static_assert(offsetof(RaSectionDescriptor, section_offset) == 0);
static_assert(offsetof(RaSectionDescriptor, section_size) == 8);
static_assert(offsetof(RaSectionDescriptor, n_rows) == 16);
static_assert(offsetof(RaSectionDescriptor, n_cols) == 24);
static_assert(offsetof(RaSectionDescriptor, col_desc_offset) == 28);
static_assert(offsetof(RaSectionDescriptor, payload_crc32c) == 32);
static_assert(offsetof(RaSectionDescriptor, kind) == 36);
static_assert(offsetof(RaSectionDescriptor, name) == 37);
static_assert(offsetof(RaSectionHeader, section_size) == 8);
static_assert(offsetof(RaSectionHeader, n_rows) == 16);
static_assert(offsetof(RaSectionHeader, n_cols) == 24);
static_assert(offsetof(RaSectionHeader, col_desc_offset) == 28);
static_assert(offsetof(RaSectionHeader, data_offset) == 32);
static_assert(offsetof(RaSectionHeader, payload_crc32c) == 36);
static_assert(offsetof(RaSectionHeader, kind) == 44);
static_assert(offsetof(RaColumnDescriptor, data_offset) == 0);
static_assert(offsetof(RaColumnDescriptor, data_size) == 8);
static_assert(offsetof(RaColumnDescriptor, aux_offset) == 16);
static_assert(offsetof(RaColumnDescriptor, aux_size) == 24);
static_assert(offsetof(RaColumnDescriptor, aux_count) == 32);
static_assert(offsetof(RaColumnDescriptor, dtype) == 36);
static_assert(offsetof(RaColumnDescriptor, name_len) == 38);
static_assert(offsetof(RaColumnDescriptor, name) == 40);
static_assert(offsetof(RaColumnDescriptor, unit) == 80);

// ── Writer inputs (in-memory staging; NOT on-disk ABI) ───────────────────────

// Byte width of ONE stored element of `t`'s column array. Dict-str stores a u32
// code per row (the string table is aux data); u8-enum stores a u8 code per row.
[[nodiscard]] constexpr std::uint64_t ra_dtype_size(RaDType t) noexcept {
  switch (t) {
  case RaDType::F64:
  case RaDType::I64:
    return 8;
  case RaDType::U32:
  case RaDType::DictStr:
    return 4;
  case RaDType::U8Enum:
    return 1;
  }
  return 0; // unreachable for a valid RaDType; writer rejects anything else
}

// One column's staged values. Value-semantic and NON-OWNING: exactly the span(s)
// matching `dtype` are consumed by the writer and every span must outlive the
// write call. `strings` carries the dict table (DictStr) or the label table
// (U8Enum); codes index into it and must all be < strings.size().
struct RaColumnData {
  RaDType dtype{RaDType::F64};
  std::span<const double> f64{};          // F64 values
  std::span<const std::int64_t> i64{};    // I64 values
  std::span<const std::uint32_t> u32{};   // U32 values, or DictStr codes
  std::span<const std::uint8_t> u8{};     // U8Enum codes
  std::span<const std::string> strings{}; // DictStr dict / U8Enum labels

  [[nodiscard]] static RaColumnData of_f64(std::span<const double> v) noexcept {
    RaColumnData c;
    c.dtype = RaDType::F64;
    c.f64 = v;
    return c;
  }
  [[nodiscard]] static RaColumnData of_i64(std::span<const std::int64_t> v) noexcept {
    RaColumnData c;
    c.dtype = RaDType::I64;
    c.i64 = v;
    return c;
  }
  [[nodiscard]] static RaColumnData of_u32(std::span<const std::uint32_t> v) noexcept {
    RaColumnData c;
    c.dtype = RaDType::U32;
    c.u32 = v;
    return c;
  }
  [[nodiscard]] static RaColumnData of_u8enum(std::span<const std::uint8_t> codes,
                                              std::span<const std::string> labels) noexcept {
    RaColumnData c;
    c.dtype = RaDType::U8Enum;
    c.u8 = codes;
    c.strings = labels;
    return c;
  }
  [[nodiscard]] static RaColumnData of_dict(std::span<const std::uint32_t> codes,
                                            std::span<const std::string> dict) noexcept {
    RaColumnData c;
    c.dtype = RaDType::DictStr;
    c.u32 = codes;
    c.strings = dict;
    return c;
  }
};

// One staged section: named (columns in caller order; the writer preserves it).
// Registry-known (section, column) pairs are checked against the Task 1 registry
// (dtype + section kind must match) and stamped with the registry unit; unknown
// columns (the dynamically appended per-signal backtest series) pass through
// with an empty unit.
struct RaSectionData {
  std::string name{};
  RaSectionKind kind{RaSectionKind::TimeSeries};
  std::uint64_t n_rows{0};
  std::vector<std::pair<std::string, RaColumnData>> columns{};
};

// ── Writer ───────────────────────────────────────────────────────────────────

// Serialize `sections` into an in-memory ATXRUN01 buffer:
//   RunArchiveHeader → RaSectionDescriptor[] (sorted by name) → 64-B-aligned
//   sections (RaSectionHeader + RaColumnDescriptor[] + 8-B-aligned arrays,
//   dict/label tables at each column's aux offset).
// Stamps schema_hash = ra_schema_hash(), the header CRC (own field zeroed), the
// metadata CRC over the directory, and each section's payload CRC (own field
// zeroed) mirrored into its directory descriptor — so ANY payload rewrite (even
// same-length) changes metadata_crc32c and hence the archive identity.
// `created_ts_ns` == 0 fills from the system clock.
// Errors: InvalidArgument (empty input, name too long, span/n_rows mismatch,
// code out of table range, registry dtype/kind drift); AlreadyExists (duplicate
// section or column name).
[[nodiscard]] Result<std::vector<std::byte>>
write_run_archive(std::span<const RaSectionData> sections, std::int64_t created_ts_ns,
                  std::uint64_t run_identity_hash);

// As above, persisted atomically: build the full buffer in memory, write
// "<path>.tmp", flush/close, then rename over `path` (destination removed first
// — Windows rename does not replace). Adds IoError on filesystem failure.
[[nodiscard]] Status write_run_archive_file(std::string_view path,
                                            std::span<const RaSectionData> sections,
                                            std::int64_t created_ts_ns,
                                            std::uint64_t run_identity_hash);

} // namespace atx::vol
