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

// RunDir (below) returns these by value in Result<...>, so their full
// definitions are needed here (a Result<T> instantiation needs a complete T).
// This is the one place run_archive.hpp is not "light" — the encoders above stay
// forward-declared; only the run-directory handle pulls the input types in.
#include "atx/vol/backtest.hpp"                   // Clock (RunDir::clock)
#include "atx/vol/dispersion_workflow.hpp"        // RunSpec (RunDir::spec)
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedDispersionSchedule (RunDir::schedule)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace atx::vol {

// Cheap content/build identity shared with the surface archives (the F6 trick;
// full definition in surface_archive.hpp — forward-declared here to keep this
// header light; callers of RunArchive::identity() include surface_archive.hpp).
struct ArchiveContentIdentity;

// Task 5 encoder sources — forward-declared to keep this header light (the
// encoders' definitions include the owning headers).
struct BacktestResult;                 // backtest.hpp
struct ListedDispersionReconciliation; // listed_dispersion_reconciliation.hpp
struct ListedDispersionSchedule;       // listed_dispersion_schedule.hpp
struct RunSpec;                        // dispersion_workflow.hpp

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
  // Type-erased owned backing for encoder-synthesized arrays (dict codes and
  // string tables, enum codes/labels, flattened per-leg columns): the Task 5
  // encoders park their scratch here so a returned section's spans stay valid
  // for the section's lifetime (copies/moves share it). The writer never reads
  // this field; hand-staged sections may leave it null and manage lifetimes
  // themselves.
  std::shared_ptr<const void> storage{};
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

// ── Section encoders (Task 5): library type → staged RaSectionData ───────────
//
// Each encoder mirrors the TSV writer that owns the section's output today,
// column-for-column in registry order (registry Task 1 is authoritative for
// name/dtype/order; the writer cross-checks). Synthesized arrays (dict codes,
// string/label tables, flattened per-leg columns) live in the returned
// section's `storage`; columns that already exist as columnar vectors on the
// SOURCE object (the BacktestResult series) are spanned in place, so that
// source must outlive the write call — the RaColumnData lifetime rule.
//
// Enum vocabulary (u8 code == the C++ enum value; labels in enum order):
//   side            {"C", "P"}                  — the TSV 'C'/'P' convention
//   role            {"Entry", "Held"}           — to_string(ListedMarkRole)
//   status          {"Ok", "NoRawQuote", "CrossedQuote", "NoSurface",
//                    "PricingError"}            — to_string(ListedMarkStatus)
//   bools           {"0", "1"}                  — the TSV '0'/'1' convention
// NA-able F64 fields (contract marks raw_bid/raw_ask/raw_mid when status !=
// Ok; model_mark under NoSurface/PricingError) store quiet NaN where the TSV
// writer emits "NA" — the registry pins those dtypes as F64.

// `backtest` / `projected_cold` / `projected_nodiv` (pass the section name):
// the 27 registry columns + one F64 column per r.signals entry, value-equal to
// append_backtest_series_tsv (tearsheet.cpp). Spans borrow `r`.
[[nodiscard]] RaSectionData encode_backtest_section(std::string name, const BacktestResult &r);

// `reconciliation` over rows: serialize_listed_reconciliation column set.
[[nodiscard]] RaSectionData
encode_reconciliation_section(const ListedDispersionReconciliation &reconciliation);

// `trade_schedule` / `projected_schedule` (pass the section name): rolls×legs
// flattened one row per leg, roll fields repeated — the
// serialize_listed_dispersion_schedule kHeader column set (fingerprints as
// I64 bit patterns per the registry: there is no U64 dtype).
[[nodiscard]] RaSectionData encode_schedule_section(std::string name,
                                                    const ListedDispersionSchedule &schedule);

// `contract_marks` over marks: serialize_listed_contract_marks column set.
[[nodiscard]] RaSectionData
encode_contract_marks_section(const ListedDispersionReconciliation &reconciliation);

// `meta` ScalarKV: the resolved-spec echo (write_resolved_spec key vocabulary,
// including the date_lo/date_hi window; doubles %.17g, bools "0"/"1") followed
// by caller-supplied extra pairs (roll scalars, input hashes, counts) in order.
[[nodiscard]] RaSectionData
encode_meta_section(const RunSpec &spec,
                    std::span<const std::pair<std::string, std::string>> extra = {});

// ── Reader ───────────────────────────────────────────────────────────────────

// Zero-copy view over a dict/label string table (u32 offsets[count+1] + blob)
// inside an opened archive's bytes. BORROWS the archive's bytes: like
// PricedSurfaceView, it must not outlive the RunArchive (and, for open_mapped,
// the mapping that archive owns). Framing (offset monotonicity / bounds) is
// validated by RunArchive::section() before a table is handed out, so `at` is
// noexcept and unchecked beyond the index guard.
class RaStringTable {
public:
  RaStringTable() noexcept = default;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

  // Entry `i`, or "" when i >= size().
  [[nodiscard]] std::string_view at(std::size_t i) const noexcept {
    if (i >= count_) {
      return {};
    }
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    std::memcpy(&lo, offsets_ + 4 * i, 4);
    std::memcpy(&hi, offsets_ + 4 * (i + 1), 4);
    return {reinterpret_cast<const char *>(blob_) + lo, hi - lo};
  }
  [[nodiscard]] std::string_view operator[](std::size_t i) const noexcept { return at(i); }

private:
  friend class RaSectionView;
  RaStringTable(const std::byte *offsets, const std::byte *blob, std::uint32_t count) noexcept
      : offsets_(offsets), blob_(blob), count_(count) {}

  const std::byte *offsets_{nullptr}; // u32[count_ + 1], offsets_[0] == 0
  const std::byte *blob_{nullptr};    // concatenated string bytes
  std::uint32_t count_{0};
};

// One dict-str column: the zero-copy u32 code span plus its string table.
// Borrows the archive's bytes (same lifetime rule as RaStringTable).
class RaDictColumn {
public:
  RaDictColumn() noexcept = default;

  [[nodiscard]] std::span<const std::uint32_t> codes() const noexcept { return codes_; }
  [[nodiscard]] const RaStringTable &table() const noexcept { return table_; }
  [[nodiscard]] std::size_t size() const noexcept { return codes_.size(); }
  [[nodiscard]] bool empty() const noexcept { return codes_.empty(); }

  // Row `row` decoded through the dict, or "" when row >= size(). Codes are
  // range-checked against the table at section() time.
  [[nodiscard]] std::string_view at(std::size_t row) const noexcept {
    if (row >= codes_.size()) {
      return {};
    }
    return table_.at(codes_[row]);
  }

private:
  friend class RaSectionView;
  RaDictColumn(std::span<const std::uint32_t> codes, RaStringTable table) noexcept
      : codes_(codes), table_(table) {}

  std::span<const std::uint32_t> codes_{};
  RaStringTable table_{};
};

// Zero-copy typed view over one section record. Column accessors return spans
// straight over the archive's (possibly mapped) bytes — no copies. A view (and
// everything it returns) BORROWS the owning RunArchive's bytes and must not
// outlive it — the PricedSurfaceView lifetime rule. A name/dtype miss returns
// an empty span/table (the section's framing was already validated, so a miss
// is a lookup outcome, not corruption).
class RaSectionView {
public:
  RaSectionView() = default;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] RaSectionKind kind() const noexcept { return header_.kind; }
  [[nodiscard]] std::uint64_t n_rows() const noexcept { return header_.n_rows; }
  [[nodiscard]] std::uint32_t n_cols() const noexcept { return header_.n_cols; }
  [[nodiscard]] std::span<const RaColumnDescriptor> columns() const noexcept { return cols_; }

  // Typed column accessors (empty on name/dtype miss).
  [[nodiscard]] std::span<const double> f64_col(std::string_view name) const noexcept;
  [[nodiscard]] std::span<const std::int64_t> i64_col(std::string_view name) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> u32_col(std::string_view name) const noexcept;
  // U8Enum: the u8 code column, and its label table.
  [[nodiscard]] std::span<const std::uint8_t> u8enum_col(std::string_view name) const noexcept;
  [[nodiscard]] RaStringTable u8enum_labels(std::string_view name) const noexcept;
  // DictStr: code span + string table bundled.
  [[nodiscard]] RaDictColumn dict_col(std::string_view name) const noexcept;

private:
  friend class RunArchive;
  [[nodiscard]] const RaColumnDescriptor *find_col(std::string_view name,
                                                   RaDType dtype) const noexcept;
  [[nodiscard]] RaStringTable string_table(const RaColumnDescriptor &cd) const noexcept;

  const std::byte *base_{nullptr}; // section record start inside the archive
  RaSectionHeader header_{};
  std::vector<RaColumnDescriptor> cols_{}; // parsed copies (metadata only)
  std::string name_{};
};

