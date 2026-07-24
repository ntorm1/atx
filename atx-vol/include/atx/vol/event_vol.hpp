#pragma once

// Earnings event-variance model, following SpiderRock LiveVolSurfaces /
// FLEXVolInterpolation (Connect 8.6.6.3 Analytics).
//
// ## Model (verbatim)
//
//   w_total(T) ≡ σ_T²·T = n·eMove² + σ_C²·T
//
// where `n` is the number of scheduled earnings events strictly between "now"
// and an option's expiry, `eMove` is the (flat, per-event) instantaneous
// move vol contributed by one earnings print, and `σ_C` is the *censored*
// diffusive vol — the smooth, event-free component of the total variance.
// Rearranged, the censored total variance is
//
//   w_censored = w_total − n·eMove²
//
// FLEX recombination re-adds the event contribution when serving a vol at a
// given T:
//
//   atmVol = sqrt(atmCen² + n·eMove²/T)
//
// and censored interpolation ACROSS EXPIRIES happens entirely in censored
// variance space (which is assumed piecewise-linear in T, same as the
// event-free surface elsewhere in atx-vol) — the event variance for the
// QUERY expiry's own event count is re-added only after that interpolation,
// never blended between the bracketing slices' event counts.
//
// ## Concepts
//
//   - `EventSchedule`: an immutable, sorted set of earnings-announcement
//     instants (epoch ns). `count_between(now, expiry)` counts events in the
//     half-open-below / closed-above interval `(now_ns, expiry_ns]` — an
//     event exactly at "now" has already happened and is excluded; an event
//     exactly at expiry still moves that expiry's contract and is included.
//   - `censored_total_variance`: strips `n·eMove²` out of a total variance,
//     floored at `kWCenFloor` so a downstream `sqrt`/division never sees a
//     non-positive censored variance even when `n·eMove²` overshoots `w_total`
//     (an ill-conditioned/inconsistent input rather than a real market state).
//   - `event_recombined_vol`: the FLEX recombination formula, direct.
//   - `implied_emove`: SpiderRock calibrates `eMove` from two expiries that
//     straddle an earnings date (different `n`) under the assumption both
//     share one common censored instantaneous variance `σ_C²`:
//         (w1 − n1·e²)/T1 = (w2 − n2·e²)/T2
//     Solving for `e²`:
//         e² = (w1·T2 − w2·T1) / (n1·T2 − n2·T1)
//     The denominator vanishes exactly when `n1/T1 == n2/T2` (the two
//     expiries carry event density in the same proportion as their time —
//     no earnings-driven information distinguishes them, so `e` is not
//     identified from this pair).
//   - `event_aware_w`: the FLEX per-query recombination — censor both
//     bracketing slices, linearly interpolate the censored variance in T,
//     re-add the QUERY expiry's own `n_query·eMove²`.
//
// ## PORT NOTE — error-code mapping (spec used gRPC-style names)
//
// The design brief for this module (ported from SpiderRock analytics docs)
// specifies `InvalidArgument` for malformed inputs to `implied_emove` and a
// gRPC-style `FailedPrecondition` for the case where the *solved* `e²` comes
// out negative (a physically-impossible instantaneous move variance, given
// otherwise well-formed inputs). This codebase's `atx::core::ErrorCode` has
// no `FailedPrecondition` enumerator. Following the established precedent
// elsewhere in atx-vol for the distinction between "bad inputs" vs. "a
// computed/solved quantity landed outside its valid domain" — we map
// non-finite inputs to `InvalidArgument` (following `andersen_lake`,
// `american.cpp` 1262-1263) and a negative-`e²` to `OutOfRange` (by analogy
// to `american_iv.cpp`'s out-of-band price check, 108-113).
//
// ## PORT NOTE — e² clamp window
//
// "e² in [−eps, 0] clamps to 0" (brief, verbatim) does not name `eps`. This
// header defines `kEmoveSqClampEps = 1e-9` as that window: a solved `e²`
// landing in `[-kEmoveSqClampEps, 0)` is floating-point noise around an
// exact "no event move" (`e == 0`) solution and clamps to `0.0`; anything
// more negative than that is treated as a genuinely inconsistent pair of
// inputs (`ErrorCode::OutOfRange`). `1e-9` sits ~9 orders of magnitude below
// a typical `e²` (an eMove of 1% already gives `e² = 1e-4`), so it absorbs
// double-precision cancellation noise without masking a real inconsistency.
//
// ## Self-review notes (documented chosen behavior)
//
//   - `censored_total_variance`/`event_recombined_vol`: NaN in any argument
//     propagates to NaN out (IEEE-754 comparisons with NaN are false, so the
//     floor/domain checks fall through to the natural NaN result rather than
//     substituting a floor or sentinel value).
//   - `event_aware_w` has no `Result` return (matches the brief's `noexcept`
//     free-function signature) and therefore performs NO cross-consistency
//     validation of its inputs: it does not check `T_lo < T_hi`, does not
//     clamp `T_query` to `[T_lo, T_hi]` (a query outside the bracket
//     extrapolates the linear-in-censored-variance formula rather than being
//     rejected or clamped), and does not check `n_query` against `n_lo`/
//     `n_hi` (a caller-supplied `n_query` inconsistent with the bracketing
//     counts is used as given — this module has no way to know the "right"
//     count for an arbitrary query expiry; that is `EventSchedule`'s job).
//     `T_lo == T_hi` is not special-cased: the interpolation weight becomes
//     `0/0` (NaN) or `x/0` (±inf), which then propagates — callers must pass
//     distinct bracketing expiries.
//   - `EventSchedule`'s constructor sorts but does not de-duplicate (unlike
//     `VolTimeCalendar`, which explicitly de-duplicates holiday dates): a
//     repeated timestamp in the input is a caller data-quality issue, not
//     this type's to silently absorb, and duplicate timestamps still count
//     correctly (once each) under `count_between`'s `std::upper_bound` scan.
//
// ## Thread-safety
//
// Every free function here is a pure function of its arguments — safe to
// call concurrently from any number of threads. `EventSchedule` is an
// immutable value once constructed (its one mutable step, sorting, happens
// synchronously in the constructor) — safe to read concurrently thereafter.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/earnings_term_fit.hpp" // CensorObsInput, EarningsFitConfig, EmoveFitCode
#include "atx/vol/types.hpp"            // Result, ErrorCode

