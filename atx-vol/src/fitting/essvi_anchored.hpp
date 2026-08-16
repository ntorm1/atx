#pragma once

// Anchored eSSVI slice calibration — Corbetta, Cohort, Laachir & Martini,
// "Robust calibration and arbitrage-free interpolation of SSVI slices"
// (arXiv:1804.04924; Zeliade ZWP-008; Decisions in Economics and Finance 2019).
//
// This is an ALTERNATIVE to the cube-space Levenberg-Marquardt fitter in
// `essvi_calib.hpp`, not a replacement: it is reachable only through an
// explicit opt-in and the legacy path is untouched when the opt-in is off.
//
// ── Why a second calibrator ───────────────────────────────────────────────
//
// The LM fitter needs a starting point (`fit_core` seeds psi from a crude
// band-ratio of the nearest-ATM quote and p / lambda from the constants 0.3 /
// 0.4), fits THREE free parameters per slice, and treats calendar arbitrage as
// a post-fit projection or a floor bolted onto the objective. The anchored
// parameterization removes all three properties:
//
//   * TWO free parameters per slice (rho, psi) — the third, theta, is a closed
//     form of the other two through the anchor;
//   * NO starting point — a deterministic rho sweep plus a bounded 1-D Brent
//     minimisation in psi, so there is no basin to land in and no seed to tune;
//   * no-arbitrage is an INTERVAL in psi for each rho, so every point the
//     optimiser can reach is admissible BY CONSTRUCTION. Infeasibility is a
//     property of the interval being empty, detectable before any work is done,
//     not a verdict handed down after a completed fit is discarded.
//
// ── Coordinates ──────────────────────────────────────────────────────────
//
// The slice is carried in (theta, psi, rho) with `psi := theta * phi`, which is
// the coordinate system the no-arbitrage conditions are interval-shaped in.
// Substituting phi = psi / theta into the eSSVI backbone
// (`essvi_backbone_w`, vol_surface.cpp) gives the equivalent form this header
// works in:
//
//   w(k) = 0.5 * ( theta + rho*psi*k + sqrt( (psi*k + rho*theta)^2
//                                            + theta^2 * (1 - rho^2) ) )
//
// with w(0) == theta and dw/dk(0) == rho*psi. The auxiliary coordinate
// `chi := rho * psi` is the slice's ATM total-variance slope; the calendar
// conditions are stated in (theta, psi, chi) because they are LINEAR there.
//
// ── The anchor ───────────────────────────────────────────────────────────
//
// Let (k*, theta*) be the observed (log-moneyness, total variance) pair nearest
// the ATM forward. Because dw/dk(0) == rho*psi, matching the slice to that
// observation to first order in k* gives Corbetta et al.'s anchoring relation
//
//   theta = theta* - rho * psi * k*                                      (A)
//
// which removes theta as a free parameter AND removes the need to pre-estimate
// an ATM total variance by interpolating between bracketing strikes — the exact
// step that is ill-posed on a thin chain with no true ATM quote. (A) is exact in
// the limit k* -> 0 and carries an O(psi^2 k*^2 / theta) truncation otherwise,
// which is why the anchor is taken at the SMALLEST |k| available. The linear
// form is deliberate and is the paper's own choice: it is what keeps the
// butterfly bound below a closed-form quadratic root rather than a cubic.
//
// ── No-arbitrage as intervals ────────────────────────────────────────────
//
// Butterfly (Gatheral-Jacquier 2014 arXiv:1204.0646, in the psi coordinate;
// identical content to `essvi_phi_max`, vol_surface.cpp:175):
//
//   psi * (1 + |rho|) <= 4          and        psi^2 * (1 + |rho|) <= 4 * theta
//
// Substituting (A) into the second turns it into a quadratic in psi whose
// positive root is Corbetta et al.'s
//
//   psi_+(rho, k*, theta*) = -2*rho*k*/(1+|rho|)
//                            + sqrt( 4*rho^2*k*^2/(1+|rho|)^2
//                                    + 4*theta*/(1+|rho|) )
//
// so the admissible set for a given rho is the plain interval
// `0 < psi <= min(psi_+, 4/(1+|rho|))`. Note theta > 0 needs no separate check:
// `psi^2 (1+|rho|) <= 4 theta` with psi > 0 already forces it.
//
// Calendar, against the previously calibrated (shorter-maturity) slice
// (theta_1, psi_1, chi_1) — necessary AND sufficient for eSSVI:
//
//   theta_2 >= theta_1,   psi_2 >= psi_1,   |chi_2 - chi_1| <= psi_2 - psi_1
//
// At a FIXED rho each of the three is again a one-sided bound on psi (the third
// splits into two, and the first is one-sided because (A) is affine in psi), so
// the feasible set stays an interval. That is the whole design: the optimiser
// only ever searches inside a closed interval on which both arbitrage classes
// hold identically, so a calibrated slice cannot be arbitrageable and there is
// nothing for a downstream rejection gate to discard.
//
// ── Arbitrage-free interpolation ─────────────────────────────────────────
//
// See `anchored_interpolate` for the proof and its preconditions. This is what
// makes a quote-starved expiry a non-event rather than a failure.
//
// ── Thread-safety ────────────────────────────────────────────────────────
//
// Every function here is a pure function of its arguments — no globals, no
// shared state, no allocation outside the returned vectors. Safe to call
// concurrently.

