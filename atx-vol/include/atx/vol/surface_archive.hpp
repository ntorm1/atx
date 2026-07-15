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

#include "atx/vol/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/surface_policy.hpp" // purpose, quality, health, validation
#include "atx/vol/types.hpp"

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
  // Stamp for `ArchiveHeader::created_ts_ns`. 0 => fill from the system clock.
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

// As above, persisted to `path` (written to `path` + ".tmp" then renamed for
// atomic replacement). Adds IoError on any filesystem failure.
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

} // namespace atx::vol
