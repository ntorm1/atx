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
//  Then each STYLE column is z-scored cross-sectionally over its non-NaN universe:
//  (v − mean)/popstd. A single-instrument or zero-variance column standardizes to
//  0 (DEGENERATE — there is no cross-sectional spread to normalize). Sector dummies
//  (0/1) are NOT standardized.
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

#include <algorithm> // std::sort, std::unique
#include <cmath>     // std::isnan, std::log, std::sqrt
#include <limits>    // std::numeric_limits (quiet NaN sentinel)
#include <span>      // std::span (optional external inputs)
#include <vector>    // std::vector (cold-path scratch)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, u8, u32, usize

#include "atx/core/linalg/linalg.hpp" // MatX (column-major Eigen), VecX

#include "atx/engine/loop/panel_types.hpp" // PanelView
#include "atx/engine/risk/fwd.hpp"         // StyleFactor / FactorModelConfig fwd decls

namespace atx::engine::risk {

// ===========================================================================
//  §4 types — StyleFactor + FactorModelConfig (match fwd.hpp `: atx::u8`).
// ===========================================================================

// Barra-style style-factor identifier. Order is LOAD-BEARING: it fixes both the
// style_mask bit assignment (bit i == StyleFactor i) and the emitted column order.
enum class StyleFactor : atx::u8 { Size, Momentum, Volatility, Beta, Liquidity };

// Number of style factors (the style_mask is a bitset over [0, kStyleFactorCount)).
inline constexpr atx::usize kStyleFactorCount = 5U;

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
};

