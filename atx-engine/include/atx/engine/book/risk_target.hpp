#pragma once

// atx::engine::book -- ex-ante risk target (platform v8 R-8, rule risk-target-v1): the leverage
// a book's aim carries so that its ex-ante volatility, raised by a declared bias, meets a target.
//
//   L_t = clip(sigma_star / (b x sigma_hat_t), lo x L, hi x L),     lo = .8, hi = 1.25
//
//   sigma_star   the target volatility, annualised (the registration: .05)
//   b            the declared bias of the ex-ante volatility, realised / ex-ante (registered 1.15)
//   L            the book's base leverage (the NAV replay's --aim-leverage)
//   sigma_hat_t  sqrt(P x w' Sigma w), w the book's current weights scaled to gross 1 over the
//                whole book (w = c / sum_i |c_i|), Sigma = B F B' + diag(d) a one-period
//                factor model of the names it prices, P = 252 periods per year
//
// Cadence: L_t is estimated at the book's first estimable decision (gross above 0 and
// w' Sigma w > 0) and again at the first estimable decision at least `cadence` sessions
// (registered 21) after the previous estimate; between estimates it holds. Before the first
// estimate it is L. A due decision that cannot be estimated leaves the clock where it was, so
// the next decision is due as well.
//
// B, the exposure matrix, has the layout of the target-tracking problem (target_tracking.hpp):
// column 0 an intercept every name loads 1 on, columns 1..groups one-hot groups (a name loads 1
// on at most one), then `styles` dense columns. The variance is accumulated in name order as
// e' F e + sum_i d_i w_i^2 with e = B'w, the arithmetic of the NAV replay's book_variance
// (atx-impl strategy_spo.cpp) over the names it prices. Names the caller's model does not price
// (no forecast) enter only the gross that scales the book: they add no variance.
//
// Deterministic: no RNG, clock or map; same inputs, same outputs. Allocation: one K-vector per
// factor_variance call and one n-vector per gross_one_vol call (an estimate, not every decision).

#include <limits>
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::book {

// ---- registered constants (risk-target-v1) ------------------------------------------------
inline constexpr atx::f64 risk_target_clip_lo = 0.8;          // L_t >= .8 L
inline constexpr atx::f64 risk_target_clip_hi = 1.25;         // L_t <= 1.25 L
inline constexpr atx::f64 risk_target_default_bias = 1.15;    // b (a flag may move it)
inline constexpr atx::usize risk_target_default_cadence = 21; // sessions (a flag may move it)
inline constexpr atx::f64 risk_target_periods_per_year = 252.0;

struct RiskTargetParams {
  atx::f64 sigma_star{};                                // annualised target, in (0, 1]
  atx::f64 bias{risk_target_default_bias};              // b, in (0, 10]
  atx::usize cadence{risk_target_default_cadence};      // sessions, in [1, 10000]
};
// The ranges above; InvalidArgument naming the first parameter outside its range.
[[nodiscard]] atx::core::Status validate_risk_target(const RiskTargetParams& p);

// A one-period factor model over n names (spans borrowed for the call).
struct FactorRiskView {
  atx::usize groups{}, styles{};
  std::span<const atx::u32> group;      // per name: group column in [1, groups], 0 = none
  std::span<const atx::f64> exposures;  // n x styles, row-major
  std::span<const atx::f64> covariance; // F: K x K row-major, K = 1 + groups + styles
  std::span<const atx::f64> specific;   // d_i, finite >= 0
  [[nodiscard]] atx::usize factors() const noexcept { return 1 + groups + styles; }
};

// w' (B F B' + diag(d)) w, per period, accumulated in name order as described above (names with
// w_i == 0 skipped). InvalidArgument: w, group or specific not n = specific.size() long,
// exposures not n x styles, covariance not K x K, a group above `groups`, or a non-finite
// weight, exposure, covariance or specific entry, or a negative specific variance.
[[nodiscard]] atx::core::Result<atx::f64> factor_variance(const FactorRiskView& m,
                                                          std::span<const atx::f64> w);

// sigma_hat of risk-target-v1: sqrt(P x factor_variance(m, w / gross)), the annualised ex-ante
// volatility of the book scaled to gross 1. `w` holds the weights of the names the model prices;
// `gross` is the whole book's sum |c_i| (the unpriced names included). Unavailable when gross
// is 0 (a flat book has no shape) or the variance is not positive (nothing to scale);
// InvalidArgument when gross or P is not finite and positive, or gross is below sum_i |w_i|
// (beyond rounding), or factor_variance refuses the model.
[[nodiscard]] atx::core::Result<atx::f64>
gross_one_vol(const FactorRiskView& m, std::span<const atx::f64> w, atx::f64 gross,
              atx::f64 periods_per_year = risk_target_periods_per_year);

enum class RiskTargetClip : atx::i8 { Low = -1, None = 0, High = 1 };
// One estimate's leverage: raw = sigma_star / (b sigma_hat), leverage = clip(raw, lo L, hi L)
// (raw below lo L: Low; above hi L: High; the bounds themselves are inside).
struct RiskTargetLeverage {
  atx::f64 raw{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 leverage{std::numeric_limits<atx::f64>::quiet_NaN()};
  RiskTargetClip clip{RiskTargetClip::None};
};
// Precondition: sigma_hat and base finite and > 0 (risk_target_update checks both).
[[nodiscard]] RiskTargetLeverage risk_target_leverage(const RiskTargetParams& p,
                                                      atx::f64 sigma_hat,
                                                      atx::f64 base) noexcept;

// One book's state across decisions (sessions are the caller's decision indexes, ascending).
struct RiskTargetState {
  bool estimated{};
  atx::usize last{};  // session of the latest estimate
  atx::f64 sigma_hat{std::numeric_limits<atx::f64>::quiet_NaN()}; // of the latest estimate
  RiskTargetLeverage at{};                                          // of the latest estimate
};
// No estimate yet, or session at least `cadence` after the latest one.
[[nodiscard]] bool risk_target_due(const RiskTargetState& s, atx::usize session,
                                   atx::usize cadence) noexcept;
// L_t in force: the latest estimate's leverage, else base (before the first estimate).
[[nodiscard]] atx::f64 risk_target_in_force(const RiskTargetState& s, atx::f64 base) noexcept;

// One decision of one book. When due (risk_target_due) and the book can be estimated
// (gross_one_vol), records the estimate at `session` and returns true; otherwise returns false
// with the state untouched (not due, a flat book, or no positive variance). InvalidArgument,
// state untouched: invalid parameters, base not finite and positive, or gross_one_vol's.
[[nodiscard]] atx::core::Result<bool>
risk_target_update(const RiskTargetParams& p, atx::f64 base, atx::usize session,
                   const FactorRiskView& m, std::span<const atx::f64> w, atx::f64 gross,
                   RiskTargetState& s);

} // namespace atx::engine::book