namespace atx::vol {

// Floor on a censored total variance (`censored_total_variance`'s return),
// so a downstream `sqrt`/division never observes a non-positive value.
inline constexpr double kWCenFloor = 1e-10;

// Clamp window on a solved `e²` in `implied_emove`: `e²` in
// `[-kEmoveSqClampEps, 0)` clamps to exactly `0.0` rather than reporting
// `ErrorCode::OutOfRange`. See the PORT NOTE above.
inline constexpr double kEmoveSqClampEps = 1e-9;

// Immutable, sorted (not de-duplicated) schedule of earnings-announcement
// instants (epoch ns).
class EventSchedule {
 public:
  // Sorts `event_ts_ns` in place (ascending); does not de-duplicate.
  explicit EventSchedule(std::vector<std::int64_t> event_ts_ns);

  // Count of events in `(now_ns, expiry_ns]`: an event exactly at `now_ns` is
  // excluded (already happened); an event exactly at `expiry_ns` is included
  // (still moves that expiry's contract). Returns 0 if `expiry_ns < now_ns`.
  [[nodiscard]] std::size_t count_between(std::int64_t now_ns,
                                          std::int64_t expiry_ns) const noexcept;

  // The full sorted event list.
  [[nodiscard]] std::span<const std::int64_t> events() const noexcept;