struct FactorModelConfig {
  bool sector_factors = true;        // emit one dummy column per IndClass group
  atx::u8 style_mask = 0x1F;         // bitset over StyleFactor (bit i = factor i); default all 5
  atx::usize n_stat_factors = 0;     // P4-7 (PCA) — ignored here
  atx::usize n_dead_factors = 0;     // P4-7 — ignored here
  atx::f64 factor_cov_shrink = -1.0; // P4-7 — ignored here
  CovarianceConfig cov{};            // S8 covariance-construction knobs (defaults ⇒ P4)
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
[[nodiscard]] inline atx::f64 step_return(const PanelView &panel, atx::usize r,
                                          atx::usize i) noexcept {
  const atx::f64 c_new = panel.close(r, i);
  const atx::f64 c_old = panel.close(r + 1U, i);
  if (std::isnan(c_new) || std::isnan(c_old) || c_old <= 0.0) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  return c_new / c_old - 1.0;
}

// Number of consecutive valid returns available starting at `row` (needs row+1 to
// exist). `want` caps the scan so the lookbacks stay bounded.
[[nodiscard]] inline atx::usize valid_returns(const PanelView &panel, atx::usize row, atx::usize i,
                                              atx::usize want) noexcept {
  atx::usize n = 0U;
  for (atx::usize k = 0U; k < want; ++k) {
    const atx::usize r = row + k;
    if (r + 1U >= panel.rows() || std::isnan(step_return(panel, r, i))) {
      break;
    }
    ++n;
  }
  return n;
}

inline constexpr atx::usize kMomLong = 252U;    // 12-month return band
inline constexpr atx::usize kMomShort = 21U;    // 1-month return band
inline constexpr atx::usize kVolWindow = 60U;   // volatility lookback
inline constexpr atx::usize kBetaWindow = 252U; // beta lookback
inline constexpr atx::usize kAdvWindow = 20U;   // adv20 lookback

// Momentum = Σ_{r∈[row,row+252)} ret − Σ_{r∈[row,row+21)} ret. NaN if fewer than
// 252 valid returns are available (need closes through row+252).
[[nodiscard]] inline atx::f64 momentum(const PanelView &panel, atx::usize row,
                                       atx::usize i) noexcept {
  if (valid_returns(panel, row, i, kMomLong) < kMomLong) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 long_sum = 0.0;
  atx::f64 short_sum = 0.0;
  for (atx::usize k = 0U; k < kMomLong; ++k) { // ascending row -> order-fixed sum
    const atx::f64 ret = step_return(panel, row + k, i);
    long_sum += ret;
    if (k < kMomShort) {
      short_sum += ret;
    }
  }
  return long_sum - short_sum;
}

// Volatility = population stddev of the newest 60 returns. NaN if < 60 valid.
[[nodiscard]] inline atx::f64 volatility(const PanelView &panel, atx::usize row,
                                         atx::usize i) noexcept {
  if (valid_returns(panel, row, i, kVolWindow) < kVolWindow) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 sum = 0.0;
  for (atx::usize k = 0U; k < kVolWindow; ++k) {
    sum += step_return(panel, row + k, i);
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(kVolWindow);
  atx::f64 ss = 0.0;
  for (atx::usize k = 0U; k < kVolWindow; ++k) {
    const atx::f64 d = step_return(panel, row + k, i) - mean;
    ss += d * d;
  }
  return std::sqrt(ss / static_cast<atx::f64>(kVolWindow)); // population std
}

// Liquidity = ln(adv20), adv20 = mean over 20 trailing rows of close·volume. NaN
// if fewer than 20 rows exist from `row`, any cell is NaN, or adv20 <= 0.
[[nodiscard]] inline atx::f64 liquidity(const PanelView &panel, atx::usize row,
                                        atx::usize i) noexcept {
  if (row + kAdvWindow > panel.rows()) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 sum = 0.0;
  for (atx::usize k = 0U; k < kAdvWindow; ++k) {
    const atx::f64 c = panel.close(row + k, i);
    const atx::f64 v = panel.volume(row + k, i);
    if (std::isnan(c) || std::isnan(v)) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    sum += c * v;
  }
  const atx::f64 adv = sum / static_cast<atx::f64>(kAdvWindow);
  return (adv <= 0.0) ? std::numeric_limits<atx::f64>::quiet_NaN() : std::log(adv);
}

// Equal-weight market return at row r: mean over PRESENT instruments of ret[r][·].
// NaN if no instrument has a valid return at r.
[[nodiscard]] inline atx::f64 market_return(const PanelView &panel, atx::usize r) noexcept {
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (atx::usize i = 0U; i < panel.instruments(); ++i) { // ascending instrument
    const atx::f64 ret = step_return(panel, r, i);
    if (!std::isnan(ret)) {
      sum += ret;
      ++n;
    }
  }
  return (n == 0U) ? std::numeric_limits<atx::f64>::quiet_NaN() : sum / static_cast<atx::f64>(n);
}

// Beta = cov(ret_i, ret_mkt)/var(ret_mkt) over the trailing 252 rows. NaN if
// fewer than 252 paired (ret_i, ret_mkt) observations exist or var(ret_mkt)==0.
[[nodiscard]] inline atx::f64 beta(const PanelView &panel, atx::usize row, atx::usize i) noexcept {
  if (row + kBetaWindow + 1U > panel.rows()) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 si = 0.0;
  atx::f64 sm = 0.0;
  atx::usize n = 0U;
  for (atx::usize k = 0U; k < kBetaWindow; ++k) {
    const atx::f64 ri = step_return(panel, row + k, i);
    const atx::f64 rm = market_return(panel, row + k);
    if (std::isnan(ri) || std::isnan(rm)) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    si += ri;
    sm += rm;
    ++n;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  const atx::f64 mi = si / nf;
  const atx::f64 mm = sm / nf;
  atx::f64 cov = 0.0;
  atx::f64 var = 0.0;
  for (atx::usize k = 0U; k < kBetaWindow; ++k) {
    const atx::f64 di = step_return(panel, row + k, i) - mi;
    const atx::f64 dm = market_return(panel, row + k) - mm;
    cov += di * dm;
    var += dm * dm;
  }
  return (var <= 0.0) ? std::numeric_limits<atx::f64>::quiet_NaN() : cov / var;
}

// Raw (un-standardized) style value for factor `f` at instrument i. Size reads the
// external cap; the rest read the panel. EXHAUSTIVE switch over StyleFactor (no
// default — a new enumerator is a compile error).
[[nodiscard]] inline atx::f64 raw_style(StyleFactor f, const PanelView &panel, atx::usize row,
                                        atx::usize i,
                                        std::span<const atx::f64> market_cap) noexcept {
  switch (f) {
  case StyleFactor::Size: {
    const atx::f64 cap = market_cap[i];
    return (std::isnan(cap) || cap <= 0.0) ? std::numeric_limits<atx::f64>::quiet_NaN()
                                           : std::log(cap);
  }
  case StyleFactor::Momentum:
    return momentum(panel, row, i);
  case StyleFactor::Volatility:
    return volatility(panel, row, i);
  case StyleFactor::Beta:
    return beta(panel, row, i);
  case StyleFactor::Liquidity:
    return liquidity(panel, row, i);
  }
  return std::numeric_limits<atx::f64>::quiet_NaN(); // unreachable (switch exhaustive)
}

// The style factors EMITTED this date, in enum order: in style_mask AND — for Size
// — with a non-empty cap span. (Availability of the panel rows is per-instrument
// and handled by the drop rule, not the emit set.)
[[nodiscard]] inline std::vector<StyleFactor> emitted_styles(const FactorModelConfig &cfg,
                                                             bool have_cap) {
  std::vector<StyleFactor> out;
  for (atx::usize b = 0U; b < kStyleFactorCount; ++b) {
    if ((cfg.style_mask & static_cast<atx::u8>(1U << b)) == 0U) {
      continue;
    }
    const auto f = static_cast<StyleFactor>(b);
    if (f == StyleFactor::Size && !have_cap) {
      continue; // cap absent -> Size never fabricated
    }
    out.push_back(f);
  }
  return out;
}

// In-place cross-sectional z-score of one column over the surviving rows: subtract
// the mean, divide by population std. DEGENERATE (single row or zero variance) ->
// the whole column is set to 0 (no cross-sectional spread to normalize). All rows
// are non-NaN here (the drop rule already removed NaN-style instruments).
inline void zscore_column(atx::core::linalg::MatX &x, Eigen::Index col) noexcept {
  const Eigen::Index m = x.rows();
  if (m <= 1) {
    for (Eigen::Index r = 0; r < m; ++r) {
      x(r, col) = 0.0; // single instrument -> degenerate
    }
    return;
  }
  atx::f64 sum = 0.0;
  for (Eigen::Index r = 0; r < m; ++r) {
    sum += x(r, col);
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(m);
  atx::f64 ss = 0.0;
  for (Eigen::Index r = 0; r < m; ++r) {
    const atx::f64 d = x(r, col) - mean;
    ss += d * d;
  }
  const atx::f64 var = ss / static_cast<atx::f64>(m); // population variance
  if (var <= 0.0) {
    for (Eigen::Index r = 0; r < m; ++r) {
      x(r, col) = 0.0; // zero-variance column -> degenerate
    }
    return;
  }
  const atx::f64 inv_std = 1.0 / std::sqrt(var);
  for (Eigen::Index r = 0; r < m; ++r) {
    x(r, col) = (x(r, col) - mean) * inv_std;
  }
}

// Distinct sector group ids among the surviving instruments, ASCENDING (the sector
// column order). Each becomes one 0/1 dummy column.
[[nodiscard]] inline std::vector<atx::u32> sector_groups(const std::vector<atx::usize> &survivors,
                                                         std::span<const atx::u32> group_id) {
  std::vector<atx::u32> groups;
  groups.reserve(survivors.size());
  for (const atx::usize inst : survivors) {
    groups.push_back(group_id[inst]);
  }
  std::sort(groups.begin(), groups.end());
  groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
  return groups;
}

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
[[nodiscard]] inline atx::core::linalg::VecX
sqrt_cap_weight(std::span<const atx::f64> market_cap,
                const std::vector<atx::usize> &kept_instrument_rows) {
  const Eigen::Index nk = static_cast<Eigen::Index>(kept_instrument_rows.size());
  atx::core::linalg::VecX w(nk);
  atx::f64 sum_root = 0.0; // Σ_j √(cap_j) over positive-cap kept names (order-fixed)
  atx::usize n_pos = 0U;
  for (const atx::usize inst : kept_instrument_rows) {
    const atx::f64 cap = market_cap[inst];
    if (cap > 0.0) {
      sum_root += std::sqrt(cap);
      ++n_pos;
    }
  }
  const atx::f64 mean_root = (n_pos == 0U) ? 0.0 : sum_root / static_cast<atx::f64>(n_pos);
  for (atx::usize j = 0U; j < kept_instrument_rows.size(); ++j) {
    const atx::f64 cap = market_cap[kept_instrument_rows[j]];
    w[static_cast<Eigen::Index>(j)] =
        (cap > 0.0 && mean_root > 0.0) ? std::sqrt(cap) / mean_root : 0.0; // 0.0 ⇒ fallback flag
  }
  return w;
}

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
inline void apply_industry_sum_to_zero(atx::core::linalg::MatX &xsr, const ExposureMatrix &xm,
                                       const std::vector<atx::usize> &keep,
                                       std::span<const atx::f64> market_cap) {
  const Eigen::Index nk = xsr.rows();
  if (nk == 0) {
    return;
  }
  atx::core::linalg::VecX nu(nk); // cap weights ν_i, normalized to Σ ν_i = 1
  atx::f64 total = 0.0;
  const bool use_cap = !market_cap.empty();
  for (atx::usize j = 0U; j < keep.size(); ++j) {
    const atx::f64 cap = use_cap ? market_cap[xm.instrument_rows[keep[j]]] : 1.0;
    const atx::f64 root = (cap > 0.0) ? std::sqrt(cap) : 0.0;
    nu[static_cast<Eigen::Index>(j)] = root;
    total += root;
  }
  if (total <= 0.0) {
    return; // no positive cap weight ⇒ constraint undefined ⇒ leave design as-is
  }
  nu /= total;
  for (atx::usize c = 0U; c < xm.columns.size(); ++c) {
    if (xm.columns[c].kind != ColumnTag::Kind::Sector) {
      continue; // only industry/sector dummies are mean-centered
    }
    const Eigen::Index col = static_cast<Eigen::Index>(c);
    atx::f64 wmean = 0.0;
    for (Eigen::Index r = 0; r < nk; ++r) {
      wmean += nu[r] * xsr(r, col);
    }
    for (Eigen::Index r = 0; r < nk; ++r) {
      xsr(r, col) -= wmean;
    }
  }
}

} // namespace detail

// ===========================================================================
//  build_exposures — the per-date X builder (§8 P4-6).
//
//  PIT: reads only panel rows >= `row`. PURE given the inputs. Err on
//  (a) row >= panel.rows(), or (b) a non-empty cap/group span whose length !=
//  instruments(). Empty optional spans mean the corresponding columns are omitted.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<ExposureMatrix>
build_exposures(const PanelView &panel, const FactorModelConfig &cfg, atx::usize row,
                std::span<const atx::f64> market_cap, std::span<const atx::u32> group_id) {
  if (row >= panel.rows()) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "build_exposures: row offset is beyond the panel's valid rows");
  }
  const atx::usize n_inst = panel.instruments();
  if (!market_cap.empty() && market_cap.size() != n_inst) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_exposures: market_cap span length must equal instruments()");
  }
  if (!group_id.empty() && group_id.size() != n_inst) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_exposures: group_id span length must equal instruments()");
  }

  const bool have_cap = !market_cap.empty();
  const bool have_sectors = cfg.sector_factors && !group_id.empty();
  const std::vector<StyleFactor> styles = detail::emitted_styles(cfg, have_cap);

  // Pass 1: compute every emitted style's raw value per present instrument, apply
  // the §3.3 drop rule (any required-style NaN drops the instrument), and collect
  // the surviving rows (ascending universe column) + their raw style values.
  std::vector<atx::usize> survivors;
  std::vector<std::vector<atx::f64>> raw; // raw[surviving_row][style_index]
  survivors.reserve(n_inst);
  raw.reserve(n_inst);
  for (atx::usize i = 0U; i < n_inst; ++i) {
    if (!panel.present(row, i)) {
      continue; // no bar at the current date -> not in the cross-section
    }
    std::vector<atx::f64> vals(styles.size());
    bool drop = false;
    for (atx::usize s = 0U; s < styles.size() && !drop; ++s) {
      vals[s] = detail::raw_style(styles[s], panel, row, i, market_cap);
      drop = std::isnan(vals[s]); // missing a required style -> drop the instrument
    }
    if (!drop) {
      survivors.push_back(i);
      raw.push_back(std::move(vals));
    }
  }

  // Column layout: sector dummies first (ascending group id), then style columns.
  std::vector<atx::u32> groups =
      have_sectors ? detail::sector_groups(survivors, group_id) : std::vector<atx::u32>{};
  const atx::usize n_sector = groups.size();
  const atx::usize n_style = styles.size();
  const atx::usize m = survivors.size();

  // SAFETY: m, n_sector, n_style are bounded by instruments()/kStyleFactorCount;
  //         the static_casts to Eigen::Index (signed) cannot overflow on any
  //         realistic universe (<< 2^31). Column-major MatX matches P4-7's WLS.
  atx::core::linalg::MatX x(static_cast<Eigen::Index>(m),
                            static_cast<Eigen::Index>(n_sector + n_style));
  std::vector<ColumnTag> columns;
  columns.reserve(n_sector + n_style);

  // Sector dummy columns (0/1, NOT standardized).
  for (atx::usize g = 0U; g < n_sector; ++g) {
    const atx::u32 gid = groups[g];
    for (atx::usize r = 0U; r < m; ++r) {
      const bool in_group = (group_id[survivors[r]] == gid);
      x(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(g)) = in_group ? 1.0 : 0.0;
    }
    columns.push_back(ColumnTag{ColumnTag::Kind::Sector, StyleFactor{}, gid});
  }

  // Style columns (raw -> cross-sectional z-score over the surviving set).
  for (atx::usize s = 0U; s < n_style; ++s) {
    const Eigen::Index col = static_cast<Eigen::Index>(n_sector + s);
    for (atx::usize r = 0U; r < m; ++r) {
      x(static_cast<Eigen::Index>(r), col) = raw[r][s];
    }
    detail::zscore_column(x, col);
    columns.push_back(ColumnTag{ColumnTag::Kind::Style, styles[s], 0U});
  }

  return atx::core::Ok(ExposureMatrix{std::move(x), std::move(survivors), std::move(columns)});
}

} // namespace atx::engine::risk