#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"           // Result, Status, ErrorCode
#include "atx/vol/api/fitting/calib.hpp"        // FitObs
#include "atx/vol/api/fitting/vol_surface.hpp"  // EssviParams

namespace atx::vol {

// ── Slice ────────────────────────────────────────────────────────────────

// One anchored eSSVI slice. `theta`/`psi`/`rho` fully determine the curve; the
// anchor pair is retained for provenance and for re-deriving bounds.
struct AnchoredSlice {
  double theta{};       // ATM total variance w(0); > 0
  double psi{};         // theta * phi; > 0
  double rho{};         // slice skew, in (-1, 1)
  double k_star{};      // anchor log-moneyness (0 for an interpolated slice)
  double theta_star{};  // anchor total variance (theta for an interpolated slice)
  double T{};           // year-fraction to expiry; > 0
  double F{};           // forward at this expiry

  // ATM total-variance slope dw/dk(0). The coordinate the calendar conditions
  // are linear in.
  [[nodiscard]] constexpr double chi() const noexcept { return rho * psi; }
};

// ── Options / diagnostics ────────────────────────────────────────────────

// Calibration policy. Every field is a fixed budget: the cost of a slice fit is
// bounded by `n_rho * (1 + n_refine_passes) * brent_max_iter` objective
// evaluations regardless of the data, which is the load-balance property the
// LM's data-dependent iteration count does not have.
struct AnchoredOpts {
  // rho sampling points on the open interval (-rho_max, rho_max). Corbetta et
  // al.: "20 points is enough according to us to grant a calibration within the
  // bid ask in general".
  std::uint16_t n_rho{20};
  // Number of successive refinement sweeps, each re-sampling `n_rho` points on
  // the bracket around the incumbent best rho. Each pass divides the rho
  // resolution by ~n_rho, so three passes take the initial ~1e-1 spacing to
  // ~1e-4 — below which the fitted total variance stops moving at the 1e-6
  // level (AnchoredEssviCalibration.RecoversAKnownSliceWithoutAnyStartingPoint).
  std::uint16_t n_refine_passes{3};
  // Hard iteration cap for the 1-D Brent minimisation in psi.
  std::uint16_t brent_max_iter{40};
  // Relative tolerance for the Brent bracket.
  double brent_tol{1.0e-9};
  // |rho| ceiling. Mirrors the LM path's kRhoMax so the two calibrators span
  // the same skew range.
  double rho_max{0.999};
};

// Per-slice fit diagnostics.
struct AnchoredDiag {
  std::uint32_t n_obs{};              // observations entering the objective
  std::uint32_t n_objective_evals{};  // total w-model evaluations of the sweep
  std::uint32_t n_rho_feasible{};     // rho samples with a non-empty psi interval
  double sse{};                       // weighted SSE at the optimum (w units)
  double rmse_w{};                    // unweighted RMS of (w_model - w_mkt)
  double k_star{};                    // anchor used
  double theta_star{};
};

// ── Feasible interval ────────────────────────────────────────────────────

// Closed interval of admissible psi. `empty()` iff no psi satisfies every
// no-arbitrage constraint for the given rho — the structural infeasibility
// signal, available BEFORE any objective evaluation.
struct PsiInterval {
  double lo{};
  double hi{};

