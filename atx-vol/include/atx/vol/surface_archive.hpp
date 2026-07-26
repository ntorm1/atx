#pragma once

// Priced curve-surface archive (ATXVSA v3) — a proprietary on-disk / in-memory
// binary container that packs many fitted `PricedSurface`s into one file with
// O(1) symbol lookup and per-surface random access.
//
// v3 is a ground-up revamp for the configurable curve family (vol_curve.hpp): a
// serialized surface is now a `PricedSurface` — a polymorphic `CurveSurface` of
// ANY `VolCurveKind` (ConvexDense / Essvi / Svi) plus the per-slice re-pricing
// context and the cold pricing scalars — so a surface of any selected type
// round-trips fit -> serialize -> deserialize -> price with IDENTICAL theo values.
// There is NO backward compatibility with the v2 (VolSurface-only, fixed-stride
// eSSVI/SVI) format; the magic, schema hash, and blob shape all changed.
//
// ── On-disk shape ─────────────────────────────────────────────────────────
//
//   header (464 B) -> lookup table -> directory (jump table) -> 4096-aligned blobs
//
//   * lookup table — open-addressed hash (symbol_hash -> slot). Resolve one symbol
//                    to its blob's byte offset/size without scanning the archive.
//   * directory    — one entry per surface, sorted by canonical symbol: the
//                    "jump table" of (offset, size, uid, kinds, n_slices) that lets
//                    a reader seek to and reconstruct a SINGLE surface without
//                    touching any other blob's bytes.
//   * blob         — a self-describing, variable-length record. Unlike v2's
//                    fixed-stride POD slice array, a v3 blob holds a kind-TAGGED
//                    sequence of slice records (ConvexDense carries a
//                    variable-length node array; Essvi/Svi carry a fixed POD).
//
// ── Integrity + SOTA deserialization ───────────────────────────────────────
//
// Layered CRC-32C: a header CRC (its own field zeroed), a metadata CRC over the
// lookup ‖ directory span, and a per-blob CRC (in the owning lookup slot) plus a
// payload CRC (in the blob header). The CRC-32C is HARDWARE-accelerated — an
// SSE4.2 `_mm_crc32_u64` path (8 bytes/step), runtime-dispatched by CPUID, with a
// table-driven fallback that produces bit-identical results (cross-checked in the
// tests). Deserialization is a single bounds-checked pass with direct value
// construction (no intermediate `create`/validate churn): a single-surface
// `map_symbol` is a hash probe + one blob parse, independent of archive size.
//
// ── Schema hash / endianness ────────────────────────────────────────────────
//
// The header stores a compile-time fingerprint folded from the `sizeof` of every
// on-disk record and the serialized POD slice structs (+ a v3 format salt). A
// reader built against a different struct shape recomputes a different hash and
// refuses the file with ParseError. Records are host byte order; the header stamps
// endian = 1 (little) / pointer_bits = 64 and a reader rejects any mismatch. Only
// little-endian LP64 hosts are supported (matches the rest of atx-vol).
//
// ── Thread safety ────────────────────────────────────────────────────────────
//
// A parsed `SurfaceArchive` is immutable after `open`; `count`, `header`,
// `directory`, `find`, `map_symbol`, and `map_all` are all `const` and touch no
// shared mutable state, so any number of threads may query one `const` archive
// concurrently. Each `map_symbol` returns a fresh, independently-owned
// `PricedSurface` — no aliasing with the archive's bytes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "atx/vol/priced_surface.hpp"      // PricedSurface, PricingContext
#include "atx/vol/priced_surface_view.hpp" // PricedSurfaceView (v2 zero-copy read view)
#include "atx/vol/surface_policy.hpp"      // purpose, quality, health, validation
#include "atx/vol/types.hpp"

#include <memory> // std::shared_ptr (v2 backing owner)

namespace atx::vol {

// ── On-wire format constants ─────────────────────────────────────────────

inline constexpr std::uint16_t kArchiveMajor = 3;
inline constexpr std::uint16_t kArchiveMinor = 0;

// Maximum canonical-symbol length stored inline. Longer symbols are truncated.
inline constexpr std::size_t kArchiveSymbolMax = 32;

// Surface blobs start on this file alignment; sections/arrays inside a blob start
// on `kArchiveArrayAlign`.
inline constexpr std::uint32_t kArchiveBlobAlign = 4096;
inline constexpr std::uint32_t kArchiveArrayAlign = 64;

// Lookup-slot occupancy flags.
inline constexpr std::uint16_t kArchiveSlotEmpty = 0;
inline constexpr std::uint16_t kArchiveSlotOccupied = 1;

// ── On-disk records (POD, little-endian, fixed layout) ───────────────────
//
// Every field is fixed-width; the structs are trivially copyable + standard
// layout, so (de)serialization is a bounds-checked `std::memcpy`. Sizes are pinned
// by static_assert; layout drift is a compile error (and changes the schema hash).

// File header. Lives at offset 0. `header_crc32c` covers the whole header with
// that field zeroed; `metadata_crc32c` covers the lookup ‖ directory span.
struct ArchiveHeader {
  char magic[8]{};       // "ATXVSA03", no NUL
  std::uint16_t major{}; // kArchiveMajor
  std::uint16_t minor{};
  std::uint16_t header_size{};    // sizeof(ArchiveHeader)
  std::uint16_t endian{};         // 1 = little
  std::uint16_t pointer_bits{};   // 64
  std::uint16_t alignment_log2{}; // 12 -> 4096 blob alignment
  std::uint32_t flags{};

