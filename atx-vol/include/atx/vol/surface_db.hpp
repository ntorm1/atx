#pragma once

// surface_db — the on-disk symbol/partition MANIFEST for a directory of ATXVSA
// v3 surface archives (surface_archive.hpp), plus the per-symbol fit
// configuration a production pipeline needs to fit "this underlying, this
// way" without hand-assembling `SessionInputs` at every call site.
//
// A `SurfaceDb` (Tasks 3-5) is a directory: one `manifest.atxdb` (this format)
// indexing zero or more `.atxvsa` partition files under `partitions/`. This
// header is the manifest's binary shape plus its in-memory writer/reader; file
// IO and the `SurfaceDb` class land in Task 3, `apply_symbol_config` in a later
// task.
//
// ── On-disk shape ───────────────────────────────────────────────────────────
//
//   header (192 B) -> symbol records (sorted) -> partition records (sorted)
//
//   * symbol records    — one per configured symbol, sorted by canonical
//                          symbol; a fixed-width, field-for-field mirror of
//                          `SymbolFitConfig` (bools packed into `flags`, enums
//                          as `uint8`, every `CurveConfig`/`CalibOpts`/
//                          `ConvexFitOpts`/`AlOpts` knob laid out flat).
//   * partition records — one per `.atxvsa` partition file, sorted by
//                          canonical key; identity + bookkeeping only — the
//                          partition FILE's own integrity is the archive's job
//                          (layered CRC-32C inside the `.atxvsa`, see
//                          surface_archive.hpp).
//
// ── Integrity ────────────────────────────────────────────────────────────────
//
// The same hardware-accelerated CRC-32C as the archive (atx::vol::detail,
// shared so both formats agree bit-for-bit): a `header_crc32c` (header bytes
// with that field zeroed) and a `payload_crc32c` over the contiguous
// `[symbols_offset, end-of-partitions)` span. `open` validates framing
// (magic/version/endian/pointer width/record sizes/schema hash), both CRCs,
// section bounds, sort order, and every record's enum wire values EAGERLY —
// once, at open — so `find_symbol`/`find_partition` stay cheap lookups with no
// per-query re-validation.
//
// ── Schema hash / endianness ──────────────────────────────────────────────────
//
// Same discipline as the archive: the header stores a compile-time
// fingerprint folded from the `sizeof` of every on-disk record + a v1 format
// salt, so a reader built against a different struct shape refuses the file
// (ParseError) instead of mis-reading it. Records are host byte order; the
// header stamps endian = 1 (little) / pointer_bits = 64. Little-endian LP64
// hosts only.
//
// ── Thread safety ────────────────────────────────────────────────────────────
//
// `write_db_manifest` is a pure function of its inputs. A parsed `DbManifest`
// is immutable after `open`; `header`, `generation`, `symbols`, `partitions`,
// `find_symbol`, and `find_partition` are all `const` and touch no shared
// mutable state, so any number of threads may query one `const` manifest
// concurrently.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "atx/vol/american.hpp"        // AlOpts
#include "atx/vol/session.hpp"         // FitPreset, SessionInputs
#include "atx/vol/surface_archive.hpp" // SurfaceArchive, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"  // CalendarRepair
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"       // CurveConfig, VolCurveKind

