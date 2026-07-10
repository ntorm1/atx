#pragma once

// CurveSelector — pick the best curve family + config for a board when the caller
// supplies no explicit config.
//
// Different underlyings want different curves. A penny-dense index board (SPY)
// is best fit by the arb-free convex DENSE curve (near-interpolating, 99.5%
// in-band). A sparse, wide-spread single-name board (XOM) has too few, too noisy
// quotes to pin a dense curve — it OVERFITS quote noise and generalizes worse than
// a parsimonious 3-parameter eSSVI backbone.
//
// The principled, symbol-agnostic way to choose is OUT-OF-SAMPLE generalization,
// not in-sample error (which always rewards more degrees of freedom). This
// selector scores each candidate by LEAVE-EVERY-OTHER-STRIKE-OUT held-out
// price-in-band (the honest metric from spy_oos_check): fit on half the strikes,
// score the strikes the fit never saw. The candidate with the best held-out
// in-band wins; a near-tie breaks toward FEWER degrees of freedom (parsimony), so
// a dense curve only wins when its extra flexibility genuinely pays off out of
// sample. On SPY this selects ConvexDense; on XOM it prefers eSSVI.
//
// Stateless / pure: safe to call concurrently on distinct underlyings.

#include <cstddef>
#include <vector>

#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs
#include "atx/vol/types.hpp"          // Result
#include "atx/vol/universe.hpp"       // Underlying
#include "atx/vol/vol_curve.hpp"      // CurveConfig, VolCurveKind

namespace atx::vol {

// One candidate's out-of-sample score.
struct CandidateScore {
  VolCurveKind kind{VolCurveKind::ConvexDense};
  double oos_in_band{0.0};  // held-out price-in-band fraction (0..1)
  double oos_vw{0.0};       // held-out vega^2-weighted in-band fraction (0..1)
  std::size_t dof_sum{0};   // summed effective DoF over scored slices (parsimony)
  std::size_t n_holdout{0}; // held-out strikes scored
  std::size_t n_slices{0};  // expiries that produced a scorable fit
};

// The selection outcome. `chosen` is ready to drop into
// `SessionInputs::curve` / `PricerConfig::curve`.
struct SelectorResult {
  CurveConfig chosen{};
  std::size_t chosen_index{0};        // index into the candidate list
  std::vector<CandidateScore> scores; // per candidate (‖ candidate list)
};

// Search policy.
struct SelectorConfig {
  // Candidate configs to try. Empty => `default_selector_candidates()`.
  std::vector<CurveConfig> candidates{};
  // Cap on expiries scored (subsample for speed; liquid near-money expiries are
  // preferred). 0 => score all.
  unsigned oos_max_expiries{8};
  // Out-of-sample ties within this vega-weighted margin break toward fewer DoF.
  double parsimony_margin{0.004};
};

// Default family ladder: convex dense, cache-friendly linear variance, eSSVI,
// raw SVI, and event-capable C8. High-confidence boards bypass this expensive
// validation; it is the ambiguity fallback and explicit research mode.
[[nodiscard]] std::vector<CurveConfig> default_selector_candidates();

// Choose the best curve config for `under` under `in` (market/carry policy).
//
// @return InvalidArgument if S <= 0 / r non-finite / no candidates; NotFound if
//         no candidate produced a scorable fit on any expiry; otherwise the
//         selection (with per-candidate diagnostics).
[[nodiscard]] Result<SelectorResult> select_curve(const Underlying &under,
                                                  const SurfaceParityInputs &in,
                                                  const SelectorConfig &sel = {});

} // namespace atx::vol
