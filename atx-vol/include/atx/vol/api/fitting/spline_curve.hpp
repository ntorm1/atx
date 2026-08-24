#pragma once

// SpiderRock LiveVolSurfaces-style "SRCubic" curve family: a cubic natural
// spline over STANDARDIZED moneyness of the volatility MULTIPLE
// `m(z) = sigma(K) / sigma_ATM`, on a fixed 29-point moneyness grid
// (`kSrMoneynessGrid`). Standardized moneyness is `z = ln(K/F) / (sigma_ATM *
// sqrt(T))` (a "LogStd" coordinate: a strike a fixed number of ATM standard
// deviations away always maps to the same z, unlike raw log-moneyness). This
// is the curve family SpiderRock ships for equity index / single-name vol
// surfaces; it is a strict sibling of `LinearVarianceCurve` in this file's
// family (see vol_curve.hpp), differing in TWO ways: (1) the interpolation
// variable is the vol MULTIPLE on a standardized axis, not total variance on
// raw log-moneyness, and (2) the interpolant is a smooth natural cubic spline
// rather than piecewise-linear, so it also has curvature between knots.
//
// ## Fit algorithm (`fit_spline_vol_slice`)
//
// 1. Seed sigma_ATM: a vega-weighted mean IV over quotes within a half-ATM-
//    vol band, falling back to the global vega-weighted mean when the band is
//    empty. One refinement pass re-seeds sigma_ATM at the fitted spline's
//    value at z = 0 and re-standardizes + refits once more (two solves total,
//    deterministic — no iteration to convergence).
// 2. Standardize every observation to (z_i, y_i = iv_i / sigma_ATM, wt_i =
//    FitObs::weight_w).
// 3. Restrict the fixed 29-point grid to the ACTIVE knots whose z lies within
//    one standardized unit of the observed [min z, max z] range (never fewer
//    than 4, expanded outward when the board is too narrow) — knots outside
//    this window contribute no degrees of freedom and are effectively pinned
//    by the natural-spline flat extension.
// 4. Build the CARDINAL basis: for each active knot j, solve the tridiagonal
//    natural-cubic-spline system once for the unit vector e_j (value 1 at
//    knot j, 0 at every other active knot) and evaluate that unit spline at
//    every observation's z_i — this yields `B[i][j] = basis_j(z_i)` in O(K^2)
//    total (K <= 29 active knots).
// 5. Solve the penalized weighted least squares system
//        (B^T W B + lambda * D^T D) * m = B^T W y
//    via `atx::core::linalg::solve_spd` (the same SPD helper the C8 LM uses),
//    where D is the second-difference matrix over the active knots (a P-
//    spline-style roughness penalty on the fitted multiples). The solved `m`
//    is clamped elementwise to `[mult_floor, +inf)`.
// 6. Diagnostics: a post-fit Lee/Roper butterfly-density scan
//    (g(k) = (1 - k*w'/(2w))^2 - (w'/2)^2*(1/4 + 1/w) + w''/2, central finite
//    differences, matching `arb_check_butterfly`'s convention) over 128
//    points spanning the OBSERVED z-range. Violations are counted, not
//    projected — `SplineVolParams::n_butterfly_viol` exposes the count so a
//    caller may reject or accept per its own policy.
//
// Serving: `w(k) = (sigma_ATM * m(z))^2 * T`, with `z` clamped to
// `[z.front(), z.back()]` before the spline evaluation — a flat extension
// beyond the outermost active knot ("flat wings").
//
// `SplineVolCurve` (the `IVolCurve` adapter over `SplineVolParams`) is
// declared in vol_curve.hpp alongside the other curve-family adapters
// (mirrors the `C8Params`/`C8Curve` split: the parameter + fit-options types
// live in the family-specific header with NO dependency on `IVolCurve`, so
// vol_curve.hpp can include this header for `CurveConfig::spline` without a
// header cycle; `IVolCurve` itself is only forward-declared here, for the
// `fit_spline_vol_slice` return type).
//
// ## Thread-safety
//
// `fit_spline_vol_slice` is a pure function of its inputs (the input
// observation span is read-only). A fitted `SplineVolCurve` (defined in
// vol_curve.cpp) is an immutable value after construction; concurrent
// `w`/`iv` reads are safe, matching every other `IVolCurve` adapter.

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "atx/vol/api/fitting/calib.hpp"  // FitObs
#include "atx/vol/api/core/types.hpp"  // Result