  std::uint64_t file_size{};
  std::uint64_t created_ts_ns{};
  std::uint64_t schema_hash{};         // sizeof-based layout fingerprint
  std::uint64_t writer_version_hash{}; // informational; 0 if unset

  std::uint32_t surface_count{};
  std::uint32_t lookup_slot_count{}; // power of two
  std::uint64_t lookup_offset{};
  std::uint64_t directory_offset{};
  std::uint64_t data_offset{};

  std::uint32_t index_slot_size{};          // sizeof(ArchiveIndexSlot)
  std::uint32_t dir_entry_size{};           // sizeof(ArchiveDirEntry)
  std::uint32_t surface_blob_header_size{}; // sizeof(SurfaceBlobHeader)
  std::uint32_t slice_header_size{};        // sizeof(ArchiveSliceHeader)

  std::uint32_t header_crc32c{};   // over header bytes, this field = 0
  std::uint32_t metadata_crc32c{}; // over (lookup ‖ directory) bytes

  std::uint8_t reserved[352]{}; // pad tail; covered by header_crc
};
static_assert(sizeof(ArchiveHeader) == 464, "ArchiveHeader layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveHeader>);
static_assert(std::is_standard_layout_v<ArchiveHeader>);

// R-19 (F6): a cheap content/build identity for an on-disk archive, derived
// purely from its 464-byte header. `file_size` + `created_ts_ns` distinguish a
// rewrite that changes size or timestamp; `header_crc32c` covers the header
// (incl. schema_hash, surface_count, created_ts) and `metadata_crc32c` covers
// the lookup ‖ directory span — and because every per-blob CRC lives in a lookup
// slot, ANY blob-payload rewrite changes `metadata_crc32c` too, EVEN one that
// preserves the byte length (the same-length/different-CRC case). So two archives
// with the same identity are byte-equivalent for serving purposes. Reading this
// identity is one small header read, not a whole-file hash — see
// `SnapshotCache`, which keys/evicts on it so a rewritten archive never serves a
// stale cached snapshot.
struct ArchiveContentIdentity {
  std::uint64_t file_size{0};
  std::uint64_t created_ts_ns{0};
  std::uint32_t header_crc32c{0};
  std::uint32_t metadata_crc32c{0};

  [[nodiscard]] bool operator==(const ArchiveContentIdentity &) const noexcept = default;
};

// Pure projection of the content identity from an already-parsed header (no I/O).
// Callers that only have the raw header bytes memcpy them into an `ArchiveHeader`
// and call this; the file read stays in the caller (keeps <fstream> out of this
// widely-included header).
[[nodiscard]] inline ArchiveContentIdentity
archive_identity_from_header(const ArchiveHeader &header) noexcept {
  return ArchiveContentIdentity{header.file_size, header.created_ts_ns, header.header_crc32c,
                                header.metadata_crc32c};
}

// Open-addressed lookup slot. Carries the blob's whole-blob CRC so a symbol probe
// verifies integrity without a second table.
struct ArchiveIndexSlot {
  std::uint64_t symbol_hash{};
  std::uint64_t surface_offset{};
  std::uint64_t surface_size{};
  std::uint32_t surface_crc32c{}; // CRC-32C over the whole blob
  std::uint32_t uid{};
  std::uint16_t symbol_len{};
  std::uint16_t flags{}; // kArchiveSlot*
  char symbol[32]{};     // canonical, not NUL-terminated
  std::uint8_t reserved[60]{};
};
static_assert(sizeof(ArchiveIndexSlot) == 128, "ArchiveIndexSlot layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveIndexSlot>);
static_assert(std::is_standard_layout_v<ArchiveIndexSlot>);

// Directory entry (one per surface, sorted by canonical symbol) — the jump table.
struct ArchiveDirEntry {
  std::uint64_t surface_offset{};
  std::uint64_t surface_size{};
  std::uint64_t symbol_hash{};
  std::uint32_t uid{};
  std::uint32_t reserved0{};
  std::uint16_t symbol_len{};
  std::uint16_t kind_bits{}; // OR of (1u << VolCurveKind) present
  std::uint16_t n_slices{};
  std::uint16_t flags{};
  char symbol[32]{};
  std::uint8_t reserved[16]{};
};
static_assert(sizeof(ArchiveDirEntry) == 88, "ArchiveDirEntry layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveDirEntry>);
static_assert(std::is_standard_layout_v<ArchiveDirEntry>);

// Per-surface blob header. Lives at the start of each blob; the symbol bytes, the
// pricing record, and the sequential slice records follow on aligned offsets.
// `payload_crc32c` covers [blob_header_size, blob_size); the owning slot's
// `surface_crc32c` covers the whole blob.
struct SurfaceBlobHeader {
  char magic[8]{}; // "ATXVSB03"
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint16_t flags{};
  std::uint16_t n_slices{};
  std::uint32_t uid{};
  std::uint32_t blob_header_size{}; // sizeof(SurfaceBlobHeader)
  std::uint64_t blob_size{};
  std::uint64_t symbol_offset{};
  std::uint64_t symbol_size{};
  std::uint64_t pricing_offset{}; // ArchivePricingRecord
  std::uint64_t pricing_size{};
  std::uint64_t slices_offset{}; // start of the sequential slice records
  std::uint64_t slices_size{};   // total bytes across all slice records
  std::uint32_t payload_crc32c{};
  std::uint32_t reserved0{};
  std::uint8_t reserved[40]{};
};
static_assert(sizeof(SurfaceBlobHeader) == 128, "SurfaceBlobHeader layout drift");
static_assert(std::is_trivially_copyable_v<SurfaceBlobHeader>);
static_assert(std::is_standard_layout_v<SurfaceBlobHeader>);

// Versioned payload stored inside SurfaceBlobHeader::reserved. Keeping this
// record exactly 40 bytes preserves the complete ATXVSA v3 framing and schema
// hash. A zero marker is a legacy v3 blob written before provenance existed.
struct ArchiveSurfaceProvenanceRecord {
  std::uint32_t marker{};      // kArchiveProvenanceMarker, or 0 legacy
  std::uint8_t purpose{};      // SurfacePurpose
  std::uint8_t quality_mode{}; // FitQualityMode
  std::uint8_t state{};        // SurfaceState
  std::uint8_t reserved0{};
  std::uint32_t validation_failures{}; // ValidationFailure bitmask
  std::uint32_t reserved1{};           // explicit alignment / future flags
  std::uint64_t validation_id{};
  std::uint64_t source_generation{};
  std::uint64_t served_generation{};
};
static_assert(sizeof(ArchiveSurfaceProvenanceRecord) == 40,
              "archive provenance must fit the v3 reserved blob-header bytes");
static_assert(std::is_trivially_copyable_v<ArchiveSurfaceProvenanceRecord>);
static_assert(std::is_standard_layout_v<ArchiveSurfaceProvenanceRecord>);

inline constexpr std::uint32_t kArchiveProvenanceMarker = 0x31565053u; // "SPV1"

// Public metadata paired with an archived surface. ValidationDigest's identity
// and failure mask are persisted; detailed per-gate counts remain in the
// machine-readable validation report keyed by validation_id.
struct SurfaceProvenance {
  SurfacePurpose purpose{SurfacePurpose::Risk};
  FitQualityMode quality_mode{FitQualityMode::Balanced};
  SurfaceState state{SurfaceState::Healthy};
  ValidationDigest validation{};
  std::uint64_t source_generation{};
  std::uint64_t served_generation{};
  bool legacy_format{false};
};

// Safe interpretation for a v3 archive whose reserved provenance bytes are all
// zero. Legacy surfaces were never independently admitted, so they are exposed
// as degraded market marks rather than silently promoted to risk.
[[nodiscard]] SurfaceProvenance legacy_surface_provenance() noexcept;

// One independently-owned reconstruction paired with the provenance parsed from
// the same archive blob. The record is move-only because PricedSurface owns its
// polymorphic curves; moving transfers both the surface and its exact metadata.
struct ArchivedSurface {
  PricedSurface surface;
  SurfaceProvenance provenance;
};
static_assert(!std::is_copy_constructible_v<ArchivedSurface>);
static_assert(!std::is_copy_assignable_v<ArchivedSurface>);
static_assert(std::is_nothrow_move_constructible_v<ArchivedSurface>);
static_assert(std::is_nothrow_move_assignable_v<ArchivedSurface>);

// Blob-level pricing scalars (one per surface) — the cold re-pricing context.
// Mirrors `PricingContext` + the `AlOpts` fields, laid out fixed-width.
struct ArchivePricingRecord {
  double S{};
  double r{};
  std::int64_t now_ts_ns{};
  std::uint32_t uid{};
  std::uint8_t method{}; // AmericanMethod
  std::uint8_t reserved0[3]{};
  std::uint16_t al_n_collocation{};
  std::uint16_t al_n_quadrature{};
  std::uint16_t al_max_newton_iter{};
  std::uint16_t reserved1{};
  double al_tol{};
};
static_assert(sizeof(ArchivePricingRecord) == 48, "ArchivePricingRecord layout drift");
static_assert(std::is_trivially_copyable_v<ArchivePricingRecord>);
static_assert(std::is_standard_layout_v<ArchivePricingRecord>);

// Fixed header preceding each slice's (possibly variable-length) payload. Carries
// the slice's re-pricing context (SliceContext), the curve scalars (T, forward,
// df) needed to reconstruct the polymorphic curve, and — for ConvexDense — the fit
// diagnostics + node count. `rec_size` is the total padded bytes of this record
// (header + payload + pad), so the reader walks slices with a single running
// offset. `payload_size` is the pre-pad payload bytes.
struct ArchiveSliceHeader {
  std::uint8_t kind{}; // VolCurveKind
  std::uint8_t reserved0[3]{};
  std::uint32_t rec_size{};     // total padded record bytes
  std::uint32_t node_count{};   // ConvexDense node count; 0 otherwise
  std::uint32_t payload_size{}; // payload bytes following the header

  double T{};                // year-fraction to expiry
  double forward{};          // term forward F
  double borrow{};           // implied/fixed per-term borrow
  double q_eff{};            // effective carry r - ln(F/S)/T
  double df{};               // discount factor exp(-rT)
  std::uint64_t n_used{};    // strikes that survived to the fit
  std::uint64_t n_dropped{}; // strikes skipped

  double conv_rmse_price{};      // ConvexDense: vega/spread-weighted RMSE
  std::uint64_t conv_n_obs{};    // ConvexDense: observations fit
  std::uint64_t conv_n_active{}; // ConvexDense: active constraints
};
static_assert(sizeof(ArchiveSliceHeader) == 96, "ArchiveSliceHeader layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveSliceHeader>);
static_assert(std::is_standard_layout_v<ArchiveSliceHeader>);

// ── Writer inputs ────────────────────────────────────────────────────────

struct SurfaceArchiveWriteOpts {
  std::uint32_t flags{0};
  // Lookup hash-table load factor (percent), in (0, 100]. Lower reserves slack.
  std::uint32_t lookup_load_pct{70};
  std::uint32_t blob_alignment{kArchiveBlobAlign};
  std::uint32_t array_alignment{kArchiveArrayAlign};
  // Stamp for `ArchiveHeader::created_ts_ns`. A 0 sentinel is filled from a
  // DETERMINISTIC content hash — CRC-32C of the payload span [header, EOF) folded
  // with file_size — NOT the wall clock. It is therefore a content-derived
  // IDENTITY, not a timestamp: it may be negative as int64, ordering by it is
  // meaningless, it carries only ~32 bits of content entropy, and it is never
  // tamper evidence (CRC is linear/forgeable). Two identical builds get identical
  // stamps (SnapshotCache reproducibility); two different builds get distinct ones
  // (staleness). An explicit nonzero value is honored verbatim (tests pin it). See
  // docs/atxvsa2-format.md §5.2. (Both v1 and v2 archive writers behave this way;
  // the SurfaceDb MANIFEST timestamps, by contrast, stay wall-clock.)
  std::int64_t created_ts_ns{0};
};

// One (symbol, surface) entry to archive. Non-owning: `surface` must outlive the
// write call, and `symbol` need only be valid for its duration.
struct SurfaceArchiveItem {
  std::string_view symbol{};
  const PricedSurface *surface{nullptr};
  // nullopt preserves the byte-compatible legacy-v3 representation. New V2
  // writers should always supply explicit provenance.
  std::optional<SurfaceProvenance> provenance{};
};

// ── Writer ───────────────────────────────────────────────────────────────

// Serialize `items` into an in-memory archive byte buffer. Symbols are
// canonicalized (ASCII upper-cased, truncated to kArchiveSymbolMax) before hashing
// / storage; a canonical-symbol collision is a duplicate.
//
// Errors: InvalidArgument (empty list, null/empty surface, empty symbol, bad load
// factor); AlreadyExists (duplicate canonical symbol).
[[nodiscard]] Result<std::vector<std::byte>>
write_surface_archive(std::span<const SurfaceArchiveItem> items,
                      const SurfaceArchiveWriteOpts &opts = {});

// As above, persisted to `path` through a unique same-directory temp, with
// same-destination serialization and durable atomic replacement. Adds IoError
// on any filesystem failure.
[[nodiscard]] Status write_surface_archive_file(std::string_view path,
                                                std::span<const SurfaceArchiveItem> items,
                                                const SurfaceArchiveWriteOpts &opts = {});

// ── Reader ───────────────────────────────────────────────────────────────

// An opened, validated archive. Owns its bytes; all query methods are `const` and
// thread-safe to call concurrently. Rule of Zero: movable, trivially destructible.
class SurfaceArchive {
public:
  // Take ownership of `bytes` and validate the full framing (magic, version,
  // endian, sizes, schema hash, header CRC, metadata CRC, directory bounds).
  // Errors: ParseError on any framing / checksum / bounds failure.
  [[nodiscard]] static Result<SurfaceArchive> open(std::vector<std::byte> bytes);

  // Read `path` fully into a buffer and `open` it. Adds IoError / NotFound.
  [[nodiscard]] static Result<SurfaceArchive> open_file(std::string_view path);

  [[nodiscard]] std::uint32_t count() const noexcept { return header_.surface_count; }
  [[nodiscard]] const ArchiveHeader &header() const noexcept { return header_; }
  [[nodiscard]] std::span<const ArchiveDirEntry> directory() const noexcept { return directory_; }

  // Resolve `symbol` (case-insensitive) to its directory entry fabricated from the
  // lookup slot. NotFound if absent.
  [[nodiscard]] Result<ArchiveDirEntry> find(std::string_view symbol) const;

  // Read the versioned metadata from a symbol's blob. Legacy zero-filled v3
  // records return legacy_surface_provenance(); malformed tagged metadata is a
  // ParseError. The blob CRC is verified before metadata is returned.
  [[nodiscard]] Result<SurfaceProvenance> provenance(std::string_view symbol) const;

  // Resolve `symbol` and reconstruct its `PricedSurface` (an owned copy). NotFound
  // if absent; ParseError on a bad blob (magic / bounds / CRC / kind).
  [[nodiscard]] Result<PricedSurface> map_symbol(std::string_view symbol) const;

  // Reconstruct every surface, in directory order. ParseError if any blob fails.
  [[nodiscard]] Result<std::vector<PricedSurface>> map_all() const;

  // Reconstruct every surface and its same-blob provenance in directory order.
  // Each blob checksum and header are parsed once. ParseError if any blob or
  // tagged provenance record fails validation.
  [[nodiscard]] Result<std::vector<ArchivedSurface>> map_all_with_provenance() const;

  // Reconstruct every surface into caller storage, in directory order. Returns the
  // number written. OutOfRange if `out.size() < count()`; ParseError on a bad blob.
  [[nodiscard]] Result<std::size_t> map_all_into(std::span<std::optional<PricedSurface>> out) const;

private:
  SurfaceArchive() = default;

  [[nodiscard]] const ArchiveIndexSlot *find_slot(std::string_view symbol) const noexcept;
  [[nodiscard]] Result<ArchivedSurface> reconstruct(const ArchiveIndexSlot &slot,
                                                    const ArchiveDirEntry *directory) const;
  [[nodiscard]] Result<SurfaceProvenance> read_provenance(std::uint64_t offset, std::uint64_t size,
                                                          std::uint32_t expected_crc) const;

  std::vector<std::byte> buffer_{};        // owns the raw archive bytes
  ArchiveHeader header_{};                 // parsed copy
  std::vector<ArchiveIndexSlot> lookup_{}; // parsed copy of the lookup table
  std::vector<ArchiveDirEntry> directory_{};
};

// ═══════════════════════════════════════════════════════════════════════════
// ATXVSA2 — zero-copy mmap columnar format (v2). Spec + design lineage:
// atx-vol/docs/atxvsa2-format.md. Single contiguous region; per-symbol directory
// of BYTE-OFFSETS (no pointers → no relocation on map); per-surface COLUMNAR SoA
// slice arrays (Arrow-style contiguous typed columns); every field naturally
// aligned (FlatBuffers); surfaces packed on 64-B (NO 4096-B blob pad). Integrity
// is LAZY: `open` checks header + metadata + framing only; per-record CRC is
// validate-on-demand, never on the price path. Sources cited in the design note.
// ═══════════════════════════════════════════════════════════════════════════

inline constexpr std::uint16_t kArchiveV2Major = 4; // distinct from v1 (major 3)
inline constexpr std::uint16_t kArchiveV2Minor = 1; // minor 1: SplineVol payload gained mult_cap+w_offset

// Surfaces pack on this file alignment (cache line / SIMD headroom) — replaces
// v1's 4096-B kArchiveBlobAlign (bottleneck #6). Columns inside a record align to
// their natural size (8 for f64/u64). The file TAIL is padded to a page for mmap.
inline constexpr std::uint32_t kArchiveV2SurfaceAlign = 64;
inline constexpr std::uint32_t kArchiveV2ColumnAlign = 8;
inline constexpr std::uint32_t kArchiveV2FilePageAlign = 4096;

inline constexpr std::uint16_t kArchiveV2SlotEmpty = 0;
inline constexpr std::uint16_t kArchiveV2SlotOccupied = 1;

// ── File header (offset 0). All fields naturally aligned (8-byte block first). ──
struct ArchiveV2Header {
  char magic[8]{}; // "ATXVSA20", no NUL
  std::uint64_t file_size{};
  std::uint64_t created_ts_ns{};
  std::uint64_t schema_hash{};         // v2 sizeof-fold + v2 salt
  std::uint64_t writer_version_hash{}; // informational; 0 if unset
  std::uint64_t lookup_offset{};
  std::uint64_t directory_offset{};
  std::uint64_t data_offset{};
  std::uint32_t surface_count{};
  std::uint32_t lookup_slot_count{}; // power of two
  std::uint32_t lookup_slot_size{};  // sizeof(ArchiveV2LookupSlot)
  std::uint32_t dir_entry_size{};    // sizeof(ArchiveV2DirEntry)
  std::uint32_t surface_header_size{}; // sizeof(ArchiveV2SurfaceHeader)
  std::uint32_t header_crc32c{};       // over header bytes, this field = 0
  std::uint32_t metadata_crc32c{};     // over (lookup ‖ directory) bytes
  std::uint32_t flags{};
  std::uint16_t major{}; // kArchiveV2Major
  std::uint16_t minor{};
  std::uint16_t header_size{};  // sizeof(ArchiveV2Header)
  std::uint16_t endian{};       // 1 = little
  std::uint16_t pointer_bits{}; // 64
  std::uint16_t reserved_u16{};
  std::uint8_t reserved[148]{}; // pad to 256; covered by header_crc
};
static_assert(sizeof(ArchiveV2Header) == 256, "ArchiveV2Header layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveV2Header>);
static_assert(std::is_standard_layout_v<ArchiveV2Header>);

// R-19 (F6) content identity from an already-parsed v2 header (no I/O) — the v2
// analogue of `archive_identity_from_header`. `file_size` + `created_ts_ns`
// distinguish a rewrite that changes size or timestamp; `header_crc32c` covers
// the header (incl. schema_hash, surface_count, created_ts) and `metadata_crc32c`
// covers the lookup ‖ directory span (and because a rewrite of any surface's
// bytes changes its directory/lookup (offset,size), it changes this too). Used by
// `SnapshotCache` to evict a stale cached snapshot when a partition is rewritten
// in place. Mirrors `SurfaceArchiveV2::identity()`.
[[nodiscard]] inline ArchiveContentIdentity
archive_v2_identity_from_header(const ArchiveV2Header &header) noexcept {
  return ArchiveContentIdentity{header.file_size, header.created_ts_ns, header.header_crc32c,
                                header.metadata_crc32c};
}

// Open-addressed lookup slot: symbol → surface record byte offset/size. O(1)
// `map_symbol` / `find`. No CRC here (CRC is lazy / per-record).
struct ArchiveV2LookupSlot {
  std::uint64_t symbol_hash{};
  std::uint64_t surface_offset{}; // byte offset (file-relative) to the record
  std::uint64_t surface_size{};   // record extent (self-contained)
  std::uint32_t uid{};
  std::uint16_t symbol_len{};
  std::uint16_t flags{}; // kArchiveV2Slot*
  char symbol[32]{};     // canonical, not NUL-terminated
};
static_assert(sizeof(ArchiveV2LookupSlot) == 64, "ArchiveV2LookupSlot layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveV2LookupSlot>);
static_assert(std::is_standard_layout_v<ArchiveV2LookupSlot>);

// Directory entry (one per surface, sorted by canonical symbol) — the ordered
// jump table for `map_all`. Byte offsets, never pointers.
struct ArchiveV2DirEntry {
  std::uint64_t surface_offset{};
  std::uint64_t surface_size{};
  std::uint64_t symbol_hash{};
  std::uint32_t uid{};
  std::uint16_t n_slices{};
  std::uint16_t kind_bits{}; // OR of (1u << VolCurveKind)
  std::uint16_t symbol_len{};
  std::uint16_t flags{};
  // R-19 (F6) content identity: a copy of this record's `payload_crc32c` (also in
  // the record header). It lives in the directory — which `metadata_crc32c`
  // covers — so ANY surface-payload rewrite (even one that preserves the record's
  // byte length and offset) changes `metadata_crc32c` and therefore the archive's
  // content identity. This is the v2 analogue of v1's per-blob `surface_crc32c`
  // in the lookup slot; without it a same-length in-place rewrite would be
  // invisible to the SnapshotCache/SurfaceDb staleness check (the record CRC is
  // otherwise only in the record header, which the metadata CRC does not cover).
  std::uint32_t payload_crc32c{};
  char symbol[32]{};
  std::uint8_t reserved[8]{};
};
static_assert(sizeof(ArchiveV2DirEntry) == 80, "ArchiveV2DirEntry layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveV2DirEntry>);
static_assert(std::is_standard_layout_v<ArchiveV2DirEntry>);

// Per-surface record header (at each record's start). Carries the pricing
// scalars, the provenance record, and the block of record-relative BYTE OFFSETS
// to the columnar SoA slice arrays (the "pointer section", Cap'n Proto style).
// Fields ordered by descending alignment so there is no internal padding.
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
  // Record-relative offsets to the columnar arrays (each 8-B aligned).
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
  std::uint32_t prov_marker{}; // kArchiveProvenanceMarker, or 0 legacy
  std::uint32_t prov_validation_failures{};
  std::uint32_t payload_crc32c{}; // over record bytes with this field = 0 (lazy)
  std::uint32_t flags{};
  std::uint16_t kind_bits{};
  std::uint8_t method{}; // AmericanMethod
  std::uint8_t prov_purpose{};
  std::uint8_t prov_quality_mode{};
  std::uint8_t prov_state{};
  std::uint16_t al_n_collocation{};
  std::uint16_t al_n_quadrature{};
  std::uint16_t al_max_newton_iter{};
  // AlOpts::n_quad_price — the decoupled premium (pricing) Gauss-Legendre order
  // (SE-P1-2). 0 ties it to al_n_quadrature (the historical behavior), so this
  // field reads back as 0 on every pre-C2 archive (it occupied a zero-filled
  // reserved u16), and the reader maps 0 -> tied. Reusing the reserved slot keeps
  // the layout (and the sizeof-fold in schema_hash_v2) byte-identical; the salt
  // is bumped ANYWAY so a pre-C2 reader rejects a NEW archive that actually sets a
  // decoupled premium order (rather than silently mispricing it with the tied
  // order), while this reader's prior-salt accept-list keeps every pre-C2 archive
  // openable (see schema_hash_v2 / open_impl and docs/atxvsa2-format.md §5).
  std::uint16_t al_n_quad_price{};
  std::uint8_t reserved[66]{}; // pad to 256
};
static_assert(sizeof(ArchiveV2SurfaceHeader) == 256, "ArchiveV2SurfaceHeader layout drift");
static_assert(std::is_trivially_copyable_v<ArchiveV2SurfaceHeader>);
static_assert(std::is_standard_layout_v<ArchiveV2SurfaceHeader>);

// Per-field offsets pinned explicitly (not just sizeof): this is an on-disk ABI,
// so a field reorder that preserved sizeof would still silently corrupt readers.
static_assert(offsetof(ArchiveV2Header, file_size) == 8);
static_assert(offsetof(ArchiveV2Header, lookup_offset) == 40);
static_assert(offsetof(ArchiveV2Header, directory_offset) == 48);
static_assert(offsetof(ArchiveV2Header, data_offset) == 56);
static_assert(offsetof(ArchiveV2Header, header_crc32c) == 84);
static_assert(offsetof(ArchiveV2Header, metadata_crc32c) == 88);
static_assert(offsetof(ArchiveV2LookupSlot, symbol_hash) == 0);
static_assert(offsetof(ArchiveV2LookupSlot, surface_offset) == 8);
static_assert(offsetof(ArchiveV2LookupSlot, surface_size) == 16);
static_assert(offsetof(ArchiveV2DirEntry, surface_offset) == 0);
static_assert(offsetof(ArchiveV2DirEntry, surface_size) == 8);
static_assert(offsetof(ArchiveV2DirEntry, symbol_hash) == 16);
static_assert(offsetof(ArchiveV2DirEntry, payload_crc32c) == 36);
static_assert(offsetof(ArchiveV2SurfaceHeader, record_size) == 8);
static_assert(offsetof(ArchiveV2SurfaceHeader, S) == 16);
static_assert(offsetof(ArchiveV2SurfaceHeader, r) == 24);
static_assert(offsetof(ArchiveV2SurfaceHeader, now_ts_ns) == 32);
static_assert(offsetof(ArchiveV2SurfaceHeader, al_tol) == 40);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_kind_off) == 72);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_T_off) == 80);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_forward_off) == 88);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_qeff_off) == 96);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_df_off) == 104);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_borrow_off) == 112);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_nused_off) == 120);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_ndropped_off) == 128);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_nodecount_off) == 136);
static_assert(offsetof(ArchiveV2SurfaceHeader, col_payload_off_off) == 144);
static_assert(offsetof(ArchiveV2SurfaceHeader, uid) == 152);
static_assert(offsetof(ArchiveV2SurfaceHeader, n_slices) == 156);
static_assert(offsetof(ArchiveV2SurfaceHeader, payload_crc32c) == 168);
// n_quad_price (C2): pins the reused reserved slot; a pre-C2 archive stored 0 here.
static_assert(offsetof(ArchiveV2SurfaceHeader, al_n_quad_price) == 188);

