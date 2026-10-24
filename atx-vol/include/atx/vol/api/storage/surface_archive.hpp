#pragma once

// Priced curve-surface archive — ATXVSA2 revision 1 (file magic "ATXVSA21", major 5).
//
// One file packs many fitted `PricedSurface`s with an O(1) hash lookup, an ordered
// directory (the jump table whole-board readers walk) and self-contained columnar
// records. Two read paths:
//
//   * POINT READ (`point_read_archive_surface` / `point_read_archive_reconstruct`):
//     serve ONE symbol from a partition file by reading the 128-B header, the probed
//     lookup slot(s) and that record's extent — three positional reads in the common
//     case, independent of the partition's size — verifying every byte it touches
//     (header CRC, per-slot CRC, record CRC + symbol binding). No mapping is created
//     and the file handle is closed before the call returns, so a point read never
//     blocks an atomic republish of the partition.
//   * WHOLE BOARD (`SurfaceArchiveV2`): eager, fully cross-checked metadata parse at
//     open, then zero-copy views (`map_*`) or owned reconstructs (`reconstruct_*`)
//     with per-record framing/binding checks and a lazy record CRC (`validate_*`).
//
// The on-disk layout, the point-read algorithm and the integrity model are specified
// in atx-vol/docs/atxvsa2-format.md; every size and offset below is pinned by a
// static_assert so this header and that spec cannot drift silently.
//
// ── FORMAT NAMES ─────────────────────────────────────────────────────────────
//
// The family is ATXVSA2 (the "2" in `SurfaceArchiveV2` / `write_surface_archive_v2`).
// The name of a revision is its on-disk magic: "ATXVSA21" is the only one this reader
// accepts. "ATXVSA20" (ATXVSA2 revision 0, major 4) and "ATXVSA03" (the retired
// ATXVSA03 format, major 3) are refused with a ParseError that names the magic found.
// A record keeps its own magic "ATXVSR20": it tags "this extent is a surface record";
// the record LAYOUT is versioned by the file magic + major + schema hash.
//
// ── Thread safety ────────────────────────────────────────────────────────────
//
// A parsed `SurfaceArchiveV2` is immutable after `open`; every query is `const` and
// touches no shared mutable state, so any number of threads may query one archive.
// `reconstruct_*` returns independently-owned `PricedSurface`s; `map_*` returns
// `PricedSurfaceView`s that BORROW the archive's bytes and must not outlive it. The
// point-read functions are free functions with no shared state.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "atx/vol/api/backtest/priced_surface.hpp"      // PricedSurface, PricingContext
#include "atx/vol/api/backtest/priced_surface_view.hpp" // PricedSurfaceView (zero-copy read view)
#include "atx/vol/api/fitting/surface_policy.hpp"      // purpose, quality, health, validation
#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

// ── Shared vocabulary ──────────────────────────────────────────────────────

// Maximum canonical-symbol length. The writer REFUSES a longer symbol (no truncation:
// a truncated key would alias two names onto one surface) and every reader refuses a
// longer query.
inline constexpr std::size_t kArchiveSymbolMax = 32;

// A cheap content/build identity for an on-disk archive, derived purely from its
// header: `file_size` + `created_ts_ns` (content-derived, see the writer opts) +
// `header_crc32c` + `metadata_crc32c`. The metadata CRC covers the lookup ‖ directory,
// both of which carry a copy of every record's payload CRC, so any record rewrite —
// even a same-length one — changes it. Used by the backtest `SnapshotCache` /
// `SnapshotPool` staleness checks. A staleness signal, never tamper evidence.
struct ArchiveContentIdentity {
  std::uint64_t file_size{0};
  std::uint64_t created_ts_ns{0};
  std::uint32_t header_crc32c{0};
  std::uint32_t metadata_crc32c{0};

  [[nodiscard]] bool operator==(const ArchiveContentIdentity &) const noexcept = default;
};

