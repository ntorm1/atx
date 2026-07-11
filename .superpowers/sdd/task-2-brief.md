### Task 2: `surface_db.hpp` on-disk records, `SymbolFitConfig`, manifest write/parse (in-memory)

The manifest binary format. Fixed-layout POD records mirroring the full per-symbol fit configuration (`CurveConfig` = kind + `ConvexFitOpts` + full `CalibOpts`, plus `AlOpts` override, plus session policy scalars), a writer that serializes to bytes, and an immutable parsed `DbManifest` with O(log n) symbol/partition lookup. Pure in-memory in this task — file IO is Task 3.

**Files:**
- Create: `atx-vol/include/atx/vol/surface_db.hpp`
- Create: `atx-vol/src/surface_db.cpp`
- Create: `atx-vol/tests/surface_db_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add `src/surface_db.cpp`)
- Modify: `atx-vol/tests/CMakeLists.txt` (add `surface_db_test.cpp` to the `add_executable(atx-vol-tests ...)` list)

**Interfaces:**
- Consumes: Task 1 `atx::vol::detail::{crc32c, align_up, canonicalize_symbol}`; existing types `CurveConfig` (vol_curve.hpp:287), `ConvexFitOpts` (dense_slice.hpp:70), `CalibOpts` (calib.hpp:133), `AlOpts` (american.hpp:46), `FitPreset` (session.hpp:120), `CalendarRepair` (surface_parity.hpp:87), enums `VolCurveKind`, `CalibLossKind`, `CalibAnchorKind`, `EssviRhoMode`, `OptimizationLevel`, `ResidualBasisKind`.
- Produces (used by Tasks 3-5): everything below, exactly as declared.

- [ ] **Step 1: Write the header.** `atx-vol/include/atx/vol/surface_db.hpp` — start with a house-style top-of-file doc comment (see surface_archive.hpp:1-56 for tone: on-disk shape, integrity, thread-safety sections), then:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
// sorted internally. Errors: InvalidArgument (empty/oversized symbol, bad
// partition key); AlreadyExists (duplicate canonical symbol or key).
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

}  // namespace atx::vol
```

(The `SurfaceDb` class and `apply_symbol_config` are added to this same header in Tasks 3-5.)

- [ ] **Step 2: Write failing tests.** `atx-vol/tests/surface_db_test.cpp` — follow surface_archive_test.cpp's include/pch style. Tests to write now:

