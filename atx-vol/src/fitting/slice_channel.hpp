#pragma once

// Channel-constrained knot spline for ONE expiry (pure numerical core).
//
// SpiderRock-style per-expiry curve: a natural cubic spline of volatility on the
// fixed 29-point standardized-moneyness grid (`kSrMoneynessGrid`,
// z = ln(K/F) / (sigma_ref * sqrt(T))), flat beyond the outermost active knot,
// fitted INSIDE each contract's bid/ask volatility channel rather than to mids.
// The caller converts bid and ask to volatilities with the same pricer and carry
// it uses for the mid, so the channel is de-Americanized consistently.
//
// Objective in the knot volatilities v (convex, piecewise quadratic):
//
//   mid_weight     * sum_i mid_scale_i * ((sigma(z_i) - sigma_mid_i) / h_i)^2
// + channel_weight * sum_i ((lo_i - sigma(z_i))_+^2 + (sigma(z_i) - hi_i)_+^2) / u^2
// + strength       * sum_j ((v_j - v_prior_j) / (prior_scale * v_prior_j))^2
// + roughness      * sum_j (second difference of v_j / sigma_ref)^2
//
// with h_i the channel half-width in volatility (floored at u = min_half_width).
// The mid anchor is spread-normalised; the channel term is measured in the
// fixed unit u, so it is equally steep for tight, wide and bidless channels
// (fit inside the channel, report error against the mid). Bidless contracts
// have lo = 0 (an upper bound only); an ask above the maximum-volatility price
// has hi = +inf. The prior is a term-structure prior (neighbouring expiries) or,
// when none is supplied, a flat default shape at the data-seeded ATM level.
//
// Solver: semismooth active-set Newton on the hinge terms with backtracking on
// the exact objective, at most `max_iterations` linear solves of size <= 29,
// followed by one re-standardization pass at the fitted ATM volatility (the same
// two-solve structure as `fit_spline_vol_slice`). The knot system uses
// fixed-capacity storage; the only heap use is O(observations x knots) basis
// scratch on this cold fitting path. Pure function; thread-safe.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/spline_curve.hpp" // SplineVolParams, kSrMoneynessGrid

namespace atx::vol {

struct ChannelObservation {
  double k{};             // ln(K / F_reference)
  double sigma_lo{};      // >= 0 (0 = no lower information)
  double sigma_hi{std::numeric_limits<double>::infinity()}; // > sigma_lo, +inf allowed
  double sigma_mid{std::numeric_limits<double>::quiet_NaN()}; // optional mid IV
  double mid_scale{1.0};  // >= 0 multiplier on this contract's mid term
};

struct ChannelPrior {
  double atm_vol{};                  // > 0
  std::span<const double> z;         // increasing standardized grid (on atm_vol)
  std::span<const double> multiples; // sigma / atm_vol on z, > 0
  double strength{};                 // >= 0 effective prior weight
};

struct ChannelFitConfig {
  double channel_weight{100.0};
  double mid_weight{1.0};
  double default_shape_weight{0.05}; // flat-shape strength when no prior is supplied
  double prior_scale{0.10};          // relative volatility unit of the prior penalty
  double roughness{0.02};
  double min_half_width{0.0025};     // volatility floor on h_i
  double mult_floor{0.05};           // served multiple floor (relative to sigma_ref)
  double mult_ceil{3.0};             // served multiple ceiling
  std::uint16_t max_iterations{50};
  std::size_t min_observations{2};
};

struct ChannelFitDiagnostics {
  std::size_t observations{};      // usable channel observations
  std::size_t mid_observations{};  // of which carry a mid volatility
  std::size_t knots{};
  std::size_t iterations{};        // total Newton iterations over both passes
  std::size_t violations{};        // observations outside their channel at the solution
  double max_violation_half_widths{};
  double objective{};
  double prior_strength{};         // strength actually applied
  bool used_term_prior{false};
  bool converged{false};
};

struct ChannelFit {
  // params.n_butterfly_viol is NOT computed here (left 0, meaning "not
  // scanned"); served prices are certified downstream by AmericanPriceSlice.
  SplineVolParams params;
  ChannelFitDiagnostics diagnostics;
};

// @return InvalidArgument for a malformed configuration, prior or variance time;
//         Unavailable when fewer than `min_observations` usable observations
//         remain or no ATM level can be seeded; Internal when the knot system is
//         not positive definite. Otherwise the fitted parameters.
[[nodiscard]] Result<ChannelFit> fit_slice_channel(std::span<const ChannelObservation> observations,
                                                   double variance_years,
                                                   const ChannelPrior *prior,
                                                   const ChannelFitConfig &config = {});

// ── The same objective solved by an arbitrary point-curve family ─────────────
//
// For families that can only be fitted to weighted point targets (eSSVI, SVI,
// C8, convex-dense, the point spline). Bounded active-set reweighting: each
// step fits the family to
//   * every contract's mid anchor (weight mid_weight * mid_scale / h^2), merged
//     with its bound (weight channel_weight / u^2) while the previous step's
//     curve lies outside that side of the channel, and
//   * the prior as point targets at the knots the knot spline would use
//     (weight strength / (prior_scale * prior)^2),
// then the active set is recomputed from the fitted curve. Roughness is left to
// the family's own regularisation, and the prior grid is standardized once at
// the seeded (or prior) ATM volatility. The step with the lowest exact data +
// prior objective is the solution; iteration stops when the active set repeats.
// APPROXIMATION: the family's own robust loss, filters and weight floors act on
// these targets, so the minimised objective is the family's, not exactly this.
struct ChannelTarget {
  double k{};      // ln(K / F_reference); targets are sorted by k, distinct
  double sigma{};  // target volatility, > 0
  double weight{}; // objective weight in 1 / vol^2
  double unit{};   // the target's own volatility unit: 2 h, or the prior unit
};

// Fits the family to `targets` and writes its volatility at each `k_eval` into
// `sigma` (same length). `sigma_ref` is the ATM reference of the prior grid.
using ChannelFamilyStep =
    std::function<Status(double sigma_ref, std::span<const ChannelTarget> targets,
                         std::span<const double> k_eval, std::span<double> sigma)>;

struct ChannelFamilyFit {
  std::size_t step{};     // 0-based index, among successful steps, of the solution
  double sigma_ref{};
  ChannelFitDiagnostics diagnostics; // iterations = successful steps
};

// @return as fit_slice_channel; additionally InvalidArgument for zero
//         `max_steps` or an empty step, and the first step's error when the
//         family cannot fit at all. A later failing step ends the iteration.
[[nodiscard]] Result<ChannelFamilyFit>
fit_slice_channel_family(std::span<const ChannelObservation> observations, double variance_years,
                         const ChannelPrior *prior, const ChannelFitConfig &config,
                         std::uint16_t max_steps, const ChannelFamilyStep &step);

} // namespace atx::vol