// Public metadata paired with an archived surface. ValidationDigest's identity and
// failure mask are persisted; detailed per-gate counts remain in the machine-readable
// validation report keyed by validation_id. `legacy_format` is true exactly for a
// surface written WITHOUT provenance (an `Unattested` record, see
// `ArchiveV2ProvenanceStatus`); the backtest risk gate reads it.
struct SurfaceProvenance {
  SurfacePurpose purpose{SurfacePurpose::Risk};
  FitQualityMode quality_mode{FitQualityMode::Balanced};
  SurfaceState state{SurfaceState::Healthy};
  ValidationDigest validation{};
  std::uint64_t source_generation{};
  std::uint64_t served_generation{};
  bool legacy_format{false};
};

// The provenance an Unattested record (written from `provenance == nullopt`) reads
// back as. Such surfaces were never independently admitted, so they are exposed as
// degraded market marks rather than silently promoted to risk.
[[nodiscard]] SurfaceProvenance legacy_surface_provenance() noexcept;

// One independently-owned reconstruction paired with the provenance parsed from the
// same record. Move-only because PricedSurface owns its polymorphic curves.
struct ArchivedSurface {
  PricedSurface surface;
  SurfaceProvenance provenance;
};
static_assert(!std::is_copy_constructible_v<ArchivedSurface>);
static_assert(!std::is_copy_assignable_v<ArchivedSurface>);
static_assert(std::is_nothrow_move_constructible_v<ArchivedSurface>);
static_assert(std::is_nothrow_move_assignable_v<ArchivedSurface>);

// One (symbol, surface) entry to archive. Non-owning: `surface` must outlive the write
// call, and `symbol` need only be valid for its duration. `provenance == nullopt`
// writes an Unattested record (reads back as `legacy_surface_provenance()`).
struct SurfaceArchiveItem {
  std::string_view symbol{};
  const PricedSurface *surface{nullptr};
  std::optional<SurfaceProvenance> provenance{};
};

// ── On-wire constants ──────────────────────────────────────────────────────

inline constexpr char kArchiveV2Magic[8] = {'A', 'T', 'X', 'V', 'S', 'A', '2', '1'};
inline constexpr char kArchiveV2RecordMagic[8] = {'A', 'T', 'X', 'V', 'S', 'R', '2', '0'};
inline constexpr std::uint16_t kArchiveV2Major = 5; // revision 0 (ATXVSA20) was 4
inline constexpr std::uint16_t kArchiveV2Minor = 0;

// Records pack on this file alignment by default (cache line); columns inside a record
// align to 8; the file tail pads to a page.
inline constexpr std::uint32_t kArchiveV2SurfaceAlign = 64;
inline constexpr std::uint32_t kArchiveV2ColumnAlign = 8;
inline constexpr std::uint32_t kArchiveV2FilePageAlign = 4096;

// Upper bound on surfaces per archive (writer refuses more; reader refuses a header
// claiming more). Keeps the lookup-slot arithmetic inside u32 with margin.
inline constexpr std::uint32_t kArchiveV2MaxSurfaces = 1u << 24;
// A record's extent must fit the slot's u32 size field.
inline constexpr std::uint64_t kArchiveV2MaxRecordBytes = 0xFFFFFFFFull;

inline constexpr std::uint8_t kArchiveV2SlotEmpty = 0;
inline constexpr std::uint8_t kArchiveV2SlotOccupied = 1;

// The point read fetches lookup slots in runs of this many (one positional read).
inline constexpr std::uint32_t kArchiveV2SlotReadRun = 8;

// `ArchiveV2SurfaceHeader::prov_status` wire values. 0 (and anything else) is refused.
enum class ArchiveV2ProvenanceStatus : std::uint8_t {
  Attested = 1,   // the prov_* fields hold a validated SurfaceProvenance
  Unattested = 2, // written without provenance; every prov_* field is zero
};