// ── v2 writer ────────────────────────────────────────────────────────────────

struct ArchiveV2WriteOpts {
  std::uint32_t flags{0};
  std::uint32_t lookup_load_pct{70};                    // (0, 100]
  std::uint32_t surface_alignment{kArchiveV2SurfaceAlign};
  // 0 => a DETERMINISTIC content hash (CRC-32C of [header,EOF) folded with
  // file_size), NOT the wall clock: a content-derived identity, not a timestamp —
  // may be negative int64, ~32-bit entropy, never tamper evidence. Explicit
  // nonzero honored verbatim. See docs/atxvsa2-format.md §5.2.
  std::int64_t created_ts_ns{0};
};

// Serialize `items` into an in-memory ATXVSA2 buffer (memcpy-bound). Same
// `SurfaceArchiveItem` inputs as v1 — callers re-target by swapping the function
// name (clean break §0: no compatibility overload). Errors mirror v1's writer.
[[nodiscard]] Result<std::vector<std::byte>>
write_surface_archive_v2(std::span<const SurfaceArchiveItem> items,
                         const ArchiveV2WriteOpts &opts = {});

// As above, persisted through a unique same-directory temp with serialized,
// durable atomic replacement.
[[nodiscard]] Status write_surface_archive_v2_file(std::string_view path,
                                                   std::span<const SurfaceArchiveItem> items,
                                                   const ArchiveV2WriteOpts &opts = {});

