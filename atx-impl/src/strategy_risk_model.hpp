#pragma once

// atx-risk-v1 (platform v7 lane L4; literature-v7 R3.1, R3.3): a daily fundamental equity
// risk model on a research role plus its bias harness.
//
// Model (Barra USE4S recipe, Menchero-Orr-Wang 2011; parameters are the literature values,
// never fitted on returns):
//  - Exposures X_t known at the close of session t: market (1), one FF49 industry slot
//    (industries with fewer than min_industry_names regression names on the date, and
//    unknown ids, pool into the residual slot 49), and 11 styles z-scored cross-sectionally
//    (cap-weighted mean 0, equal-weighted SD 1, winsorized at 5 SD, clipped at 3 SD, missing
//    = 0): size, beta, residual_vol, momentum, value, earnings_yield, profitability,
//    asset_growth, leverage, liquidity, short_interest.
//  - Factor returns f_t: WLS of the session-t simple returns on X_{t-1}, weights sqrt(cap),
//    subject to sum_k cap_k f_k = 0 over the active industries (the market is the cap-weighted
//    industry mix).
//  - Factor covariance forecast at t: EWMA variances (half-life 84) times the Newey-West
//    ratio (lags 5, half-life 252), EWMA correlations (half-life 504, NW lags 2); the same
//    estimator as atx-engine risk V2 (cov_ewma.cpp: ewma_variance_v2 / covariance()), kept
//    recursively (O(K^2) per session, tested equal to the engine); PSD by eigenvalue floor;
//    volatility regime adjustment lambda^2 = EWMA_42 of the factor cross-sectional bias
//    B_t^2 = mean_k (f_kt / sigma_k,t-1)^2 on the prior (pre-VRA) forecasts of the fully
//    observed factors (>= min_factor_history returns).
//  - Structural factor forecasts (platform v7 F2; R3.1 structural fallback, USE4 practice): a
//    factor with n < min_factor_history returns that has a history (n >= 1) or an exposed name
//    at t gets variance w own + (1 - w) prior with w = n / min_factor_history, own = its own
//    EWMA + NW variance (clamped at 0), prior = its class's mean forecast variance at t over
//    the fully observed factors (industries: weighted by the industry's eligible cap at t, equal
//    weights when none holds cap; styles: equal weights; the market has no class and is never
//    short of history while another factor has it). Its correlation with a fully observed
//    factor is w rho, with another structural factor w w' rho (rho = the correlation
//    estimator's value, 0 without joint history). The fully observed block is never altered
//    (bit-identical to the model without the fallback) and structural forecasts stay out of the
//    VRA; if the bordered matrix is not positive definite, every structural off-diagonal term is
//    scaled by the largest s = 2^-j, j = 0..30 (else 0), that makes it so.
//  - Specific variance: EWMA-84 residual variance with NW lags 5 (engine specific_risk_v2's
//    time-series step), blended with a structural ln-vol model (the same constrained WLS of
//    ln sigma on X_t over full-history names, exponentiation-corrected) by gamma =
//    min(1, h/252) with h the residual observations in the last 252 sessions, Bayesian-shrunk
//    to the cap-weighted size-decile mean (q = 0.1, engine formula), specific VRA (half-life
//    42, cap-weighted bias on prior forecasts).
// Timing: every quantity at t reads rows <= t only.

#include <array>
#include <iosfwd>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy::risk {

inline constexpr atx::usize style_count = 11;
inline constexpr std::array<const char*, style_count> style_names{
    "size", "beta", "residual_vol", "momentum", "value", "earnings_yield", "profitability",
    "asset_growth", "leverage", "liquidity", "short_interest"};
enum class Style : atx::u8 {
  Size = 0, Beta, ResidualVol, Momentum, Value, EarningsYield, Profitability, AssetGrowth,
  Leverage, Liquidity, ShortInterest
};
inline constexpr atx::usize industry_slots = 50;     // FF49 1..49 -> 0..48, residual 49
inline constexpr atx::usize residual_industry = 49;
inline constexpr atx::usize factor_count = 1 + industry_slots + style_count; // 62
inline constexpr atx::u8 no_exposure = 255;
[[nodiscard]] constexpr atx::usize industry_factor(atx::usize slot) noexcept { return 1 + slot; }
[[nodiscard]] constexpr atx::usize style_factor(atx::usize style) noexcept {
  return 1 + industry_slots + style;
}
[[nodiscard]] std::string factor_name(atx::usize k);

