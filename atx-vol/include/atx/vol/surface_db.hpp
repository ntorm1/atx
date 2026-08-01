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
  // C3: the populate default is the right-sized Populate tier — Robust's eSSVI fit
  // quality with the fast Andersen-Lake preset for de-Am / cache sampling / baked
  // cold marks (the K1 audit found Robust's al_default_opts was never validated as
  // the populate choice and over-pays 4-8x on every solve). Economic parity vs
  // Robust (surface RMSE byte-identical to 5dp, coverage + calendar-arb preserved)
  // is gated on real OPRA boards; see VolaSession.C3PopulateTierEconomicParityVsRobust.
  // Robust stays available for final-fit / certification (docs/al-preset-ladder.md).
  FitPreset preset{FitPreset::Populate};
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
  // AlOpts::n_quad_price (C2 / SE-P1-2): the decoupled premium quadrature order
  // of the symbol's fit config. 0 ties it to al_n_quadrature; pre-C2 manifests
  // stored 0 here (a zero-filled reserved slot), so 0 -> tied preserves them. The
  // reuse is layout-invariant (sizeof unchanged), so the DB schema hash is
  // unchanged and every existing manifest still opens (no salt bump on the DB
  // side — a fit-config field cannot misprice already-stored surfaces).
  std::uint16_t al_n_quad_price{};
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

// ── Carry-over fit salt (FIX-D) ──────────────────────────────────────────────
//
// Mixed into `surface_db_config_fingerprint`. Bumping it invalidates every
// stored fingerprint, so every partition re-fits once instead of reusing its
// stored surfaces.
//
// WHAT THIS IS NOT: it is **not** a build fingerprint, and it does **not**
// detect a changed fitter. Nothing in either on-disk format records which binary
// produced a surface — `ArchiveV2Header::writer_version_hash` exists but the
// writer hard-codes it to 0 (surface_archive.cpp). So a change to
// `pricer_fitter.cpp`, `curve_fit.cpp`, `essvi_calib.cpp`, `surface_parity.cpp`
// or any curve fitter alters fitted surface bytes with NO config change and NO
// automatic invalidation. Whoever makes such a change must bump this BY HAND,
// the same discipline `kV2Salt` / `kV3Salt` already rely on in
// surface_archive.cpp.
//
// WHY THAT IS ACCEPTABLE HERE: the same exposure already exists, entirely
// unguarded, on the path this one mirrors. A date with no failing cell is
// `dates_skipped_complete` and its stored surfaces are never refreshed under ANY
// predicate — not config, not hive contents, not fitter version (3 of the 6
// dates in the measured production run). Carry-over extends that existing,
// accepted contract to the remaining dates and puts a STRICTER gate on it than
// the skip path has, which is none. Do not read it as a guarantee that a stale
// fit will be noticed.
//
// BUT THE TWO ARE NOT THE SAME SHAPE, and the earlier framing of this argument
// ("carry-over only makes an existing exposure uniform") elided the difference.
// A SKIPPED date leaves its partition byte-for-byte as some single earlier
// binary wrote it: stale, perhaps, but SELF-CONSISTENT. A CARRIED REWRITE
// produces a partition whose records come from two different binaries — the
// carried ones re-emitted verbatim from the old write, the newly-fitted ones
// produced by the current fitter — inside one file, under one header. That is a
// shape the skip path can never produce, and it is strictly newer exposure, not
// merely the old one spread wider. It is judged acceptable because a partition
// is a directory of independent per-symbol records with no cross-record
// invariant (the archive's CRCs are per record; `reconstruct_entry` is
// byte-lossless, gated by SurfaceArchiveV2.Reemit*), so mixing provenances
// cannot corrupt the file — but a consumer that assumed one partition means one
// fitter version would be wrong, and nothing on disk records which records came
// from where.
//
// IT ALSO DOES NOT COVER THE MARKET INPUTS — READ THIS BEFORE TRUSTING A CARRY.
// `fold_symbol_configs` folds the manifest's per-symbol `DbSymbolRecord`, i.e. the
// FIT CONFIGS, and nothing else. `OpraHiveSpec::r` — the carry rate, which the
// build CLI's `--r` sets — is not in `SymbolFitConfig` and is NOT folded. Neither
// is the snapshot minute nor the hive's contents. That is the more likely change
// of the two documented here: a changed fitter takes a source edit, a wrong `--r`
// takes one CLI flag, and a wrong `--r` is this tool's headline hazard
// (atx-vol/docs/surface-db-build.md, "Interest rate / carry").
//
// THE CONSEQUENCE IS OPERATIONAL, AND IT HAS BEEN MEASURED. A build at the wrong
// `--r` partly succeeds — on a production-shaped copy, `cells_ok 55 /
// cells_failed 98`, and a wrong-`--r` run of that class DESTROYED 95 already-
// stored surfaces on the dates it rewrote (a present, enabled cell whose re-fit
// fails loses its stored surface; see surface_db_populate.cpp's degraded-cell
// block). The operator notices and re-runs at the CORRECTED rate. Nothing is
// re-fitted:
//   - a date with nothing left to add is `dates_skipped_complete` and is not
//     touched at all (pre-existing, unchanged by carry-over);
//   - a date that IS rewritten carries its wrong-rate surfaces forward VERBATIM,
//     because their configs did not change and so this fingerprint still matches.
//     Before carry-over those siblings were re-fit and the corrected rate did
//     reach them. This is the part carry-over made worse.
// `cells_carried` is the only trace, and `verify` reports the database green —
// every byte checksums, because the bytes are exactly the ones the wrong rate
// produced. A poisoned database CANNOT be repaired by re-running it.
//
// WHAT AN OPERATOR MUST ACTUALLY DO. There is no `--force-refit` flag; a re-run is
// not a repair. Two remedies, both blunt, and they are the whole list:
//   1. DELETE the affected partition files (`<db-root>/partitions/<KEY>.atxvsa`)
//      and re-run the build over those dates. `open_partition` then fails, the
//      date is treated as never written, every loaded cell is re-fit at the new
//      rate, and `write_partition` overwrites the stale manifest record. The
//      REV-R3 coverage guard does not stand in the way of this, and that is a
//      deliberate property rather than a coincidence: it probes for the FILE
//      (`open_partition_file`), and this remedy's whole point is that there is no
//      file, so there is nothing on disk it could destroy. Do not "improve" that
//      probe into a manifest lookup — it would both re-open the fail-open this
//      guard closed and break this instruction. Between
//      the delete and the rebuild `verify` reports those cells `unmappable` and
//      `verdict FAILED` — correct, the bytes really are gone. A symbol stored on
//      that date but NOT in the rebuild's loaded set is not restored: deleting a
//      partition deletes it.
//   2. Or build into a FRESH `--db` root and swap the roots when it finishes.
//      Slower, and the only option that never leaves a half-state on disk.
// Bumping this salt is NOT a third remedy: it forces a re-fit only of the dates a
// run REWRITES, leaves every `dates_skipped_complete` date exactly as it was, and
// needs a rebuilt binary.
inline constexpr std::uint64_t kSurfaceDbCarryOverFitSalt = 0x5CA1'AB1E'F17D'0001ull;

