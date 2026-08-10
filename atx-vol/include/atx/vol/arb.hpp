#pragma once

// Static-arbitrage validators for fitted vol surfaces.
//
// Ported from the C `ats-vol` library (ats_arb.h / ats_arb.c). For eSSVI
// surfaces parametrized through Mingone's reparametrization the arb-free
// conditions hold by construction; these validators are a defensive
// belt-and-braces check, useful in CI / unit tests / and as a sanity gate
// after a calibration. For SVI per-slice surfaces (fit slice-by-slice
// independently) the calendar-spread no-arb condition across consecutive
// slices must be checked — and, if desired, repaired — explicitly.
//
// Two kinds of static arbitrage are checked:
//
//   1. Calendar-spread arbitrage: for each fixed log-moneyness k and any
//      T1 < T2, total variance must be non-decreasing in maturity,
//          w(k, T1) <= w(k, T2).
//
//   2. Butterfly arbitrage (per-slice risk-neutral density positivity): the
//      Lee/Roper density
//          g(k) = (1 - k*w'/(2w))^2 - (w'/2)^2 * (1/4 + 1/w) + w''/2
//      must be non-negative for every k where w(k) > 0. w' and w'' are taken
//      by finite difference along k on the total surface variance.
//
// ── Return-code translation from the C ────────────────────────────────────
//
// The C surfaced arbitrage through a negative `ATS_VOL_ERR_ARBITRAGE` status.
// Here arbitrage is NOT an error: the `_check_*` entries return
// `Result<std::vector<ArbViolation>>` where an EMPTY vector means "no arb"
// (mirrors the C's `out_n_violations == 0`) and a populated vector carries
// one entry per violation (no cap — every sampled violation is recorded).
// Genuine `ATS_VOL_ERR_INVALID` inputs (e.g. k_max <= k_min on the butterfly
// path) map to `ErrorCode::InvalidArgument`. A no-op on the "wrong"
// parametrization (the C returned OK) maps to an Ok result with an empty /
// zeroed payload.
//
// ── Thread-safety ─────────────────────────────────────────────────────────
//
// The read-only `arb_check_*` / `arb_filter_*` / `prefit_filter_*` entries
// against a fixed surface / quote batch are safe to call concurrently from
// any thread (the surface is "many readers OR one writer"). The mutating
// `arb_project_calendar_*` and `arb_repair_calendar_residual` entries take
// exclusive ownership of the surface for the call duration — the caller
// fences them against any concurrent reader.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/c8.hpp"
#include "atx/vol/rates_curve.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

