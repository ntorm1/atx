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
#include <cstdint>
#include <vector>

#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs
#include "atx/vol/types.hpp"          // Result
#include "atx/vol/universe.hpp"       // Underlying
#include "atx/vol/vol_curve.hpp"      // CurveConfig, VolCurveKind

namespace atx::vol {

namespace detail {
// Per-kind butterfly violation count for a fitted slice — the exact
// selection-time mapping `select_curve` applies to every candidate's fitted
// slice (closed-form Martini-Mingone for raw-SVI, grid Durrleman g-check for
// C8, the fitted diagnostic count carried on the params for SplineVol, and 0
// for the by-construction / out-of-scope kinds). Defined in curve_selector.cpp
// and exposed here (rather than staying an anonymous-namespace helper) purely
// as a unit-test seam — production code should go through `select_curve`,
// never call this directly. `k_lo`/`k_hi` bound the C8 grid (padded by the
// caller).
[[nodiscard]] std::uint32_t slice_butterfly_violations(const IVolCurve &cv, double T, double k_lo,
                                                        double k_hi) noexcept;
}  // namespace detail

// One candidate's out-of-sample score.
struct CandidateScore {
  VolCurveKind kind{VolCurveKind::ConvexDense};
  double oos_in_band{0.0}; // held-out price-in-band fraction (0..1)
  double oos_vw{0.0};      // held-out vega^2-weighted in-band fraction (0..1)
  double vega_weight_in_band{0.0};
  double vega_weight_total{0.0};
  std::size_t dof_sum{0};   // summed effective DoF over scored slices (parsimony)
  std::size_t n_holdout{0}; // held-out strikes scored
  std::size_t n_in_band{0}; // held-out strikes priced inside their bid/ask
  std::size_t n_slices{0};  // expiries that produced a scorable fit

  // ── Task C2.5: fit-metrics selection signal ──
  // Held-out slice metrics (Vola-style; see fit_metrics.hpp), aggregated over
  // every scored held-out observation for this family. `chi2_reduced` is the
  // secondary tie-break (after oos_vw, before parsimony DoF). `metrics_valid`
  // is false when the held-out sample was too thin for a reduced chi-square
  // (N <= dof), in which case chi2_reduced does not participate in the tie-break.
  double chi2_reduced{0.0};          // reduced chi^2 over held-out obs
  double rmse_vol{0.0};              // held-out vol RMSE
  double avE5_vol{0.0};              // held-out mean|resid|*1e5
  std::size_t n_within_band{0};      // held-out obs inside their 1σ error bar
  bool metrics_valid{false};         // slice_fit_metrics succeeded
  // Butterfly no-arb disqualification: a family whose fitted slice fails its
  // butterfly check (closed-form MM for raw-SVI, grid g-check for C8;
  // by-construction kinds are skipped) is DISQUALIFIED and scores as a
  // fit-failure — excluded from the winner search.
  std::uint32_t n_butterfly_viol{0};
  bool disqualified{false};
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

// Pure winner-selection policy over already-scored candidates (Task C2.5). The
// ordering is: (1) best out-of-sample vega-weighted in-band `oos_vw`; within
// the `parsimony_margin` tie band, (2) reduced chi-square closest to 1 — i.e.
// smallest |chi2_reduced − 1| — among candidates whose metrics are valid; then
// (3) fewer average degrees of freedom (parsimony); then (4) higher `oos_vw`.
// Candidates that failed to fit (`n_holdout == 0`) or were butterfly-
// DISQUALIFIED are excluded. Returns the winning index into `scores` (0 when no
// candidate is scorable — callers gate on that upstream). Deterministic; pure.
[[nodiscard]] std::size_t
select_best_candidate(const std::vector<CandidateScore> &scores,
                      double parsimony_margin) noexcept;

// Choose the best curve config for `under` under `in` (market/carry policy).
//
// @return InvalidArgument if S <= 0 / r non-finite / no candidates; NotFound if
//         no candidate produced a scorable fit on any expiry; otherwise the
//         selection (with per-candidate diagnostics).
[[nodiscard]] Result<SelectorResult> select_curve(const Underlying &under,
                                                  const SurfaceParityInputs &in,
                                                  const SelectorConfig &sel = {});

// Score one already-selected family on the selector's deterministic even/odd
// holdout. This performs no all-family search and is the quality path for direct
// policy routes and successful fallback rungs.
[[nodiscard]] Result<CandidateScore> score_curve_oos(const Underlying &under,
                                                     const SurfaceParityInputs &in,
                                                     const CurveConfig &curve,
                                                     const SelectorConfig &scoring = {});

} // namespace atx::vol