// Does the caller of `write_partition` ATTEST that the surfaces it is handing
// over came out of the fitter under the manifest's CURRENT configs?
//
// FIX-D fix-1 (I5). The fingerprint stamped on a partition is a claim that a
// later resume trusts well enough to re-emit the stored surfaces instead of
// re-fitting them, and nothing in `write_partition` can verify it: a
// `PricedSurface` carries no record of the config that produced it. Stamping
// unconditionally made every caller assert it silently — and `write_partition`
// is public API documented to accept arbitrary symbols, so a caller storing
// surfaces produced elsewhere (a different config, a different fitter, a
// hand-built or migrated surface) for symbols that happen to be in the manifest
// permanently blessed them for carry-over without ever saying so.
//
// So the attestation is EXPLICIT and the default is `None`, which fails CLOSED:
// an unstamped partition folds to the 0 "unknown" sentinel and is re-fit rather
// than reused. Forgetting to attest costs one wasted re-fit; the opposite default
// costs a silently carried stale surface, which is the outcome this whole design
// ranks worst.
//
// WHO MAY MAKE THE CLAIM (FIX-D fix-2, I-3). `populate_surface_db` fits its own
// items, but it also re-emits whatever `SurfaceDbPopulateConfig::carry_over` names
// — a set that struct explicitly carries no predicate for — so it cannot vouch for
// the whole write either. It therefore FORWARDS its caller's
// `SurfaceDbPopulateConfig::attest` instead of asserting one, and
// `populate_universe_streaming` — the frame that actually runs the carry gate
// (`carry_valid`) — is the one that sets `FitterProduced`. The claim travels with
// the decision, so the fail-closed chain reaches the gate instead of stopping one
// frame short of it.
//
// WHEN THE CLAIM IS EVALUATED (M-4): the fold is taken at WRITE time, over the
// manifest snapshot held at that instant — not at the instant the caller resolved
// the configs it fitted under. `populate_surface_db` resolves its configs once at
// entry, so an in-process `upsert_symbol` racing a populate would stamp a
// fingerprint over configs the surfaces were NOT fitted under. Do not mutate the
// manifest concurrently with a populate. (`build_surface_db` serialises the config
// and populate stages, so the CLI cannot reach this.)
enum class DbConfigAttestation : std::uint8_t {
  None = 0,      // do not stamp; this partition will never be carried over
  FitterProduced // these surfaces came out of the fitter under the current configs
};