namespace atx::vol {

// ── On-wire constants ─────────────────────────────────────────────────────
inline constexpr std::uint16_t kSurfaceDbMajor = 1;
inline constexpr std::uint16_t kSurfaceDbMinor = 0;
inline constexpr std::size_t kSurfaceDbKeyMax = 32;   // partition-key chars
inline constexpr std::string_view kSurfaceDbManifestName = "manifest.atxdb";
inline constexpr std::string_view kSurfaceDbPartitionDir = "partitions";
inline constexpr std::string_view kSurfaceDbPartitionExt = ".atxvsa";
inline constexpr std::uint32_t kSurfaceDbSectionAlign = 64;

// DbSymbolRecord::flags bits.
inline constexpr std::uint16_t kDbSymEnabled = 1u << 0;
inline constexpr std::uint16_t kDbSymPinCurve = 1u << 1;
inline constexpr std::uint16_t kDbSymAlOverride = 1u << 2;
inline constexpr std::uint16_t kDbSymUseCorrectionCache = 1u << 3;
inline constexpr std::uint16_t kDbSymScoreParity = 1u << 4;
inline constexpr std::uint16_t kDbSymEnforceCalendarFloor = 1u << 5;
inline constexpr std::uint16_t kDbSymUseDeamCacheForFit = 1u << 6;
inline constexpr std::uint16_t kDbSymConvexBoundSlopeBelow = 1u << 7;
inline constexpr std::uint16_t kDbSymLeeBoundProject = 1u << 8;
inline constexpr std::uint16_t kDbSymMorozovStop = 1u << 9;
inline constexpr std::uint16_t kDbSymValidateNoArb = 1u << 10;
inline constexpr std::uint16_t kDbSymResidualDisable = 1u << 11;
inline constexpr std::uint16_t kDbSymEssviAsymmetricRho = 1u << 12;

// ── Per-symbol fitting configuration (public, in-memory) ──────────────────
//
// The manifest's unit of configuration: how the fitting pipeline should fit
// THIS underlying. `apply_symbol_config` (below) maps it onto SessionInputs:
// `preset` is applied first (apply_fit_preset — DeAm/cache/iv-tol policy),
// then every explicit field here overwrites the preset's choice, so the
// stored values are always the final word on the fields this struct carries.
struct SymbolFitConfig {
  bool enabled{true};                 // pipeline may skip disabled symbols
  FitPreset preset{FitPreset::Robust};
  bool pin_curve{false};              // false => preset/selector decides family
  CurveConfig curve{};                // used when pin_curve; parametric knobs
                                      // also mirror into SessionInputs::calib
  bool al_override{false};            // true => deam.al_opts = al
  AlOpts al{};
  double band_k{1.0};
  CalendarRepair calendar_repair{CalendarRepair::None};
  bool use_correction_cache{true};
  bool score_parity{true};
  bool enforce_calendar_floor{true};
  bool use_deam_cache_for_fit{false};
};

// ── On-disk records (POD, little-endian, fixed layout) ────────────────────

// Manifest file header, at offset 0. `header_crc32c` covers the header with
// that field zeroed; `payload_crc32c` covers the symbols ‖ partitions span.
struct DbManifestHeader {
  char magic[8]{};                    // "ATXVDB01", no NUL
  std::uint16_t major{};              // kSurfaceDbMajor
  std::uint16_t minor{};
  std::uint16_t header_size{};        // sizeof(DbManifestHeader)
  std::uint16_t endian{};             // 1 = little
  std::uint16_t pointer_bits{};       // 64
  std::uint16_t reserved0{};
  std::uint32_t flags{};
  std::uint64_t file_size{};
  std::int64_t created_ts_ns{};
  std::int64_t updated_ts_ns{};
  std::uint64_t generation{};         // ++ on every manifest rewrite
  std::uint64_t schema_hash{};        // sizeof-based layout fingerprint
  std::uint32_t symbol_count{};
  std::uint32_t partition_count{};
  std::uint64_t symbols_offset{};
  std::uint64_t partitions_offset{};
  std::uint32_t symbol_record_size{};    // sizeof(DbSymbolRecord)
  std::uint32_t partition_record_size{}; // sizeof(DbPartitionRecord)
  std::uint32_t header_crc32c{};
  std::uint32_t payload_crc32c{};
  std::uint8_t reserved[88]{};
};
static_assert(sizeof(DbManifestHeader) == 192, "DbManifestHeader layout drift");
static_assert(std::is_trivially_copyable_v<DbManifestHeader>);
static_assert(std::is_standard_layout_v<DbManifestHeader>);

// One symbol's full fit configuration, fixed-width. Sorted by canonical
// symbol in the file. Field-for-field mirror of SymbolFitConfig (bools in
// `flags`, enums as uint8, CurveConfig/CalibOpts/ConvexFitOpts/AlOpts fields
// laid out flat).
struct DbSymbolRecord {
  char symbol[32]{};                  // canonical, not NUL-terminated
  std::uint16_t symbol_len{};
  std::uint16_t flags{};              // kDbSym* bits
  // enums (uint8 wire width)
  std::uint8_t preset{};              // FitPreset
  std::uint8_t curve_kind{};          // VolCurveKind (meaningful when PinCurve)
  std::uint8_t calendar_repair{};     // CalendarRepair
  std::uint8_t convex_loss{};         // CalibLossKind (curve.convex.loss)
  std::uint8_t essvi_rho_mode{};      // EssviRhoMode
  std::uint8_t optimization_level{};  // OptimizationLevel
  std::uint8_t residual_basis_kind{}; // ResidualBasisKind
  std::uint8_t residual_n_basis_terms{};
  std::uint8_t loss_kind{};           // CalibLossKind (calib loss)
  std::uint8_t anchor_kind{};         // CalibAnchorKind
  std::uint8_t reserved1[2]{};
  // 16-bit knobs
  std::uint16_t max_outer_iter{};
  std::uint16_t max_inner_iter{};
  std::uint16_t max_iter_quick_mark{};
  std::uint16_t max_iter_trading{};
  std::uint16_t max_iter_risk{};
  std::uint16_t max_iter_reference{};
  std::uint16_t max_iter_cold_fast{};
  std::uint16_t al_n_collocation{};
  std::uint16_t al_n_quadrature{};
  std::uint16_t al_max_newton_iter{};
  // 32-bit knobs
  std::int32_t convex_node_cap{};
  std::int32_t convex_max_iter{};
  std::uint32_t max_obs_per_slice{};
  std::uint32_t n_butterfly_grid{};
  std::uint32_t min_obs_per_slice{};
  // 64-bit knobs
  double convex_lambda{};
  double tol_param{};
  double tol_residual{};
  double huber_k{};
  double min_vega_weight{};
  double max_spread_vol{};
  double max_weight{};
  double max_otm_shortcut_premium_spread_frac{};
  double prior_strength{};
  double essvi_fallback_rmse_threshold{};
  double wing_floor_alpha{};
  double morozov_tau{};
  double residual_ridge_factor{};
  double max_post_fit_sigma{};
  double max_spread_to_mid_pct{};
  double al_tol{};
  double band_k{};
  std::uint8_t reserved[32]{};
};
static_assert(sizeof(DbSymbolRecord) == 256, "DbSymbolRecord layout drift");
static_assert(std::is_trivially_copyable_v<DbSymbolRecord>);
static_assert(std::is_standard_layout_v<DbSymbolRecord>);

// One partition's index entry, sorted by canonical key. Integrity of the
// partition file itself is the archive's job (layered CRCs inside .atxvsa);
// the manifest tracks identity + bookkeeping.
struct DbPartitionRecord {
  char key[32]{};                     // canonical, not NUL-terminated
  std::uint16_t key_len{};
  std::uint16_t flags{};
  std::uint32_t surface_count{};
  std::uint64_t file_size{};          // partition file bytes at write time
  std::int64_t created_ts_ns{};
  std::uint64_t reserved0{};
  std::uint8_t reserved[64]{};
};
static_assert(sizeof(DbPartitionRecord) == 128, "DbPartitionRecord layout drift");
static_assert(std::is_trivially_copyable_v<DbPartitionRecord>);
static_assert(std::is_standard_layout_v<DbPartitionRecord>);

// ── Manifest writer inputs ────────────────────────────────────────────────

struct DbSymbolEntry {
  std::string_view symbol{};          // canonicalized before storage
  SymbolFitConfig config{};
};

struct DbPartitionInfo {
  std::string key{};                  // canonical
  std::uint32_t surface_count{};
  std::uint64_t file_size{};
  std::int64_t created_ts_ns{};
};

struct SurfaceDbManifestWriteOpts {
  std::uint64_t generation{1};
  std::int64_t created_ts_ns{0};      // 0 => system clock
  std::int64_t updated_ts_ns{0};      // 0 => system clock
  std::uint32_t flags{0};
};

// Serialize a manifest to bytes. Symbols/partitions are canonicalized and
// sorted internally; oversized symbols are truncated to kSurfaceDbKeyMax
// (matching the archive's canonical keys), not rejected. Errors:
// InvalidArgument (empty symbol, bad partition key); AlreadyExists
// (duplicate canonical symbol or key).
[[nodiscard]] Result<std::vector<std::byte>>
write_db_manifest(std::span<const DbSymbolEntry> symbols,
                  std::span<const DbPartitionInfo> partitions,
                  const SurfaceDbManifestWriteOpts& opts = {});

// ── Parsed manifest (immutable) ───────────────────────────────────────────

// Owns its bytes; validated on open (magic, version, endian, sizes, schema
// hash, header CRC, payload CRC, bounds, sort order). All queries const +
// thread-safe.
class DbManifest {
 public:
  [[nodiscard]] static Result<DbManifest> open(std::vector<std::byte> bytes);

