#pragma once

// atx::engine::risk -- the `atx.cov-container` 1.0 binary file (`model.atxcov`, contract K-P9-12;
// platform P9 lane COV, cov-design.md section 4): a stamped, per-session series of factored risk
// models (factor covariance F_t, exposures X_t, specific variances D_t) with one root pin, read by
// memory mapping with zero copies.
//
// Layout (all integers / floats little-endian, every padding byte zero, offsets from the file
// start, no wall-clock field anywhere -- the same inputs give the same bytes):
//   [0, 4096)             FileHeader (256 bytes used, the rest zero)
//   [4096, B_end)         BLOCKS: one block per session, ascending as_of, each 4096-aligned and a
//                         multiple of 4096 bytes long (the block hash covers its padding)
//   [B_end, ...)          SECTION TABLE (64 B per entry, 4096-aligned), then META_JSON,
//                         FACTOR_NAMES, DATE_INDEX, BLOCK_HASHES (each 64-aligned)
//   [file_bytes - 64, ..) Trailer
// A block is a BlockHeader, its ArrayEntry table (zero-padded to 64 bytes) and its arrays, each
// 64-byte aligned (cov-design 4.6). Factor order: column 0 the market, 1..G the one-hot groups,
// G+1..G+S the dense exposure columns (engine::book::FactorRiskView's order).
//
// Row rule (cov-design 4.6): rows [0, P) are the priced names (group in 1..G and a finite
// positive specific variance) in ascending role index, then the other rows in ascending role
// index; FactorRiskView over rows [0, P) is then zero-copy (exposures when f64; the covariance
// when finite everywhere).
//
// Integrity (4.7): block_hash[t] = SHA-256 of block t's bytes; root_sha256 = SHA-256(header[0,
// 224) || section table || every section flagged "in root", in table order) -- a two-level
// Merkle tree over every byte of the model. The trailer repeats file_bytes and the root; a file
// without the header's complete flag or with a missing / mismatched trailer is an unfinished
// write.
//
// No look-ahead (cov-design 3.2, I-1..I-4): block t is the model as of the close of session t
// (as_of) built from data through data_cutoff <= as_of; at_or_before(s) never returns a block
// whose cutoff exceeds s, and exact(s) refuses a missing s.
//
// Errors: atx::core::Result -- ParseError for structure (magic, sizes, offsets, alignment,
// overlap, hashes of a truncated or corrupted file), InvalidArgument for refusals of the caller's
// request (a root pin mismatch, a session outside the file, an append that breaks a rule).
// Deterministic: no RNG, no clock; Full verification partitions the blocks over a fixed number
// of workers and reports the smallest failing block whatever the worker count.

#include <array>
#include <bit>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/risk_target.hpp"