// ── v2 reader ────────────────────────────────────────────────────────────────

// One reconstructed view paired with the provenance parsed from the same record
// (the v2 analogue of ArchivedSurface; move-only because the view owns its
// materialized heavy curves and never-reused instance id).
struct ArchivedSurfaceView {
  PricedSurfaceView view;
  SurfaceProvenance provenance;
};

// An opened ATXVSA2 archive. BACKING-AGNOSTIC: it owns a `std::span<const
// std::byte>` over the mapped region plus a type-erased owner keeping the backing
// (an owned buffer today; a real `atx::tsdb::Mapping` in wave 2) alive. All query
// methods are `const`; a returned `PricedSurfaceView` BORROWS this archive's bytes
// and must not outlive it. Move-only.
class SurfaceArchiveV2 {
public:
  // Take ownership of `bytes` and validate framing (magic, version, endian,
  // sizes, schema hash, header CRC, metadata CRC, directory/lookup bounds). Does
  // NOT CRC any surface record (lazy). ParseError on any framing failure.
  [[nodiscard]] static Result<SurfaceArchiveV2> open(std::vector<std::byte> bytes);

  // Read `path` fully into a buffer and `open` it. Adds IoError / NotFound.
  [[nodiscard]] static Result<SurfaceArchiveV2> open_file(std::string_view path);