// An opened ATXRUN01 archive. BACKING-AGNOSTIC like SurfaceArchiveV2: owns a
// span over the region plus a type-erased owner keeping the backing (a resident
// buffer, or an atx::tsdb::Mapping under open_mapped) alive. All query methods
// are `const`; every returned RaSectionView borrows this archive's bytes and
// must not outlive it. Integrity is LAZY: open() validates framing only
// (magic, major/minor, endian == 1, pointer_bits == 64, schema_hash ==
// ra_schema_hash(), header CRC, metadata CRC over the directory, directory
// bounds); per-section payload CRC is validate_section / validate_all only.
class RunArchive {
public:
  // Take ownership of `bytes` and validate framing. ParseError on any failure.
  [[nodiscard]] static Result<RunArchive> open(std::vector<std::byte> bytes);

  // Read `path` fully into a buffer and open it. Adds IoError / NotFound.
  [[nodiscard]] static Result<RunArchive> open_file(std::string_view path);

  // Map `path` read-only (atx::tsdb::Mapping) and open_borrowed it: opening
  // faults in only the metadata pages; section bytes fault in lazily on access.
  // The mapping is kept alive for the archive's whole lifetime. Adds IoError /
  // NotFound / InvalidArgument (empty file).
  [[nodiscard]] static Result<RunArchive> open_mapped(std::string_view path);