namespace atx::engine::risk {

static_assert(std::endian::native == std::endian::little,
              "atx.cov-container assumes a little-endian target (cov-design 4.1)");

namespace cov {
inline constexpr std::array<atx::u8, 8> file_magic{0x89, 0x43, 0x4F, 0x56, 0x0D, 0x0A, 0x1A, 0x0A};
inline constexpr std::array<atx::u8, 8> block_magic{'C', 'O', 'V', 'B', 'L', 'K', '0', '1'};
inline constexpr std::array<atx::u8, 8> trailer_magic{'C', 'O', 'V', 'E', 'N', 'D', '0', '1'};
inline constexpr atx::u16 format_major = 1;
inline constexpr atx::u16 format_minor = 0;
inline constexpr std::string_view format_name = "atx.cov-container/1.0";
inline constexpr atx::u32 header_bytes = 4096;
inline constexpr atx::u32 endian_tag = 0x01020304U;
inline constexpr atx::u64 page_bytes = 4096;
inline constexpr atx::u64 array_align = 64;
inline constexpr atx::usize root_prefix_bytes = 224; // header[0, 224) enters the root

// Geometry (writer, block encoder and reader alike): G <= 4096 groups, S <= 4096 dense columns and
// K = 1 + G + S, the sum taken in 64 bits so no file-supplied count can wrap K (review R2b).
inline constexpr atx::u32 max_groups = 4096;
inline constexpr atx::u32 max_dense = 4096;
[[nodiscard]] constexpr bool geometry_ok(atx::u32 factors, atx::u32 groups,
                                         atx::u32 dense) noexcept {
  return groups <= max_groups && dense <= max_dense &&
         atx::u64{factors} == atx::u64{1} + atx::u64{groups} + atx::u64{dense};
}

// FileHeader.flags
inline constexpr atx::u32 flag_complete = 1U << 0U;
inline constexpr atx::u32 flag_f32_exposures = 1U << 1U;
inline constexpr atx::u32 flag_unsealed = 1U << 2U;
inline constexpr atx::u32 known_file_flags = flag_complete | flag_f32_exposures | flag_unsealed;

// SectionEntry.kind (ascending in the table, each at most once) and flags.
enum class SectionKind : atx::u32 {
  MetaJson = 1,
  FactorNames = 2,
  DateIndex = 3,
  BlockHashes = 4,
  Blocks = 5,
};
inline constexpr atx::u32 section_required = 1U << 0U;
inline constexpr atx::u32 section_in_root = 1U << 1U;

enum class Dtype : atx::u32 {
  U8 = 1,
  U16 = 2,
  U32 = 3,
  U64 = 4,
  I64 = 5,
  F32 = 6,
  F64 = 7,
  Utf8 = 8
};
[[nodiscard]] constexpr atx::u64 dtype_bytes(Dtype d) noexcept {
  switch (d) {
  case Dtype::U8:
    return 1;
  case Dtype::U16:
    return 2;
  case Dtype::U32:
    return 4;
  case Dtype::U64:
    return 8;
  case Dtype::I64:
    return 8;
  case Dtype::F32:
    return 4;
  case Dtype::F64:
    return 8;
  case Dtype::Utf8:
    return 1;
  }
  return 0;
}

// DateIndexEntry.flags
inline constexpr atx::u32 date_forecast = 1U << 0U;        // the legacy diagnostics.csv forecast
inline constexpr atx::u32 date_factor_complete = 1U << 1U; // F finite everywhere
inline constexpr atx::u32 date_structural = 1U << 2U;      // structural factor forecasts present
inline constexpr atx::u32 date_eigen_adjusted = 1U << 3U;  // S2b applied
inline constexpr atx::u32 date_vra = 1U << 4U;             // a factor VRA applied
inline constexpr atx::u32 known_date_flags = 0x1FU;

// ArrayEntry.id (bit 31 = optional). The required arrays come first, in this order.
inline constexpr atx::u32 array_optional = 1U << 31U;
enum class ArrayId : atx::u32 {
  Ids = 1,
  RoleIndex = 2,
  Group = 3,
  Exposures = 4,
  Specific = 5,
  FactorCov = 6,
  FactorReturn = 7U | array_optional,
  FactorFlags = 8U | array_optional,
  RowFlags = 9U | array_optional,
  Diag = 10U | array_optional,
  EigenGamma = 11U | array_optional,
};
inline constexpr atx::usize diag_count = 32;
// FACTOR_FLAGS / ROW_FLAGS bits.
inline constexpr atx::u8 factor_forecast = 1U << 0U;
inline constexpr atx::u8 factor_structural = 1U << 1U;
inline constexpr atx::u8 row_eligible = 1U << 0U;
inline constexpr atx::u8 row_structural_specific = 1U << 1U;
inline constexpr atx::u8 row_bounded = 1U << 2U;

[[nodiscard]] constexpr atx::u64 align_up(atx::u64 x, atx::u64 a) noexcept {
  return (x + a - 1U) / a * a;
}
} // namespace cov

// ---- POD records (memcpy'd to and from the file verbatim) ---------------------------------------
using Sha256Bytes = std::array<atx::u8, 32>;

struct CovFileHeader {
  std::array<atx::u8, 8> magic;
  atx::u16 major;
  atx::u16 minor;
  atx::u32 header_bytes;
  atx::u32 endian_tag;
  atx::u32 flags;
  atx::u64 file_bytes;
  atx::u32 section_count;
  atx::u32 date_count;
  atx::u64 section_table_offset;
  atx::u32 factor_count; // K = 1 + G + S
  atx::u32 group_count;  // G
  atx::u32 dense_count;  // S
  atx::u32 max_rows;
  atx::i64 first_session_ns;
  atx::i64 last_session_ns;
  Sha256Bytes recipe_sha256;
  Sha256Bytes role_sha256;
  Sha256Bytes inputs_sha256;
  std::array<atx::u8, 48> reserved;
  Sha256Bytes root_sha256;
};
struct CovSectionEntry {
  atx::u32 kind;
  atx::u32 flags;
  atx::u64 offset;
  atx::u64 bytes;
  atx::u64 count;
  atx::u32 dtype;
  atx::u32 align;
  std::array<atx::u8, 24> zero;
};
struct CovDateIndexEntry {
  atx::i64 as_of_session_ns;
  atx::i64 data_cutoff_ns;
  atx::i64 history_first_ns;
  atx::u64 block_offset;
  atx::u64 block_bytes;
  atx::u32 rows;
  atx::u32 priced_rows;
  atx::u32 role_date_index;
  atx::u32 flags;
  atx::u32 unforecast_factors;
  atx::u32 zero;
};
struct CovBlockHeader {
  std::array<atx::u8, 8> magic;
  atx::i64 as_of;
  atx::i64 cutoff;
  atx::u32 rows;
  atx::u32 priced;
  atx::u32 factors;
  atx::u32 groups;
  atx::u32 dense;
  atx::u32 array_count;
};
struct CovArrayEntry {
  atx::u32 id;
  atx::u32 dtype;
  atx::u64 offset_in_block;
  atx::u64 count;
};
struct CovTrailer {
  std::array<atx::u8, 8> magic;
  atx::u64 file_bytes;
  Sha256Bytes root_sha256;
  std::array<atx::u8, 16> zero;
};
static_assert(sizeof(CovFileHeader) == 256, "CovFileHeader layout drift");
static_assert(sizeof(CovSectionEntry) == 64, "CovSectionEntry layout drift");
static_assert(sizeof(CovDateIndexEntry) == 64, "CovDateIndexEntry layout drift");
static_assert(sizeof(CovBlockHeader) == 48, "CovBlockHeader layout drift");
static_assert(sizeof(CovArrayEntry) == 24, "CovArrayEntry layout drift");
static_assert(sizeof(CovTrailer) == 64, "CovTrailer layout drift");
static_assert(std::is_trivially_copyable_v<CovFileHeader> &&
              std::is_trivially_copyable_v<CovSectionEntry> &&
              std::is_trivially_copyable_v<CovDateIndexEntry> &&
              std::is_trivially_copyable_v<CovBlockHeader> &&
              std::is_trivially_copyable_v<CovArrayEntry> &&
              std::is_trivially_copyable_v<CovTrailer>);
static_assert(std::is_standard_layout_v<CovFileHeader> && std::is_standard_layout_v<CovTrailer> &&
              std::is_standard_layout_v<CovSectionEntry> &&
              std::is_standard_layout_v<CovDateIndexEntry> &&
              std::is_standard_layout_v<CovBlockHeader> &&
              std::is_standard_layout_v<CovArrayEntry>);
// Field offsets (cov-design 4.3-4.6), pinned so a reordering fails the build.
static_assert(
    offsetof(CovFileHeader, major) == 8 && offsetof(CovFileHeader, minor) == 10 &&
    offsetof(CovFileHeader, header_bytes) == 12 && offsetof(CovFileHeader, endian_tag) == 16 &&
    offsetof(CovFileHeader, flags) == 20 && offsetof(CovFileHeader, file_bytes) == 24 &&
    offsetof(CovFileHeader, section_count) == 32 && offsetof(CovFileHeader, date_count) == 36 &&
    offsetof(CovFileHeader, section_table_offset) == 40 &&
    offsetof(CovFileHeader, factor_count) == 48 && offsetof(CovFileHeader, group_count) == 52 &&
    offsetof(CovFileHeader, dense_count) == 56 && offsetof(CovFileHeader, max_rows) == 60 &&
    offsetof(CovFileHeader, first_session_ns) == 64 &&
    offsetof(CovFileHeader, last_session_ns) == 72 &&
    offsetof(CovFileHeader, recipe_sha256) == 80 && offsetof(CovFileHeader, role_sha256) == 112 &&
    offsetof(CovFileHeader, inputs_sha256) == 144 && offsetof(CovFileHeader, reserved) == 176 &&
    offsetof(CovFileHeader, root_sha256) == 224);
static_assert(offsetof(CovSectionEntry, flags) == 4 && offsetof(CovSectionEntry, offset) == 8 &&
              offsetof(CovSectionEntry, bytes) == 16 && offsetof(CovSectionEntry, count) == 24 &&
              offsetof(CovSectionEntry, dtype) == 32 && offsetof(CovSectionEntry, align) == 36 &&
              offsetof(CovSectionEntry, zero) == 40);
static_assert(offsetof(CovDateIndexEntry, data_cutoff_ns) == 8 &&
              offsetof(CovDateIndexEntry, history_first_ns) == 16 &&
              offsetof(CovDateIndexEntry, block_offset) == 24 &&
              offsetof(CovDateIndexEntry, block_bytes) == 32 &&
              offsetof(CovDateIndexEntry, rows) == 40 &&
              offsetof(CovDateIndexEntry, priced_rows) == 44 &&
              offsetof(CovDateIndexEntry, role_date_index) == 48 &&
              offsetof(CovDateIndexEntry, flags) == 52 &&
              offsetof(CovDateIndexEntry, unforecast_factors) == 56 &&
              offsetof(CovDateIndexEntry, zero) == 60);
static_assert(offsetof(CovBlockHeader, as_of) == 8 && offsetof(CovBlockHeader, cutoff) == 16 &&
              offsetof(CovBlockHeader, rows) == 24 && offsetof(CovBlockHeader, priced) == 28 &&
              offsetof(CovBlockHeader, factors) == 32 && offsetof(CovBlockHeader, groups) == 36 &&
              offsetof(CovBlockHeader, dense) == 40 && offsetof(CovBlockHeader, array_count) == 44);
static_assert(offsetof(CovArrayEntry, dtype) == 4 &&
              offsetof(CovArrayEntry, offset_in_block) == 8 &&
              offsetof(CovArrayEntry, count) == 16);
static_assert(offsetof(CovTrailer, file_bytes) == 8 && offsetof(CovTrailer, root_sha256) == 16 &&
              offsetof(CovTrailer, zero) == 48);

// ---- writer inputs ------------------------------------------------------------------------------
// Constant over a file. The JSON texts are opaque bytes here: the caller passes its canonical
// spellings (recipe_sha256 = SHA-256(recipe_json), inputs_sha256 = SHA-256(inputs_json)) and a
// META text that embeds them. factor_names: K names, none empty or holding a NUL. groups and dense
// at most cov::max_groups / cov::max_dense (refused otherwise, before factors() is used).
struct CovContainerSpec {
  atx::u32 groups{}, dense{};
  bool f32_exposures{true};
  bool unsealed{};
  Sha256Bytes role_sha256{};
  std::string recipe_json, inputs_json, meta_json;
  std::vector<std::string> factor_names;
  [[nodiscard]] atx::u32 factors() const noexcept { return 1U + groups + dense; }
};

// One session. Required spans: ids, role_index, group, specific (rows each), exposures_f32 or
// exposures_f64 (rows x dense, the one the spec names), factor_cov (K x K). Optional spans (empty
// = the array is absent): factor_return (K), factor_flags (K), row_flags (rows), diag
// (cov::diag_count), eigen_gamma (K). `flags` carries the caller's date flags except
// date_factor_complete, which the writer sets from factor_cov.
struct CovBlockInput {
  atx::i64 as_of_session_ns{}, data_cutoff_ns{}, history_first_ns{};
  atx::u32 role_date_index{};
  atx::u32 flags{};
  atx::u32 priced_rows{};
  std::span<const atx::u64> ids;
  std::span<const atx::u32> role_index;
  std::span<const atx::u32> group;
  std::span<const atx::f32> exposures_f32;
  std::span<const atx::f64> exposures_f64;
  std::span<const atx::f64> specific;
  std::span<const atx::f64> factor_cov;
  std::span<const atx::f64> factor_return;
  std::span<const atx::u8> factor_flags;
  std::span<const atx::u8> row_flags;
  std::span<const atx::f64> diag;
  std::span<const atx::f64> eigen_gamma;
};

// The block's bytes (header, array table, arrays, zero padding to a multiple of 4096) after the
// append rules a block can check on its own: geometry, the row rule (priced rows first, each part
// in strictly ascending role index, rows >= P not priced), groups <= G, cutoff <= as_of and
// history_first <= cutoff. InvalidArgument on a violation. `out` is resized to the block length.
[[nodiscard]] atx::core::Status encode_cov_block(const CovContainerSpec &spec,
                                                 const CovBlockInput &block,
                                                 std::vector<atx::u8> &out);

// ---- reader view ----------------------------------------------------------------------------
// Spans into the reader's mapping (or the caller's bytes for parse_cov_block): valid while it
// lives. Optional arrays absent from the block are empty spans.
struct CovBlockView {
  atx::i64 as_of{}, cutoff{};
  atx::u32 rows{}, priced{}, factors{}, groups{}, dense{};
  std::span<const atx::u64> ids;
  std::span<const atx::u32> role_index, group;
  std::span<const atx::f32> exposures_f32; // when the file's exposures are f32
  std::span<const atx::f64> exposures_f64; // when they are f64
  std::span<const atx::f64> specific, factor_cov;
  std::span<const atx::f64> factor_return, diag, eigen_gamma;
  std::span<const atx::u8> factor_flags, row_flags;
};
// Parses one block's bytes (`bytes` 64-byte aligned): magic, geometry equal to (factors, groups,
// dense) and cov::geometry_ok (G, S <= 4096, K = 1 + G + S in 64 bits), array table in bounds,
// aligned, non-overlapping, ids known (an unknown required id is refused, an unknown optional one
// ignored), dtype x count consistent, exposures in the stated dtype. ParseError otherwise. No hash
// is checked here.
[[nodiscard]] atx::core::Result<CovBlockView> parse_cov_block(std::span<const atx::u8> bytes,
                                                              atx::u32 factors, atx::u32 groups,
                                                              atx::u32 dense, bool f32_exposures);

// FactorRiskView over rows [0, P) of a block (the priced names): zero-copy for group, specific
// and (f64) exposures; f32 exposures are widened into scratch.exposures; a covariance that is not
// finite everywhere is copied into scratch.covariance with each non-finite entry 0 (the legacy
// store's NaN -> 0 rule). The view borrows `v` and `scratch`.
struct CovViewScratch {
  std::vector<atx::f64> exposures, covariance;
};
[[nodiscard]] atx::engine::book::FactorRiskView factor_risk_view(const CovBlockView &v,
                                                                 CovViewScratch &scratch);

// ---- writer -------------------------------------------------------------------------------------
// Streaming writer: creates `<path>.partial` exclusively, reserves the header, appends one block
// per session at the next 4096 boundary while hashing it (only the index and the hashes stay in
// memory), and finalize() writes the section table, META, names, index and hashes, the header
// (complete flag, root) and the trailer, flushes to the device and renames to `path`. append
// enforces ascending as_of across blocks on top of encode_cov_block's rules. A refused append or
// a writer destroyed before finalize() leaves only the `.partial` file. Move-only.
class CovContainerWriter {
public:
  [[nodiscard]] static atx::core::Result<CovContainerWriter> create(const std::string &path,
                                                                    CovContainerSpec spec);
  CovContainerWriter(CovContainerWriter &&) noexcept;
  CovContainerWriter &operator=(CovContainerWriter &&) noexcept;
  CovContainerWriter(const CovContainerWriter &) = delete;
  CovContainerWriter &operator=(const CovContainerWriter &) = delete;
  ~CovContainerWriter();