  // MMAP OPEN (S2/WS-S): map `path` read-only and `open_borrowed` it, so opening
  // faults in only the metadata pages (header/lookup/directory that framing +
  // CRC validation reads) — NOT the surface records. A reader that then maps or
  // reconstructs a subset of records faults in only those records' pages via the
  // OS page cache, never the whole file. The owning `atx::tsdb::Mapping` is kept
  // alive by the archive for its whole lifetime; every returned `PricedSurfaceView`
  // borrows into the mapped pages and must not outlive the archive. Adds
  // IoError / NotFound / InvalidArgument (empty file).
  [[nodiscard]] static Result<SurfaceArchiveV2> open_mapped(std::string_view path);

  // COPIED OPEN (WS-ZC1): map, memcpy once into an OWNED buffer, drop the mapping.
  // For readers that BORROW records (`PricedSurfaceView`) beyond the open call and so
  // cannot keep a mapping alive — on Windows a file with a live mapped section cannot
  // be replaced, which would break atomic partition republish — but which should not
  // pay `open_file`'s much slower stream read. Adds IoError / NotFound.
  [[nodiscard]] static Result<SurfaceArchiveV2> open_copied(std::string_view path);

  // The MMAP SEAM. View over externally-owned bytes; `owner` keeps the backing
  // (an `atx::tsdb::Mapping`-owning shared_ptr under `open_mapped`) alive for the
  // archive's lifetime. Same framing validation as `open`.
  [[nodiscard]] static Result<SurfaceArchiveV2>
  open_borrowed(std::span<const std::byte> bytes, std::shared_ptr<const void> owner);

