#pragma once

// atx::engine::risk — the factor exposure matrix `X` builder (P4-6).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  build_exposures(panel, cfg, row, market_cap, group_id) assembles the per-date
//  M_valid×K Barra-style exposure block `X` from the panel cross-section at `row`
//  (row 0 = current date) plus two OPTIONAL external inputs. P4-7 consumes one `X`
//  per historical date to estimate factor returns + the factored covariance
//  V = XFXᵀ + D. This is a COLD, allocating builder (Eigen MatX scratch); it is
//  PURE given (panel, cfg, row, market_cap, group_id).
//
// ===========================================================================
//  As-built OHLCV reconciliation (OVERRIDES the plan §5.4 data assumptions)
// ===========================================================================
//  The as-built PanelView (loop/panel_types.hpp) exposes ONLY OHLCV
//  (open/high/low/close/volume, newest-first; row 0 = current, increasing row =
//  older). There is NO `cap`, NO `adv` field, and NO `IndClass`/sector group in
//  the panel. So the §5.4 exposures split into two classes:
//
//   * PANEL-DERIVED (pure OHLCV) — Momentum, Volatility, Beta, Liquidity. The
//     per-step return is ret[r][i] = close(r,i)/close(r+1,i) − 1 (row r is NEWER
//     than r+1), and adv20 = mean over 20 trailing rows of (close·volume).
//   * EXTERNAL — Size (= ln cap) needs fundamental market-cap, and the sector
//     dummies need the §0-H IndClass group map; NEITHER lives in a price-volume
//     panel. They are taken as OPTIONAL spans:
//       - `market_cap` : per-universe-instrument cap (length == instruments()
//         when present; EMPTY = absent). When empty and Size is in `style_mask`,
//         the Size column is OMITTED (never fabricated).
//       - `group_id`   : per-universe-instrument IndClass group (length ==
//         instruments(); EMPTY = absent). When empty OR cfg.sector_factors is
//         false, NO sector columns are emitted.
//
// ===========================================================================
//  PIT — point-in-time is STRUCTURAL
// ===========================================================================
//  PanelView is already a trailing newest-first view; the builder reads ONLY
//  rows >= `row` (= the present date and OLDER). There is no `t` index and no API
//  to read a row < `row`, so a future bar physically cannot enter `X`. `row` lets
//  P4-7 rebuild `X` at each historical date s by passing row = s.
//
// ===========================================================================
//  §5.4 column construction (computed at cross-section `row`)
// ===========================================================================
//  For each universe instrument i (skipped if close(row,i) is NaN/absent):
//   * Size       = ln(market_cap[i])                  (NaN if cap[i] <= 0)
//   * Momentum   = ts_sum(ret,252) − ts_sum(ret,21)   (12m minus 1m band)
//   * Volatility = population stddev(ret, 60)
//   * Beta       = cov(ret_i, ret_mkt)/var(ret_mkt) over 252 rows, ret_mkt =
//                  equal-weight mean over present instruments per row
//   * Liquidity  = ln(adv20), adv20 = mean(close·volume) over 20 rows
//  A factor is NaN when the trailing rows are insufficient (documented per helper).
//  Then each STYLE column is z-scored cross-sectionally over its non-NaN universe
//  per cfg.zscore_rule: DEFAULT CapWeightedWinsorV2 = iterative ±3σ winsorizing, then
//  cap-weighted mean and equal-weight population std (W0-R0, R-06); EqualWeightV1 = the pre-W0
//  (v − mean)/popstd. A single-instrument or zero-variance column standardizes to
//  0 (DEGENERATE — there is no cross-sectional spread to normalize). Sector dummies
//  (0/1) are NOT standardized.
//
//  W0-R0 PIT note: this builder standardizes ONE date with the cap/group spans it is
//  given. A multi-date caller must pass each date's own cap/group — use the
//  PitSideInputs overload (per_date) rather than today's values for every date.
//
// ===========================================================================
//  Drop rule (§3.3) + column order
// ===========================================================================
//  REQUIRED COLUMNS = the set of style factors actually EMITTED (in style_mask,
//  AND — for Size — with cap present). An instrument with a NaN in ANY required
//  style column (after the cap/availability gate) is DROPPED from this date: it
//  appears in no `instrument_rows` entry and contributes to no column (sector
//  membership alone does NOT keep a style-NaN instrument). The z-score is computed
//  AFTER the drop, over the surviving set.
//  COLUMN ORDER (deterministic): sector dummies FIRST (ascending group id), then
//  style columns in StyleFactor enum order (Size, Momentum, Volatility, Beta,
//  Liquidity), each gated by style_mask + availability.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG. Every reduction runs in canonical ascending (row, instrument) order;
//  the surviving-instrument list and sector-group list are ascending. Same inputs
//  -> byte-identical X.