namespace atx::vol {

class IVolCurve;  // fwd decl only — see file-top comment on the header split.

// ── Fixed SpiderRock 29-point standardized-moneyness grid ──────────────────
//
// z-values in ATM-standard-deviation units (LogStd). Ascending, symmetric.
inline constexpr std::array<double, 29> kSrMoneynessGrid = {
    -25,   -14,   -11,  -8.5, -6.5, -5,    -3.75, -2.75, -2,   -1.5, -1,
    -0.75, -0.5,  -0.25, 0,    0.25, 0.5,   0.75,  1,     1.5,  2,    2.75,
    3.75,  5,     6.5,   8.5,  11,   14,    25};

// ── Fitted parameters ───────────────────────────────────────────────────────
//
// `z` / `mult` are the ACTIVE knot subset (a slice of `kSrMoneynessGrid`) —
// the knots that carried degrees of freedom in the fit; `dof()` on the curve
// returns `z.size()`. `z_lo_valid` / `z_hi_valid` are the OBSERVED
// standardized-moneyness range (before the +/-1 active-knot padding); the
// served curve is flat beyond `[z.front(), z.back()]` regardless.
struct SplineVolParams {
  double atm_vol{0.0};           // sigma_ATM > 0
  // INVARIANT (required, not enforced by this struct): strictly increasing and
  // finite. The spline core divides by the gaps z[i+1]-z[i]; a duplicated or
  // unsorted knot from a hand-built or deserialized params degrades the curve to
  // the piecewise-linear interpolant (all-zero second derivatives) rather than
  // serving NaN, but it is still not the curve the caller asked for.
  std::vector<double> z;         // active knot grid, strictly increasing
  std::vector<double> mult;      // per-knot vol multiples, > 0 (>= mult_floor)
  double z_lo_valid{0.0};        // observed standardized-moneyness range;
  double z_hi_valid{0.0};        // flat outside [z.front(), z.back()] regardless.
  // Served-multiple ceiling (set from SplineFitOpts::mult_ceil at fit time): the
  // served m(z) is clamped to <= mult_cap in `w()`, bounding the natural cubic's
  // between-knot / data-gap OVERSHOOT so no served point spikes to an
  // economically impossible vol. This keeps the calendar-cone floor (which reads
  // a prior slice's served w) from reacting to a phantom overshoot spike and
  // over-lifting the whole next slice off its bid-ask band. 0 (or non-finite) ==
  // no ceiling (legacy behaviour).
  double mult_cap{0.0};
  // Uniform additive total-variance offset applied by the calendar-cone
  // projection (SplineVolCurve::project_calendar): served w = (atm*m(z))^2*T +
  // w_offset. 0 for a freshly fitted slice and for the front expiry (no w_prev);
  // set >0 only to clear a genuine calendar crossing against the prior slice.
  double w_offset{0.0};
  // Post-fit Lee/Roper butterfly-density violation count on a 128-pt k-grid
  // spanning [z_lo_valid, z_hi_valid] (NOT projected — see file-top comment
  // step 6). 0 == clean.
  std::size_t n_butterfly_viol{0};
};

// ── Fit options ──────────────────────────────────────────────────────────
struct SplineFitOpts {
  // BORROWED, not owned — `std::span` as a MEMBER, so this options object does
  // not extend the lifetime of whatever backs it. The default names a
  // `constexpr` static (`kSrMoneynessGrid`) whose storage outlives every caller,
  // but `SplineFitOpts{.grid = make_grid()}` with a temporary vector stores a
  // DANGLING span and the fitter reads freed memory. House style §1 forbids a
  // non-owning view as a member without a lifetime guarantee; the guarantee here
  // is the caller's, and it is stated rather than enforced because changing the
  // member to an owning container would allocate on a per-slice fit path.
  //
  // CONTRACT: the storage `grid` names must outlive every `fit_spline_vol_slice`
  // call that receives this object. Bind the grid to a NAMED object that
  // outlives the options:
  //     const std::vector<double> knots = make_grid();
  //     SplineFitOpts opts; opts.grid = knots;   // OK
  //     SplineFitOpts bad{.grid = make_grid()};  // DANGLING
  //
  // Must be strictly increasing and finite (validated at the fitter's entry, and
  // the spline core divides by the knot gaps — see fit_spline_vol_slice).
  std::span<const double> grid{kSrMoneynessGrid};  // candidate knot z-grid
  // 2nd-difference (P-spline) roughness penalty on the fitted multiples. Raised
  // 20x from the historical 1e-3 after a full-OPRA-universe sweep: the light
  // penalty under-regularized the multiple curve, letting the natural cubic
  // OVERSHOOT between the unevenly-spaced SR knots in sparse/wing regions. Those
  // overshoot spikes fed the calendar-cone floor (which reads a prior slice's
  // served w on a dense grid) and forced large ATM-level lifts on every
  // subsequent expiry, pushing model prices above the ask across dense
  // many-slice boards (the tail that dragged the universe mean down). At 0.02
  // the multiple curve is smooth enough that the calendar floor lifts only for
  // genuine crossings, lifting universe-mean frac-in-bidask 0.928 -> 0.963 and
  // RMSE 0.070 -> 0.028 with calendar-arb-free held at 100% -- while staying
  // gentle enough to still recover a clean SVI smile to <2e-3 vol RMSE and
  // introduce no butterfly-density violations (SplineVol.RecoversSviSmile /
  // .ButterflyViolationCounterOnConvexData). Larger lambda (>=0.1) lifts the
  // universe mean further but over-smooths clean data into butterfly arb, so
  // 0.02 is the accuracy/no-arb sweet spot.
  double lambda{0.02};        // 2nd-difference roughness penalty on multiples
  double mult_floor{0.05};    // post-solve clamp on the fitted multiples
  // Served-multiple ceiling (see SplineVolParams::mult_cap): the fitted curve's
  // served m(z) is clamped to <= mult_ceil, bounding the residual between-knot
  // overshoot the roughness penalty does not fully absorb so a single phantom
  // spike cannot drive the calendar floor. 3.0 (== 300% of sigma_ATM) is well
  // above any real tradeable-strike vol multiple, so it clips only overshoot
  // (a tighter cap like 2.0 clips genuine deep-wing vols and adds bid misses);
  // set <= 0 to disable. On top of the raised lambda it holds total bid misses
  // flat (real wings unclipped) while trimming the worst pathological boards.
  double mult_ceil{3.0};
  std::size_t min_obs{6};     // below this (raw or post-filter): InvalidArgument
};

// Penalized WLS fit of the vol-multiple cubic spline from de-Americanized
// European observations (see file-top comment for the full algorithm).
//
// @return InvalidArgument if F/T/df <= 0, `opts.grid.size() < 4`, or fewer
//         than `opts.min_obs` observations survive (raw count, or after
//         dropping non-finite / non-positive k, sigma_mkt, weight_w rows);
//         Unavailable if the ATM-vol seed (or its refinement) is degenerate,
//         or the underlying SPD solve fails (ill-conditioned design);
//         otherwise Ok with the fitted `SplineVolCurve`.
[[nodiscard]] Result<std::unique_ptr<IVolCurve>>
fit_spline_vol_slice(std::span<const FitObs> obs_eu, double F, double T, double df,
                     const SplineFitOpts &opts = {});

}  // namespace atx::vol