  [[nodiscard]] const DbManifestHeader& header() const noexcept { return header_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return header_.generation; }
  [[nodiscard]] std::span<const DbSymbolRecord> symbols() const noexcept { return symbols_; }
  [[nodiscard]] std::span<const DbPartitionRecord> partitions() const noexcept {
    return partitions_;
  }

  // Case-insensitive (canonicalized) binary search. NotFound if absent.
  [[nodiscard]] Result<SymbolFitConfig> find_symbol(std::string_view symbol) const;
  [[nodiscard]] const DbPartitionRecord* find_partition(std::string_view key) const noexcept;

 private:
  DbManifest() = default;
  DbManifestHeader header_{};
  std::vector<DbSymbolRecord> symbols_{};      // sorted by canonical symbol
  std::vector<DbPartitionRecord> partitions_{}; // sorted by canonical key
};

// Decode one symbol record into the public config (exact inverse of the
// writer's encoding; used by DbManifest::find_symbol and tests).
[[nodiscard]] SymbolFitConfig decode_symbol_record(const DbSymbolRecord& rec);

// ── SurfaceDb: create/open, atomic manifest persistence, symbol CRUD,
// refresh() (Task 3); partition IO (Task 4) ───────────────────────────────

struct SurfaceDbCreateOpts {
  std::int64_t created_ts_ns{0};      // 0 => system clock
};

// An opened surface database. Const queries are thread-safe (they read an
// immutable manifest snapshot swapped under a mutex); mutating calls are
// serialized internally. Cross-process: single writer, many readers; every
// mutation is an atomic manifest rewrite (tmp+rename) with generation++ so a
// reader process picks it up via refresh().
class SurfaceDb {
 public:
  // Create <root>/ (and partitions/) and write an empty manifest
  // (generation 1). Errors: AlreadyExists if a manifest already exists at
  // root; IoError on filesystem failure.
  [[nodiscard]] static Result<SurfaceDb> create(std::string_view root,
                                                const SurfaceDbCreateOpts& opts = {});