// Non-template exposure math is implemented in src/risk/exposures.cpp.
// Keep these declarations available to existing builder and diagnostic callers.

#include <limits>    // std::numeric_limits (quiet NaN sentinel)
#include <span>      // std::span (optional external inputs)
#include <vector>    // std::vector (cold-path scratch)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, u8, u32, usize

#include "atx/core/linalg/linalg.hpp" // MatX (column-major Eigen), VecX

#include "atx/engine/loop/panel_types.hpp" // PanelView
#include "atx/engine/risk/estimator_policy.hpp"
#include "atx/engine/risk/fwd.hpp"         // StyleFactor / FactorModelConfig fwd decls

namespace atx::engine::risk {

// ===========================================================================
//  §4 types — StyleFactor + FactorModelConfig (match fwd.hpp `: atx::u8`).
// ===========================================================================

// Barra-style style-factor identifier. Order is LOAD-BEARING: it fixes both the
// style_mask bit assignment (bit i == StyleFactor i) and the emitted column order.
// The first kStyleFactorCount enumerators are the P4 PANEL (OHLCV) styles the legacy
// u8 `style_mask` addresses; the enumerators after them are the L7 FUNDAMENTAL styles
// (+ the Market intercept) addressed ONLY by the 32-bit StyleMask of
// risk/fundamental_factors.hpp. The legacy build_exposures never emits them (its mask
// loop stops at kStyleFactorCount), so appending here leaves the P4 path byte-identical.
enum class StyleFactor : atx::u8 {
  Size,
  Momentum,
  Volatility,
  Beta,
  Liquidity,
  // --- L7 fundamental block (fundamental_factors.hpp) ---
  BookToPrice,
  EarningsYield,
  Growth,
  Profitability,
  Leverage,
  DivYield,
  ResidVol,
  ShortInterest,
  STReversal,
  Market, // the all-ones market (country) intercept column
};

// Number of P4 panel style factors (the legacy u8 style_mask is a bitset over
// [0, kStyleFactorCount)).
inline constexpr atx::usize kStyleFactorCount = 5U;

// Number of StyleFactor enumerators overall (the L7 StyleMask addresses [0, this)).
inline constexpr atx::usize kAllStyleFactorCount = 15U;

// ===========================================================================
//  §3 CovarianceConfig — the S8 covariance-construction knobs (all opt-in).
// ===========================================================================
//  Carried on FactorModelConfig as the member `cov`. EVERY default reproduces the
//  P4 path bit-for-bit, so the existing risk suite stays green untouched. The full
//  set of S8-a fields is added NOW even though only the S8.1 robust-regression
//  fields (robust_regression / huber_c / robust_iters / cap_weight /
//  industry_sum_to_zero) are wired in this unit; landing the unused S8.2–S8.4
//  fields up front is INTENTIONAL — it avoids churning this struct (and re-touching
//  every including TU) once per later unit. Half-lives are in observations (rows);
//  0 / the default enum value selects the P4 path. All EWMA/NW reductions are
//  order-fixed (ascending lag, then factor). Method enums match the `: atx::u8`
//  underlying-type convention used by StyleFactor.
enum class FactorCovMethod : atx::u8 { LedoitWolfSingle /*P4 default*/, EwmaNeweyWest };
enum class SpecificRiskMethod : atx::u8 { PopVariance /*P4 default*/, EwmaNeweyWestStructural };

// ===========================================================================
//  W0-R0 versioned numeric rules (plan §5 "Correctness first"). Each V1 enumerator
//  restores the pre-W0 arithmetic OF ITS OWN RULE; the DEFAULTS are the corrected V2
//  behaviour. Append-only. An all-V1 config is NOT a full pre-W0 replay: three R-04 /
//  R-06 changes are unversioned because the pre-W0 behaviour was a defect —
//    (a) a date whose sector has no member with a clean return drops that empty
//        column and is estimated (pre-W0 skipped it as rank-deficient);
//    (b) a date whose column set differs from X[0] is mapped by column identity
//        (pre-W0 wrote by position and could read past the coefficient vector, UB);
//    (c) an instrument with group id kNoGroup is dropped from that date.
//  Pre-W0 output is therefore reproduced only on panels where every date has the same
//  sector set as X[0] with at least one clean member per sector and no kNoGroup id.
// ===========================================================================

// R-03: which panel row's exposures explain the return r_s = close(s)/close(s+1) − 1.
//   ContemporaneousV1 — X built at row s. Its trailing windows (vol, beta, momentum,
//                       adv) start AT row s, so they contain r_s (and close(s)) itself:
//                       the regressor is built from the regressand (look-ahead).
//   LaggedV2          — X built at row s+1 (the prior close). Every window ends before
//                       r_s is realized, so X_{s−1} explains r_s in calendar terms.
// The model's OWN exposures X[0] (the forecast exposures for the next return) are
// built at row 0 under both rules.
enum class ExposureTiming : atx::u8 { ContemporaneousV1, LaggedV2 };

// R-06: cross-sectional standardization of a style column.
//   EqualWeightV1       — (v − equal-weight mean) / population std, no winsorizing.
//   CapWeightedWinsorV2 — iterative ±kZScoreWinsor·σ winsorizing of the raw values
//                         (equal-weight centre), then cap-weighted mean and
//                         equal-weighted population std (Barra USE4 convention: trim the
//                         raw descriptor, do NOT clip after cap-weighted centring). Without
//                         caps the mean is equal-weighted (still winsorized).
enum class ZScoreRule : atx::u8 { EqualWeightV1, CapWeightedWinsorV2 };

// R-05: specific-variance floor for names with too little residual history.
//   NoneV1             — D is used as estimated; a name with < 2 residuals gets D = 0,
//                        floored only to kSpecificVarFloor (1e-12) by FactorModel::create.
//   StructuralMedianV2 — a name with fewer than the effective min_obs residuals is
//                        shrunk toward a structural ln-D-on-exposures prediction, then
//                        EVERY name is floored at specific_floor_frac·median(D).
enum class SpecificFloorRule : atx::u8 { NoneV1, StructuralMedianV2 };

struct CovarianceConfig {
  // --- S8.1 robust regression (WIRED in S8.1) ---
  bool robust_regression = false;    // false ⇒ plain inverse-d0 WLS (P4)
  atx::f64 huber_c = 1.345;          // Huber tuning constant (95% Gaussian efficiency)
  atx::usize robust_iters = 5;       // FIXED IRLS iterations (determinism: no convergence exit)
  bool cap_weight = false;           // √-cap instrument weighting (needs market_cap)
  bool industry_sum_to_zero = false; // cap-weighted industry-dummy sum-to-zero constraint
  // --- S8.2 EWMA + Newey-West factor covariance (reserved; not wired in S8.1) ---
  FactorCovMethod factor_cov_method = FactorCovMethod::LedoitWolfSingle;
  atx::usize vol_halflife = 0;  // fast HL for variances    (0 ⇒ unused; e.g. 60 short-horizon)
  atx::usize corr_halflife = 0; // slow HL for correlations (0 ⇒ unused; e.g. 125)
  atx::usize nw_lags = 0;       // Newey-West Bartlett lags  (0 ⇒ no serial-corr adjustment)
  // --- S8.3 eigenfactor risk adjustment (reserved; the ONLY future RNG site) ---
  atx::usize eigen_adjust_sims = 0;    // 0 ⇒ no adjustment; e.g. 1000 sims when enabled
  atx::f64 eigen_adjust_amplify = 1.0; // a in γ(k)=a(v(k)−1)+1. DEFAULT 1.0 (NOT the paper's 1.4)
  atx::u64 eigen_adjust_seed = 0;      // recorded; same seed ⇒ byte-identical F̂
  // --- S8.4 specific risk (reserved; not wired in S8.1) ---
  SpecificRiskMethod specific_method = SpecificRiskMethod::PopVariance;
  atx::usize spec_halflife = 0;  // EWMA HL for specific-return variance (0 ⇒ unused)
  atx::usize spec_nw_lags = 0;   // Newey-West lags for specific autocorrelation
  bool structural_blend = false; // blend thin-history names toward ln-vol-on-exposures model
  // --- S8.5 Volatility Regime Adjustment (VRA) ---
  atx::usize vra_halflife = 0; // 0 ⇒ no VRA (λ²≡1); e.g. 42 (short) / 168 (long) — USE4
  // --- S8.6 statistical factors (APCA); activates when FactorModelConfig.n_stat_factors > 0 ---
  bool apca_gls_reweight = true; // 2nd APCA pass (GLS residual-variance reweight); false ⇒ 1-pass
  // --- S8.8 short/long-horizon blend (the long-horizon half-life set + convex weight) ---
  bool horizon_blend = false;          // false ⇒ single-horizon (P4 / S8.2 path), byte-identical
  atx::f64 horizon_blend_weight = 0.5; // w in F = w·F_short + (1−w)·F_long (clamped [0,1])
  atx::usize vol_halflife_long = 0;  // long-horizon vol HL  (short HL is the existing vol_halflife)
  atx::usize corr_halflife_long = 0; // long-horizon corr HL (short = existing corr_halflife)
  atx::usize spec_halflife_long = 0; // long-horizon specific HL (short = existing spec_halflife)
  // --- W0-R0 thin-name specific-variance floor (R-05) ---
  SpecificFloorRule specific_floor = SpecificFloorRule::StructuralMedianV2; // V1 ⇒ pre-W0
  // A name is THIN when its residual count n < min(specific_min_obs, max(2, ⌈full/2⌉)),
  // `full` = the deepest residual count in the current cross-section (so a short test
  // window does not mark every name thin). 21 ≈ one trading month.
  atx::usize specific_min_obs = 21;
  // D_i >= frac · median(D) for EVERY name (V2), on both the fundamental and the
  // statistical (APCA) builder. Clamped to [0, 1]; non-finite ⇒ InvalidArgument.
  atx::f64 specific_floor_frac = 0.1;
  RiskEstimatorPolicy estimator{}; // LegacyV1 by default; V2 supersedes old cleaning knobs.
};

struct FactorModelConfig {
  bool sector_factors = true;        // emit one dummy column per IndClass group
  atx::u8 style_mask = 0x1F;         // bitset over StyleFactor (bit i = factor i); default all 5
  atx::usize n_stat_factors = 0;     // P4-7 (PCA) — ignored here
  atx::usize n_dead_factors = 0;     // P4-7 — ignored here
  atx::f64 factor_cov_shrink = -1.0; // P4-7 — ignored here
  CovarianceConfig cov{};            // S8 covariance-construction knobs (defaults ⇒ P4)
  // W0-R0 (R-03 / R-06). Defaults are the corrected V2 rules; V1 reproduces pre-W0.
  ExposureTiming exposure_timing = ExposureTiming::LaggedV2;
  ZScoreRule zscore_rule = ZScoreRule::CapWeightedWinsorV2;
};

// ===========================================================================
//  W0-R0 point-in-time side inputs (R-06): per-date market cap and group id.
//
//  Layout: `market_cap` / `group_id` are row-major [n_rows × instruments()] with
//  row r ↔ panel row r (newest-first, row 0 = the current date), so the cap and the
//  group used for the exposures of date r are the values KNOWN AT r — never today's
//  value broadcast backwards. An EMPTY span means "absent" (Size / sector columns
//  omitted, exactly as in the single-date build_exposures).
//
//  n_rows == 0 is the STATIC (broadcast) form: each non-empty span has length
//  instruments() and is reused at every date. That is the legacy contract of the
//  (market_cap, group_id) FactorModelBuilder overloads — correct only when the
//  values really are constant over the fit window; PIT callers use per_date().
//
//  Group sentinel: an instrument whose group id is kNoGroup at a date (not yet
//  classified / not listed) is DROPPED from that date's cross-section when sector
//  columns are emitted (it cannot be given a sector dummy).
// ===========================================================================
inline constexpr atx::u32 kNoGroup = std::numeric_limits<atx::u32>::max();

struct PitSideInputs {
  std::span<const atx::f64> market_cap{}; // [n_rows × instruments] (or [instruments] static)
  std::span<const atx::u32> group_id{};   // [n_rows × instruments] (or [instruments] static)
  atx::usize n_rows = 0U;                 // rows covered; 0 ⇒ static broadcast