  [[nodiscard]] atx::core::Status append(const CovBlockInput &block);
  // Writes the file and renames it; returns the root. A second call is refused.
  [[nodiscard]] atx::core::Result<Sha256Bytes> finalize();
  [[nodiscard]] atx::usize blocks() const noexcept;

private:
  struct Impl;
  explicit CovContainerWriter(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// ---- reader -------------------------------------------------------------------------------------
enum class VerifyMode : atx::u8 {
  Full, // every block hashed at open (deterministic worker pool); the default for ledgered runs
  Lazy, // a block is hashed on its first view (per-block atomic flag); research loops, benches
};
// One difference between two containers (first_difference).
struct CovDifference {
  bool differs{};
  atx::usize index{}; // block index of the first difference (when differs)
  atx::i64 session{}; // its session (when differs)
  std::string what;   // "geometry", "sessions", "block" or "missing block"
};

// Read-only, memory-mapped container. Immutable after open except the lazy verification flags
// (idempotent atomics), so it may be shared across threads (e.g. by std::shared_ptr<const ...>).
// Move-only: views borrow its mapping.
class CovModelFile {
public:
  // Maps `path` (at most max_bytes) and checks (cov-design 4.8): header (magic, major <= 1, any
  // minor, header_bytes, endian tag, complete flag, no unknown flag, file_bytes == size, reserved
  // and padding zero), trailer (magic, size, root), section table (kinds ascending and unique,
  // every required kind present, unknown required kind refused, unknown optional kind ignored,
  // bounds, alignment, no overlap, dtype x count == bytes), the root over the metadata against
  // the header and the caller's pin, the date index (as_of strictly ascending, cutoff <= as_of,
  // history_first <= cutoff, blocks 4096-aligned, inside BLOCKS, ascending and non-overlapping,
  // priced <= rows <= max_rows, no unknown flag), the factor names, then per `mode`: Full hashes
  // every block now on `workers` threads (0: up to 8, by the hardware) over a fixed partition.
  // ParseError on structure and hashes, InvalidArgument on a root pin mismatch.
  [[nodiscard]] static atx::core::Result<CovModelFile> open(const std::string &path,
                                                            std::optional<Sha256Bytes> root_pin,
                                                            VerifyMode mode, atx::u64 max_bytes,
                                                            atx::usize workers = 0);

  CovModelFile(CovModelFile &&) noexcept;
  CovModelFile &operator=(CovModelFile &&) noexcept;
  CovModelFile(const CovModelFile &) = delete;
  CovModelFile &operator=(const CovModelFile &) = delete;
  ~CovModelFile();

  [[nodiscard]] const CovFileHeader &header() const noexcept;
  [[nodiscard]] atx::usize date_count() const noexcept;
  [[nodiscard]] atx::u32 factors() const noexcept;
  [[nodiscard]] atx::u32 groups() const noexcept;
  [[nodiscard]] atx::u32 dense() const noexcept;
  [[nodiscard]] bool f32_exposures() const noexcept;
  [[nodiscard]] bool unsealed() const noexcept;
  [[nodiscard]] VerifyMode mode() const noexcept;
  [[nodiscard]] atx::u64 file_bytes() const noexcept;
  [[nodiscard]] std::span<const CovDateIndexEntry> index() const noexcept;
  [[nodiscard]] std::string_view meta_json() const noexcept;
  [[nodiscard]] const std::vector<std::string> &factor_names() const noexcept;
  [[nodiscard]] const Sha256Bytes &root_sha256() const noexcept;
  [[nodiscard]] const Sha256Bytes &recipe_sha256() const noexcept;
  [[nodiscard]] const Sha256Bytes &role_sha256() const noexcept;
  [[nodiscard]] const Sha256Bytes &inputs_sha256() const noexcept;
  // The stored hash of block t (OutOfRange past the end).
  [[nodiscard]] atx::core::Result<Sha256Bytes> block_sha256(atx::usize t) const;

  // Block t after its header is checked against the index (and, Lazy, its hash on first touch).
  // InvalidArgument past the end; ParseError on a corrupt block.
  [[nodiscard]] atx::core::Result<CovBlockView> view(atx::usize t) const;
  // The block whose as_of is exactly `session`; InvalidArgument when there is none.
  [[nodiscard]] atx::core::Result<CovBlockView> exact(atx::i64 session) const;
  // The latest block whose data_cutoff <= `session` (never one with a later cutoff);
  // InvalidArgument when there is none.
  [[nodiscard]] atx::core::Result<CovBlockView> at_or_before(atx::i64 session) const;
  // Index of at_or_before(session)'s block, without touching it (nullopt when none).
  [[nodiscard]] std::optional<atx::usize> index_at_or_before(atx::i64 session) const noexcept;

private:
  struct Impl;
  explicit CovModelFile(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// The first block, in ascending session order through `through_session` (inclusive), where the
// two files differ: their geometry (K, G, S, exposure dtype), their session sequences, or a block
// hash; a block present in one file only (through the session) is a difference. Recipe, role and
// inputs are not compared (a truncated role is another role): the prefix property is a statement
// about blocks.
[[nodiscard]] CovDifference first_difference(const CovModelFile &a, const CovModelFile &b,
                                             atx::i64 through_session);

// Lowercase hex of a digest, and its parse (nullopt unless 64 lowercase hex digits).
[[nodiscard]] std::string cov_hex(const Sha256Bytes &digest);
[[nodiscard]] std::optional<Sha256Bytes> cov_parse_hex(std::string_view hex);

} // namespace atx::engine::risk
