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
#include <span>
#include <vector>

#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs
#include "atx/vol/types.hpp"          // Result
#include "atx/vol/universe.hpp"       // Underlying
#include "atx/vol/vol_curve.hpp"      // CurveConfig, VolCurveKind

namespace atx::vol {

struct FitAdmissionPolicy;

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
} // namespace detail

// One candidate's out-of-sample score.
struct CandidateScore {
  VolCurveKind kind{VolCurveKind::ConvexDense};
  double oos_in_band{0.0}; // held-out price-in-band fraction (0..1)
  double oos_vw{0.0};      // held-out vega^2-weighted in-band fraction (0..1)
  double vega_weight_in_band{0.0};
  double vega_weight_total{0.0};
  std::size_t dof_sum{0};              // summed effective DoF over scored slices (parsimony)
  std::size_t n_holdout{0};            // required held-out strikes (fixed denominator)
  std::size_t n_successful_holdout{0}; // keys successfully model-priced
  std::size_t n_in_band{0};            // held-out strikes priced inside their bid/ask
  std::size_t n_slices{0};             // expiries that produced a scorable fit
  std::size_t n_required_slices{0};
  std::size_t n_required_holdout{0};
  double expiry_coverage{0.0};
  double holdout_coverage{0.0};
  bool admitted{false};

  // ── Task C2.5: fit-metrics selection signal ──
  // Held-out slice metrics (Vola-style; see fit_metrics.hpp), aggregated over
  // every scored held-out observation for this family. `chi2_reduced` is the
  // secondary tie-break (after oos_vw, before parsimony DoF). `metrics_valid`
  // is false when the held-out sample was too thin for a reduced chi-square
  // (N <= dof), in which case chi2_reduced does not participate in the tie-break.
  double chi2_reduced{0.0};     // reduced chi^2 over held-out obs
  double rmse_vol{0.0};         // held-out vol RMSE
  double avE5_vol{0.0};         // held-out mean|resid|*1e5
  std::size_t n_within_band{0}; // held-out obs inside their 1σ error bar
  bool metrics_valid{false};    // slice_fit_metrics succeeded
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
  std::vector<std::size_t> sampled_expiry_indices;
  // Candidates fully scored before an optional budget stopped the search.
  std::size_t scores_evaluated{0};
  bool budget_exhausted{false};
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
  // Whole-call steady-clock budget in milliseconds. Zero is unlimited. The
  // budget is checked only between fully-scored candidates, so one candidate
  // is the maximum non-preemptible unit. Expiry preparation is included. If the
  // first completed candidate is not admissible, the call returns Unavailable
  // instead of silently ignoring the deadline to search the remaining ladder.
  double time_budget_ms{0.0};
  // Common-key coverage floors. Missing candidate outputs remain missing; they
  // are never removed from the denominator to make the candidate look better.
  double min_expiry_coverage{1.0};
  double min_holdout_coverage{1.0};
  // The candidate chosen on the common held-out population is rebuilt by its
  // serving driver before publication. That driver may apply a different
  // preparation policy, so require it to retain at least a majority of the
  // board's attempted strike population. The gate is selector-only: direct or
  // explicitly pinned curves continue to use FitAdmissionPolicy unchanged.
  double min_served_quote_coverage{0.50};
};

namespace detail {
// Tighten (never relax) the caller's publication contract for a family chosen
// by cross-validation. Kept as a pure seam so the selector-to-serving rebuild
// cannot silently optimize quality by abstaining on most of the board.
[[nodiscard]] FitAdmissionPolicy
selector_served_admission_policy(const FitAdmissionPolicy &base,
                                 const SelectorConfig &selector) noexcept;
} // namespace detail

// Deterministic lexicographic rank used by `select_curve`: admission, expiry
// coverage, held-out-key coverage, OOS quality, then complexity within the
// parsimony margin.
[[nodiscard]] Result<std::size_t> select_candidate_index(std::span<const CandidateScore> scores,
                                                         double parsimony_margin);

// Default family ladder: the broad-coverage eSSVI baseline first, then
// cache-friendly linear variance, convex dense, raw SVI, and event-capable C8.
// High-confidence boards bypass this expensive validation; it is the ambiguity
// fallback and explicit research mode.
[[nodiscard]] std::vector<CurveConfig> default_selector_candidates();

// Bounded production policy for ambiguous boards. Production evaluates only
// the broad-coverage eSSVI baseline; callers that explicitly request research
// cross-validation may use SelectorConfig{} (all default candidates, unlimited
// soft deadline) or provide their own candidate ladder/budget.
[[nodiscard]] SelectorConfig production_selector_config();

// Pure winner-selection policy over already-scored candidates (Task C2.5). The
// ordering is: (1) best out-of-sample vega-weighted in-band `oos_vw`; within
// the `parsimony_margin` tie band, (2) reduced chi-square closest to 1 — i.e.
// smallest |chi2_reduced − 1| — among candidates whose metrics are valid; then
// (3) fewer average degrees of freedom (parsimony); then (4) higher `oos_vw`.
// Candidates that failed to fit (`n_holdout == 0`) or were butterfly-
// DISQUALIFIED are excluded. Returns the winning index into `scores` (0 when no
// candidate is scorable — callers gate on that upstream). Deterministic; pure.
[[nodiscard]] std::size_t select_best_candidate(const std::vector<CandidateScore> &scores,
                                                double parsimony_margin) noexcept;

// Choose the best curve config for `under` under `in` (market/carry policy).
//
// @return InvalidArgument if S <= 0 / r non-finite / no candidates; NotFound if
//         no candidate produced a scorable fit on any expiry; Unavailable if a
//         bounded search expires before one is admissible; otherwise the
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