  // The mmap seam: view over externally-owned bytes; `owner` keeps the backing
  // alive for the archive's lifetime. Same framing validation as open().
  [[nodiscard]] static Result<RunArchive> open_borrowed(std::span<const std::byte> bytes,
                                                        std::shared_ptr<const void> owner);

  [[nodiscard]] std::uint32_t count() const noexcept { return header_.section_count; }
  [[nodiscard]] const RunArchiveHeader &header() const noexcept { return header_; }
  [[nodiscard]] std::span<const RaSectionDescriptor> directory() const noexcept {
    return directory_;
  }
  // Content identity (file_size, created_ts_ns, header/metadata CRCs). Every
  // section's payload CRC is mirrored in the directory the metadata CRC covers,
  // so ANY payload rewrite changes the identity (F6).
  [[nodiscard]] ArchiveContentIdentity identity() const noexcept;

  // Build a zero-copy typed view over section `name` (exact match). Validates
  // the section record's framing (magic, descriptor agreement, per-column
  // bounds/alignment/dtype sizes, dict/label table well-formedness, code
  // ranges) — but NOT its payload CRC. NotFound if absent; ParseError on any
  // framing failure.
  [[nodiscard]] Result<RaSectionView> section(std::string_view name) const;

  // Lazy integrity: verify one section's payload CRC (own field zeroed) against
  // both the section header and its directory copy. NotFound if absent;
  // ParseError on mismatch.
  [[nodiscard]] Status validate_section(std::string_view name) const;
  // Verify every section's payload CRC. Ok iff all pass.
  [[nodiscard]] Status validate_all() const;

private:
  RunArchive() = default;

  [[nodiscard]] static Result<RunArchive> open_impl(std::span<const std::byte> bytes,
                                                    std::shared_ptr<const void> owner);
  [[nodiscard]] const RaSectionDescriptor *find_descriptor(std::string_view name) const noexcept;

  std::span<const std::byte> bytes_{};  // the whole archive region (borrowed)
  std::shared_ptr<const void> owner_{}; // keeps the backing alive
  RunArchiveHeader header_{};           // parsed copy
  std::vector<RaSectionDescriptor> directory_{}; // parsed copy, sorted by name
};

// ── RunDir: typed handle over a backtest run directory ───────────────────────
//
// A "run directory" is the on-disk home of one backtest invocation. It pairs the
// AUTHORED / derived inputs the pipeline deliberately keeps as text TSV (the run
// spec, universe schedule, surface manifest, trade schedule) with the single
// binary result container run.atxrun (the economic result sections). RunDir owns
// the directory path and provides:
//   * typed reads of the retained text inputs — spec() / clock() / schedule() —
//     that REUSE the library's existing TSV parsers (read_run_spec,
//     read_manifest_file + Clock::from_manifest, read_listed_dispersion_schedule_
//     file); RunDir never reimplements a parser;
//   * write_run_archive(sections) — atomically publishes <dir>/run.atxrun with a
//     deterministic run_identity_hash stamped in the header;
//   * archive() — open_mapped(<dir>/run.atxrun);
//   * verify() — the example orchestrator's acceptance gates (the verify_command
//     in spy_dispersion_backtest.cpp) lifted to library level, now over the
//     archive's sections (checked through the layered CRC) plus the retained text
//     inputs.

