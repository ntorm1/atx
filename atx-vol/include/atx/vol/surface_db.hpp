#pragma once

// surface_db — the on-disk symbol/partition MANIFEST for a directory of ATXVSA
// v3 surface archives (surface_archive.hpp), plus the per-symbol fit
// configuration a production pipeline needs to fit "this underlying, this
// way" without hand-assembling `SessionInputs` at every call site.
//
// A `SurfaceDb` is a directory: one `manifest.atxdb` (this format) indexing
// zero or more `.atxvsa` partition files under `partitions/`. This header
// covers the manifest's binary shape and in-memory writer/reader, the
// `SurfaceDb` class itself (create/open, atomic manifest persistence, symbol
// CRUD, partition IO, refresh()), and `apply_symbol_config` /
// `symbol_config_from_preset` for binding a stored config onto
// `SessionInputs`.
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
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "atx/vol/american.hpp"        // AlOpts
#include "atx/vol/session.hpp"         // FitPreset, SessionInputs
#include "atx/vol/surface_archive.hpp" // SurfaceArchive, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"  // CalendarRepair
#include "atx/vol/surface_policy.hpp"  // SurfacePolicy
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp" // CurveConfig, VolCurveKind

namespace atx::vol {

// ── On-wire constants ─────────────────────────────────────────────────────
inline constexpr std::uint16_t kSurfaceDbMajor = 1;
inline constexpr std::uint16_t kSurfaceDbMinor = 0;
inline constexpr std::size_t kSurfaceDbKeyMax = 32; // partition-key chars
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
  bool enabled{true}; // pipeline may skip disabled symbols
  FitPreset preset{FitPreset::Robust};
  bool pin_curve{false};   // false => preset/selector decides family
  CurveConfig curve{};     // used when pin_curve; parametric knobs
                           // also mirror into SessionInputs::calib
  bool al_override{false}; // true => deam.al_opts = al
  AlOpts al{};
  double band_k{1.0};
  CalendarRepair calendar_repair{CalendarRepair::None};
  bool use_correction_cache{true};
  bool score_parity{true};
  bool enforce_calendar_floor{true};
  bool use_deam_cache_for_fit{false};
  // Product-level intent persisted independently from the legacy numerical
  // preset. Quality adjusts work; requesting Risk always implies admission.
  SurfacePolicy surface_policy{};
};

// ── Fitting-pipeline binding ───────────────────────────────────────────────

// Map `cfg` onto the fit-policy fields of `in`, leaving the market snapshot
// (S, r, expiry rates, cash_divs, now_ts_ns) untouched. Order: apply_fit_preset
// (cfg.preset) first — it sets the DeAm/cache/inversion policy — then every
// explicit SymbolFitConfig field overwrites the preset's choice:
//   in.curve = cfg.curve (when pin_curve; otherwise the preset's curve stands),
//   in.calib = cfg.curve.parametric (when pin_curve),
//   in.deam.al_opts = cfg.al (when al_override),
//   in.band_k / in.calendar_repair / in.use_correction_cache / in.score_parity
//   / in.enforce_calendar_floor / in.use_deam_cache_for_fit = cfg.<same>.
void apply_symbol_config(const SymbolFitConfig &cfg, SessionInputs &in);

// Same binding as above, plus copy `cfg.surface_policy` — the product-level
// quality_mode/outputs/risk_admission/fallback fields (SurfacePolicy) — into
// `policy`, unconditionally. SurfacePolicy has no preset-implicit "unset"
// state to defer to (unlike pin_curve/al_override): `symbol_config_from_preset`
// already seeds `surface_policy` from `map_legacy_fit_preset` at capture time,
// so any persisted value is a deliberate stored choice and is always the final
// word, exactly like `band_k`/`calendar_repair`/the other unconditional fields
// above. `in` receives the same fit-policy binding as the two-argument
// overload; `policy` is untouched by that half of the mapping.
void apply_symbol_config(const SymbolFitConfig &cfg, SessionInputs &in, SurfacePolicy &policy);

// Capture `preset`'s effective policy into a SymbolFitConfig whose explicit
// fields equal what apply_fit_preset(in, preset) would produce — the identity
// starting point for per-symbol tuning (adjust one knob, store, done).
[[nodiscard]] SymbolFitConfig symbol_config_from_preset(FitPreset preset);

// ── On-disk records (POD, little-endian, fixed layout) ────────────────────