struct DbPartitionInfo {
  std::string key{}; // canonical
  std::uint32_t surface_count{};
  std::uint64_t file_size{};
  std::int64_t created_ts_ns{};
  // FIX-D: fingerprint of the per-symbol fit CONFIGS this partition was written
  // under (computed by `SurfaceDb::config_fingerprint`). Stored in the record's
  // previously-unused `reserved0`, so no on-disk struct changes size and no
  // existing manifest is rejected. 0 means UNKNOWN — either a manifest written
  // before this field existed, or a symbol whose config is no longer in the
  // manifest — and unknown always means "do not reuse the stored surfaces".
  std::uint64_t config_fingerprint{0};
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

  // ── Fit-config fingerprint (FIX-D carry-over predicate) ──
  //
  // Fold the CURRENT manifest fit-config of each named symbol into one stable
  // u64. Symbols are canonicalized and sorted internally, so caller order is
  // irrelevant; the fold is over `encode_symbol_record(.., provenance=nullopt)`,
  // the exact fixed-width mirror of `SymbolFitConfig` (INCLUDING surface_policy),
  // with the provenance half zeroed so that `write_partition`'s own provenance
  // write-back does not move the value. Returns 0 if ANY named symbol has no
  // manifest entry — 0 is the "unknown, never reuse" sentinel and is never
  // returned for a successful fold.
  //
  // A caller compares this against the fingerprint stored on the partition
  // (`partition_config_fingerprint`) to answer "were these surfaces produced by
  // the configs currently in the manifest?".
  [[nodiscard]] std::uint64_t config_fingerprint(std::span<const std::string> symbols) const;

  // The fingerprint recorded when `key`'s partition was last written. 0 if the
  // partition is absent, or was written before this field existed.
  [[nodiscard]] std::uint64_t partition_config_fingerprint(std::string_view key) const;

  // ── Manifest mutation (serialized; atomic rewrite; generation++) ──
  [[nodiscard]] Status upsert_symbol(
      std::string_view symbol, const SymbolFitConfig &cfg,
      std::optional<SurfaceProvenance> provenance = std::nullopt);

  // Batch twin of upsert_symbol: N entries, ONE atomic manifest rewrite and ONE
  // generation++ (a duplicate canonical name within the batch: the LAST entry
  // wins, mirroring N sequential upserts). An entry's nullopt provenance keeps
  // the stored provenance, exactly like upsert_symbol. A batch whose every
  // entry leaves its stored record BYTE-IDENTICAL persists nothing and the
  // generation does not move — the universe populate re-seeds its configs on
  // every invocation, so the resume path must be a zero-write no-op instead of
  // N full-manifest rewrites (O(N^2) bytes per build at N symbols; the reason
  // this exists). Validation is all-or-nothing: any invalid entry rejects the
  // whole batch with the on-disk manifest untouched. An empty span is Ok.
  [[nodiscard]] Status upsert_symbols(std::span<const DbSymbolEntry> upserts);
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
  //
  // CALLER OBLIGATION (FIX-D). `attest` decides whether this write stamps the
  // partition with a fingerprint of the manifest's CURRENT fit configs for the
  // symbols in `items` — a claim, which a later resume trusts by re-emitting the
  // stored surfaces instead of re-fitting them, that these surfaces came out of
  // those configs. Nothing here can verify it, so it is not assumed: the default
  // `DbConfigAttestation::None` stamps nothing, the partition keeps the 0
  // "unknown" fingerprint, and it is re-fit rather than carried. Pass
  // `FitterProduced` only if you fitted these surfaces yourself under the configs
  // currently in this manifest. See `DbConfigAttestation` for the full argument.
  //
  // The partition's ARCHIVE and the manifest's provenance write-back are
  // unaffected by `attest`; it governs the carry-over fingerprint alone.
  [[nodiscard]] Status write_partition(std::string_view key,
                                       std::span<const SurfaceArchiveItem> items,
                                       const ArchiveV2WriteOpts &opts = {},
                                       DbConfigAttestation attest = DbConfigAttestation::None);
  [[nodiscard]] Result<SurfaceArchiveV2> open_partition(std::string_view key) const;