struct RiskModelConfig {
  // Factor covariance (USE4S Table 4.1): vol HL 84 + NW 5, correlation HL 504 + NW 2, NW
  // weighting HL 252 (engine RiskEstimatorPolicy), VRA HL 42.
  atx::usize vol_halflife{84}, correlation_halflife{504}, variance_nw_lags{5};
  atx::usize correlation_nw_lags{2}, nw_halflife{252}, vra_halflife{42}, vra_min_dates{21};
  atx::usize min_factor_history{63}; // observed factor returns before a factor's own forecast
  // Structural forecasts for factors with fewer returns (F2). false = the pre-F2 model (such
  // factors unforecast); kept to prove the fully observed block identical.
  bool structural_factor_forecast{true};
  // Specific risk: EWMA HL 84, NW 5, structural blend under 252 sessions, Bayesian q 0.1.
  atx::usize specific_halflife{84}, specific_nw_lags{5}, structural_history{252};
  atx::f64 bayesian_q{0.1};
  // Exposures.
  atx::usize min_industry_names{10};
  atx::usize beta_window{252}, min_beta_pairs{126}, momentum_window{252}, momentum_skip{21};
  atx::usize adv_window{63};
  atx::f64 winsor_sd{5.0}, clip_z{3.0};
  atx::usize min_regression_names{100}; // rows per cross-sectional fit (> 62 parameters)
};
[[nodiscard]] atx::core::Status validate_config(const RiskModelConfig& cfg);

// ---- cross-sectional regression kernel ----------------------------------------------------
// Rows: industry slot (< industry_slots), style z (row-major rows x style_count), WLS weight
// > 0, constraint cap >= 0, regressand. Parameters [market, industries with >= 1 row, styles
// with nonzero weighted mass]; minimize sum_r w_r (y_r - x_r f)^2 subject to
// sum_active cap_k f_k = 0 (cap_k = sum of the rows' cap in industry k), solved as a KKT
// system (full-pivot LU). Inactive factors are NaN in `factor` and contribute 0 to residuals.
struct CrossSection {
  std::span<const atx::u8> industry;
  std::span<const atx::f64> styles, weight, cap, value;
};
struct CrossSectionFit {
  std::array<atx::f64, factor_count> factor{};
  std::vector<atx::f64> residual; // per row
  atx::f64 r2{};
  atx::usize rows{}, active_industries{};
};
// InvalidArgument on geometry/values; Unavailable when rows <= parameters or the system is
// singular (collinear styles).
[[nodiscard]] atx::core::Result<CrossSectionFit> regress_cross_section(const CrossSection& rows);

// ---- recursive EWMA / Newey-West moments --------------------------------------------------
// Recursive twin of atx-engine risk V2 covariance(a, b, ages, H, L) (cov_ewma.cpp) for a
// series that grows by one newest row per push (contiguous ages; NaN/inf = missing): row age
// a weighs 2^(-a/H) (H = 0: equal weights); means are the columns' own EWMA means; the zero-lag
// term sums over rows where both columns are finite, divided by that mass; each lag l <= L adds
// (1 - l/(L+1)) (ab_l + ba_l) / mass with the pair weighted by the older row's age.
// full: every ordered pair (i, j); else only (i, i). Memory O(K^2 (4 + 4L)) resp. O(K (4 + 4L)).
class LaggedEwma {
public:
  LaggedEwma(atx::usize columns, atx::usize half_life, atx::usize lags, bool full);
  void push(std::span<const atx::f64> row);
  [[nodiscard]] atx::usize columns() const noexcept { return k_; }
  [[nodiscard]] atx::usize observations(atx::usize i) const noexcept { return count_[i]; }
  [[nodiscard]] atx::f64 mean(atx::usize i) const noexcept;
  // Zero-lag EWMA covariance; NaN without joint mass. (i, j) must be tracked (i == j if !full).
  [[nodiscard]] atx::f64 zero_lag(atx::usize i, atx::usize j) const noexcept;
  // Zero-lag plus the Newey-West lag terms (engine covariance()); NaN without joint mass.
  [[nodiscard]] atx::f64 covariance(atx::usize i, atx::usize j) const noexcept;

private:
  [[nodiscard]] atx::usize pair(atx::usize i, atx::usize j) const noexcept {
    return full_ ? i * k_ + j : i;
  }
  atx::usize k_{}, lags_{}, pushed_{};
  bool full_{};
  atx::f64 decay_{1.0};
  std::vector<atx::usize> count_;
  std::vector<atx::f64> s0_, s1_;             // per column: sum w, sum w a
  std::vector<atx::f64> w0_, p0_, a0_, b0_;   // per pair, zero lag
  std::vector<atx::f64> wl_, ql_, al_, bl_;   // per (lag, pair), pair = (older col, newer col)
  std::vector<atx::f64> ring_;                // the last `lags` rows, k each
};
// The engine V2 variance (ewma_variance_v2): fast (HL vol, lag 0) x max(0, hac / base) with
// base/hac the zero-lag/NW moments at the NW half-life; ratio 1 when base <= 0.
[[nodiscard]] atx::f64 serial_adjusted_variance(const LaggedEwma& fast, const LaggedEwma& nw,
                                                atx::usize i) noexcept;

