#pragma once

// atx::engine::risk — L7 risk-model validation scorecard.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  validate_risk_model(factory, returns, cfg) walks forward over `cfg.n_periods`
//  forecast dates. At each as_of row a (a = first_as_of + p·step, newest-first rows,
//  see hybrid_factor_model.hpp for the PIT convention) it asks the factory for the
//  model known at a and scores its forecast of the NEXT period's returns (row a−1):
//
//   * bias statistic  b = sd(z),  z_t = R_p,t / σ̂_p,t   (Connor/Barra; ≈ 1 when
//     calibrated; 95% band 1 ± √(2/T)) for
//       - the equal-weight book,
//       - `n_random` random long/short books (fixed per asset, seeded), and
//       - the MINIMUM-VARIANCE book w ∝ V⁻¹1 and `n_optimized` random-alpha
//         OPTIMIZED books w ∝ V⁻¹α (the optimizer-bias test: an optimizer loads on the
//         directions the model underestimates, so a model with estimation error shows
//         b > 1 here even when random books look fine — Menchero-Wang-Orr), and
//       - caller-supplied named books (e.g. factor-mimicking or the live alpha book);
//   * Q-statistic     Q = mean(z² − ln z²)  (Patton's QLIKE-style loss; minimised by
//     σ̂ = σ, E[Q] = 1 + ln 2 + γ ≈ 2.27 for Gaussian returns, lower is better);
//   * asset-level     per-name bias b_i over its forecasts; reported as the mean and
//     the mean rolling absolute deviation MRAD = mean |b_i − 1|.
//
//  The scorecard renders to deterministic JSON (fixed key order, %.10g numbers,
//  escaped strings).
//  A model asset whose next-period return is NaN (e.g. it left the universe at row
//  a−1) is DROPPED from that period's books. Each book's remaining weights are
//  renormalized: sum 1 for equal-weight and MinVar, unit gross for the others. This
//  way no name adds predicted variance without also adding realized P&L. The mean
//  number of dropped names per period is reported (mean_excluded). Such names are
//  also skipped for the asset-level statistic.

#include <functional>  // std::function
#include <string>      // std::string
#include <string_view> // std::string_view
#include <vector>      // std::vector

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, u64, usize

#include "atx/engine/risk/factor_model.hpp"        // FactorModel
#include "atx/engine/risk/hybrid_factor_model.hpp" // ReturnPanel, ExposureSeries, HybridCfg

namespace atx::engine::risk {

// A model plus the ReturnPanel column of each of its rows.
struct ModelSnapshot {
  FactorModel model;
  std::vector<atx::usize> assets;
};

// Returns the model known at as_of row `a` (it must read no return row < a).
using RiskModelFactory = std::function<atx::core::Result<ModelSnapshot>(atx::usize as_of)>;

// Factory over HybridFactorModelBuilder. `ret` / `exp` are captured BY REFERENCE and
// must outlive the returned factory.
[[nodiscard]] RiskModelFactory hybrid_model_factory(const ReturnPanel &ret,
                                                    const ExposureSeries &exp,
                                                    const HybridCfg &cfg);

// A caller-supplied test book: weights over the ReturnPanel columns (restricted to
// each model's assets and rescaled to unit gross per period).
struct NamedBook {
  std::string name;
  std::vector<atx::f64> w;
};

struct ValidationCfg {
  std::string label = "model";
  atx::usize first_as_of = 1U; // newest as_of tested (forecasts row first_as_of − 1)
  atx::usize n_periods = 60U;  // forecast dates
  atx::usize step = 1U;        // as_of stride between forecasts
  atx::usize n_random = 50U;   // random long/short books
  atx::usize n_optimized = 20U; // random-alpha optimized books w ∝ V⁻¹α
  std::vector<NamedBook> books; // extra caller books (length == ret.n_assets() each)
  atx::u64 seed = 7U;          // random-book seed (determinism)
  atx::usize min_asset_obs = 10U; // forecasts a name needs for the asset-level stat
};

struct BiasStat {
  std::string name;
  atx::usize n = 0U;
  atx::f64 bias = 0.0;          // sd(z)
  atx::f64 q = 0.0;             // mean(z² − ln z²)
  atx::f64 mean_pred_vol = 0.0; // mean σ̂
  atx::f64 realized_vol = 0.0;  // sd(R)
  bool in_band = false;         // |bias − 1| <= √(2/n)
};

struct ValidationScorecard {
  std::string label;
  atx::usize n_periods = 0U; // forecasts actually scored
  atx::f64 band_lo = 0.0;    // 1 − √(2/n_periods)
  atx::f64 band_hi = 0.0;    // 1 + √(2/n_periods)
  BiasStat equal_weight;
  BiasStat min_variance;
  atx::f64 random_bias_mean = 0.0;
  atx::f64 random_in_band_frac = 0.0;
  atx::f64 random_q_mean = 0.0;
  atx::f64 optimized_bias_mean = 0.0;
  atx::f64 optimized_in_band_frac = 0.0;
  std::vector<BiasStat> books; // one per ValidationCfg::books entry, same order
  atx::usize n_assets_scored = 0U;
  atx::f64 asset_bias_mean = 0.0;
  atx::f64 asset_mrad = 0.0;
  atx::f64 asset_in_band_frac = 0.0;
  atx::f64 mean_factors = 0.0; // mean K of the forecasting models
  atx::f64 mean_excluded = 0.0; // mean model names per period dropped (NaN return at a−1)

  [[nodiscard]] std::string to_json() const;
};

// `s` as a JSON string literal (quoted; backslash, quote and control characters
// escaped, other bytes passed through as UTF-8).
[[nodiscard]] std::string json_quote(std::string_view s);

// Walk-forward validation. Err when cfg.first_as_of == 0, n_periods == 0, step == 0,
// the last as_of runs past the panel, a book's length != ret.n_assets(), or the
// factory fails (its error propagates).
[[nodiscard]] atx::core::Result<ValidationScorecard>
validate_risk_model(const RiskModelFactory &factory, const ReturnPanel &ret,
                    const ValidationCfg &cfg);

// Explicit V2 protocol; old validate_risk_model remains the exact daily recipe.
// Fixed forecast-date holdings; realized P&L is the sum of 21 daily arithmetic
// returns, predicted variance is 21 * daily long-run variance. This is NOT a
// compounded buy-and-hold forecast. Missing nonzero holdings invalidate a book
// observation; holdings are never renormalized using realized availability.
struct Validation21Cfg {
  std::string label{"model"};
  atx::usize first_as_of{21}, n_periods{20}, step{21};
  atx::usize n_min_variance{100}, n_optimized{20};
  atx::u64 seed{7}, max_working_bytes{268'435'456};
  std::vector<NamedBook> books;
};
struct ValidationMetric21 {
  std::string name, cohort;
  atx::usize observations{}, unavailable{}, zero_realizations{};
  atx::f64 bias{}, mrad{}, qlike{}, mean_pred_vol{}, realized_vol{};
  bool defined{false};
};
struct ValidationScorecard21 {
  std::string label;
  atx::usize forecast_dates{}, unverified_vra_dates{};
  bool overlapping{false};
  // Bands are deliberately absent: overlap, optimized selection and pooled
  // decile residuals do not supply independent Gaussian calibration evidence.
  std::vector<ValidationMetric21> metrics;
  [[nodiscard]] std::string to_json() const;
};
[[nodiscard]] atx::core::Result<ValidationScorecard21> validate_risk_model_21d(
    const RiskModelFactory& factory, const ReturnPanel& ret, const Validation21Cfg& cfg);

} // namespace atx::engine::risk