  // Open an existing database. Errors: NotFound (no manifest), ParseError,
  // IoError.
  [[nodiscard]] static Result<SurfaceDb> open(std::string_view root);

  [[nodiscard]] const std::string& root() const noexcept { return root_; }

  // ── Manifest snapshot queries (thread-safe) ──
  [[nodiscard]] std::shared_ptr<const DbManifest> manifest() const;
  [[nodiscard]] std::uint64_t generation() const;
  [[nodiscard]] std::vector<std::string> symbols() const;   // canonical, sorted
  [[nodiscard]] Result<SymbolFitConfig> symbol_config(std::string_view symbol) const;
  [[nodiscard]] std::vector<DbPartitionInfo> partitions() const;

  // ── Manifest mutation (serialized; atomic rewrite; generation++) ──
  [[nodiscard]] Status upsert_symbol(std::string_view symbol, const SymbolFitConfig& cfg);
  [[nodiscard]] Status remove_symbol(std::string_view symbol);  // NotFound if absent

  // Re-read the manifest from disk iff its generation advanced past the
  // in-memory snapshot (external writer). Ok and no-op when current.
  [[nodiscard]] Status refresh();

  // ── Partition IO (Task 4) ──
  //
  // A partition's ATXVSA archive stores whatever symbols the caller passes to
  // write_partition; those symbols need NOT appear in the manifest's symbol
  // table above (upsert_symbol/symbols()/symbol_config()). The two are
  // orthogonal namespaces: the symbol table configures HOW a symbol should be
  // fit (SymbolFitConfig knobs), while a partition stores WHERE a fitted
  // surface lives for a given key (e.g. a trading date). A symbol may be
  // written into any number of partitions without ever being registered in
  // the symbol table, and the symbol table may hold config for symbols that
  // never appear in a partition.
  [[nodiscard]] Status write_partition(std::string_view key,
                                       std::span<const SurfaceArchiveItem> items,
                                       const SurfaceArchiveWriteOpts& opts = {});
  [[nodiscard]] Result<SurfaceArchive> open_partition(std::string_view key) const;
  [[nodiscard]] Result<PricedSurface> load_surface(std::string_view key,
                                                   std::string_view symbol) const;
  [[nodiscard]] Status drop_partition(std::string_view key);

 private:
  SurfaceDb() = default;
  [[nodiscard]] Status persist_locked(std::vector<DbSymbolEntry> symbols,
                                      std::vector<DbPartitionInfo> partitions);
  [[nodiscard]] std::string manifest_path() const;
  [[nodiscard]] std::string partition_path(std::string_view canonical_key) const;

  std::string root_{};
  mutable std::unique_ptr<std::mutex> mu_{};      // guards snapshot_ swap + writes
  std::shared_ptr<const DbManifest> snapshot_{};
};

}  // namespace atx::vol