// Manifest file header, at offset 0. `header_crc32c` covers the header with
// that field zeroed; `payload_crc32c` covers the symbols ‖ partitions span.
struct DbManifestHeader {
  char magic[8]{};       // "ATXVDB01", no NUL
  std::uint16_t major{}; // kSurfaceDbMajor
  std::uint16_t minor{};
  std::uint16_t header_size{};  // sizeof(DbManifestHeader)
  std::uint16_t endian{};       // 1 = little
  std::uint16_t pointer_bits{}; // 64
  std::uint16_t reserved0{};
  std::uint32_t flags{};
  std::uint64_t file_size{};
  std::int64_t created_ts_ns{};
  std::int64_t updated_ts_ns{};
  std::uint64_t generation{};  // ++ on every manifest rewrite
  std::uint64_t schema_hash{}; // sizeof-based layout fingerprint
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
  char symbol[32]{}; // canonical, not NUL-terminated
  std::uint16_t symbol_len{};
  std::uint16_t flags{}; // kDbSym* bits
  // enums (uint8 wire width)
  std::uint8_t preset{};              // FitPreset
  std::uint8_t curve_kind{};          // VolCurveKind (meaningful when PinCurve)
  std::uint8_t calendar_repair{};     // CalendarRepair
  std::uint8_t convex_loss{};         // CalibLossKind (curve.convex.loss)
  std::uint8_t essvi_rho_mode{};      // EssviRhoMode
  std::uint8_t optimization_level{};  // OptimizationLevel
  std::uint8_t residual_basis_kind{}; // ResidualBasisKind
  std::uint8_t residual_n_basis_terms{};
  std::uint8_t loss_kind{};   // CalibLossKind (calib loss)
  std::uint8_t anchor_kind{}; // CalibAnchorKind
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
  char key[32]{}; // canonical, not NUL-terminated
  std::uint16_t key_len{};
  std::uint16_t flags{};
  std::uint32_t surface_count{};
  std::uint64_t file_size{}; // partition file bytes at write time
  std::int64_t created_ts_ns{};
  std::uint64_t reserved0{};
  std::uint8_t reserved[64]{};
};
static_assert(sizeof(DbPartitionRecord) == 128, "DbPartitionRecord layout drift");
static_assert(std::is_trivially_copyable_v<DbPartitionRecord>);
static_assert(std::is_standard_layout_v<DbPartitionRecord>);

// ── Manifest writer inputs ────────────────────────────────────────────────

struct DbSymbolEntry {
  std::string_view symbol{}; // canonicalized before storage
  SymbolFitConfig config{};
  std::optional<SurfaceProvenance> provenance{};
};

struct DbPartitionInfo {
  std::string key{}; // canonical
  std::uint32_t surface_count{};
  std::uint64_t file_size{};
  std::int64_t created_ts_ns{};
};

struct SurfaceDbManifestWriteOpts {
  std::uint64_t generation{1};
  std::int64_t created_ts_ns{0}; // 0 => system clock
  std::int64_t updated_ts_ns{0}; // 0 => system clock
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
                  const SurfaceDbManifestWriteOpts &opts = {});

// ── Parsed manifest (immutable) ───────────────────────────────────────────

// Owns its bytes; validated on open (magic, version, endian, sizes, schema
// hash, header CRC, payload CRC, bounds, sort order). All queries const +
// thread-safe.
class DbManifest {
public:
  [[nodiscard]] static Result<DbManifest> open(std::vector<std::byte> bytes);

  [[nodiscard]] const DbManifestHeader &header() const noexcept { return header_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return header_.generation; }
  [[nodiscard]] std::span<const DbSymbolRecord> symbols() const noexcept { return symbols_; }
  [[nodiscard]] std::span<const DbPartitionRecord> partitions() const noexcept {
    return partitions_;
  }

  // Case-insensitive (canonicalized) binary search. NotFound if absent.
  [[nodiscard]] Result<SymbolFitConfig> find_symbol(std::string_view symbol) const;
  [[nodiscard]] Result<std::optional<SurfaceProvenance>>
  find_symbol_provenance(std::string_view symbol) const;
  [[nodiscard]] const DbPartitionRecord *find_partition(std::string_view key) const noexcept;

private:
  DbManifest() = default;
  DbManifestHeader header_{};
  std::vector<DbSymbolRecord> symbols_{};       // sorted by canonical symbol
  std::vector<DbPartitionRecord> partitions_{}; // sorted by canonical key
};

// Decode one symbol record into the public config (exact inverse of the
// writer's encoding; used by DbManifest::find_symbol and tests).
[[nodiscard]] SymbolFitConfig decode_symbol_record(const DbSymbolRecord &rec);
[[nodiscard]] std::optional<SurfaceProvenance>
decode_symbol_provenance(const DbSymbolRecord &rec) noexcept;

// ── SurfaceDb: create/open, atomic manifest persistence, symbol CRUD,
// refresh(), partition IO ─────────────────────────────────────────────────