// ---- bias statistics (USE4 Appendix A) ----------------------------------------------------
struct Band {
  atx::f64 lo{}, hi{};
};
// Sample SD of the finite values; NaN below 2.
[[nodiscard]] atx::f64 bias_statistic(std::span<const atx::f64> z) noexcept;
// m4 / m2^2 of the finite values (3 for a normal); NaN below 4.
[[nodiscard]] atx::f64 sample_kurtosis(std::span<const atx::f64> z) noexcept;
[[nodiscard]] Band bias_band(atx::usize t) noexcept;                       // 1 +- sqrt(2/T)
[[nodiscard]] Band kurtosis_band(atx::usize t, atx::f64 kurtosis) noexcept; // 1 +- 1.96 sqrt((k-1)/(4T))
// b over each trailing window of `window` entries (NaN until the first full window), one value
// per input; a non-finite entry inside a window is skipped by bias_statistic, so that window's b
// uses fewer values. The bias harness only ever records finite z.
[[nodiscard]] std::vector<atx::f64> rolling_bias(std::span<const atx::f64> z, atx::usize window);

// ---- in-memory driver ---------------------------------------------------------------------
// Descriptors (raw, per cell, NaN missing): value (book / cap), earnings yield (ni_ttm / cap),
// profitability (gp_ttm / at), asset growth (at / at_lag4 - 1), leverage (lt / at), SI ratio
// (si_shares / shares_out), days to cover (si_dtc). Empty span = unavailable (style off).
inline constexpr atx::usize descriptor_count = 7;
inline constexpr std::array<const char*, descriptor_count> descriptor_names{
    "book_to_price", "earnings_yield", "gross_profitability", "asset_growth", "leverage",
    "si_ratio", "days_to_cover"};
struct RiskPanel {
  atx::usize dates{}, instruments{};
  std::span<const atx::i64> sessions;
  std::span<const atx::u64> ids;
  std::span<const atx::f64> close, raw_close, volume; // role prices (adjusted, raw), raw shares
  std::span<const atx::u8> present, member;
  std::span<const atx::f64> cap;       // market cap $, NaN missing
  std::span<const atx::u8> industry;   // FF49 id 1..49, 0 unknown; empty: all residual
  std::array<std::span<const atx::f32>, descriptor_count> descriptors{};
};
// Simple adjusted return over (t-1, t]: both endpoints present with finite positive close and
// raw close and not guarded (|log adj| > 1.5 or > |log raw| + .10); NaN otherwise.
[[nodiscard]] atx::f64 interval_return(const RiskPanel& p, atx::usize t, atx::usize i) noexcept;

struct RiskDay {
  atx::usize date{};
  atx::i64 session{};
  bool fitted{}, forecast{};
  atx::usize regression_rows{}, active_industries{}, structural_names{}, specific_names{};
  atx::f64 r2{}, lambda2_factor{1.0}, lambda2_specific{1.0}, bias_factor{}, bias_specific{};
  std::span<const atx::f64> factor_return;     // factor_count, realized at t, NaN inactive
  std::span<const atx::f64> covariance;        // factor_count^2, forecast at t, NaN unforecast
  std::span<const atx::f64> specific_variance; // instruments, forecast at t, NaN none
  std::span<const atx::u8> industry_slot;      // instruments, no_exposure without a row
  std::span<const atx::u8> eligible;           // instruments: member with cap (the universe)
  std::span<const atx::f64> styles;            // instruments x style_count
  // Factor forecast audit at t (F2): factors forecast structurally, the scale s applied to their
  // off-diagonal terms (1 = none needed), factors a present name has a nonzero exposure to but
  // without a forecast, and present names pooled into the residual industry slot.
  atx::usize structural_factors{}, unforecast_exposed_factors{}, residual_names{};
  atx::f64 structural_scale{1.0};
  std::span<const atx::u8> factor_structural;  // factor_count: 1 = structural forecast at t
};
class RiskSink {
public:
  RiskSink() = default;
  RiskSink(const RiskSink&) = default;
  RiskSink& operator=(const RiskSink&) = default;
  RiskSink(RiskSink&&) = default;
  RiskSink& operator=(RiskSink&&) = default;
  virtual ~RiskSink() = default;
  [[nodiscard]] virtual atx::core::Status on_day(const RiskDay& day) = 0;
};
// Runs the model over every session of the panel, calling each sink once per session in order.
[[nodiscard]] atx::core::Status run_risk_model(const RiskPanel& panel, const RiskModelConfig& cfg,
                                               std::span<RiskSink* const> sinks);

