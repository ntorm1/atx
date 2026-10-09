#pragma once

// Per-slice raw-SVI calibrators — the quasi-explicit (Zeliade / De Marco 2009)
// fitter and the Martini-Mingone (2020) constrained price-domain LM.
//
// Ported from the C `ats-vol` library:
//   - src/ats_calibrate_svi.c/.h  — `ats_vol_svi_fit_observations`
//     (quasi-explicit: inner bounded-LSQ over (a, d_uv, c_uv) at fixed
//     (m, sigma), outer Nelder-Mead on (m, sigma), IRLS-Huber outer-outer),
//     `ats_vol_svi_calib_surface`, and the raw <-> Jump-Wings conversions
//     `ats_vol_svi_raw_to_jw` / `ats_vol_svi_jw_to_raw`;
//   - src/ats_calibrate_svi_mm.c/.h — `ats_vol_svi_mm_fit_observations`
//     (raw-SVI 5-DoF Levenberg-Marquardt in the price domain, projecting each
//     iterate onto the Mingone admissible polytope) and
//     `ats_vol_svi_mm_calib_surface`.
//
// ## What is reused (not re-ported)
//
// The shared calibration infrastructure (`CalibOpts`, `FitObs`, `FitDiag`,
// `ObsSet`, `build_observations`) lives in calib.hpp; the SVI math
// (`SviParams`, `svi_total_w`, `VolSurface::set_slice_svi`), the arb
// projection (`arb_project_calendar_svi`) and the Mingone admissibility check
// (`arb_check_butterfly_svi_mm`) live in vol_surface.hpp / arb.hpp; the linear
// solves are backed by `atx::core::linalg::solve_spd`. This header only adds
// the two per-slice fitters, their surface drivers, and the JW utilities.
//
// ## Error mapping (agent profile §4)
//
// The C's negative-integer status channel is replaced by `Result<T>` / `Status`:
//   ERR_INVALID  -> InvalidArgument   ERR_NO_DATA  -> NotFound
//   ERR_DOMAIN / ERR_RANGE -> OutOfRange   ERR_NO_CONVERGE -> Unavailable.
//
// ## Thread-safety
//
// The per-slice fitters are pure functions of their inputs (they copy the
// observation span into a private working buffer before mutating weights), so
// concurrent calls from any threads are safe. The surface drivers take
// exclusive ownership of the `VolSurface` they fill for the call duration
// (they call the mutating `set_slice_svi` / `arb_project_calendar_svi`); the
// caller fences them against any concurrent reader of that surface. The read
// inputs (`Underlying`, `CurveSet`) follow the "many readers OR one writer"
// contract.

#include <array>
#include <cmath>
#include <cstddef>
#include <span>

#include "atx/vol/api/fitting/calib.hpp"        // CalibOpts, FitObs, FitDiag
#include "atx/vol/api/pricing/rates_curve.hpp"  // CurveSet
#include "atx/vol/api/core/types.hpp"        // Result, Status
#include "atx/vol/api/marketdata/universe.hpp"     // Underlying
#include "atx/vol/api/fitting/vol_surface.hpp"  // VolSurface, SviParams, Parametrization