// Default LRU bound for the S5 partition view cache. Each resident entry pins a
// whole partition file's bytes in RAM: SurfaceArchiveV2::open_file reads the
// entire file into an owned buffer (the real-mmap `atx::tsdb::Mapping` seam is
// deferred to a later wave — see surface_archive.hpp), so an UNBOUNDED cache
// would grow RSS by one full partition per distinct key the reader touches. The
// backtest hot loop (B1) sweeps ~135 daily partitions, which without a bound
// would keep every day's bytes resident for the SurfaceDb's lifetime and break
// the sprint's RSS = O(in-flight) invariant. 16 keeps a working set of recent
// partitions hot — comfortably covering the typical lookback / roll windows a
// step reprices across — while bounding resident bytes to O(16 partitions).
// Callers with a wider working set raise it via SurfaceDb{Create,Open}Opts; a
// zero is normalized to 1 (see SurfaceDb::open).
inline constexpr std::size_t kSurfaceDbDefaultPartitionCacheCapacity = 16;

struct SurfaceDbCreateOpts {
  std::int64_t created_ts_ns{0}; // 0 => system clock
  // Passed through to the SurfaceDb::open that create() returns, so a
  // create-then-read caller configures the view cache in one call.
  std::size_t partition_cache_capacity{kSurfaceDbDefaultPartitionCacheCapacity};
};

struct SurfaceDbOpenOpts {
  // Max partition mappings held resident in the S5 view cache (LRU-evicted).
  // See kSurfaceDbDefaultPartitionCacheCapacity for the resident-bytes rationale
  // and the default. Zero is normalized to 1.
  std::size_t partition_cache_capacity{kSurfaceDbDefaultPartitionCacheCapacity};
};

// Observability for the S5 partition view cache. `resident` is the number of
// mappings held right now; `capacity` is the configured LRU bound. A
// diagnostic/test hook (mirrors SnapshotCache::stats()); off every hot path.
struct SurfaceDbCacheStats {
  std::size_t resident{0};
  std::size_t capacity{0};
};

// An opened surface database. Const queries are thread-safe (they read an
// immutable manifest snapshot swapped under a mutex). Manifest mutations
// (upsert_symbol, remove_symbol, and the manifest half of write_partition /
// drop_partition) are serialized internally by that same mutex. Partition
// FILE operations are NOT fully covered by it, though: write_partition
// writes the .atxvsa archive before taking the lock, and drop_partition's
// unlink happens under the lock but after persist_locked's rename (see
// surface_db.cpp for why that ordering is deliberate). Concurrent in-process
// callers racing write_partition/drop_partition against the SAME key can
// therefore still interleave a file op from one call with the other's
// manifest update -- callers must serialize same-key partition mutations
// themselves; distinct keys are unaffected. Cross-process: single writer,
// many readers; every manifest mutation is an atomic rewrite (tmp+rename)
// with generation++ so a reader process picks it up via refresh().
// A zero-copy surface handle returned by SurfaceDb::map_surface (S5). It CO-OWNS
// the partition mapping it borrows (via the shared_ptr) so the view stays valid
// even after the partition is later rewritten or evicted from the db's view
// cache — the seam §4 "never let a view outlive the archive" rule, enforced by
// construction. `operator*` / `operator->` forward to the view so callers write
// `loaded->fair_value(...)`. Move-only (the view is move-only).
struct LoadedSurface {
  std::shared_ptr<const SurfaceArchiveV2> partition; // keeps the mapping alive
  PricedSurfaceView view;                            // borrows `partition`'s bytes
  [[nodiscard]] const PricedSurfaceView *operator->() const noexcept { return &view; }
  [[nodiscard]] const PricedSurfaceView &operator*() const noexcept { return view; }
};

class SurfaceDb {
public:
  // Create <root>/ (and partitions/) and write an empty manifest
  // (generation 1). Errors: AlreadyExists if a manifest already exists at
  // root; IoError on filesystem failure.
  [[nodiscard]] static Result<SurfaceDb> create(std::string_view root,
                                                const SurfaceDbCreateOpts &opts = {});

  // Open an existing database. Errors: NotFound (no manifest), ParseError,
  // IoError. `opts` configures the S5 partition view-cache LRU bound.
  [[nodiscard]] static Result<SurfaceDb> open(std::string_view root,
                                              const SurfaceDbOpenOpts &opts = {});

  [[nodiscard]] const std::string &root() const noexcept { return root_; }

  // Current partition view-cache occupancy + its LRU bound (S5). Diagnostic /
  // test hook; thread-safe.
  [[nodiscard]] SurfaceDbCacheStats partition_cache_stats() const;