namespace atx::vol {

// ── Quote-filter flag bits ───────────────────────────────────────────────
//
// Mirrors the C `ATS_VOL_QFLAG_*` macros (ats_vol_types.h). universe.hpp
// stores per-(strike, side) quote flags as raw `std::uint8_t`; this scoped
// enum names the bits that the pre-fit filters below stamp into those words.
enum class QuoteFlag : std::uint8_t {
  None = 0x00u,
  Locked = 0x01u,      // bid >= ask
  Crossed = 0x02u,     // bid > ask
  Stale = 0x04u,       // exceeded staleness threshold
  Halted = 0x08u,      // underlying is halted
  WideSpread = 0x10u,  // (ask - bid) / mid > wide_spread_pct
  Penny = 0x20u,       // bid < penny_floor (sub-tick noise)
  LowVega = 0x40u,     // vega < min_vega_filter
};

[[nodiscard]] constexpr QuoteFlag operator|(QuoteFlag a, QuoteFlag b) noexcept {
  return static_cast<QuoteFlag>(static_cast<std::uint8_t>(a) |
                                static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr QuoteFlag operator&(QuoteFlag a, QuoteFlag b) noexcept {
  return static_cast<QuoteFlag>(static_cast<std::uint8_t>(a) &
                                static_cast<std::uint8_t>(b));
}
constexpr QuoteFlag &operator|=(QuoteFlag &a, QuoteFlag b) noexcept {
  a = a | b;
  return a;
}
[[nodiscard]] constexpr bool has_flag(QuoteFlag value, QuoteFlag flag) noexcept {
  return (value & flag) != QuoteFlag::None;
}
[[nodiscard]] constexpr std::uint8_t to_u8(QuoteFlag f) noexcept {
  return static_cast<std::uint8_t>(f);
}

// ── Violation record ─────────────────────────────────────────────────────

// One static-arbitrage violation located on the sampling grid. `T1`/`T2` are
// the maturity pair for a calendar violation (T2 = T1 for butterfly); `slack`
// is the signed magnitude by which the constraint was breached (always > 0 on
// a recorded violation: w_prev - w for calendar, -g(k) for butterfly).
struct ArbViolation {
  double k_log{};  // log-moneyness where the violation occurs
  double T1{};     // shorter maturity (calendar) / slice T (butterfly / price bounds)
  double T2{};     // longer maturity (calendar) / slice T (butterfly / price bounds)
  double slack{};  // signed breach magnitude
  enum class Kind : std::uint8_t { Calendar = 0, Butterfly = 1, PriceBounds = 2 };
  Kind kind{Kind::Calendar};
};

// ── Exact slice-crossing detection (Gatheral & Jacquier Lemma 3.3 / eq. 3.7) ─
//
// Two raw-SVI slices in TOTAL variance,
//     w_i(k) = a_i + b_i*( rho_i*(k - m_i) + sqrt((k - m_i)^2 + sigma_i^2) ),
// are equal exactly where a quartic in k has a real root. Writing
// D = w_1 - w_2 = L + B_1 - B_2 with L(k) linear and B_i(k) = b_i*R_i(k) >= 0
// the two radical terms, D = 0 <=> L = B_2 - B_1; squaring twice clears both
// radicals and leaves
//     P(k) = (L^2 - B_1^2 - B_2^2)^2 - 4*B_1^2*B_2^2,
// of degree <= 4 because L^2, B_1^2 and B_2^2 are all quadratics. P factors as
//     P = (L - B_1 - B_2)(L + B_1 + B_2)(L - B_1 + B_2)(L + B_1 - B_2),
// which is the whole price of the two squarings: only the LAST factor is the
// equation that was asked, so three quarters of P's roots are SPURIOUS. Every
// root is therefore BACK-SUBSTITUTED into D and dropped unless D vanishes
// there. Skipping that step manufactures confident false crossings, and no test
// that feeds the solver only genuine crossings can detect the omission.
//
// Because crossings are LOCATED rather than sampled, the comparison they
// support is decided over ALL of R: between two consecutive crossings D has no
// zero, so a single evaluation settles its sign on that entire interval, and
// the same argument covers both unbounded tails. Roper's IV4 is quantified over
// R and anything reading Dupire local vol off the surface needs it there, so
// this is the statement the check actually owes. It also cannot miss a crossing
// for being narrower than a grid step — the failure a sampled scan has no
// defence against.

// A raw-SVI slice the exact solver is TOTAL on. The only ways to obtain one are
// the factories below, each of which returns nullopt for anything it cannot
// decide exactly, so a value of this type cannot represent an undecidable
// slice and every member below is total and noexcept.
class SviCrossingSlice {
public:
  // Refuses non-finite parameters, b < 0, sigma < 0 and |rho| >= 1 — outside
  // that set w is either not real-valued or not a hyperbola in k.
  [[nodiscard]] static std::optional<SviCrossingSlice>
  from_svi(const SviParams &p) noexcept;

  // eSSVI BACKBONE, via the exact reparametrisation obtained by pulling
  // theta*phi/2 out of eSSVI's w:
  //   a = theta*(1 - rho^2)/2,  b = theta*phi/2,
  //   m = -rho/phi,             sigma = sqrt(1 - rho^2)/phi.
  // Refuses a slice carrying a wing RESIDUAL layer (`resid_scale > 0`): the
  // residual is a clamped piecewise hinge-quadratic, not an SVI hyperbola, so
  // its crossings are NOT roots of the quartic and treating it as exact would
  // silently under-report. Also refuses an armed asymmetric rho-blend, whose
  // evaluator is NaN by construction.
  [[nodiscard]] static std::optional<SviCrossingSlice>
  from_essvi_backbone(const EssviParams &p) noexcept;

  [[nodiscard]] double w(double k) const noexcept;
  [[nodiscard]] double dw_dk(double k) const noexcept;
  // Asymptotic limsup w(k)/|k| on each wing; Lee bounds both by 2.
  [[nodiscard]] double wing_slope_right() const noexcept { return b_ * (1.0 + rho_); }
  [[nodiscard]] double wing_slope_left() const noexcept { return b_ * (1.0 - rho_); }
  [[nodiscard]] double T() const noexcept { return T_; }
  [[nodiscard]] double a() const noexcept { return a_; }
  [[nodiscard]] double b() const noexcept { return b_; }
  [[nodiscard]] double rho() const noexcept { return rho_; }
  [[nodiscard]] double m() const noexcept { return m_; }
  [[nodiscard]] double sigma() const noexcept { return sigma_; }

private:
  SviCrossingSlice() = default;
  double a_{};
  double b_{};
  double rho_{};
  double m_{};
  double sigma_{};
  double T_{};
};

// The log-moneyness points where two slices' total variances are equal.
// Capacity 4 with no allocation: P has degree <= 4, so four is the hard
// maximum and a heap vector would only add a failure mode. Entries [0, n) are
// finite and strictly ascending.
struct SliceCrossings {
  std::array<double, 4> k{};
  std::uint8_t n{};
};

// Exact crossings of `lo` and `hi`, spurious quartic roots removed. Total and
// allocation-free; a pair that never crosses yields n == 0.
[[nodiscard]] SliceCrossings
svi_pair_crossings(const SviCrossingSlice &lo, const SviCrossingSlice &hi) noexcept;

// One maximal interval on which w_lo > w_hi — a calendar violation when `lo` is
// the shorter maturity. `k_lo`/`k_hi` are the bounding crossings, infinite on
// the unbounded tails. `k_witness` is a finite interior point and `slack` is
// `w_lo - w_hi > 0` there.
//
// CONTRACT: the DECOMPOSITION is exact — no violating interval is missed and
// none is invented. `slack` is a witness value sampled inside the interval,
// hence a LOWER BOUND on the supremum of the deficit rather than the supremum
// itself: locating that would need the roots of D', which is not a polynomial
// system of workable degree. Callers gating on "is there a violation" get an
// exact answer; callers gating on a slack THRESHOLD must read it as a bound.
struct CalendarInterval {
  double k_lo{};
  double k_hi{};
  double k_witness{};
  double slack{};
};

// Every interval of R on which w_lo exceeds w_hi by more than `tol`. At most
// three intervals: four crossings cut R into five pieces of alternating sign.
[[nodiscard]] std::vector<CalendarInterval>
svi_pair_calendar_intervals(const SviCrossingSlice &lo, const SviCrossingSlice &hi,
                            double tol);

// ── Calendar / butterfly checks ──────────────────────────────────────────

// Check that total variance w(k, T) is monotone non-decreasing in T across
// consecutive slices over [k_min, k_max]. An empty result means "no calendar
// arbitrage". No-op (empty) when the surface carries fewer than two slices or
// `n_grid == 0`.
//
// TWO REGIMES, and which one ran is observable from the records themselves:
//
//  * EXACT (raw-SVI / SVI-MM surfaces, and eSSVI surfaces whose slices carry no
//    residual layer, with strictly increasing slice maturities). Crossings are
//    located by the quartic above and one violation is recorded per violating
//    INTERVAL, at a witness point inside it. `n_grid` is unused. This is what
//    changed in T4: the previous behaviour recorded one violation per violating
//    GRID POINT, so a count was a function of the sampling density and a
//    crossing narrower than one step was reported as no crossing at all.
//
//  * SAMPLED (everything else) — `n_grid` equispaced points in [k_min, k_max),
//    one violation per violating point, exactly as before. A point where
//    `VolSurface::w` is non-finite is SKIPPED: it is not a violation, and — the
//    defect this guards — not a BASELINE either. The last finite (w, T) stays
//    the baseline, so a violation straddling the unusable slice is still
//    reported, carrying the maturities of the two FINITE slices it spans.
//
// Callers reading only `empty()` (the `calendar_arb_free` flag and every repair
// gate in surface_parity.cpp) are unaffected in meaning and strictly better
// served: the exact regime cannot miss a narrow crossing. Callers reading
// `size()` as a count must read it as "violating intervals", not grid hits.
// There is still NO error path — every returned `false`/non-empty is a found
// crossing, never a failed check.
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_calendar(const VolSurface &s, double k_min, double k_max,
                   std::uint32_t n_grid);

// ── The certified band, stated once ──────────────────────────────────────
//
// A fixed |k| <= 0.60 window is delta-INHOMOGENEOUS across a board by about
// eleven orders of magnitude: at 1 week / 35 vol the forward call delta at
// k = +0.60 is zero to machine precision; at 2 years it is 0.084. "Violations
// inside the band" counted over a fixed k window therefore means something
// different on every slice of the same surface, and the number is not
// comparable between two tenors, two boards, or the two fit lanes.
//
// The band below is Wystup's 1%-99% DELTA band mapped into log-moneyness in
// closed form. With forward call delta = Phi(d_1) and
// d_1 = -k/sqrt(w) + sqrt(w)/2, the delta levels 1-p and p sit at
//     k = w/2 -/+ z*sqrt(w),      z = Phi^{-1}(1 - p),
// so the band is centred at k = w/2 with half-width z*sqrt(w). It scales with
// sqrt(w) exactly as the slice's own moneyness scale does, which is precisely
// what makes it comparable across tenors.
//
// `w` is the slice's ATM total variance. A skew-exact delta band is implicit in
// k — the w at the edge is the w being solved for — so pinning it at ATM keeps
// the band a closed-form monotone interval, which is the standard flat-vol
// reading of a delta quote and is what a trader means by "25 delta".
//
// `z` is a parameter rather than a tail probability so the number is auditable
// at the call site and this header needs no inverse normal CDF.
inline constexpr double kWystupTailQuantile = 2.3263478740408408;  // Phi^-1(0.99)

struct DeltaBand {
  double k_lo{};
  double k_hi{};
  [[nodiscard]] bool contains(double k) const noexcept {
    return k >= k_lo && k <= k_hi;
  }
  [[nodiscard]] bool usable() const noexcept { return k_hi > k_lo; }
};

// Empty (k_lo == k_hi == 0) for a non-positive or non-finite `w_atm`, which is
// not a band and must not silently read as one.
[[nodiscard]] DeltaBand delta_band_from_atm_w(double w_atm, double z) noexcept;

// Calendar violations split by whether they fall inside the certified band.
//
// OUT-OF-BAND IS NOT A DEFECT unless something evaluates there. It is reported
// rather than dropped because Roper's IV4 is quantified over all of R and
// anything computing Dupire local vol off this surface needs it there — so the
// unbounded check is kept as an internal invariant and surfaced as its own
// counter, instead of being folded into the number a gate reads.
//
// `certified_over_r()` is the honest half of that promise: the exact regime
// decides both tails, the sampled fallback cannot, and the caller can tell
// which it got instead of having to assume.
struct CalendarBandReport {
  std::vector<ArbViolation> in_band;
  std::vector<ArbViolation> out_of_band;
  std::uint32_t n_pairs_exact{};
  std::uint32_t n_pairs_sampled{};
  [[nodiscard]] bool certified_over_r() const noexcept {
    return n_pairs_sampled == 0;
  }
};

// Outer window the SAMPLED fallback can see at all. Matches the +/-3.0
// diagnostic grid the eSSVI lane already used, so a sampled out-of-band count
// stays comparable with what that lane reported before.
inline constexpr double kSampledOuterK = 3.0;

// One metric, one band, both fit lanes. The band for a slice PAIR is the
// intersection of the two slices' delta bands: a strike outside either slice's
// 1%-99% window is not tradeable on that pair, so a crossing there is not
// economically realisable and belongs in the out-of-band counter.
//
// No error path, matching `arb_check_calendar`: a surface with fewer than two
// slices reports two zero counters and two empty vectors.
[[nodiscard]] Result<CalendarBandReport>
arb_check_calendar_banded(const VolSurface &s, double z, std::uint32_t n_grid);

// Calendar check for a polymorphic CurveSurface (ConvexDense/SVI served path).
// Sample n_grid equispaced log-moneyness points in [k_min,k_max]; record a
// Calendar violation wherever total variance DECREASES across a consecutive
// (shorter-T, longer-T) slice pair, w_prev(k) - w_curr(k) > kCalendarTol.
// Empty result => calendar-arb-free. No-op (empty) for < 2 slices or n_grid==0.
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_calendar(const CurveSurface &s, double k_min, double k_max,
                   std::uint32_t n_grid);

// Independent per-curve butterfly check used after parameter-space calendar
// projection. Smooth parametric families retain the Lee/Roper density scan over
// the final IVolCurve values that downstream consumers receive. ConvexDense is
// represented by a convex piecewise-linear call price, whose density contains
// atoms at its knots and is not C2-smooth in total variance; it is checked in its
// native representation instead by requiring non-decreasing call-price slopes
// over the exact in-domain QP nodes plus the requested-domain endpoints. Price
// bounds remain the separate `arb_check_price_bounds` responsibility.
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_butterfly(const IVolCurve &curve, double k_min, double k_max,
                    std::uint32_t n_grid);

// Self-check confined to the served CALL-PRICE representation of each
// ConvexDense slice in `s` (oracle finding I-2). Every other arb_check_*
// entry samples total variance w(k, T) alone, which the convex dense fit's
// iv() clamps into Black's no-arb interval BEFORE forming (dense_slice.cpp),
// laundering a sub-intrinsic / super-forward fitted price into a merely
// near-zero or near-max vol — invisible to any w-space check. This samples
// each ConvexDense slice's OWN call_price(K) directly (most commonly
// exercised by the wing extrapolation on a one-sided, all-ITM or all-OTM,
// board — oracle finding M-7) and records a violation wherever it sits
// outside [discounted intrinsic, discounted forward] by more than a bare
// FP-roundoff tolerance (far below the fitting QP's node-level price_epsilon
// margin, which honest fits clear by orders of magnitude).
// Non-ConvexDense slices contribute nothing (their w-space checks above are
// sufficient). No-op (empty) for an empty surface, n_grid == 0, or
// k_max <= k_min.
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_price_bounds(const CurveSurface &s, double k_min, double k_max,
                       std::uint32_t n_grid);

// Per-slice Lee/Roper density positivity via finite-difference w'/w'' on the
// total surface variance. Empty result means "no butterfly arbitrage". No-op
// (empty) when the surface has no slices or `n_grid < 4`.
// @return InvalidArgument if `k_max <= k_min` (and the no-op guards did not
//         already short-circuit).
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_butterfly(const VolSurface &s, double k_min, double k_max,
                    std::uint32_t n_grid);

// Grid Durrleman g(k) >= 0 density-positivity check for ONE slice given a
// total-variance callable `w_of_k`. Uses the SAME finite-difference scheme as
// the surface-level `arb_check_butterfly` (which delegates to the shared
// file-local helper this front-ends), so the two agree pointwise on a slice.
// `T` labels the violation records only (`T1 == T2 == T`); the density math is
// scale-free in T. Empty result means "no butterfly arbitrage". No-op (empty)
// when `n_grid < 4`.
// @return InvalidArgument if `k_max <= k_min`.
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_butterfly_slice(const std::function<double(double)> &w_of_k, double T,
                          double k_min, double k_max, std::uint32_t n_grid);

// Convenience: run the calendar check then the butterfly check and
// concatenate their violations (calendar entries first). Propagates a
// butterfly InvalidArgument (k_max <= k_min) as the overall error, matching
// the C's `ats_arb_check_all`.
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_all(const VolSurface &s, double k_min, double k_max,
              std::uint32_t n_grid);

// ── Total-surface no-arb counts ──────────────────────────────────────────

// Per-kind violation counts (AtsVolSurface total-surface tally). Side-effect
// free.
struct TotalSurfaceArbCounts {
  std::uint32_t n_calendar{};
  std::uint32_t n_butterfly{};
};

// Count total-surface calendar and butterfly violations on a uniform k-grid,
// sampling `VolSurface::w` (which includes the wing residual). Unlike the
// butterfly check this never fails on `k_max <= k_min` — each block is guarded
// internally and simply contributes zero. Mirrors
// `ats_arb_check_total_surface_all`. Non-finite grid points are skipped on both
// axes, with the same baseline rule the calendar check documents above.
[[nodiscard]] Result<TotalSurfaceArbCounts>
arb_check_total_surface_all(const VolSurface &s, double k_min, double k_max,
                            std::uint32_t n_grid);

// ── SVI-MM (Martini-Mingone) admissibility ───────────────────────────────

// Closed-form admissibility tally for one raw-SVI slice.
struct SviMmAdmissibility {
  std::uint32_t n_violations{};  // count of breached inequalities (0 = admissible)
  double max_slack{};            // worst absolute slack across the breached set
};

// Verify the Martini-Mingone admissible-polytope inequalities on a raw-SVI
// slice (arXiv:2005.03340 §6.3 + Lee 2004):
//
//   1.  b > 0
//   2.  sigma > 0
//   3.  |rho| < 1
//   4.  a + b*sigma*sqrt(1 - rho^2) >= 0     (global w >= 0; Lemma 3.2)
//   5.  b * (1 + |rho|) <= 4 / T             (Lee wing-slope butterfly bound)
//
// A 1e-12 tolerance absorbs FP noise on a boundary-touching iterate. Inequality
// (4) is skipped when (3) already fired (the radical is not real). Inequality
// (5) is skipped for a non-positive `T`. Closed-form — no FD sampling.
[[nodiscard]] SviMmAdmissibility
arb_check_butterfly_svi_mm(const SviParams &slice, double T) noexcept;

// Surface-level walker: accumulate per-slice admissibility over an SVI-MM
// surface. Ok with a zeroed tally for surfaces that are not SVI-MM (this walker
// only enforces the polytope for the SVI-MM tag; the eSSVI cube enforces a
// different, tighter set by construction) — mirrors the C no-op.
//
// NOTE (Task C2.5): raw-SVI slices are NOT admissible-by-construction at the
// parametrization level, but every raw-SVI slice SERVED through
// `fit_slice_curve(VolCurveKind::Svi)` is now validated at the serving seam —
// `arb_check_butterfly_svi_mm` is run on the fitted slice, which is projected
// onto the Mingone polytope (`svi_project_mm`) and rejected if it still
// violates. So a served raw-SVI slice carries the closed-form admissibility
// guarantee even though this pure-parametrization walker still no-ops on the
// plain `Svi` tag.
[[nodiscard]] Result<SviMmAdmissibility>
arb_check_butterfly_svi_mm_surface(const VolSurface &s);

// Diagnostics from a shared-k pair projection. `scale` is the cumulative
// multiplicative level change (SVI's additive a-shift is reported as 1), and
// `max_deficit_before` is in total-variance units.
struct CalendarPairProjection {
  std::uint32_t passes{};
  double scale{1.0};
  double max_deficit_before{};
};

// Fidelity budget for every calendar LEVEL repair below (the eSSVI / SVI / C8
// pair projections and the surface-level theta / `a` projectors): a repair may
// raise a slice's ATM total variance by at most this fraction of its pre-repair
// value (`kCalendarRepairMinBudgetW` is an absolute floor guarding degenerate
// near-zero slices). A crossing that needs more than that is not repaired by a
// level shift — it is FABRICATED: the served slice's level no longer resembles
// the quotes it was fit to, and it silently passes every downstream geometry
// gate because the shift is shape-preserving. Measured on the sp100-2026
// database, extrapolated-wing deficits demanded +83%..+263% ATM shifts on XOM
// mid-tenor slices, turning a 29-vol ATM into a served 53. Beyond budget the
// projection returns Unavailable and leaves the slice/surface untouched
// (transactional); callers drop the slice or walk their family ladder, both of
// which are honest outcomes.
inline constexpr double kCalendarRepairMaxAtmShiftFrac = 0.10;
inline constexpr double kCalendarRepairMinBudgetW = 1.0e-6;

// Project one longer-dated parametric candidate above an arbitrary previously
// admitted w(k) curve on a shared lattice. Each projection stays in the model's
// shape-safe parameterization; callers must then run the independent butterfly
// checker above before publication. Failure to close the calendar gap returns
// Unavailable rather than an unchecked candidate — including a gap whose
// closure would exceed the fidelity budget above; on EVERY non-Ok return
// `current` is exactly what was passed in (transactional).
[[nodiscard]] Result<CalendarPairProjection> arb_project_calendar_essvi_pair(
    EssviParams &current, const std::function<double(double)> &w_prev,
    double k_min, double k_max, std::uint32_t n_grid);
[[nodiscard]] Result<CalendarPairProjection> arb_project_calendar_svi_pair(
    SviParams &current, const std::function<double(double)> &w_prev,
    double k_min, double k_max, std::uint32_t n_grid);
[[nodiscard]] Result<CalendarPairProjection> arb_project_calendar_c8_pair(
    C8Params &current, const std::function<double(double)> &w_prev,
    double k_min, double k_max, std::uint32_t n_grid);

// ── Calendar-spread arb projection / repair (post-fit, mutating) ─────────

// SVI per-slice calendar projection: walk consecutive (T_{i-1}, T_i) over a
// uniform k-grid and minimally increase the longer-T slice's `a` (the smallest
// shape-preserving parallel shift) until w(k, T_{i-1}) <= w(k, T_i) at every
// sampled k. Iterative with 4x grid refinement per pass (bounded at 6 passes /
// 65536 grid points). Idempotent on an already-monotone surface; no-op for a
// surface that is not SVI / SVI-MM parametrized. TRANSACTIONAL: runs on a
// private copy; on any non-Ok return `s` is untouched.
// @return InvalidArgument if `k_max <= k_min` (after the no-op guards);
//         Unavailable if any slice's cumulative shift would exceed the
//         fidelity budget (`kCalendarRepairMaxAtmShiftFrac`).
[[nodiscard]] Status arb_project_calendar_svi(VolSurface &s, double k_min,
                                              double k_max,
                                              std::uint32_t n_grid);

// eSSVI calendar projection on the BACKBONE only: bump theta_i up by the
// minimum multiplier max_k(w_back_{i-1} / w_back_i) that restores backbone
// monotonicity, then re-clamp phi_i to the butterfly ceiling at the new theta.
// Same 6-pass / grid-refine loop. No-op for a non-eSSVI surface. TRANSACTIONAL:
// runs on a private copy; on any non-Ok return `s` is untouched.
// @return InvalidArgument if `k_max <= k_min` (after the no-op guards);
//         Unavailable if any slice's cumulative scale would exceed the
//         fidelity budget (`kCalendarRepairMaxAtmShiftFrac`).
[[nodiscard]] Status arb_project_calendar_essvi(VolSurface &s, double k_min,
                                                double k_max,
                                                std::uint32_t n_grid);

// Repair total-surface calendar arb by damping residuals on the lower-T slice
// of any residual-induced crossing: bisect a multiplicative damper
// alpha in [0, 1] on the lower slice's residual coefficients until
// monotonicity holds (alpha = 0 collapses it to the backbone, which
// `arb_project_calendar_essvi` makes monotone against the next slice's
// BACKBONE). Up to 5 outer sweeps. No-op for a non-eSSVI surface.
//
// TRANSACTIONAL: the sweeps run on a private copy of the slices, so on ANY
// non-Ok return `s` is exactly what was passed in.
// @return InvalidArgument if `k_max <= k_min` (after the no-op guards).
// @return Unavailable if the alpha = 0 endpoint does not repair some pair, i.e.
//         the lower slice's backbone still crosses the next slice's TOTAL
//         variance. Damping the lower slice cannot fix that pair.
//
//         Running `arb_project_calendar_essvi` over the same grid first is
//         NECESSARY BUT NOT SUFFICIENT to rule this out: that projection compares
//         backbone against backbone, while this test compares the lower backbone
//         against the upper TOTAL. Residual coefficients are unconstrained in
//         sign, so a NEGATIVE residual on the higher-T slice puts its total below
//         its backbone and can trip this status even after a successful
//         projection. Callers that must not abort on it (the `run_surface_parity`
//         repair modes propagate it) have to map it to a counted/reported
//         outcome — the surface is then honestly un-repaired rather than
//         silently stripped of the lower slice's residual layer.
[[nodiscard]] Status arb_repair_calendar_residual(VolSurface &s, double k_min,
                                                  double k_max,
                                                  std::uint32_t n_grid);

// ── Quote-level pre-fit filters ──────────────────────────────────────────

// Profile-aware quote-filter thresholds (ports AtsVolFilterOpts). The member
// initializers are the conservative `ordinary_single_name` defaults, so a
// default-constructed value equals `filter_default_opts()`.
struct FilterOpts {
  std::int64_t stale_seconds{30};  // quotes older than now - this are STALE
  std::int64_t now_ts_ns{0};       // staleness reference instant; 0 disables it
  double wide_spread_pct{1.50};    // (ask-bid)/max(mid, wide_min_mid) above => WIDE
  double wide_min_mid{0.05};       // floor on the spread denominator
  double penny_floor{0.05};        // bid below this (USD) => PENNY
  double min_vega_filter{1.0e-5};  // vega below this => LOW_VEGA (needs vegas[])
};

// The conservative ordinary-single-name defaults (`ats_vol_filter_default_opts`).
[[nodiscard]] FilterOpts filter_default_opts() noexcept;

// Extended batch quote filter (ports `ats_arb_filter_quotes_ex`). Reads the
// batch's bid/ask/timestamp/flag columns and stamps `QuoteFlag` bits into
// `flags_out`, which is seeded from `batch.flags` so caller-supplied bits
// (e.g. an upstream HALTED) are preserved. An empty `vegas` span skips the
// LOW_VEGA check. Returns the number of quotes whose flag word strictly gained
// a bit during the pass.
// @return InvalidArgument if the batch bid/ask columns differ in length, if
//         `flags_out` (or a non-empty flags/ts_ns/vegas column) is shorter
//         than the batch.
[[nodiscard]] Result<std::uint32_t>
arb_filter_quotes_ex(const QuoteBatch &batch, const FilterOpts &opts,
                     std::span<const double> vegas,
                     std::span<std::uint8_t> flags_out);

// Chain-level pre-fit filter (ports `ats_vol_prefit_filter_underlier`). Walks
// every chain x strike x side in `under` and stamps LOCKED / CROSSED / PENNY /
// WIDE_SPREAD / STALE bits into `chain.flags[]` in place using the same
// predicate stack as `arb_filter_quotes_ex` (LOW_VEGA is skipped — it needs a
// per-quote IV inversion the calibrator handles later). `now_ns == 0` falls
// back to `opts.now_ts_ns`. `curves` is reserved (the predicates need no curve
// context; matches the C's `(void)cs`). Returns the number of quotes whose
// flag word strictly gained a bit.
[[nodiscard]] Result<std::uint32_t>
prefit_filter_underlier(Underlying &under, const CurveSet &curves,
                        const FilterOpts &opts, std::int64_t now_ns);

}  // namespace atx::vol