namespace atx::vol {

// ── SVI Jump-Wings parametrization (ports `AtsVolSliceSviJw`) ─────────────
//
// Gatheral & Jacquier 2014 (arXiv:1204.0646, §3.3) trader-interpretable
// coordinates: v = ATM total variance / T (= sigma_ATM^2), psi = ATM skew
// slope, p = left-wing slope, c = right-wing slope, v_min = smile-floor total
// variance / T. Better conditioned than raw (a, b, m, rho, sigma) for the
// long-dated wings. Aggregate; trivially copyable; all members initialized.
struct SviJwParams {
  double v{0.0};      // ATM total variance / T
  double psi{0.0};    // ATM skew slope
  double p{0.0};      // left-wing slope
  double c{0.0};      // right-wing slope
  double v_min{0.0};  // minimum total variance / T
  double T{0.0};      // year-fraction to expiry
};

// ── Per-slice fitters ────────────────────────────────────────────────────

// Quasi-explicit raw-SVI fit (Zeliade WP zwp-0005, De Marco & Martini 2009).
// `obs` supplies the prepared observation rows (log-moneyness `k`, market IV
// `sigma_mkt`, total variance `w_mkt`, and the w-space weight `weight_w`);
// `T` is the slice year-fraction and `F` the forward, stored on the result.
// The span is NOT mutated — the fitter copies it into a private buffer before
// running the IRLS-Huber reweight. `diag` (when non-null) receives the
// vega-weighted RMSE, max residual, iteration counts, and quote count.
//
// @return InvalidArgument if `obs` is empty or `T <= 0`; Unavailable if the
//         observation set is rank-deficient (fewer usable design directions than
//         the three linear coefficients — e.g. a single quote, coincident
//         strikes, or all-zero weights), in which case there is no
//         least-squares solution to return and the caller must NOT substitute
//         one; otherwise Ok with the fitted slice (a, b, rho, m, sigma, T, F).
//         Given a full-rank observation set the quasi-explicit method always
//         terminates at a feasible optimum, so there is no NoConverge path
//         (matching the C, which only fails on invalid input).
[[nodiscard]] Result<SviParams> svi_fit_slice(std::span<const FitObs> obs,
                                              double T, double F,
                                              const CalibOpts &opts,
                                              FitDiag *diag = nullptr);

// Martini-Mingone constrained price-domain LM (arXiv:2005.03340 §6.3). Seeds
// from the quasi-explicit fit above, overwrites the observation weights with
// the price-domain form `1 / (spread^2 + (0.1*tick*vega)^2)`, then runs a
// 5-DoF (a, b, rho, m, sigma) Levenberg-Marquardt whose every iterate is
// projected onto the admissible polytope
//
//   b > 0, sigma > 0, |rho| < 1, a + b*sigma*sqrt(1-rho^2) >= 0,
//   b*(1 + |rho|) <= 4/T,
//
// so the returned slice is guaranteed admissible. `obs[i]` must carry the
// pricing fields `{k, K, F, df, mid, spread, vega, side, sigma_mkt, w_mkt}`
// (as `build_observations` populates them). The span is NOT mutated.
//
// PORT NOTE — the C consumed an Andersen-Lake American-price correction cache
// (`opts.amer_correction_call/_put`) that the ported `CalibOpts` omits; the
// price prediction here is therefore pure European Black-76 (the `corr == NULL`
// branch of the C, and exactly what the C's synthetic test exercises).
//
// @return InvalidArgument if `obs` is empty or `T <= 0`; otherwise Ok with the
//         admissible fitted slice.
[[nodiscard]] Result<SviParams> svi_mm_fit_slice(std::span<const FitObs> obs,
                                                 double T, double F,
                                                 const CalibOpts &opts,
                                                 FitDiag *diag = nullptr);

// Project a raw-SVI slice (a, b, rho, sigma) onto the Martini-Mingone admissible
// polytope with the production edge pads (the same projection `svi_mm_fit_slice`
// applies to its seed and every LM iterate). `m` carries no Mingone constraint
// and is untouched. Used at the raw-SVI serving seam
// (`fit_slice_curve(VolCurveKind::Svi)`) to repair a fitted slice that trips
// `arb_check_butterfly_svi_mm` before it can be served.
//
// @return true if any coordinate moved (the slice was inadmissible); false if it
//         was already admissible (or `T <= 0`, in which case it is left as-is).
[[nodiscard]] bool svi_project_mm(SviParams &slice, double T) noexcept;

// ── Surface drivers ──────────────────────────────────────────────────────

// Fit one raw-SVI slice per chain of `under`, in ascending-T order, then run
// the SVI calendar-arb projection. Forwards come from `cs.forward`, discount
// factors from `cs.yield`. Slices that yield too few observations, fail to
// fit, or blow up past the post-fit sigma clamp are silently skipped (the
// output is compacted). `diag` (when non-null) receives the aggregate fit
// quality; the same summary is stamped onto the surface's diagnostics.
//
// @param surface  MUST be `Parametrization::Svi`.
// @return InvalidArgument if the surface is not SVI-parametrized;
//         NotFound if `under` has no chains, or if no slice fit successfully;
//         otherwise Ok.
[[nodiscard]] Status svi_calib_surface(VolSurface &surface,
                                       const Underlying &under,
                                       const CurveSet &cs, const CalibOpts &opts,
                                       FitDiag *diag = nullptr);

// As `svi_calib_surface`, but each slice is fit with the Martini-Mingone
// constrained LM and the surface must be `Parametrization::SviMm`. Every
// stored slice is Mingone-admissible by construction.
//
// @param surface  MUST be `Parametrization::SviMm`.
// @return InvalidArgument if the surface is not SVI-MM-parametrized;
//         NotFound if `under` has no chains, or if no slice fit successfully;
//         otherwise Ok.
[[nodiscard]] Status svi_mm_calib_surface(VolSurface &surface,
                                          const Underlying &under,
                                          const CurveSet &cs,
                                          const CalibOpts &opts,
                                          FitDiag *diag = nullptr);

// ── Raw <-> Jump-Wings conversions ───────────────────────────────────────

// Raw SVI -> JW (Gatheral & Jacquier eq. 4.1). Pure arithmetic; degenerate
// inputs (T <= 0, w(0) <= 0) collapse the affected JW fields to 0.
[[nodiscard]] SviJwParams svi_raw_to_jw(const SviParams &raw) noexcept;

// JW -> raw SVI (Gatheral & Jacquier §3.3 inverse). Exact bijection on the
// admissible domain.
//
// @return OutOfRange if the JW tuple is infeasible (T/v/v_min <= 0, p/c <= 0,
//         v_min > v, |rho| >= 1, |beta| > 1, or a negative recovered sigma);
//         otherwise Ok with the recovered raw slice.
[[nodiscard]] Result<SviParams> svi_jw_to_raw(const SviJwParams &jw);

namespace detail {

// ── Nelder-Mead vertex ordering (the NaN rule, stated once) ─────────────────
//
// FT-T3. `nm_search` picked its winner with `if (f[1] < f[best]) best = 1;`,
// which is FALSE when `f[best]` is NaN — IEEE-754 makes every ordered
// comparison against NaN false. So a NaN vertex 0 beat two perfectly good
// finite vertices, `out_best_sse` came back non-finite, and the function broke
// its OWN documented contract ("non-finite iff EVERY vertex the simplex visited
// was unusable"). The caller reads that contract literally and discards the
// slice, so one NaN threw away a fit that had usable candidates.
//
// `svi_blls_inner`'s documented unusable sentinel is +inf, which the naive
// comparison already handled; NaN reaches the objective by other routes (a
// non-finite observation surviving into `svi_qe_sse`, a normal-equation solve
// returning NaN rather than the +inf fallback). Both are "unusable", so the
// rule below ranks EVERY non-finite value behind every finite one. -inf is
// impossible for a sum of squares and is deliberately swept into the same
// bucket rather than given a special case that could never fire.
[[nodiscard]] inline bool nm_vertex_less(double lhs, double rhs) noexcept {
  if (!std::isfinite(lhs)) {
    return false; // an unusable vertex never beats anything, itself included
  }
  if (!std::isfinite(rhs)) {
    return true; // any usable vertex beats an unusable one
  }
  return lhs < rhs;
}

// Index of the winning simplex vertex under `nm_vertex_less`. Ties keep the
// EARLIEST vertex (the pre-existing tie-break). Returns 0 when every vertex is
// unusable — the one case in which a non-finite `out_best_sse` is correct.
[[nodiscard]] inline std::size_t nm_best_vertex(const std::array<double, 3> &f) noexcept {
  std::size_t best = 0u;
  if (nm_vertex_less(f[1], f[best])) {
    best = 1u;
  }
  if (nm_vertex_less(f[2], f[best])) {
    best = 2u;
  }
  return best;
}

}  // namespace detail

}  // namespace atx::vol