  // Legacy static form: the same cap/group span at every date.
  [[nodiscard]] static PitSideInputs broadcast(std::span<const atx::f64> cap,
                                               std::span<const atx::u32> group) noexcept {
    return PitSideInputs{cap, group, 0U};
  }
  // Point-in-time form: one row of cap/group per panel row [0, n_rows).
  [[nodiscard]] static PitSideInputs per_date(std::span<const atx::f64> cap,
                                              std::span<const atx::u32> group,
                                              atx::usize rows) noexcept {
    return PitSideInputs{cap, group, rows};
  }

  [[nodiscard]] bool is_static() const noexcept { return n_rows == 0U; }

  // Shape check against the panel's universe width. Err(InvalidArgument) on a length
  // that is neither empty nor the documented layout.
  [[nodiscard]] atx::core::Status validate(atx::usize n_inst) const {
    const atx::usize want = is_static() ? n_inst : n_rows * n_inst;
    if (!is_static() && n_inst != 0U && n_rows > std::numeric_limits<atx::usize>::max() / n_inst) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "PitSideInputs: n_rows × instruments overflows");
    }
    if (!market_cap.empty() && market_cap.size() != want) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "PitSideInputs: market_cap length must be n_rows × instruments "
                            "(or instruments when static)");
    }
    if (!group_id.empty() && group_id.size() != want) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "PitSideInputs: group_id length must be n_rows × instruments "
                            "(or instruments when static)");
    }
    return atx::core::Ok();
  }

  // True iff panel row `row` has side data (always true when static).
  [[nodiscard]] bool covers(atx::usize row) const noexcept { return is_static() || row < n_rows; }

  // The date-`row` slice (length instruments(), or empty when absent). PRECONDITION:
  // validate(n_inst) succeeded and covers(row).
  [[nodiscard]] std::span<const atx::f64> cap_at(atx::usize row, atx::usize n_inst) const noexcept {
    if (market_cap.empty() || is_static()) {
      return market_cap;
    }
    return market_cap.subspan(row * n_inst, n_inst);
  }
  [[nodiscard]] std::span<const atx::u32> group_at(atx::usize row,
                                                   atx::usize n_inst) const noexcept {
    if (group_id.empty() || is_static()) {
      return group_id;
    }
    return group_id.subspan(row * n_inst, n_inst);
  }
};