  [[nodiscard]] std::uint32_t count() const noexcept { return header_.surface_count; }
  [[nodiscard]] const ArchiveV2Header &header() const noexcept { return header_; }
  [[nodiscard]] std::span<const ArchiveV2DirEntry> directory() const noexcept { return directory_; }

  // ── Framing-only enumeration seam (C6 / SE-P2-6) ────────────────────────────
  //
  // `entries()` returns the parsed directory — one `ArchiveV2DirEntry` per surface,
  // sorted by canonical symbol (offset/size/uid/symbol/n_slices/kind_bits/
  // payload_crc32c) — and `entry_count()` its size. Both are O(1) and read ONLY the
  // metadata parsed at `open()`; they touch NO surface record body, so they never
  // materialize a `PricedSurfaceView` (nor the eager ConvexDense/SplineVol curves
  // that `map_all()` builds per record). A checkpoint/resume counter that only needs
  // "how many surfaces / which uids / which kinds" MUST use these, not `map_all()`,
  // whose per-record view construction makes resume O(heavy-payload) instead of
  // O(framing). This is the cross-workstream seam the corpus checkpoint consumes
  // (sprint WS-T / T2); `entries()` is the WS-T-facing name for the same span
  // `directory()` exposes.
  [[nodiscard]] std::span<const ArchiveV2DirEntry> entries() const noexcept { return directory_; }
  [[nodiscard]] std::size_t entry_count() const noexcept { return directory_.size(); }