  // ── Manifest snapshot queries (thread-safe) ──
  [[nodiscard]] std::shared_ptr<const DbManifest> manifest() const;
  [[nodiscard]] std::uint64_t generation() const;
  [[nodiscard]] std::vector<std::string> symbols() const; // canonical, sorted
  [[nodiscard]] Result<SymbolFitConfig> symbol_config(std::string_view symbol) const;
  [[nodiscard]] Result<std::optional<SurfaceProvenance>>
  surface_provenance(std::string_view symbol) const;
  [[nodiscard]] std::vector<DbPartitionInfo> partitions() const;

  // ── Manifest mutation (serialized; atomic rewrite; generation++) ──
  [[nodiscard]] Status upsert_symbol(
      std::string_view symbol, const SymbolFitConfig &cfg,
      std::optional<SurfaceProvenance> provenance = std::nullopt);
  [[nodiscard]] Status remove_symbol(std::string_view symbol); // NotFound if absent

  // Re-read the manifest from disk iff its generation advanced past the
  // in-memory snapshot (external writer). Ok and no-op when current.
  [[nodiscard]] Status refresh();

  // ── Partition IO ──
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
                                       const ArchiveV2WriteOpts &opts = {});
  [[nodiscard]] Result<SurfaceArchiveV2> open_partition(std::string_view key) const;

  // Reconstruct an OWNED surface for `symbol` in partition `key`. S5: reads the
  // partition through a shared, per-partition MMAP CACHE (keyed by canonical key,
  // evicted when the file's F6 content identity changes), so repeated loads of the
  // same partition reuse one mapping instead of re-reading the whole file each
  // call. Still materializes an owned PricedSurface (for callers that need one);
  // the zero-copy path is `map_surface`.
  [[nodiscard]] Result<PricedSurface> load_surface(std::string_view key,
                                                   std::string_view symbol) const;

  // Zero-copy view of `symbol` in partition `key`, over the shared cached mapping
  // (S5). The returned LoadedSurface co-owns the mapping so the view is safe even
  // across a concurrent rewrite/eviction. This is the reconstruct-free deserialize
  // path (kills bottleneck #2): O(1) hash-probe + a stack view on a cache hit, no
  // per-surface allocation for parametric kinds.
  [[nodiscard]] Result<LoadedSurface> map_surface(std::string_view key,
                                                  std::string_view symbol) const;
  [[nodiscard]] Status drop_partition(std::string_view key);

private:
  SurfaceDb() = default;
  [[nodiscard]] Status persist_locked(std::vector<DbSymbolEntry> symbols,
                                      std::vector<DbPartitionInfo> partitions);
  [[nodiscard]] std::string manifest_path() const;
  [[nodiscard]] std::string partition_path(std::string_view canonical_key) const;

  // S5 view cache: return the shared mapping for the partition at `canonical_key`,
  // opening it iff absent or its on-disk F6 content identity changed (rewrite).
  // Shared across cohorts/callers; I/O happens outside `cache_mu_`.
  [[nodiscard]] Result<std::shared_ptr<const SurfaceArchiveV2>>
  cached_partition(std::string_view canonical_key) const;
  // Drop any cached mapping for `canonical_key` (called on write/drop).
  void evict_partition(std::string_view canonical_key) const;

  // One cached partition mapping + the content identity it was opened against +
  // this key's slot in `cache_recency_` (its LRU position).
  struct PartitionCacheEntry {
    std::shared_ptr<const SurfaceArchiveV2> archive;
    ArchiveContentIdentity identity;
    std::list<std::string>::iterator recency;
  };

  std::string root_{};
  mutable std::unique_ptr<std::mutex> mu_{}; // guards snapshot_ swap + writes
  std::shared_ptr<const DbManifest> snapshot_{};
  // S5 partition view cache (guarded by cache_mu_). `cache_recency_` orders keys
  // least-recently-USED at the front, most-recently at the back; each map entry
  // stores an iterator into it. On a hit the key is spliced to the back; when the
  // map exceeds partition_cache_capacity_ the front (LRU) key is evicted. Evicting
  // only drops the cache's shared_ptr — an outstanding LoadedSurface / caller that
  // still co-owns the mapping keeps it alive, so no in-use view is invalidated.
  mutable std::unique_ptr<std::mutex> cache_mu_{};
  mutable std::unordered_map<std::string, PartitionCacheEntry> partition_cache_{};
  mutable std::list<std::string> cache_recency_{};
  std::size_t partition_cache_capacity_{kSurfaceDbDefaultPartitionCacheCapacity};
};

} // namespace atx::vol