// ===========================================================================
//  Column descriptor + output block.
// ===========================================================================

// Describes one column of the exposure matrix (the column->factor map entry).
struct ColumnTag {
  enum class Kind : atx::u8 { Style, Sector } kind;
  StyleFactor style{}; // valid iff kind == Kind::Style
  atx::u32 group_id{}; // valid iff kind == Kind::Sector
};

// Per-date exposure block, COMPACTED to the surviving instruments.
struct ExposureMatrix {
  atx::core::linalg::MatX x;               // M_valid × K, column-major Eigen
  std::vector<atx::usize> instrument_rows; // universe column index of each matrix row, ascending
  std::vector<ColumnTag> columns;          // K descriptors: sector cols first, then style cols

  [[nodiscard]] atx::usize n_instruments() const noexcept { return instrument_rows.size(); }
  [[nodiscard]] atx::usize n_factors() const noexcept { return columns.size(); }
};

namespace detail {

// One trailing return: ret[r][i] = close(r,i)/close(r+1,i) − 1 (row r NEWER than
// r+1). NaN if either close is NaN/absent or the denominator is non-positive.
[[nodiscard]] atx::f64 step_return(const PanelView &panel, atx::usize r,
                                          atx::usize i) noexcept;

// Number of consecutive valid returns available starting at `row` (needs row+1 to
// exist). `want` caps the scan so the lookbacks stay bounded.
[[nodiscard]] atx::usize valid_returns(const PanelView &panel, atx::usize row, atx::usize i,
                                              atx::usize want) noexcept;

// CROSS-REFERENCE (atx-impl/src/stage_riskmodel.cpp's `deepest_lookback`): a
// PanelView-backing buffer must carry `estimation_window + deepest_lookback`
// rows so build_components's OLDEST estimation date still has a FULL trailing
// lookback for every style column it emits (an estimation date at row
// `estimation_window - 1` reads forward from that row, so it needs
// `deepest_lookback` more rows to exist beyond the estimation window's own
// span). `deepest_lookback` is `max` over the style columns actually emitted
// (cfg.style_mask) of THESE per-factor constants -- Beta needs the deepest
// (kBetaWindow, +1 row for its own trailing return), Momentum next
// (kMomLong), Volatility shallowest of the three (kVolWindow); Liquidity uses
// kAdvWindow. A sectors-only / all-style-off config still needs a floor of 1
// (the per-date return computation itself always reads one row past the
// estimation date, independent of which style columns are emitted -- see
// stage_riskmodel.cpp's own comment on that floor).
inline constexpr atx::usize kMomLong = 252U;    // 12-month return band
inline constexpr atx::usize kMomShort = 21U;    // 1-month return band
inline constexpr atx::usize kVolWindow = 60U;   // volatility lookback
inline constexpr atx::usize kBetaWindow = 252U; // beta lookback
inline constexpr atx::usize kAdvWindow = 20U;   // adv20 lookback

// Momentum = Σ_{r∈[row,row+252)} ret − Σ_{r∈[row,row+21)} ret. NaN if fewer than
// 252 valid returns are available (need closes through row+252).
[[nodiscard]] atx::f64 momentum(const PanelView &panel, atx::usize row,
                                       atx::usize i) noexcept;

// Volatility = population stddev of the newest 60 returns. NaN if < 60 valid.
[[nodiscard]] atx::f64 volatility(const PanelView &panel, atx::usize row,
                                         atx::usize i) noexcept;

// Liquidity = ln(adv20), adv20 = mean over 20 trailing rows of close·volume. NaN
// if fewer than 20 rows exist from `row`, any cell is NaN, or adv20 <= 0.
[[nodiscard]] atx::f64 liquidity(const PanelView &panel, atx::usize row,
                                        atx::usize i) noexcept;

// Equal-weight market return at row r: mean over PRESENT instruments of ret[r][·].
// NaN if no instrument has a valid return at r.
[[nodiscard]] atx::f64 market_return(const PanelView &panel, atx::usize r) noexcept;

// Beta = cov(ret_i, ret_mkt)/var(ret_mkt) over the trailing 252 rows. NaN if
// fewer than 252 paired (ret_i, ret_mkt) observations exist or var(ret_mkt)==0.
[[nodiscard]] atx::f64 beta(const PanelView &panel, atx::usize row, atx::usize i) noexcept;

// The equal-weight market return for rows [row, row + n) — mkt[k] ==
// market_return(panel, row + k) BIT-FOR-BIT (same function, same order). Rows past
// the panel end are NaN. Hoisting this out of the per-instrument beta loop turns the
// cross-section's O(M²·252) market-return recompute into O(M·252): the values (and
// therefore every downstream beta) are byte-identical to the uncached path.
[[nodiscard]] std::vector<atx::f64> market_returns(const PanelView &panel, atx::usize row,
                                                          atx::usize n);

// beta() with the market series precomputed by market_returns(panel, row, kBetaWindow).
// Identical arithmetic (same accumulation order) to beta(); `mkt.size() >= kBetaWindow`.
[[nodiscard]] atx::f64 beta_cached(const PanelView &panel, atx::usize row, atx::usize i,
                                          std::span<const atx::f64> mkt) noexcept;

// Raw (un-standardized) style value for factor `f` at instrument i. Size reads the
// external cap; the rest read the panel. EXHAUSTIVE switch over StyleFactor (no
// default — a new enumerator is a compile error).
[[nodiscard]] atx::f64 raw_style(StyleFactor f, const PanelView &panel, atx::usize row,
                                        atx::usize i,
                                        std::span<const atx::f64> market_cap) noexcept;

// The style factors EMITTED this date, in enum order: in style_mask AND — for Size
// — with a non-empty cap span. (Availability of the panel rows is per-instrument
// and handled by the drop rule, not the emit set.)
[[nodiscard]] std::vector<StyleFactor> emitted_styles(const FactorModelConfig &cfg,
                                                             bool have_cap);

// In-place cross-sectional z-score of one column over the surviving rows: subtract
// the mean, divide by population std. DEGENERATE (single row or zero variance) ->
// the whole column is set to 0 (no cross-sectional spread to normalize). All rows
// are non-NaN here (the drop rule already removed NaN-style instruments).
void zscore_column(atx::core::linalg::MatX &x, Eigen::Index col) noexcept;

// ±kZScoreWinsor is the ZScoreRule::CapWeightedWinsorV2 winsorizing bound (in σ).
inline constexpr atx::f64 kZScoreWinsor = 3.0;
// Upper bound on the iterative winsorizing passes (each pass clips at μ ± 3σ and
// re-estimates μ, σ; clipping only ever shrinks σ, so the loop converges quickly —
// the bound keeps it provably finite).
inline constexpr atx::usize kZScoreWinsorPasses = 16U;

// ZScoreRule::CapWeightedWinsorV2 standardization of one column IN PLACE (R-06).
//   w_r = cap weight of row r (`weights`, length x.rows(), NaN/≤0 ⇒ 0). When `weights`
//         is empty or sums to ≤ 0 the mean is equal-weighted.
//   Winsorize (≤ kZScoreWinsorPasses passes): μ_eq, σ = equal-weight mean and population
//         std; clip every x to [μ_eq − 3σ, μ_eq + 3σ]; stop when nothing was clipped.
//         The bounds are centred on the EQUAL-weight mean on purpose: a cap-weighted
//         centre is owned by the few largest names, so one mega-cap outlier would
//         capture it and push every other name to the bound.
//   Standardize: z = (x − μ_w)/σ on the winsorized values, μ_w = Σ w x / Σ w (the
//         cap-weighted mean, Barra USE4), σ the equal-weight population std.
// Contract: the cap-weighted mean of z is 0 and its equal-weight population std is 1
// (up to rounding), and every |z − mean_eq(z)| ≤ 3 + δ, where δ is the residual of the
// bounded winsorizing iteration: 0 when it stops with nothing clipped; it converges
// geometrically, so an extreme outlier can leave δ > 0 after kZScoreWinsorPasses
// (4.4e-8 for a planted 40-log-point ln-adv outlier among 40 names).
// There is deliberately NO clip of z itself after cap-weighted centring (W0-R0 fix 1,
// reviewer minor 3): μ_w sits about σ_ln above μ_eq for a lognormal cap spread, so a
// ±3 clip of z would pin the whole small-cap tail of Size (≈16% of names at ln-cap
// sd 2) at −3 and erase its ordering. |z| can therefore exceed 3 by |μ_eq − μ_w|/σ.
// DEGENERATE (≤ 1 row or σ == 0) ⇒ the column is 0, same as V1.
// Order-fixed (ascending row). All inputs non-NaN.
void zscore_column_v2(atx::core::linalg::MatX &x, Eigen::Index col,
                             std::span<const atx::f64> weights) noexcept;

// Distinct sector group ids among the surviving instruments, ASCENDING (the sector
// column order). Each becomes one 0/1 dummy column.
[[nodiscard]] std::vector<atx::u32> sector_groups(const std::vector<atx::usize> &survivors,
                                                         std::span<const atx::u32> group_id);

// ===========================================================================
//  §4/§5 robust-regression exposure helpers (S8.1; reuse home for later units +
//  the optimizer). Pure functions over the exposure types — no builder state.
// ===========================================================================

// Normalized √-cap instrument weight over a date's KEPT instruments:
//   w_i = √(cap_i) / mean_j √(cap_j)
// taken over the kept rows whose cap is POSITIVE (the normalizer is the mean of
// √cap across positive-cap kept names, so a flat-cap cross-section yields w ≡ 1).
// This down-weights mega-caps relative to plain cap-weighting without letting a few
// names dominate.
//
// Contract:
//   * `kept_instrument_rows` are universe instrument indices (ExposureMatrix row
//     order; ascending by construction); `market_cap[inst]` is that instrument's cap.
//   * A non-positive cap row (or an all-non-positive block, mean undefined) is FLAGGED
//     by returning 0.0 for that row — the caller substitutes its own fallback weight
//     (S8.1: 1/d0_i). 0.0 is an unambiguous sentinel: a real √-cap weight is strictly
//     positive, and a 0.0 prior weight is never the intended robust weighting.
//   * Order-fixed (ascending kept index). Returns a length == kept_instrument_rows.size()
//     vector. `market_cap` MUST be non-empty (the caller gates on cap availability).
[[nodiscard]] atx::core::linalg::VecX
sqrt_cap_weight(std::span<const atx::f64> market_cap,
                const std::vector<atx::usize> &kept_instrument_rows);

// Cap-weighted industry-sum-to-zero constraint, applied IN PLACE to a date's kept
// design `xsr` (rows aligned with `keep`, columns described by `xm.columns`). The
// market level (the all-ones direction captured by the style intercept / a market
// dummy) and the industry dummies are collinear; the canonical Barra resolution is
// to require the cap-weighted industry returns to sum to zero. We enforce the
// equivalent design-side condition by MEAN-CENTERING each industry (Kind::Sector)
// dummy column by its cap-weighted mean: column c ← x(:,c) − Σ_i ν_i x(i,c), with
// ν_i = √cap_i normalized to Σ ν_i = 1. After centering the cap-weighted sum of every
// industry column is 0, so the industry block is orthogonal to the cap-weighted
// market level and the collinear direction is removed (this replaces the as-built
// kNeutralizeRidge crutch when enabled). Style (z-scored) columns are already
// cross-sectionally centered and are left untouched. Falls back to equal-weight
// (1/M) centering when caps are absent so the constraint stays well defined. A block
// with no positive cap weight is left as-is. Order-fixed.
//
// NOTE (rank): the constraint deletes one degree of freedom — the cap-weighted market
// level shared by the industry dummies. When the dummies partition the universe and
// NO separate market-level/style column is present (the as-built sectors-only design),
// they already sum to the all-ones level, so centering them collapses the block to
// rank K−1 and the design becomes rank-deficient; the builder's OLS rank probe then
// skips that date. The constraint is non-degenerate only when a market-level column
// accompanies the dummies (it absorbs the removed direction).
void apply_industry_sum_to_zero(atx::core::linalg::MatX &xsr, const ExposureMatrix &xm,
                                       const std::vector<atx::usize> &keep,
                                       std::span<const atx::f64> market_cap);

} // namespace detail

// ===========================================================================
//  build_exposures — the per-date X builder (§8 P4-6).
//
//  PIT: reads only panel rows >= `row`. PURE given the inputs. Err on
//  (a) row >= panel.rows(), or (b) a non-empty cap/group span whose length !=
//  instruments(). Empty optional spans mean the corresponding columns are omitted.
// ===========================================================================
[[nodiscard]] atx::core::Result<ExposureMatrix>
build_exposures(const PanelView &panel, const FactorModelConfig &cfg, atx::usize row,
                std::span<const atx::f64> market_cap, std::span<const atx::u32> group_id);

// ===========================================================================
//  build_exposures — point-in-time side-input overload (W0-R0, R-06).
//
//  Same builder, but the cap and group used at `row` are side.cap_at(row) /
//  side.group_at(row): the values known at that date. Err(InvalidArgument) on a
//  malformed side-input shape; Err(OutOfRange) when a per-date side input does not
//  cover `row` (never silently falls back to another date's value).
// ===========================================================================
[[nodiscard]] atx::core::Result<ExposureMatrix>
build_exposures(const PanelView &panel, const FactorModelConfig &cfg, atx::usize row,
                const PitSideInputs &side);

} // namespace atx::engine::risk