  [[nodiscard]] ArchiveContentIdentity identity() const noexcept {
    return ArchiveContentIdentity{header_.file_size, header_.created_ts_ns, header_.header_crc32c,
                                  header_.metadata_crc32c};
  }

  // Resolve `symbol` (case-insensitive) to its directory entry. The returned
  // value is the STORED entry — byte-identical to the matching `directory()` /
  // `entries()` element, so `n_slices`/`kind_bits`/`payload_crc32c` are the real
  // ones and the result is interchangeable with a directory element (e.g. as
  // `map_entry` / `reconstruct_entry` input). NotFound if absent.
  [[nodiscard]] Result<ArchiveV2DirEntry> find(std::string_view symbol) const;

  // Provenance from a symbol's record header (no payload CRC). NotFound if absent.
  [[nodiscard]] Result<SurfaceProvenance> provenance(std::string_view symbol) const;

  // SUBSET MAP: hash-probe `symbol`, build a zero-copy view over ONLY that
  // record's byte extent — touches no other surface's bytes (bottleneck #1).
  // NotFound if absent; ParseError on a bad record.
  [[nodiscard]] Result<PricedSurfaceView> map_symbol(std::string_view symbol) const;

  // Whole-board: a view over every surface, in directory order.
  [[nodiscard]] Result<std::vector<PricedSurfaceView>> map_all() const;