```cpp
#include <gtest/gtest.h>

#include <vector>

#include "atx/vol/calib.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::vol {
namespace {

// A config where EVERY field differs from its default, so a round-trip that
// drops or transposes any field fails the equality sweep below.
SymbolFitConfig make_full_config() {
  SymbolFitConfig c;
  c.enabled = false;
  c.preset = FitPreset::Hft;
  c.pin_curve = true;
  c.curve.kind = VolCurveKind::ConvexDense;
  c.curve.convex.lambda = 7.5e-4;
  c.curve.convex.bound_slope_below = true;
  c.curve.convex.node_cap = 56;
  c.curve.convex.max_iter = 123;
  c.curve.convex.loss = CalibLossKind::Interval;
  auto& p = c.curve.parametric;
  p.max_outer_iter = 5; p.max_inner_iter = 13;
  p.tol_param = 2e-9; p.tol_residual = 3e-10;
  p.huber_k = 1.75;
  p.min_vega_weight = 2e-6; p.max_spread_vol = 0.07; p.max_weight = 500.0;
  p.max_obs_per_slice = 96; p.max_otm_shortcut_premium_spread_frac = 0.25;
  p.prior_strength = 0.5;
  p.essvi_rho_mode = EssviRhoMode::Shared;
  p.optimization_level = OptimizationLevel::Risk;
  p.essvi_fallback_rmse_threshold = 0.02; p.n_butterfly_grid = 128;
  p.max_iter_quick_mark = 9; p.max_iter_trading = 36; p.max_iter_risk = 101;
  p.max_iter_reference = 251; p.max_iter_cold_fast = 11;
  p.wing_floor_alpha = 0.05;
  p.lee_bound_project = false;
  p.morozov_stop = true; p.morozov_tau = 1.3;
  p.validate_no_arb = false;
  p.residual_disable = false;
  p.residual_basis_kind = ResidualBasisKind::C2Bspline;
  p.residual_n_basis_terms = 8; p.residual_ridge_factor = 2e-3;
  p.loss_kind = CalibLossKind::Interval;
  p.anchor_kind = CalibAnchorKind::Ask;
  p.essvi_asymmetric_rho = true;
  p.min_obs_per_slice = 6; p.max_post_fit_sigma = 3.0;
  p.max_spread_to_mid_pct = 0.4;
  c.al_override = true;
  c.al = AlOpts{9, 20, 6, 1e-9};
  c.band_k = 1.25;
  c.calendar_repair = CalendarRepair::Project;
  c.use_correction_cache = false;
  c.score_parity = false;
  c.enforce_calendar_floor = false;
  c.use_deam_cache_for_fit = true;
  return c;
}

void expect_config_eq(const SymbolFitConfig& a, const SymbolFitConfig& b) {
  EXPECT_EQ(a.enabled, b.enabled);
  EXPECT_EQ(a.preset, b.preset);
  EXPECT_EQ(a.pin_curve, b.pin_curve);
  EXPECT_EQ(a.curve.kind, b.curve.kind);
  EXPECT_EQ(a.curve.convex.lambda, b.curve.convex.lambda);
  EXPECT_EQ(a.curve.convex.bound_slope_below, b.curve.convex.bound_slope_below);
  EXPECT_EQ(a.curve.convex.node_cap, b.curve.convex.node_cap);
  EXPECT_EQ(a.curve.convex.max_iter, b.curve.convex.max_iter);
  EXPECT_EQ(a.curve.convex.loss, b.curve.convex.loss);
  const auto& x = a.curve.parametric; const auto& y = b.curve.parametric;
  EXPECT_EQ(x.max_outer_iter, y.max_outer_iter);
  EXPECT_EQ(x.max_inner_iter, y.max_inner_iter);
  EXPECT_EQ(x.tol_param, y.tol_param);
  EXPECT_EQ(x.tol_residual, y.tol_residual);
  EXPECT_EQ(x.huber_k, y.huber_k);
  EXPECT_EQ(x.min_vega_weight, y.min_vega_weight);
  EXPECT_EQ(x.max_spread_vol, y.max_spread_vol);
  EXPECT_EQ(x.max_weight, y.max_weight);
  EXPECT_EQ(x.max_obs_per_slice, y.max_obs_per_slice);
  EXPECT_EQ(x.max_otm_shortcut_premium_spread_frac, y.max_otm_shortcut_premium_spread_frac);
  EXPECT_EQ(x.prior_strength, y.prior_strength);
  EXPECT_EQ(x.essvi_rho_mode, y.essvi_rho_mode);
  EXPECT_EQ(x.optimization_level, y.optimization_level);
  EXPECT_EQ(x.essvi_fallback_rmse_threshold, y.essvi_fallback_rmse_threshold);
  EXPECT_EQ(x.n_butterfly_grid, y.n_butterfly_grid);
  EXPECT_EQ(x.max_iter_quick_mark, y.max_iter_quick_mark);
  EXPECT_EQ(x.max_iter_trading, y.max_iter_trading);
  EXPECT_EQ(x.max_iter_risk, y.max_iter_risk);
  EXPECT_EQ(x.max_iter_reference, y.max_iter_reference);
  EXPECT_EQ(x.max_iter_cold_fast, y.max_iter_cold_fast);
  EXPECT_EQ(x.wing_floor_alpha, y.wing_floor_alpha);
  EXPECT_EQ(x.lee_bound_project, y.lee_bound_project);
  EXPECT_EQ(x.morozov_stop, y.morozov_stop);
  EXPECT_EQ(x.morozov_tau, y.morozov_tau);
  EXPECT_EQ(x.validate_no_arb, y.validate_no_arb);
  EXPECT_EQ(x.residual_disable, y.residual_disable);
  EXPECT_EQ(x.residual_basis_kind, y.residual_basis_kind);
  EXPECT_EQ(x.residual_n_basis_terms, y.residual_n_basis_terms);
  EXPECT_EQ(x.residual_ridge_factor, y.residual_ridge_factor);
  EXPECT_EQ(x.loss_kind, y.loss_kind);
  EXPECT_EQ(x.anchor_kind, y.anchor_kind);
  EXPECT_EQ(x.essvi_asymmetric_rho, y.essvi_asymmetric_rho);
  EXPECT_EQ(x.min_obs_per_slice, y.min_obs_per_slice);
  EXPECT_EQ(x.max_post_fit_sigma, y.max_post_fit_sigma);
  EXPECT_EQ(x.max_spread_to_mid_pct, y.max_spread_to_mid_pct);
  EXPECT_EQ(a.al_override, b.al_override);
  EXPECT_EQ(a.al.n_collocation, b.al.n_collocation);
  EXPECT_EQ(a.al.n_quadrature, b.al.n_quadrature);
  EXPECT_EQ(a.al.max_newton_iter, b.al.max_newton_iter);
  EXPECT_EQ(a.al.tol, b.al.tol);
  EXPECT_EQ(a.band_k, b.band_k);
  EXPECT_EQ(a.calendar_repair, b.calendar_repair);
  EXPECT_EQ(a.use_correction_cache, b.use_correction_cache);
  EXPECT_EQ(a.score_parity, b.score_parity);
  EXPECT_EQ(a.enforce_calendar_floor, b.enforce_calendar_floor);
  EXPECT_EQ(a.use_deam_cache_for_fit, b.use_deam_cache_for_fit);
}

TEST(SurfaceDbManifest, RoundTrip_FullConfig_EveryFieldPreserved) {
  const auto cfg = make_full_config();
  const std::vector<DbSymbolEntry> syms{{"aapl", cfg}, {"SPY", SymbolFitConfig{}}};
  const std::vector<DbPartitionInfo> parts{
      {"2026-07-10", 123, 456789, 1720569600000000000LL}};
  auto bytes = write_db_manifest(syms, parts, {.generation = 7});
  ASSERT_TRUE(bytes.has_value());
  auto m = DbManifest::open(std::move(*bytes));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->generation(), 7u);
  ASSERT_EQ(m->symbols().size(), 2u);
  ASSERT_EQ(m->partitions().size(), 1u);
  // canonical sort: AAPL < SPY
  auto got = m->find_symbol("AaPl");   // case-insensitive
  ASSERT_TRUE(got.has_value());
  expect_config_eq(*got, cfg);
  auto dflt = m->find_symbol("spy");
  ASSERT_TRUE(dflt.has_value());
  expect_config_eq(*dflt, SymbolFitConfig{});
  const auto* p = m->find_partition("2026-07-10");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->surface_count, 123u);
  EXPECT_EQ(p->file_size, 456789u);
  EXPECT_EQ(m->find_partition("2026-07-11"), nullptr);
  EXPECT_EQ(m->find_symbol("MSFT").error().code(), ErrorCode::NotFound);
}

TEST(SurfaceDbManifest, Write_RejectsDuplicateAndInvalid) {
  const std::vector<DbSymbolEntry> dup{{"AAPL", {}}, {"aapl", {}}};
  EXPECT_EQ(write_db_manifest(dup, {}).error().code(), ErrorCode::AlreadyExists);
  const std::vector<DbSymbolEntry> empty_sym{{"", {}}};
  EXPECT_EQ(write_db_manifest(empty_sym, {}).error().code(), ErrorCode::InvalidArgument);
  const std::vector<DbPartitionInfo> bad_key{{"bad/key", 0, 0, 0}};
  EXPECT_EQ(write_db_manifest({}, bad_key).error().code(), ErrorCode::InvalidArgument);
  const std::vector<DbPartitionInfo> dotdot{{"..", 0, 0, 0}};
  EXPECT_EQ(write_db_manifest({}, dotdot).error().code(), ErrorCode::InvalidArgument);
}

TEST(SurfaceDbManifest, Open_RejectsCorruption) {
  auto bytes = write_db_manifest({{DbSymbolEntry{"AAPL", {}}}}, {});
  ASSERT_TRUE(bytes.has_value());
  {
    auto bad = *bytes; bad[0] ^= std::byte{0xFF};  // magic
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad[100] ^= std::byte{0x01};  // header reserved => header CRC
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad[200] ^= std::byte{0x01};  // symbol record => payload CRC
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad.resize(bad.size() - 1);   // truncation
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
}

}  // namespace
}  // namespace atx::vol
```