// ---- bias harness (R3.3) ------------------------------------------------------------------
// Families: "random" (seeded dollar-neutral gross-1 portfolios over the eligible names, scores
// fixed per instrument id), "factor" (each factor's return over its prior forecast vol) and
// "book" (a supplied weights file). z_t = realized return over (t-1, t] / forecast vol at t-1
// (x' F x + sum w^2 D). A missing return counts 0 and is reported.
// Complete forecasts only (R1 M-5): an observation whose portfolio holds a name without a
// forecast (no exposure row or specific variance) or has a nonzero exposure to a factor without
// a forecast is excluded whole, never priced by a partial x'Fx or realized on a sub-book. With
// structural factor forecasts (F2) a factor lacks one only while its class has no fully
// observed factor (without them: fewer than min_factor_history returns). The factor family
// evaluates structural forecasts like any other. Exclusions before a series' first kept
// observation are its warm-up; later ones are counted (dropped_factor_exposures,
// uncovered_name_returns) and a series whose dropped share exceeds max_dropped_share is refused
// (no b is reported for it).
struct BookWeight {
  atx::i64 session{};
  atx::u64 instrument_id{};
  atx::f64 weight{};
};
struct BiasSeries {
  std::string family, name;
  std::vector<atx::i64> sessions;
  std::vector<atx::f64> forecast_vol, realized, z;
  atx::usize missing_returns{}, uncovered_names{}; // name-sessions (uncovered: book only)
  // Excluded observations (whole portfolio-sessions): before the first kept one (warm-up), then
  // by cause: an exposure to an unforecast factor, or a held name without a forecast.
  atx::usize warmup_excluded{}, dropped_factor_exposures{}, uncovered_name_returns{};
};
inline constexpr atx::f64 max_dropped_share = 0.05;
// (dropped_factor_exposures + uncovered_name_returns) / (those + kept observations); 0 if none.
[[nodiscard]] atx::f64 dropped_share(const BiasSeries& s) noexcept;
enum class SeriesStatus : atx::u8 { Ok = 0, Refused, Empty };
// Empty: no kept observation; Refused: dropped_share > max_dropped_share; else Ok.
[[nodiscard]] SeriesStatus series_status(const BiasSeries& s) noexcept;
[[nodiscard]] const char* series_status_name(SeriesStatus s) noexcept;
class BiasHarness final : public RiskSink {
public:
  // book: optional, any order; weights of sessions absent from the panel are refused by run.
  BiasHarness(const RiskPanel& panel, atx::usize random_portfolios, atx::u64 seed,
              std::span<const BookWeight> book);
  [[nodiscard]] atx::core::Status on_day(const RiskDay& day) override;
  [[nodiscard]] const std::vector<BiasSeries>& series() const noexcept { return series_; }

private:
  enum class Excluded : atx::u8 { None = 0, FactorExposure, UncoveredName };
  struct Held {
    std::vector<std::pair<atx::usize, atx::f64>> weights;
    atx::f64 vol{};
    bool live{};
    Excluded excluded{Excluded::None}; // formed, but without a complete forecast
  };
  struct Forecast {
    atx::f64 vol{};
    Excluded excluded{Excluded::None};
  };
  void realize(const RiskDay& day);
  void form(const RiskDay& day);
  static void apply_forecast(Held& held, Forecast forecast) noexcept;
  static void exclude(BiasSeries& s, Excluded why) noexcept;
  [[nodiscard]] Forecast forecast_vol(const RiskDay& day, const Held& held) const;
  const RiskPanel& panel_;
  atx::usize random_{};
  atx::u64 seed_{};
  std::vector<std::vector<std::pair<atx::usize, atx::f64>>> book_by_date_; // per panel date
  bool has_book_{};
  std::vector<Held> held_;                  // random..., book (last when has_book_)
  std::vector<atx::f64> prior_factor_var_;  // factor_count, VRA-adjusted forecast at t-1
  std::vector<BiasSeries> series_;          // random..., factor..., book
  std::vector<atx::f64> scores_;            // random x instruments
  atx::usize unmatched_book_rows_{};        // book rows outside the role: on_day refuses
};
// Deterministic N(0, 1) score of (seed, portfolio, instrument id) (splitmix64 + Box-Muller).
[[nodiscard]] atx::f64 random_score(atx::u64 seed, atx::usize portfolio, atx::u64 id) noexcept;

// ---- CLI: atx-equity-strategy-risk risk ... -----------------------------------------------
// risk --role PATH/manifest.json --role-sha256 SHA --fields PATH/manifest.json --fields-sha256
// SHA --output NEWDIR [--book-weights CSV --book-weights-sha256 SHA] [--random-portfolios 64]
// [--seed 7] [--emit-exposures none|last|all] [--max-bytes 1400000000]
[[nodiscard]] int dispatch_risk_model(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy::risk