  // Whole-board views paired with per-record provenance, in directory order.
  [[nodiscard]] Result<std::vector<ArchivedSurfaceView>> map_all_with_provenance() const;

  // WS-ZC1: the zero-copy analogue of `reconstruct_entry` — build a view plus its
  // provenance from a directory entry the caller already holds, in ONE pass over
  // that record's extent and with no hash re-probe. This is the subset-load seam.
  [[nodiscard]] Result<ArchivedSurfaceView> map_entry(const ArchiveV2DirEntry &e) const;

  // ── Owned reconstruct (whole-board deserialize keeping v1 semantics) ─────────
  //
  // S4 clean-break cutover: these rebuild an OWNED `PricedSurface` from a v2
  // record — the inverse of `write_surface_archive_v2`, bit-identical to the
  // source surface (and hence to what the deleted v1 `reconstruct` produced,
  // since v2 serializes the same SliceContext fields + curve params). They exist
  // for the whole-board readers that still feed the `PortfolioPricer`'s
  // `SurfaceSet` of `const PricedSurface*` (backtest `MarketSnapshot::load`,
  // `SurfaceDb`) — re-pointing that pricer at zero-copy `PricedSurfaceView`s is
  // B1/greeks work (seam §6), not this format cutover. The zero-copy subset-map
  // win reaches the hot path via `map_symbol` (B1), not here. Slower than the
  // views (per-surface allocation), but only used off the hot path.
  [[nodiscard]] Result<PricedSurface> reconstruct_symbol(std::string_view symbol) const;
  // S3 (WS-S): reconstruct one surface + its provenance from a directory entry the
  // caller already holds — one pass over `e`'s record extent, NO hash re-probe
  // (unlike reconstruct_symbol + provenance, which each re-run find_slot). Used by
  // the subset load path, which iterates `directory()` and already has each `e`.
  [[nodiscard]] Result<ArchivedSurface> reconstruct_entry(const ArchiveV2DirEntry &e) const;
  [[nodiscard]] Result<std::vector<PricedSurface>> reconstruct_all() const;
  [[nodiscard]] Result<std::vector<ArchivedSurface>> reconstruct_all_with_provenance() const;

  // ── Lazy integrity (validate-on-demand; never on the price path) ─────────────

  // Verify one symbol's record payload CRC. NotFound if absent; ParseError on
  // a checksum/framing failure; Ok otherwise.
  [[nodiscard]] Status validate_symbol(std::string_view symbol) const;
  // Verify every record's payload CRC. Ok iff all pass.
  [[nodiscard]] Status validate_all() const;

private:
  SurfaceArchiveV2() = default;

  [[nodiscard]] static Result<SurfaceArchiveV2> open_impl(std::span<const std::byte> bytes,
                                                          std::shared_ptr<const void> owner);
  [[nodiscard]] const ArchiveV2LookupSlot *find_slot(std::string_view symbol) const noexcept;
  [[nodiscard]] Status validate_record(std::uint64_t offset, std::uint64_t size) const;

  std::span<const std::byte> bytes_{};        // the mapped region (borrowed view)
  std::shared_ptr<const void> owner_{};       // keeps the backing alive
  ArchiveV2Header header_{};                   // parsed copy
  std::vector<ArchiveV2LookupSlot> lookup_{};  // parsed copy
  std::vector<ArchiveV2DirEntry> directory_{}; // parsed copy
};

} // namespace atx::vol