  // Open the partition FILE for `key` directly, WITHOUT the manifest lookup
  // `open_partition` performs first. So a `NotFound` here is never merely
  // "unlisted" — see the error contract at the bottom of this block for what it
  // does and does not establish.
  //
  // REV-R3 fix-1 (review I-1). `open_partition` above answers "does this
  // database SERVE this key", and it must keep doing exactly that — its callers
  // read the manifest as the index of what the db offers, and widening it
  // underneath them would silently change what a `NotFound` from it means. This
  // accessor answers the different question "is there a FILE here, and what does
  // it hold", and it exists for the one caller that must not let the manifest
  // decide: the coverage guard in `populate_surface_db`, which is about to
  // commit a WHOLE-FILE rewrite and therefore needs the file's own directory,
  // not the index's opinion of it. A partition file present on disk but absent
  // from the manifest — a crash between `write_partition`'s archive rename and
  // its manifest persist, a manifest restored from an older copy, a
  // hand-assembled or partially-copied root, or `drop_partition` interrupted
  // after its manifest commit — is exactly the disagreement that guard exists to
  // resolve in the FILE's favour. Through `open_partition` it resolved the other
  // way and the rewrite overwrote the file.
  //
  // ERROR CONTRACT — written as what is CHECKED, because the two previous
  // versions of this paragraph were written as what is guaranteed and both were
  // false (REV-R3 fix-2, review N-1):
  //
  //   `NotFound`     the existence probe was asked with an `std::error_code`,
  //                  reported no error, and reported the path absent. This is
  //                  the only code on which a caller may conclude "there is
  //                  nothing here to lose".
  //   `IoError`      either the existence probe FAILED (the filesystem declined
  //                  to answer: denied ACL on the file or on `partitions/`, a
  //                  sharing violation, a transient volume or SMB fault) or the
  //                  file is there and would not read.
  //   `ParseError`   the file is there, read, and is not a valid archive.
  //   `InvalidArgument`  `key` is not a representable partition key.
  //
  // Callers MUST keep `NotFound` apart from the rest — conflating them is the
  // fail-open this was added to close, and folding a failed probe INTO
  // `NotFound` is how that fail-open came back a second time.
  //
  // What this does NOT establish: `SurfaceArchiveV2::open_file` re-probes the
  // path internally and still collapses a failed probe into `NotFound`, so a
  // filesystem that starts failing between the two adjacent stats yields
  // `NotFound` from here. The window is two syscalls on one thread.
  //
  // NO CACHING, deliberately: this bypasses the S5 partition view cache as well
  // as the manifest, so it always reads what is on disk right now. It is a
  // once-per-written-date call on the drain thread, not a hot path.
  [[nodiscard]] Result<SurfaceArchiveV2> open_partition_file(std::string_view key) const;

  // Does the MANIFEST list a partition for `key`? Snapshot lookup only — no file
  // I/O, no view cache, nothing on disk is consulted, so this answers ONLY "is
  // this key in the index", never "is there a file".
  //
  // REV-R3 fix-2 (review N-3). `open_partition` asks listed-AND-openable in one
  // call and returns one `NotFound` for either half. Pairing this with
  // `open_partition_file` splits it: file present + listed is the ordinary
  // rewrite, file present + UNLISTED is the manifest/file disagreement the
  // coverage guard resolves in the file's favour, and the two need different
  // operator advice — telling someone whose manifest disagrees with the disk to
  // go check `--r` sends them at the wrong problem. `false` for a key that will
  // not canonicalize: it cannot be listed under any spelling.
  [[nodiscard]] bool partition_listed(std::string_view key) const;

  // Read the market timestamp for partition `key` without opening a full
  // archive view or reconstructing a surface. This reads only the archive
  // header, the first directory entry, and that entry's fixed-size surface header. Partition
  // timestamps are unique by the later MarketSnapshot load contract, which
  // rejects surfaces in one partition whose PricingContext::now_ts_ns disagree.
  //
  // Errors mirror `open_partition`: `InvalidArgument` for an unrepresentable
  // key, `NotFound` when the manifest or archive file is absent, `IoError` for an
  // opened file that cannot be read, and `ParseError` for malformed framing.
  [[nodiscard]] Result<std::int64_t> session_ts(std::string_view key) const;

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
  // Remove `key` from the manifest, then unlink its archive file. Errors:
  // `InvalidArgument` (unrepresentable key), `NotFound` (the manifest does not
  // list `key`), `IoError`/`ParseError` from the manifest persist, and — since
  // REV-R3 fix-2 (review N-2) — `IoError` when the manifest commit SUCCEEDED but
  // the unlink did not. That last one is a partial success and it is reported
  // rather than swallowed for a reason the guard created: the leftover `.atxvsa`
  // is no longer inert (see the ordering note at the unlink in `surface_db.cpp`).
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