// Canonical file names inside a run directory (fixed by the pipeline layout).
inline constexpr std::string_view kRunSpecFile = "run_spec.tsv";
inline constexpr std::string_view kUniverseScheduleFile = "universe_schedule.tsv";
inline constexpr std::string_view kSurfaceManifestFile = "surface_manifest.tsv";
inline constexpr std::string_view kTradeScheduleFile = "trade_schedule.tsv";
inline constexpr std::string_view kRunArchiveFile = "run.atxrun";

// Default existence gate for verify(): the result sections a run-backtest
// invocation folds into run.atxrun (the loose result TSVs that moved into the
// container). Static storage so RunVerifyOptions::required_sections can default
// to a span over it.
inline constexpr std::string_view kRunBacktestRequiredSections[] = {
    "backtest", "reconciliation", "contract_marks", "meta"};

// Acceptance policy for RunDir::verify — the "methodology" knob: which result
// sections run.atxrun must carry, and the core-mode date/roll/breadth floors
// (lifted verbatim from the example's verify gate). Defaults match run-backtest.
struct RunVerifyOptions {
  std::span<const std::string_view> required_sections{kRunBacktestRequiredSections};
  bool enforce_core_mode{true};
  std::size_t core_min_dates{60};
  std::size_t core_min_rolls{3};
  std::uint32_t core_min_names_per_roll{40};
};

// Typed handle over a run directory. Cheap to construct — it stores the path
// only; every accessor touches the filesystem lazily. Copyable and movable.
class RunDir {
public:
  RunDir() = default;
  explicit RunDir(std::filesystem::path dir) noexcept : dir_(std::move(dir)) {}

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return dir_; }
  // <dir>/run.atxrun — the binary result container.
  [[nodiscard]] std::filesystem::path archive_path() const { return dir_ / kRunArchiveFile; }

  // ── Retained text inputs (reuse the library's TSV parsers) ────────────────
  [[nodiscard]] Result<RunSpec> spec() const;                      // run_spec.tsv
  [[nodiscard]] Result<Clock> clock() const;                       // surface_manifest.tsv
  [[nodiscard]] Result<ListedDispersionSchedule> schedule() const; // trade_schedule.tsv

  // Deterministic identity of the producing run: a wyhash fold of the run_spec
  // bytes and the authored input fingerprint(s) — the same atx::core::hash_bytes
  // the orchestrator fingerprints inputs with (hash_file). run_spec.tsv is
  // REQUIRED (a run dir without it is not a run); the universe schedule is folded
  // in when present. Forced nonzero (0 == "unset" in the header). Deterministic
  // for identical input bytes on one platform/binary.
  [[nodiscard]] Result<std::uint64_t> run_identity_hash() const;

  // Publish <dir>/run.atxrun atomically (write_run_archive_file's tmp+rename),
  // stamping the computed run_identity_hash. created_ts_ns fills from the system
  // clock. Propagates every write_run_archive validation error plus IoError.
  [[nodiscard]] Status write_run_archive(std::span<const RaSectionData> sections) const;

  // open_mapped(<dir>/run.atxrun).
  [[nodiscard]] Result<RunArchive> archive() const;

  // Acceptance gates over the archive sections + the retained text inputs:
  //   envelope  — run.atxrun opens (framing + header/metadata CRC) and every
  //               section's payload CRC validates (validate_all);
  //   existence — every options.required_sections section is present + framed;
  //   count     — the backtest section is non-empty and its row count matches the
  //               reconciliation section's (the per-session cardinality the
  //               pipeline cross-checks in validate_listed_reconciliation_backtest);
  //   inputs    — spec / clock / schedule all parse, and the schedule passes its
  //               structural + vega-arithmetic validation;
  //   core-mode — when the resolved spec sets core_mode, the date / roll / breadth
  //               floors hold (>= 60 dates, >= 3 rolls, >= 40 names per roll).
  [[nodiscard]] Status verify(const RunVerifyOptions &options = {}) const;

private:
  std::filesystem::path dir_{};
};

} // namespace atx::vol
