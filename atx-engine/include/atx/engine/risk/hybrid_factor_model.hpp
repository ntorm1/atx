#pragma once

// atx::engine::risk — L7 hybrid fundamental + statistical (APCA) factor model.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  A fast, matrix-native risk-model estimator that scales to the production shape
//  (3000 names × 14 styles × 60 industries × 504 days) and adds a STATISTICAL block on
//  top of the fundamental one:
//
//    1. estimate_factor_returns — one weighted cross-sectional regression per date
//       r_t = X_{t+1} f_t + u_t with √cap weights, a market intercept, industry dummies
//       and style exposures. The normal equations are accumulated in STRUCTURED form
//       (industry dummies are one-hot, so only the Ks×Ks style block is dense):
//       O(N·Ks²) per date instead of O(N·K²). With both the market and industries
//       present the design is collinear; the Barra/USE4 resolution — cap-weighted
//       industry factor returns sum to zero — is imposed EXACTLY by eliminating the
//       largest industry (f_ref = −Σ_g c_g/c_ref · f_g).
//    2. The residual panel U (assets × used dates) is mined for latent factors by
//       Connor-Korajczyk APCA (stat_factor_model.hpp kernels). The count K_s is chosen
//       by Bai-Ng IC_p2 or the Marchenko-Pastur edge (or fixed).
//    3. Assembly: X = [X_fund·R | B_stat], F = blockdiag(F_fund, F_stat), D = residual
//       variance after both blocks. R maps the free factor coordinates back to the
//       full industry set (the eliminated industry), so F is non-singular.
//
//  With K_s == 0 this is simply a fast fundamental model (the bench path).
//
// ===========================================================================
//  PIT convention (the ONLY one used in this unit)
// ===========================================================================
//  ReturnPanel row t (0 = NEWEST) holds the return REALIZED over (t+1 → t).
//  ExposureSeries style[t] holds the exposures KNOWN AT THE END of date t. The return
//  of row t is therefore regressed on style[t+1]; a model "as of" row a is estimated
//  from rows [a, a + window) and carries X = style[a] — the exposures known when the
//  forecast for row a−1 is issued. No return at a row < a is ever read.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  No RNG, no clock. All reductions are order-fixed (ascending date, asset, factor);
//  APCA eigenvectors are sign-pinned. Same inputs ⇒ byte-identical model.

#include <optional> // std::optional
#include <span>   // std::span
#include <vector> // std::vector

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, u8, u32, usize, i64

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/combine/cov_targets.hpp"       // CovTarget (factor_cov_target)
#include "atx/engine/loop/panel_types.hpp"         // PanelView (adapter)
#include "atx/engine/risk/exposures.hpp"           // StyleFactor
#include "atx/engine/risk/factor_model.hpp"        // FactorModel
#include "atx/engine/risk/fundamental_factors.hpp" // FundamentalPanel, FundamentalCfg

namespace atx::engine::risk {

// T×N asset returns, row 0 = NEWEST, r(t, i) realized over (t+1 → t). NaN = missing.
struct ReturnPanel {
  atx::core::linalg::MatX r;
  [[nodiscard]] atx::usize n_dates() const noexcept { return static_cast<atx::usize>(r.rows()); }
  [[nodiscard]] atx::usize n_assets() const noexcept { return static_cast<atx::usize>(r.cols()); }
};

// Industry id meaning "no industry" (the asset is excluded when industries are used).
inline constexpr atx::u32 kNoIndustry = 0xFFFFFFFFU;

// Exposures known at the END of each date (newest-first, aligned with ReturnPanel).
struct ExposureSeries {
  std::vector<atx::core::linalg::MatX> style; // each N×Ks; size 1 ⇒ STATIC (every date)
  std::vector<StyleFactor> style_tags;        // optional, Ks labels (reporting only)
  std::vector<atx::u32> industry;             // length N dense ids < n_industries, or empty
  atx::u32 n_industries = 0U;
  bool market = true;        // include the all-ones market intercept column
  std::vector<atx::f64> cap; // length N market caps (weights), or empty ⇒ equal weights