  [[nodiscard]] constexpr bool empty() const noexcept { return !(lo <= hi); }
};

// Admissible psi interval for `rho` under the anchor (k*, theta*), optionally
// intersected with the calendar conditions against `prev` (the previously
// calibrated, strictly shorter-maturity slice; null = no calendar constraint).
//
// Preconditions (documented, not verified): theta_star > 0, |rho| < 1. A
// violated precondition yields an empty interval rather than UB.
[[nodiscard]] PsiInterval anchored_psi_bounds(double rho, double k_star,
                                              double theta_star,
                                              const AnchoredSlice* prev) noexcept;

// ── Evaluation ───────────────────────────────────────────────────────────

// Total variance of the anchored slice at log-moneyness `k`. Equals
// `essvi_backbone_w(anchored_to_essvi(s), k)` for every slice with theta > 0.
[[nodiscard]] double anchored_w(const AnchoredSlice& s, double k) noexcept;

// Project the slice into the surface container's parametrization. `theta`,
// `phi = psi/theta` and `rho` are written in natural form (what
// `essvi_backbone_w` reads); the Mingone cube coordinates are filled from
// `essvi_natural_to_reparam` for diagnostics and warm-seed reuse. The wing
// residual layer is left disarmed — the anchored backbone is the whole curve.
[[nodiscard]] EssviParams anchored_to_essvi(const AnchoredSlice& s) noexcept;

// ── Calibration ──────────────────────────────────────────────────────────

// Calibrate one slice to `obs`. NO starting point is taken because none exists:
// the search is a deterministic rho sweep over (-rho_max, rho_max), a bounded
// 1-D Brent minimisation of the weighted total-variance SSE over the admissible
// psi interval for each rho, and `opts.n_refine_passes` bracket refinements
// around the incumbent. The objective is the same weighted w-domain SSE the LM
// path minimises (`FitObs::weight_w`), so fits from the two calibrators are
// directly comparable.
//
// @param obs   observations for this expiry; >= 1 required. Two free parameters
//              means the fit stays well-posed far below the LM path's needs.
// @param T     year-fraction to expiry (> 0).
// @param F     forward (stamped into the slice; the w-domain fit does not read it).
// @param prev  optional previously calibrated slice at a STRICTLY shorter
//              maturity. Non-null intersects the calendar conditions into every
//              psi interval, so the returned slice cannot calendar-cross it.
// @return InvalidArgument on empty `obs`, T <= 0, or no usable anchor;
//         Unavailable if every sampled rho has an empty admissible interval
//         (structural infeasibility — never observed in the paper's tests, and
//         reported honestly here rather than repaired); otherwise Ok(slice).
[[nodiscard]] Result<AnchoredSlice> anchored_fit_slice(
    std::span<const FitObs> obs, double T, double F, const AnchoredOpts& opts,
    const AnchoredSlice* prev = nullptr, AnchoredDiag* out_diag = nullptr);

// ── Arbitrage-free interpolation (no fit required) ───────────────────────

// Linearly interpolate the calibrated parameters of two slices to maturity `T`.
//
// PROOF that the result is arbitrage-free, in the coordinates (theta, psi, chi)
// this header carries. Let lambda = (T - lo.T) / (hi.T - lo.T) in [0, 1] and let
// each of theta, psi, chi be the affine blend of the endpoint values.
//
//   PRECONDITION: the endpoints are themselves butterfly-free and mutually
//   calendar-free. `anchored_fit_slice` guarantees both by construction, and
//   `anchored_interpolate` verifies them and refuses otherwise, so the theorem
//   is never applied outside its hypotheses.
//
//   (1) |rho| <= 1 throughout. |chi(lambda)| <= (1-lambda)|chi_lo| + lambda*
//       |chi_hi| <= (1-lambda)*psi_lo + lambda*psi_hi = psi(lambda), so
//       |rho(lambda)| = |chi|/psi <= 1.
//   (2) Calendar between ANY two interpolated maturities lambda_1 < lambda_2
//       (which subsumes calendar against both endpoints). theta and psi are
//       affine with non-negative slope because theta_hi >= theta_lo and
//       psi_hi >= psi_lo, so both are non-decreasing. And
//       |chi(lambda_2) - chi(lambda_1)| = (lambda_2 - lambda_1)|chi_hi - chi_lo|
//       <= (lambda_2 - lambda_1)(psi_hi - psi_lo) = psi(lambda_2) - psi(lambda_1),
//       the middle step being exactly the endpoints' own calendar condition.
//   (3) Butterfly, first condition. psi*(1+|rho|) = psi + |chi| is a convex
//       function of lambda (a sum of an affine function and the absolute value
//       of an affine function), so it is bounded on [0,1] by the larger of its
//       endpoint values, each of which is <= 4.
//   (4) Butterfly, second condition. psi^2*(1+|rho|) = psi*(psi + |chi|)
//       = max( psi*(psi + chi), psi*(psi - chi) ). Each branch is a quadratic in
//       lambda whose leading coefficient is (psi_hi - psi_lo) * ((psi_hi -
//       psi_lo) +/- (chi_hi - chi_lo)), which is >= 0 precisely because
//       |chi_hi - chi_lo| <= psi_hi - psi_lo — the endpoints' calendar
//       condition again. So each branch is convex, their max is convex, and
//       subtracting the affine 4*theta(lambda) leaves a convex function of
//       lambda that is <= 0 at both endpoints, hence <= 0 throughout.
//
// This is the property Corbetta et al. state as "the natural interpolation and
// extrapolation of eSSVI parameters provides a continuous eSSVI surface which is
// indeed arbitrage free"; steps (1)-(4) are the explicit verification for the
// two butterfly conditions and the three calendar conditions this codebase
// enforces. `AnchoredEssviInterpolation` in essvi_anchored_test.cpp checks it
// numerically on dense lambda and k grids as well.
//
// INTERPOLATION ONLY. `T` must lie in [lo.T, hi.T]; a maturity outside the
// bracket is refused with InvalidArgument. The paper's short-end EXTRAPOLATION
// rule (theta_t = c*theta_1, psi_t = c*psi_1, rho_t = rho_1) is deliberately NOT
// implemented: the house target is zero extrapolated-tenor rows, and a
// convenience API that can extrapolate is how that target gets missed.
//
// @return InvalidArgument if the bracket is degenerate (hi.T <= lo.T), if `T` is
//         outside [lo.T, hi.T], or if the endpoints are not both butterfly-free
//         and mutually calendar-free; otherwise Ok(slice).
[[nodiscard]] Result<AnchoredSlice> anchored_interpolate(const AnchoredSlice& lo,
                                                         const AnchoredSlice& hi,
                                                         double T);

// ── Invariant assertions (Task 2: check, do not repair) ──────────────────
//
// With feasibility structural, these are ASSERTIONS about a calibrated slice,
// not a fitting stage. They must hold for everything `anchored_fit_slice` and
// `anchored_interpolate` return; a false is a defect in this file, not a
// property of the market. Nothing in this module repairs on a false — the
// detection capability is retained precisely so a regression is loud.

// Both Gatheral-Jacquier butterfly conditions, with a relative slack for
// floating-point round-off at the interval endpoint the optimiser may land on.
[[nodiscard]] bool anchored_butterfly_ok(const AnchoredSlice& s) noexcept;

// The three eSSVI calendar conditions for the ordered pair (`prev` shorter,
// `next` longer).
[[nodiscard]] bool anchored_calendar_ok(const AnchoredSlice& prev,
                                        const AnchoredSlice& next) noexcept;

// ── Sequential surface calibration with thin-slice interpolation ─────────

// One expiry offered to the sequential driver.
struct AnchoredSliceRequest {
  // Observations for this expiry. Non-owning: must outlive the call. May be
  // empty or below any fitting floor — that is the point of `fit_independently`.
  std::span<const FitObs> obs;
  double T{};
  double F{};
  // The caller's decision on whether this expiry carries enough information to
  // be calibrated on its own (in production: the `kMinPreparedFitRows` row
  // floor). FALSE does NOT mean "drop": it means "do not fit this one
  // independently", and the driver will interpolate it from its calibrated
  // neighbours whenever it is bracketed by two of them.
  bool fit_independently{true};
};

// How a slice came to exist.
enum class AnchoredSliceOrigin : std::uint8_t {
  // Neither calibrated nor bracketed — no slice. The ONLY drop class.
  Dropped = 0,
  // Independently calibrated against its own observations.
  Calibrated,
  // Not calibrated; parameters interpolated STRICTLY BETWEEN two calibrated
  // maturities. Never an extrapolation.
  Interpolated,
};

struct AnchoredSliceResult {
  AnchoredSlice slice{};
  AnchoredSliceOrigin origin{AnchoredSliceOrigin::Dropped};
  AnchoredDiag diag{};
};

// Calibrate a whole board. Two passes, in this order:
//
//   1. Ascending in T, calibrate every request whose `fit_independently` is set,
//      each constrained by the previously CALIBRATED slice's calendar interval.
//      A request that fails to calibrate falls through to pass 2 rather than
//      being lost.
//   2. Every remaining request that lies STRICTLY BETWEEN two calibrated
//      maturities is interpolated (`anchored_interpolate`). One outside that
//      bracket stays `Dropped` — interpolation never becomes extrapolation.
//
// @param reqs  expiries in ASCENDING T order (precondition, verified: an
//              out-of-order or non-positive T is InvalidArgument).
// @return one result per request, positionally aligned with `reqs`.
//         InvalidArgument on a malformed request sequence. An all-`Dropped`
//         return is legal and means the board carried nothing calibratable.
[[nodiscard]] Result<std::vector<AnchoredSliceResult>> anchored_fit_sequence(
    std::span<const AnchoredSliceRequest> reqs, const AnchoredOpts& opts);

// ── Qualification activation seam ────────────────────────────────────────

// Turn the anchored path on from the environment:
//
//   ATX_VOL_ESSVI_ANCHORED=1         -> CalibOpts::essvi_anchored
//   ATX_VOL_ESSVI_ANCHORED_INTERP=1  -> CalibOpts::essvi_anchored_interpolate_thin
//
// WHY an environment seam rather than a CLI flag: the A/B qualification this
// feature has to pass (cells_ok, tenor coverage and fitted-IV deltas against the
// legacy path on a full session) has to reach the production binary, and the
// driver + CLI that would otherwise carry the flag are owned by a concurrent
// change. This seam touches only the policy functions that already exist to set
// CalibOpts, matches the env-gating pattern the tree already uses for
// ATX_VOL_AL_PROBE / ATX_VOL_FIT_WORKERS, and cannot alter behaviour unless the
// variable is set: absent or unset, both flags keep their `false` default and
// every byte of the legacy path is unchanged. Replace it with a real CLI flag
// once the driver is free.
//
// The environment is read ONCE per process (first call) and cached, so this is
// not a per-board syscall and cannot change mid-run.
//
// Never DOWNGRADES: a caller that already set the flags keeps them.
void apply_anchored_env_policy(CalibOpts& calib) noexcept;

}  // namespace atx::vol