 private:
  std::vector<std::int64_t> events_;  // sorted, ascending
};

// w_censored = w_total − n·eMove², floored at `kWCenFloor`. NaN in any
// argument => NaN out (see PORT NOTE above on NaN propagation).
//
// @param w_total   total ATM variance at this expiry (σ_T²·T)
// @param n_events  earnings events counted between now and this expiry
// @param emove     per-event instantaneous move vol (eMove)
// @return          censored (event-free) variance, >= kWCenFloor (or NaN)
[[nodiscard]] double censored_total_variance(double w_total, std::size_t n_events,
                                             double emove) noexcept;

// FLEX recombination: sqrt(atm_cen² + n·emove²/T). `T <= 0` (or non-finite)
// => NaN.
//
// @param atm_cen   censored (event-free) ATM vol at this expiry
// @param T         year-fraction to expiry; must be > 0
// @param n_events  earnings events counted between now and this expiry
// @param emove     per-event instantaneous move vol (eMove)
// @return          recombined ATM vol, or NaN if T <= 0 / non-finite
[[nodiscard]] double event_recombined_vol(double atm_cen, double T, std::size_t n_events,
                                          double emove) noexcept;

// Implied per-event move from two expiries with different event counts,
// assuming a common censored instantaneous variance:
//   (w1 − n1·e²)/T1 = (w2 − n2·e²)/T2  =>  e² = (w1·T2 − w2·T1)/(n1·T2 − n2·T1)
//
// @param w1,T1,n1  total variance / year-fraction / event count, expiry 1
// @param w2,T2,n2  total variance / year-fraction / event count, expiry 2
// @return  Ok(eMove), eMove >= 0. Err(InvalidArgument) if T1 or T2 <= 0,
//          T1 == T2, n1·T2 == n2·T1 (no identification — see the module
//          comment), or any of w1/T1/w2/T2 is non-finite. Err(OutOfRange) if
//          the solved e² is negative by more than kEmoveSqClampEps (see the
//          PORT NOTE above for both the OutOfRange mapping and the clamp
//          window).
[[nodiscard]] Result<double> implied_emove(double w1, double T1, std::size_t n1,
                                           double w2, double T2, std::size_t n2);

// Events counted between `now_ns` and the maturity a Calendar365
// year-fraction `T` from now — THE single definition of the "how many
// scheduled events before this maturity" question every event-vol consumer
// asks. Composes `EventSchedule::count_between` (the `(now, expiry]`
// boundary semantics above) with `ns_from_year_fraction` (vol_time.hpp, the
// Calendar365 inverse of `time_to_expiry_years`): callers on the fit/serve
// path hold year-fraction T's, not absolute expiry instants (an arbitrary
// interpolated query T never had one, and the fitted eSSVI slices do not
// retain theirs — see the callers' own docs), so the maturity instant is
// synthesized as `now_ns + round(T * kCalendarYearNs)`. Used by
// `w_on_inserted_slice`'s event-aware blend (projection.cpp) and
// `VolaSession::build`'s implied-eMove solve (session.cpp).
//
// @param events  the schedule to count against
// @param now_ns  valuation instant, epoch nanoseconds (UTC)
// @param T       Calendar365 year-fraction from `now_ns` to the maturity
// @return        events in `(now_ns, now_ns + round(T·year)]`
[[nodiscard]] std::size_t count_events_at(const EventSchedule& events,
                                          std::int64_t now_ns,
                                          double T) noexcept;

// Event-aware total variance at T_query: censors both bracketing slices,
// linearly interpolates the censored variance in T, then re-adds
// n_query·emove² for the query expiry's own event count. Falls back to
// plain linear-in-w interpolation (no censoring, no re-add) when
// `emove <= 0` or `n_lo == n_hi == n_query == 0` — see the module comment
// and self-review notes for why this is a distinct branch rather than a
// mathematical special case of the general formula.
//
// No cross-consistency validation is performed (see self-review notes):
// T_query outside [T_lo, T_hi] extrapolates; T_lo == T_hi is undefined
// (NaN/inf propagates); n_query is used as given.
//
// @param w_lo,T_lo,n_lo  total variance / year-fraction / event count, low slice
// @param w_hi,T_hi,n_hi  total variance / year-fraction / event count, high slice
// @param T_query,n_query year-fraction / event count at the query expiry
// @param emove           per-event instantaneous move vol (eMove)
// @return  event-aware total variance at T_query
[[nodiscard]] double event_aware_w(double w_lo, double T_lo, std::size_t n_lo,
                                   double w_hi, double T_hi, std::size_t n_hi,
                                   double T_query, std::size_t n_query,
                                   double emove) noexcept;

// ── E3a / AN-P1-3: the production eMove solve ───────────────────────────────
//
// `implied_emove` above identifies eMove from exactly TWO expiries, which forces
// the assumption that ONE flat censored instantaneous variance spans the whole
// bracket. It does not, and the error is not benign: with n1 = 0 and n2 = 1,
//
//     e²_two-pillar = eMove² + T2·(σ_C(T2)² − σ_C(T1)²)
//
// so every bit of censored TERM STRUCTURE inside the bracket aliases directly
// into eMove². The wider the bracket the worse it gets — and the bracket is
// widest in exactly the case that matters, when no near expiry spans the event.
// The atmCen convention sweep measured AAPL at eMove 0.0567 against a truth of
// 0.0208: +173%.
//
// `fit_earnings_term` (earnings_term_fit.hpp) solves the identified problem —
// {eMove, st, lt, decay} over ALL usable expiries at once — and has been
// available, well-guarded and unit-tested, but wired only into the
// earnings-repro pipeline. This is the seam that promotes it to production.

// Which solve produced an eMove.
enum class EmoveMethod : std::uint8_t {
  TwoPillar = 0, // implied_emove on the first bracketing pillar pair
  Joint = 1,     // fit_earnings_term over ALL usable pillars
};

// Minimum usable pillar count before the joint fit is even attempted. The fit
// has FOUR free parameters {eMove, st, lt, decay}; four points can be
// interpolated exactly, leaving the residual — which is the entire signal the
// outer eMove search ranks on — identically zero and the optimum arbitrary.
// Five is the first count with a degree of freedom to spare.
inline constexpr std::size_t kJointMinPillars = 5;

// An eMove plus how it was obtained. `fit_code` / `fit_error` / `expiry_count`
// describe the joint fit and are only meaningful when `method == Joint`.
struct EmoveSolution {
  double emove{0.0};
  EmoveMethod method{EmoveMethod::TwoPillar};
  EmoveFitCode fit_code{EmoveFitCode::Ok};
  double fit_error{0.0};
  std::size_t expiry_count{0};
};

// Joint eMove over ALL usable pillars, with a two-pillar fallback.
//
// Observations with a non-finite / non-positive `T` or `w_dirty` are DROPPED
// rather than fatal: one unusable pillar must not cost the whole solve. `obs`
// need not be sorted; the fallback sorts by T internally.
//
// The joint fit is attempted only when the problem is actually IDENTIFIED:
//   * at least `kJointMinPillars` usable observations;
//   * at least two DISTINCT event counts among them (a constant n carries no
//     information about eMove at all — every candidate shifts the whole
//     censored curve by the same amount, which the 3-parameter term curve
//     simply absorbs); and
//   * at least two event-bearing (n > 0) observations, matching
//     `fit_earnings_term`'s own degeneracy rule.
// Otherwise — and whenever the joint fit errors, or returns a non-finite eMove,
// or reports a degenerate outcome (`Degenerate`, or `CenterFlat` with events
// actually present) — this falls back to `implied_emove` on the first
// ascending-T adjacent pair whose event count rises, i.e. exactly the pre-E3a
// behaviour, and reports `EmoveMethod::TwoPillar` so the caller can tell which
// answer it got.
//
// @param obs  per-expiry dirty ATM total variance / year-fraction / event count
// @param cfg  joint-fit knobs (search bracket, iteration cap, censoring floor)
// @return Ok(EmoveSolution) with `emove >= 0`.
//         Err(InvalidArgument) if fewer than two observations are usable.
//         Err(NotFound) if the joint fit was unavailable and no adjacent pair
//         brackets an event.
[[nodiscard]] Result<EmoveSolution> implied_emove_joint(std::span<const CensorObsInput> obs,
                                                        const EarningsFitConfig &cfg = {});

}  // namespace atx::vol