// The format's symbol hash (spec §6): FNV-1a/64 over the canonical bytes, then the
// MurmurHash3 fmix64 finalizer. Fixed arithmetic, persisted in slots and entries.
[[nodiscard]] constexpr std::uint64_t archive_v2_symbol_hash(std::string_view canonical) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (const char c : canonical) {
    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    h *= 0x100000001b3ull;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

// ── On-disk records (POD, little-endian, fixed layout, no implicit padding) ──

// File header, offset 0 (spec §2.1).
struct ArchiveV2Header {
  char magic[8]{}; // "ATXVSA21", no NUL
  std::uint64_t file_size{};
  std::uint64_t created_ts_ns{}; // content-derived identity, NOT a time (spec §8)
  std::uint64_t schema_hash{};
  std::uint64_t lookup_offset{};    // == sizeof(ArchiveV2Header)
  std::uint64_t directory_offset{}; // == lookup_offset + slots * sizeof(slot)
  std::uint64_t data_offset{};
  std::uint32_t surface_count{};
  std::uint32_t lookup_slot_count{}; // power of two, >= 8, > surface_count
  std::uint32_t lookup_slot_size{};  // sizeof(ArchiveV2LookupSlot)
  std::uint32_t dir_entry_size{};    // sizeof(ArchiveV2DirEntry)
  std::uint32_t surface_header_size{}; // sizeof(ArchiveV2SurfaceHeader)
  std::uint32_t header_crc32c{};       // over these bytes, this field = 0
  std::uint32_t metadata_crc32c{};     // over lookup ‖ directory
  std::uint32_t flags{};               // ArchiveV2WriteOpts::flags, opaque
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint16_t header_size{};
  std::uint16_t endian{};       // 1 = little
  std::uint16_t pointer_bits{}; // 64
  // Zero. Pads the header to the 64-B lookup alignment so no byte between the header
  // and the lookup escapes a checksum; a reader refuses a non-zero byte.
  std::uint8_t reserved[30]{};
};
static_assert(sizeof(ArchiveV2Header) == 128, "ArchiveV2Header layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveV2Header>);
static_assert(std::is_standard_layout_v<ArchiveV2Header>);

// R-19 (F6) content identity from an already-parsed header (no I/O). Mirrors
// `SurfaceArchiveV2::identity()`.
[[nodiscard]] inline ArchiveContentIdentity
archive_v2_identity_from_header(const ArchiveV2Header &header) noexcept {
  return ArchiveContentIdentity{header.file_size, header.created_ts_ns, header.header_crc32c,
                                header.metadata_crc32c};
}

// Open-addressed lookup slot (spec §2.2). Every slot, empty or occupied, carries its
// own CRC so a point read verifies exactly the slots it reads.
struct ArchiveV2LookupSlot {
  std::uint64_t symbol_hash{};    // archive_v2_symbol_hash(symbol)
  std::uint64_t surface_offset{}; // absolute record offset
  std::uint32_t surface_size{};   // record extent (records are < 4 GiB)
  std::uint32_t payload_crc32c{}; // copy of the record's payload_crc32c
  std::uint32_t slot_crc32c{};    // over these 64 bytes, this field = 0
  std::uint8_t symbol_len{};
  std::uint8_t flags{}; // kArchiveV2Slot*
  std::uint8_t reserved[2]{}; // zero (alignment of `symbol` to 32)
  char symbol[32]{};          // canonical, zero padded
};
static_assert(sizeof(ArchiveV2LookupSlot) == 64, "ArchiveV2LookupSlot layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveV2LookupSlot>);
static_assert(std::is_standard_layout_v<ArchiveV2LookupSlot>);

// Directory entry (spec §2.3): one per surface, sorted strictly ascending by canonical
// symbol — the ordered jump table whole-board readers walk.
struct ArchiveV2DirEntry {
  std::uint64_t surface_offset{};
  std::uint64_t surface_size{};
  std::uint64_t symbol_hash{};
  std::uint32_t uid{};
  // Copy of this record's payload_crc32c. The directory is covered by
  // `metadata_crc32c`, so ANY record rewrite (even one preserving offset and length)
  // changes the archive's content identity.
  std::uint32_t payload_crc32c{};
  std::uint16_t n_slices{};
  std::uint16_t kind_bits{}; // OR of (1u << VolCurveKind)
  std::uint16_t symbol_len{};
  std::uint16_t flags{}; // zero
  char symbol[32]{};
};
static_assert(sizeof(ArchiveV2DirEntry) == 72, "ArchiveV2DirEntry layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveV2DirEntry>);
static_assert(std::is_standard_layout_v<ArchiveV2DirEntry>);

// Per-surface record header (spec §3.1): pricing scalars, provenance, the canonical
// symbol (the slot -> record binding) and the record-relative column offsets.
struct ArchiveV2SurfaceHeader {
  char magic[8]{}; // "ATXVSR20"
  std::uint64_t record_size{};
  double S{};
  double r{};
  std::int64_t now_ts_ns{};
  double al_tol{};
  std::uint64_t prov_validation_id{};
  std::uint64_t prov_source_generation{};
  std::uint64_t prov_served_generation{};
  // Record-relative offsets to the columnar arrays (canonical geometry, spec §3.2).
  std::uint64_t col_kind_off{};        // u8[n_slices]
  std::uint64_t col_T_off{};           // f64[n_slices]  (ascending)
  std::uint64_t col_forward_off{};     // f64[n_slices]
  std::uint64_t col_qeff_off{};        // f64[n_slices]
  std::uint64_t col_df_off{};          // f64[n_slices]
  std::uint64_t col_borrow_off{};      // f64[n_slices]
  std::uint64_t col_nused_off{};       // u64[n_slices]
  std::uint64_t col_ndropped_off{};    // u64[n_slices]
  std::uint64_t col_nodecount_off{};   // u32[n_slices]
  std::uint64_t col_payload_off_off{}; // u64[n_slices] (record-relative payload offsets)
  std::uint32_t uid{};
  std::uint32_t n_slices{};
  std::uint32_t prov_validation_failures{};
  std::uint32_t payload_crc32c{}; // over the record with this field = 0
  std::uint16_t kind_bits{};
  std::uint16_t symbol_len{};
  std::uint16_t al_n_collocation{};
  std::uint16_t al_n_quadrature{};
  std::uint16_t al_max_newton_iter{};
  std::uint16_t al_n_quad_price{}; // AlOpts::n_quad_price; 0 = tied to al_n_quadrature
  std::uint8_t method{};           // AmericanMethod (0 AndersenLake, 1 Baw)
  std::uint8_t prov_status{};      // ArchiveV2ProvenanceStatus
  std::uint8_t prov_purpose{};
  std::uint8_t prov_quality_mode{};
  std::uint8_t prov_state{};
  std::uint8_t reserved[7]{}; // zero; aligns `symbol` to a 64-B boundary
  char symbol[32]{};          // canonical, zero padded
};
static_assert(sizeof(ArchiveV2SurfaceHeader) == 224, "ArchiveV2SurfaceHeader layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveV2SurfaceHeader>);
static_assert(std::is_standard_layout_v<ArchiveV2SurfaceHeader>);

// Per-field offsets pinned explicitly (spec §2-§3 tables): this is an on-disk ABI, so a
// field reorder that preserved sizeof would still silently corrupt readers.
static_assert(offsetof(ArchiveV2Header, file_size) == 8);
static_assert(offsetof(ArchiveV2Header, created_ts_ns) == 16);
static_assert(offsetof(ArchiveV2Header, schema_hash) == 24);
static_assert(offsetof(ArchiveV2Header, lookup_offset) == 32);
static_assert(offsetof(ArchiveV2Header, directory_offset) == 40);
static_assert(offsetof(ArchiveV2Header, data_offset) == 48);
static_assert(offsetof(ArchiveV2Header, surface_count) == 56);
static_assert(offsetof(ArchiveV2Header, lookup_slot_count) == 60);
static_assert(offsetof(ArchiveV2Header, header_crc32c) == 76);
static_assert(offsetof(ArchiveV2Header, metadata_crc32c) == 80);
static_assert(offsetof(ArchiveV2Header, flags) == 84);
static_assert(offsetof(ArchiveV2Header, major) == 88);
static_assert(offsetof(ArchiveV2Header, reserved) == 98);
static_assert(offsetof(ArchiveV2LookupSlot, surface_offset) == 8);
static_assert(offsetof(ArchiveV2LookupSlot, surface_size) == 16);
static_assert(offsetof(ArchiveV2LookupSlot, payload_crc32c) == 20);
static_assert(offsetof(ArchiveV2LookupSlot, slot_crc32c) == 24);
static_assert(offsetof(ArchiveV2LookupSlot, symbol_len) == 28);
static_assert(offsetof(ArchiveV2LookupSlot, flags) == 29);
static_assert(offsetof(ArchiveV2LookupSlot, symbol) == 32);
static_assert(offsetof(ArchiveV2DirEntry, symbol_hash) == 16);
static_assert(offsetof(ArchiveV2DirEntry, uid) == 24);
static_assert(offsetof(ArchiveV2DirEntry, payload_crc32c) == 28);
static_assert(offsetof(ArchiveV2DirEntry, n_slices) == 32);
static_assert(offsetof(ArchiveV2DirEntry, symbol) == 40);
static_assert(offsetof(ArchiveV2SurfaceHeader, record_size) == 8);
static_assert(offsetof(ArchiveV2SurfaceHeader, S) == 16);
static_assert(offsetof(ArchiveV2SurfaceHeader, r) == 24);
static_assert(offsetof(ArchiveV2SurfaceHeader, now_ts_ns) == 32);
static_assert(offsetof(ArchiveV2SurfaceHeader, al_tol) == 40);
static_assert(offsetof(ArchiveV2SurfaceHeader, prov_validation_id) == 48);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_kind_off) == 72);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_payload_off_off) == 144);
static_assert(offsetof(ArchiveV2SurfaceHeader, uid) == 152);
static_assert(offsetof(ArchiveV2SurfaceHeader, n_slices) == 156);
static_assert(offsetof(ArchiveV2SurfaceHeader, prov_validation_failures) == 160);
static_assert(offsetof(ArchiveV2SurfaceHeader, payload_crc32c) == 164);
static_assert(offsetof(ArchiveV2SurfaceHeader, kind_bits) == 168);
static_assert(offsetof(ArchiveV2SurfaceHeader, symbol_len) == 170);
static_assert(offsetof(ArchiveV2SurfaceHeader, al_n_quad_price) == 178);
static_assert(offsetof(ArchiveV2SurfaceHeader, method) == 180);
static_assert(offsetof(ArchiveV2SurfaceHeader, prov_status) == 181);
static_assert(offsetof(ArchiveV2SurfaceHeader, prov_state) == 184);
static_assert(offsetof(ArchiveV2SurfaceHeader, symbol) == 192);

// ── Writer ─────────────────────────────────────────────────────────────────

struct ArchiveV2WriteOpts {
  std::uint32_t flags{0};
  std::uint32_t lookup_load_pct{70}; // (0, 100]; the table always keeps one empty slot
  std::uint32_t surface_alignment{kArchiveV2SurfaceAlign}; // power of two >= 8
  // 0 => a DETERMINISTIC content hash (CRC-32C of [header, EOF) folded with file_size),
  // NOT the wall clock: a content-derived identity, not a timestamp — may be negative
  // as int64, never tamper evidence. Explicit nonzero honored verbatim. Spec §8.
  std::int64_t created_ts_ns{0};
};

// Serialize `items` into an in-memory archive. Deterministic: the same items (in any
// order) and opts give the same bytes. Errors: InvalidArgument (no items, more than
// kArchiveV2MaxSurfaces, null surface, empty or > kArchiveSymbolMax symbol, zero or
// > 65535 slices, a record >= 4 GiB, invalid provenance, bad opts); AlreadyExists
// (duplicate canonical symbol).
[[nodiscard]] Result<std::vector<std::byte>>
write_surface_archive_v2(std::span<const SurfaceArchiveItem> items,
                         const ArchiveV2WriteOpts &opts = {});

// As above, persisted through a unique same-directory temp with serialized, durable
// atomic replacement (spec §9).
[[nodiscard]] Status write_surface_archive_v2_file(std::string_view path,
                                                   std::span<const SurfaceArchiveItem> items,
                                                   const ArchiveV2WriteOpts &opts = {});

// ── Point read (spec §4) ───────────────────────────────────────────────────

// The reader's own I/O accounting for one point read.
struct ArchivePointReadStats {
  std::uint64_t bytes_read{0};   // bytes fetched: header + slot runs + record
  std::uint32_t read_calls{0};   // positional read syscalls issued
  std::uint32_t slots_probed{0}; // lookup slots examined
};

// One surface served by `point_read_archive_surface`: `backing` OWNS the verified
// bytes of exactly one record (heap, so moving the struct keeps `view` valid), `view`
// borrows them. Move-only (the view is).
struct PointReadSurface {
  std::shared_ptr<const void> backing;
  PricedSurfaceView view;
  SurfaceProvenance provenance;
  ArchivePointReadStats stats;
};

// An owned reconstruction served by `point_read_archive_reconstruct`.
struct PointReadOwnedSurface {
  ArchivedSurface archived;
  ArchivePointReadStats stats;
};

// Serve `symbol` from the archive file at `path` touching only the header, the probed
// lookup slot(s) and the record extent; every touched byte is CRC-verified and the
// record must name `symbol`. Errors: InvalidArgument (empty or > kArchiveSymbolMax
// symbol); NotFound (no such file, or the symbol is absent); IoError (the file cannot
// be opened or read); ParseError (anything malformed, including a corrupt slot or
// record — never a wrong surface).
[[nodiscard]] Result<PointReadSurface> point_read_archive_surface(std::string_view path,
                                                                  std::string_view symbol);
[[nodiscard]] Result<PointReadOwnedSurface>
point_read_archive_reconstruct(std::string_view path, std::string_view symbol);

// ── Whole-board reader ─────────────────────────────────────────────────────

// One view paired with the provenance parsed from the same record. Move-only.
struct ArchivedSurfaceView {
  PricedSurfaceView view;
  SurfaceProvenance provenance;
};

// An opened archive. BACKING-AGNOSTIC: a `std::span<const std::byte>` over the bytes
// plus a type-erased owner keeping them alive (an owned buffer or an
// `atx::tsdb::Mapping`). All queries are `const`; a returned `PricedSurfaceView`
// BORROWS this archive's bytes and must not outlive it.
class SurfaceArchiveV2 {
public:
  // Take ownership of `bytes` and run the eager metadata validation (spec §5): header,
  // metadata CRC, every slot's CRC/emptiness/hash, directory order and hashes, and the
  // lookup <-> directory bijection. No record CRC (lazy). ParseError on any failure.
  [[nodiscard]] static Result<SurfaceArchiveV2> open(std::vector<std::byte> bytes);

  // Read the whole file with ONE positional read into an owned buffer (no stream, no
  // mapping, the handle shared for delete and closed before return), then validate as
  // `open`. NotFound iff the file is absent; IoError for any other read failure.
  [[nodiscard]] static Result<SurfaceArchiveV2> open_copied(std::string_view path);
  // The same operation as `open_copied`, kept for test fixtures (no production caller).
  [[nodiscard]] static Result<SurfaceArchiveV2> open_file(std::string_view path);

  // Map `path` read-only and keep the mapping for the archive's lifetime (records fault
  // in lazily). On Windows a live mapping blocks a republish of that file, so use it
  // only for read-only corpora. IoError / InvalidArgument (empty file) from the map.
  [[nodiscard]] static Result<SurfaceArchiveV2> open_mapped(std::string_view path);

  // View over externally-owned bytes; `owner` keeps them alive. Same validation as
  // `open`.
  [[nodiscard]] static Result<SurfaceArchiveV2>
  open_borrowed(std::span<const std::byte> bytes, std::shared_ptr<const void> owner);

  [[nodiscard]] std::uint32_t count() const noexcept { return header_.surface_count; }
  [[nodiscard]] const ArchiveV2Header &header() const noexcept { return header_; }
  // The parsed directory (sorted by canonical symbol), owned by this archive object —
  // it does not point into the backing bytes. Valid for the archive's lifetime.
  [[nodiscard]] std::span<const ArchiveV2DirEntry> directory() const noexcept { return directory_; }
  // Framing-only enumeration (C6 / SE-P2-6): the same span, O(1), touches no record.
  [[nodiscard]] std::span<const ArchiveV2DirEntry> entries() const noexcept { return directory_; }
  [[nodiscard]] std::size_t entry_count() const noexcept { return directory_.size(); }

  [[nodiscard]] ArchiveContentIdentity identity() const noexcept {
    return archive_v2_identity_from_header(header_);
  }

  // Resolve `symbol` (case-insensitive) to its STORED directory entry. NotFound if
  // absent (including any symbol longer than kArchiveSymbolMax).
  [[nodiscard]] Result<ArchiveV2DirEntry> find(std::string_view symbol) const;

  // Provenance from a symbol's record header (framing + binding checked, no CRC).
  [[nodiscard]] Result<SurfaceProvenance> provenance(std::string_view symbol) const;

  // SUBSET MAP: hash-probe `symbol`, check the record's framing, binding and geometry,
  // and build a zero-copy view over ONLY that record's extent. NotFound if absent;
  // ParseError on a bad record.
  [[nodiscard]] Result<PricedSurfaceView> map_symbol(std::string_view symbol) const;

  // Whole-board views, in directory order.
  [[nodiscard]] Result<std::vector<PricedSurfaceView>> map_all() const;
  [[nodiscard]] Result<std::vector<ArchivedSurfaceView>> map_all_with_provenance() const;

  // View + provenance from a directory entry the caller already holds (no re-probe).
  [[nodiscard]] Result<ArchivedSurfaceView> map_entry(const ArchiveV2DirEntry &e) const;

  // Owned reconstructs — the inverse of the writer, bit-identical to the source.
  [[nodiscard]] Result<PricedSurface> reconstruct_symbol(std::string_view symbol) const;
  [[nodiscard]] Result<ArchivedSurface> reconstruct_entry(const ArchiveV2DirEntry &e) const;
  [[nodiscard]] Result<std::vector<PricedSurface>> reconstruct_all() const;
  [[nodiscard]] Result<std::vector<ArchivedSurface>> reconstruct_all_with_provenance() const;

  // Lazy integrity: recompute one / every record's payload CRC. NotFound if absent;
  // ParseError on a checksum or framing failure.
  [[nodiscard]] Status validate_symbol(std::string_view symbol) const;
  [[nodiscard]] Status validate_all() const;

private:
  SurfaceArchiveV2() = default;

  [[nodiscard]] static Result<SurfaceArchiveV2> open_impl(std::span<const std::byte> bytes,
                                                          std::shared_ptr<const void> owner);
  [[nodiscard]] const ArchiveV2LookupSlot *find_slot(std::string_view symbol) const noexcept;
  [[nodiscard]] const ArchiveV2DirEntry *entry_for_slot(const ArchiveV2LookupSlot &s) const noexcept;
  // The bounds-, framing-, binding- and geometry-checked extent of directory entry `e`.
  [[nodiscard]] Result<std::span<const std::byte>> checked_record(const ArchiveV2DirEntry &e) const;
  [[nodiscard]] Status validate_record_crc(const ArchiveV2DirEntry &e) const;

  std::span<const std::byte> bytes_{};
  std::shared_ptr<const void> owner_{};
  ArchiveV2Header header_{};
  std::vector<ArchiveV2LookupSlot> lookup_{};
  std::vector<ArchiveV2DirEntry> directory_{};
};

} // namespace atx::vol