Note: if `write_db_manifest({}, ...)` with zero symbols should be legal (an empty db is created before any symbol is configured), it is — empty symbol AND partition lists are valid; only malformed entries reject. Add `TEST(SurfaceDbManifest, RoundTrip_Empty)` asserting an empty manifest round-trips with counts 0.

- [ ] **Step 3: Wire the build and run tests to verify they fail.** Add `src/surface_db.cpp` (can be a stub containing only includes at first compile) and the test file to the two CMakeLists. Run: `& .\scripts\atx-build.ps1 build atx-vol-tests` — expect link/compile failure on missing symbols (that is the "failing" state for a compiled language; do not fake-pass).

- [ ] **Step 4: Implement `src/surface_db.cpp`.** Implementation notes (follow surface_archive.cpp structure):
  - Anonymous namespace: `kDbMagic = {'A','T','X','V','D','B','0','1'}`; `db_schema_hash()` — FNV-1a fold of `sizeof(DbManifestHeader)`, `sizeof(DbSymbolRecord)`, `sizeof(DbPartitionRecord)` + a salt constant (copy the fold pattern from surface_archive.cpp `schema_hash()`, line ~167, with its own salt string, e.g. `"atx-vol-surface-db-v1"`).
  - `encode_symbol_record(canon, cfg) -> DbSymbolRecord` and public `decode_symbol_record` — field-for-field, bools to/from `flags` bits, enums via `static_cast`. Reject (InvalidArgument) enum wire values outside their defined range on decode (`preset > 3`, `curve_kind > 4`, `calendar_repair > 2`, `essvi_rho_mode > 2`, `optimization_level > 4`, `residual_basis_kind > 5`, `convex_loss/loss_kind > 1`, `anchor_kind > 2`) — a manifest written by a future writer must not alias into wrong behavior. (DbManifest::open validates every record eagerly so `find_symbol` stays cheap; open returns ParseError naming the symbol on the first bad record.)
  - Partition key validation: `canonicalize_key(k)`: length 1..32, each char in `[A-Za-z0-9._-]`, no `".."` substring, upper-cased; else InvalidArgument.
  - Writer: canonicalize + dedup-check symbols and keys (AlreadyExists), sort both (memcmp on canonical), layout: header @0, symbols @ `align_up(192, 64) = 192`, partitions @ `align_up(symbols_end, 64)`; fill counts/offsets/sizes; `payload_crc32c = crc32c(symbols‖partitions span)` over the contiguous [symbols_offset, end) span; then `header_crc32c` over header bytes with that field zeroed (order: fill everything else first — same discipline as archive header, surface_archive.cpp ~586-612).
  - `DbManifest::open`: bounds-check → magic/major/endian/pointer_bits/header_size/record sizes/schema hash → header CRC → offsets/counts sanity (within file, no overlap, sorted strictly ascending) → payload CRC → memcpy records out → validate every record decodes (enum ranges, `symbol_len <= 32`, `key_len <= 32`). Every failure: `Err(ErrorCode::ParseError, ...)` with a specific message.
  - `find_symbol`: canonicalize input, `std::lower_bound` over sorted records comparing (len, bytes); decode on hit.
  - Timestamps: `opts.created_ts_ns == 0` → `std::chrono::system_clock` now (copy archive's stamp pattern).

- [ ] **Step 5: Build + run the new tests.** Run: `& .\scripts\atx-build.ps1 build atx-vol-tests` then `& .\scripts\atx-build.ps1 -Ctest -R SurfaceDbManifest` — expect ALL PASS.

- [ ] **Step 6: Run the archive regression to prove no cross-damage.** `& .\scripts\atx-build.ps1 -Ctest -R SurfaceArchive` — expect PASS.

- [ ] **Step 7: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): surface_db manifest format - records, writer, validated parser"
```

---