  [[nodiscard]] const atx::core::linalg::MatX &at(atx::usize t) const noexcept {
    return style.size() == 1U ? style[0] : style[t];
  }
  [[nodiscard]] atx::usize n_style() const noexcept {
    return style.empty() ? 0U : static_cast<atx::usize>(style[0].cols());
  }
  [[nodiscard]] atx::usize n_market() const noexcept { return market ? 1U : 0U; }
  [[nodiscard]] atx::usize n_ind() const noexcept { return industry.empty() ? 0U : n_industries; }
  // Full factor count of the design: market + industries + styles.
  [[nodiscard]] atx::usize n_factors() const noexcept { return n_market() + n_ind() + n_style(); }
};

// Per-date regression output over rows [as_of, as_of + window) (local row j = t − as_of).
struct FactorReturnSeries {
  atx::core::linalg::MatX f;     // window×K full-coordinate factor returns (skipped ⇒ NaN)
  atx::core::linalg::MatX resid; // window×N residuals (NaN where not in the regression)
  std::vector<atx::u8> used;     // per local row: 1 ⇔ the date's regression succeeded
  std::vector<atx::f64> r2;      // weighted R² per local row (NaN when skipped)
  atx::usize n_used = 0U;
  atx::usize as_of = 0U;
};

// Estimate the per-date factor returns. Err on shape mismatches, window == 0, or a
// window running past the panel / the exposure series (a non-static series must cover
// row as_of + window). Dates with too few valid assets or a singular design are
// SKIPPED (used == 0), never fabricated. Empty industries on a date get f = 0.
[[nodiscard]] atx::core::Result<FactorReturnSeries>
estimate_factor_returns(const ReturnPanel &ret, const ExposureSeries &exp, atx::usize as_of,
                        atx::usize window);

// Latent-factor count selection for the statistical block.
enum class StatFactorSelect : atx::u8 { Fixed, BaiNgIc2, MarchenkoPastur };

struct HybridCfg {
  atx::usize window = 252U;                             // estimation dates
  StatFactorSelect select = StatFactorSelect::BaiNgIc2; // K_s rule
  atx::usize n_stat_fixed = 0U;                         // K_s when select == Fixed
  atx::usize k_max = 8U;                                // cap on K_s for the IC / MP rules
  bool gls_reweight = true;                             // APCA 2nd (GLS) pass
  atx::f64 min_coverage = 0.8;   // fraction of used dates an asset needs for the APCA panel
  atx::usize min_spec_obs = 20U; // residual observations an asset needs to be modelled
  atx::f64 factor_cov_shrink = -1.0; // < 0 ⇒ Ledoit-Wolf intensity; else fixed δ
  atx::usize vol_halflife = 0U;      // any of vol/corr/nw > 0 ⇒ EWMA + Newey-West factor cov
  atx::usize corr_halflife = 0U;
  atx::usize nw_lags = 0U;
  atx::usize spec_halflife = 0U; // EWMA half-life for specific variance (0 ⇒ equal weights)
  // Opt-in factor-covariance estimator (risk::shrunk_factor_covariance over the
  // combine::CovTarget family, e.g. LW2020 nonlinear). Unset ⇒ the legacy LW-identity
  // path (factor_cov_shrink), byte-identical. The EWMA/Newey-West path above takes
  // precedence when any of its knobs is set.
  std::optional<atx::engine::combine::CovTarget> factor_cov_target;
};

// What the selection rule saw (reported in the validation scorecard).
struct StatSelection {
  atx::usize k = 0U;                   // chosen K_s
  atx::core::linalg::VecX eigenvalues; // descending eigenvalues of the rule's matrix
  atx::f64 mp_edge = 0.0;              // λ₊ (MarchenkoPastur only)
};

struct HybridModel {
  FactorModel model;
  std::vector<atx::usize> assets; // ReturnPanel column of each model row (ascending)
  atx::usize k_fundamental = 0U;  // free fundamental factor count (after the constraint)
  atx::usize k_stat = 0U;
  StatSelection selection;
  FactorReturnSeries factor_returns; // the fundamental regression series
};

class HybridFactorModelBuilder {
public:
  // Build the hybrid model as of row `as_of` (see the PIT convention above). Err when
  // the regression window is invalid, fewer than max(2, K) dates are usable, or no
  // asset has min_spec_obs residual observations.
  [[nodiscard]] static atx::core::Result<HybridModel>
  build(const ReturnPanel &ret, const ExposureSeries &exp, const HybridCfg &cfg,
        atx::usize as_of = 0U);
};

// ===========================================================================
//  PanelView adapter: returns + exposure series from the live OHLCV panel.
//
//  Builds return rows [0, window) (step_return) and style rows [0, window] via
//  build_fundamental_exposures (the Market bit ⇒ ExposureSeries::market; sectors come
//  from `group_id`, densely re-indexed ascending; caps from `market_cap`).
//  `row_dates[t]` is the day number of panel row t (newest-first, length >= window+1).
//  An instrument missing a style at a date gets a NaN style row there (it drops out of
//  that date's regression). Requires panel.rows() >= window + 2.
// ===========================================================================
struct PanelSeries {
  ReturnPanel returns;
  ExposureSeries exposures;
};

[[nodiscard]] atx::core::Result<PanelSeries>
build_panel_series(const PanelView &panel, const FundamentalPanel *fund, const FundamentalCfg &cfg,
                   std::span<const atx::i64> row_dates, std::span<const atx::f64> market_cap,
                   std::span<const atx::u32> group_id, atx::usize window);

} // namespace atx::engine::risk
