#pragma once

// Earnings-censored ATM vol term-fit core, following SpiderRock LiveVolSurfaces
// (Connect 8.6.6.3 Analytics) atmCen reproduction. This header lays down the
// value vocabulary shared by every task in the reproduction sprint, plus the
// implemented pieces: the per-expiry censoring primitive (`censored_atm_vol`,
// Task 1), the censored term-curve fit (`TermCurve` /
// `fit_term_curve_for_emove` / `term_curve_value`, Task 3), and the joint
// {eMove, st, lt, decay} fit (`fit_earnings_term`, Task 4) -- a bounded
// golden-section search over `eMove` wrapping `fit_term_curve_for_emove` as
// its inner solve, populating `EarningsTermFit`. The SR tenor grid lives in
// `sr_tenor_grid.hpp` (Task 2).
//
// ## Model
//
// `atx/vol/event_vol.hpp` already defines the SpiderRock decomposition
// `w_total(T) = n*eMove^2 + sigma_C^2*T` and its censoring primitive
// `censored_total_variance` (w_total - n*eMove^2, floored at `kWCenFloor`).
// `censored_atm_vol` here is the natural next step: convert that censored
// TOTAL VARIANCE into a censored ATM VOL for one listed expiry, given a
// candidate `eMove` --
//
//   sigma_cen(T_i) = sqrt(max(w_cen_i, floor) / T_i)
//   w_cen_i        = w_dirty_i - n_i * eMove^2   (censored_total_variance)
//
// `fit_earnings_term` (Task 4) performs that search: it computes
// `censored_atm_vol` at every listed expiry per candidate `eMove` (via
// `fit_term_curve_for_emove`) and scores how well the resulting censored
// points fit the parametric term curve, minimizing that score's RMS residual
// over `eMove` itself.
//
// ## Types
//
//   - `CensorObsInput`: one listed expiry's raw (dirty) ATM total variance,
//     year-fraction, and scheduled-event count -- the exact inputs
//     `censored_atm_vol` needs to strip the event contribution back out.
//   - `EmoveFitCode`: outcome tag for `fit_earnings_term`'s eMove search --
//     declared alongside `EarningsTermFit::fit_code` (its only field that
//     needs the type) so no later task invents its own.
//   - `SrTenorGrid`: forward-declared only. The SR-native tenor grid (12
//     year-fractions with a fixed rolling schedule) is Task 2's type; nothing
//     here depends on its definition.
//   - `EarningsFitConfig`: knobs for the fit -- the `eMove` search bounds
//     (`emove_lo`/`emove_hi`) and iteration cap (`max_iters`) `fit_earnings_term`
//     (Task 4) searches over, the shared `wcen_floor` (mirrors
//     `event_vol.hpp`'s `kWCenFloor` semantics; a caller-supplied value need
//     not equal it) both it and `fit_term_curve_for_emove` (Task 3) read, and
//     the tenor grid `fit_earnings_term` samples the fitted parametric
//     curve's secondary `atm_cen` output at.
//   - `TermCurve`: the censored term-curve fit's result for ONE fixed `eMove`
//     -- solved `{st,lt,decay}` plus the fit's RMS residual. See
//     `fit_term_curve_for_emove`'s own doc comment for the fit contract and
//     `term_curve_value` for evaluating the fitted curve at a query `T`.
//   - `EarningsTermFit`: `fit_earnings_term`'s (Task 4) result vocabulary --
//     the solved `eMove` (the OUTER search `fit_term_curve_for_emove` itself
//     does not perform), the `TermCurve` parameters at that solved `eMove`,
//     the per-grid-tenor sampled `atm_cen` curve, fit residual, and outcome
//     code.
//
// ## Self-review notes (documented chosen behavior)
//
//   - `censored_atm_vol` floors TWICE: once inside `censored_total_variance`
//     (always at the fixed `kWCenFloor = 1e-10`) and once more here against
//     the caller-supplied `wcen_floor`. This is intentional, not redundant --
//     `censored_total_variance`'s floor is a hardcoded numerical guard against
//     a non-positive censored variance ever reaching a `sqrt`; this function's
//     own `wcen_floor` parameter lets a caller (e.g. the eMove search) impose
//     a STRICTER floor than the module default without changing
//     `event_vol.hpp`'s own constant. Passing `wcen_floor == kWCenFloor` (the
//     `EarningsFitConfig` default) makes the second floor a no-op.
//   - NaN/non-finite propagation follows `event_vol.hpp`'s convention: no
//     input validation is performed here (`o.T <= 0`, non-finite `o.w_dirty`,
//     etc. are the caller's contract to uphold -- see `CensorObsInput`'s field
//     docs), and the function is `noexcept` with no `Result` return, matching
//     `censored_total_variance`/`event_recombined_vol`'s free-function-math
//     shape rather than `implied_emove`'s validated/`Result` shape.
//   - `fit_term_curve_for_emove` (Task 3): for a FIXED `decay`, `atmCen(T) =
//     lt + A*exp(-decay*T)` (A = st-lt) is LINEAR in `{lt,A}`, so the fit is a
//     1-D search over `decay` (log-spaced grid + golden-section refine,
//     `decay` bounded strictly positive) wrapping a closed-form 2x2 linear
//     least-squares solve at each candidate -- never a general nonlinear
//     optimizer over all three parameters at once. The per-decay 2x2 solve is
//     guarded against a singular/ill-conditioned Gram matrix (e.g. every
//     observation at the same `T`, so `exp(-decay*T)` is one repeated
//     constant and the `{1,b}` basis collapses onto a single column; or
//     `decay` large enough that every `b_i` underflows to the same ~0) by
//     comparing `|det|` against the Gram matrix's own diagonal scale rather
//     than a fixed absolute epsilon -- on a hit it falls back to the flat
//     mean fit (`A=0`, `lt` = mean of the censored points) instead of
//     dividing by a near-zero determinant. This also transparently covers the
//     zero-observation case (the Gram matrix is identically the zero matrix),
//     so `fit_term_curve_for_emove` needs no separate empty-span branch.
//     `rms_resid` is UNWEIGHTED (matches `EarningsFitConfig`'s "uniform in
//     v1" LSQ-weighting note) -- every observation counts equally regardless
//     of `T` or `n`.
//
//   - `fit_earnings_term` (Task 4): the OUTER search minimizes
//     `fit_term_curve_for_emove(obs, emove, cfg).rms_resid` over `emove in
//     [cfg.emove_lo, cfg.emove_hi]` -- never a general nonlinear optimizer
//     over `{emove,st,lt,decay}` jointly (the INNER `{st,lt,decay}` solve at
//     each candidate `emove` is Task 3's own decay-search + 2x2 LSQ,
//     unchanged here). It is a coarse LINEAR grid across the bracket
//     (finds a competitive neighborhood) followed by a golden-section
//     refine of that neighborhood, capped at `cfg.max_iters` refine steps --
//     the same "grid finds the region, golden-section refines it" structure
//     `fit_term_curve_for_emove`'s own decay search already uses, extended
//     to this outer layer because a bare two-point golden section over the
//     WHOLE bracket has no defense against a non-unimodal objective (see
//     below) when its two starting interior points both land on the wrong
//     side of the true optimum -- it would silently converge to the WRONG
//     local minimum with no signal anything went wrong. The grid stage is
//     what makes "golden-section finds *a* local minimum in the bracket,
//     which matches SpiderRock's own bracketed-fit behavior and is
//     acceptable" (this task's own brief) actually land on the RIGHT local
//     minimum in practice, not an arbitrary one.
//
//     Real censored-vol term structures are not guaranteed unimodal in
//     `emove` for a very concrete reason this module can name precisely:
//     `censored_atm_vol`'s floor (`kWCenFloor`, `event_vol.hpp`) exists to
//     guard a downstream `sqrt` against a non-positive censored variance,
//     not to signal a good fit -- but a large enough candidate `emove` can
//     clamp EVERY event-bearing observation to the same near-zero floored
//     value, which a flexible 3-parameter term curve then fits deceptively
//     well (a spuriously LOW `rms_resid` that reflects the clamp, not the
//     model). `search_emove`'s `outer_objective` treats any candidate that
//     clamps even one event-bearing observation as `+infinity` for ranking
//     purposes specifically to keep the search from mistaking that artifact
//     for the true optimum -- `TermCurve::rms_resid` itself (what
//     `EarningsTermFit::fit_error` ultimately reports) is untouched by this;
//     it is purely a comparison-ranking layer the outer search applies on
//     top of Task 3's own unmodified metric.
//
//     `EmoveFitCode::CenterFlat` evidence comes from the SAME coarse grid:
//     the worst FINITE (non-clamped) objective seen anywhere in it, compared
//     against the search's own best finding -- a real, identified minimum
//     improves substantially on that generic/arbitrary-point scale; an
//     under-identified objective (too few observations relative to the term
//     curve's own 3 free parameters, or an event-count pattern that does not
//     distinguish emove from the curve shape) does not. This measures
//     identification strength directly from the data rather than
//     pattern-matching structurally on `obs[i].n` (e.g. "are all counts
//     equal") -- see `earnings_term_fit.cpp`'s `search_emove`/
//     `classify_fit_code` for the full priority order between `MaxSteps`/
//     `CenterFlat`/`LeftBound`/`RightBound`/`Minimum`.
//
// ## Thread-safety
//
// `censored_atm_vol`, `fit_term_curve_for_emove`, `term_curve_value`, and
// `fit_earnings_term` are pure functions of their arguments -- safe to call
// concurrently from any number of threads. Every type in this header is a
// plain value type with no shared state.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/types.hpp" // Result, ErrorCode (fit_earnings_term, Task 4)

namespace atx::vol {

// One listed expiry's raw (dirty, event-inclusive) ATM total variance plus the
// inputs needed to censor it: the scheduled earnings-event count between now
// and this expiry (`EventSchedule::count_between`/`count_events_at`), and the
// expiry's year-fraction (convention already applied -- see `vol_time.hpp`).
struct CensorObsInput {
  double T{};       // year-fraction to expiry, convention already applied (>0)
  double w_dirty{}; // total ATM variance = sigma_atm^2 * T (>0)
  std::size_t n{};  // scheduled earnings events before this expiry
};

// Result of `fit_term_curve_for_emove`: the censored term-curve fit
// `atmCen(T) = lt + (st - lt)*exp(-decay*T)` for ONE fixed candidate `eMove`.
// `st` is the short-term/T->0 anchor (`term_curve_value` at T=0 equals `st`),
// `lt` the long-term/T->infinity anchor, `decay` the (searched, > 0) mean-
// reversion rate, and `rms_resid` the fit's unweighted RMS residual against
// the per-expiry censored points it was fit to.
struct TermCurve {
  double st{};
  double lt{};
  double decay{};
  double rms_resid{};
};

// Outcome of `fit_earnings_term`'s (Task 4) per-underlying eMove search.
// Declared alongside `EarningsTermFit::fit_code`; every enumerator beyond
// `Ok` names a specific way the search can fail to land on an interior
// optimum (bracket exhaustion, a flat objective, degenerate inputs) rather
// than a single generic failure code, so a caller can distinguish "no
// evidence either way" from "hit a numerical wall". `fit_earnings_term`
// itself only ever produces `MaxSteps`/`LeftBound`/`RightBound`/`CenterFlat`/
// `Minimum` -- `Ok` and `Degenerate` are reserved for other callers of this
// vocabulary (`Degenerate`-worthy inputs, e.g. `obs.size() < 2`, are instead
// rejected outright via `Err(InvalidArgument)`; see `fit_earnings_term`'s own
// doc comment).
enum class EmoveFitCode : std::uint8_t {
  Ok,         // converged to an interior minimum
  Minimum,    // best point found, but not a clean interior converge
  MaxSteps,   // iteration cap (EarningsFitConfig::max_iters) reached
  LeftBound,  // search pinned at emove_lo
  RightBound, // search pinned at emove_hi
  CenterFlat, // objective is flat across the search interval (no signal)
  Degenerate, // fewer than 2 usable expiries, or otherwise unsolvable
};

// SpiderRock's 12-point native tenor grid (year-fractions). Task 2's type;
// forward-declared only here so `EarningsFitConfig::tenor_T` callers (later
// tasks) share one name -- nothing in this header constructs or reads one.
struct SrTenorGrid;

// Knobs for the term-curve fit. `fit_term_curve_for_emove` (Task 3) reads
// only `wcen_floor`; `fit_earnings_term` (Task 4) additionally reads
// `emove_lo`/`emove_hi`/`max_iters` (the outer golden-section search's
// bracket and iteration cap) and `tenor_T` (the grid it samples the fitted
// parametric curve's secondary `atm_cen` output at,
// `EarningsTermFit::atm_cen`).
struct EarningsFitConfig {
  // 12 SR tenor year-fractions (precomputed via tenor_years, Task 2) for
  // PARAMETRIC-model sampling of atm_cen (secondary). Non-owning: the caller
  // must keep the backing storage alive for the config's lifetime.
  std::span<const double> tenor_T{};
  double emove_lo{0.0};
  double emove_hi{0.30};
  double wcen_floor{1e-10}; // reuse event_vol kWCenFloor semantics
  int max_iters{200};
  // weighting of each expiry in the term-curve LSQ (uniform in v1)
};

// Result of `fit_earnings_term`'s (Task 4) joint eMove / censored term-curve
// fit for one underlying's listed expiries.
struct EarningsTermFit {
  double emove{};              // iEMove
  double st{};                 // parametric censored term curve: short-term level
  double lt{};                 // parametric censored term curve: long-term level
  double decay{};              // parametric censored term curve: decay rate
  std::vector<double> atm_cen; // atmCenI sampled at each grid tenor (Task 4)
  double fit_error{};          // RMS residual of censored points vs model
  EmoveFitCode fit_code{EmoveFitCode::Ok};
  std::size_t expiry_count{};
};

// Censored ATM vol at one listed expiry for a candidate eMove: strips the
// event contribution out of the expiry's dirty total variance
// (`censored_total_variance`, `event_vol.hpp`), floors the result against
// `wcen_floor` (see the module self-review notes above for why this is a
// SECOND floor on top of `censored_total_variance`'s own fixed
// `kWCenFloor`), then converts back to a vol via `sqrt(w_cen / T)`.
//
// No input validation: `o.T <= 0`, non-finite `o.w_dirty`, or a negative
// `wcen_floor` are the caller's contract to uphold (see `CensorObsInput`'s
// field docs) -- this is a leaf math function, not a validated boundary.
//
// @param o           one expiry's dirty total variance / T / event count
// @param emove       candidate per-event instantaneous move vol
// @param wcen_floor  floor imposed on the censored variance before the sqrt
// @return            censored (event-free) ATM vol at this expiry
[[nodiscard]] double censored_atm_vol(const CensorObsInput &o, double emove,
                                      double wcen_floor) noexcept;

// Fits the censored term curve `atmCen(T) = lt + (st-lt)*exp(-decay*T)` to
// `obs`, for ONE fixed candidate `emove`: every observation is first censored
// via `censored_atm_vol(obs[i], emove, cfg.wcen_floor)`, then `decay` is
// searched over a bounded log-spaced grid plus a golden-section refine
// (`decay` strictly > 0) -- at each candidate `decay` the model is LINEAR in
// `{lt, A=st-lt}` (basis `1` and `exp(-decay*T)`), so `{lt,A}` is solved by a
// closed-form 2x2 least-squares normal-equations solve, never an iterative
// nonlinear fit over all three parameters together. See the module
// self-review notes above for the singular-guard rationale (degenerate `T`
// spread, decay-driven basis collapse, and the zero-observation case all
// fall back to a finite flat curve rather than dividing by a ~0
// determinant). No dynamic allocation inside the decay-search loop itself
// (only `obs`-sized precompute up front); every loop is statically bounded.
//
// @param obs   per-expiry raw (dirty) observations; empty is handled (see
//              module self-review notes), not otherwise validated
// @param emove candidate per-event instantaneous move vol, passed through to
//              `censored_atm_vol` for every observation
// @param cfg   fit knobs; only `wcen_floor` is read (see `EarningsFitConfig`)
// @return      best-fit `{st,lt,decay,rms_resid}`; `rms_resid` and every
//              field are always finite
[[nodiscard]] TermCurve fit_term_curve_for_emove(std::span<const CensorObsInput> obs,
                                                 double emove,
                                                 const EarningsFitConfig &cfg) noexcept;

// Evaluates a fitted term curve at year-fraction `T`:
// `lt + (st - lt)*exp(-decay*T)`. Pure arithmetic, no validation -- a
// non-finite `T`/`c` field propagates to a non-finite result.
[[nodiscard]] double term_curve_value(const TermCurve &c, double T) noexcept;

// Joint {eMove, st, lt, decay} fit (Task 4): the OUTER search wrapping
// `fit_term_curve_for_emove` (Task 3) as its inner solve. Searches
// `emove* = argmin` over `emove in [cfg.emove_lo, cfg.emove_hi]` of
// `fit_term_curve_for_emove(obs, emove, cfg).rms_resid`, via a golden-section
// search bounded by `cfg.max_iters` (see the module self-review notes above
// for why this is a 1-D search, not a joint nonlinear optimizer, and for the
// documented non-unimodality caveat). At the optimum, populates
// `{emove,st,lt,decay,fit_error}` from that `emove`'s `TermCurve`, and (when
// `cfg.tenor_T` is non-empty) samples `atm_cen[i] = term_curve_value(curve,
// cfg.tenor_T[i])` -- this `atm_cen` is the PARAMETRIC-model read, a
// SECONDARY summary; the PRIMARY `atmCenI_Nd` reproduction target is a raw
// censored-space interpolation built by later tasks, not this parametric
// sample (see the header's module doc).
//
// @param obs  per-expiry raw (dirty) observations; `obs.size() >= 2`
//             required -- with fewer, the term curve's own 3 free parameters
//             `{st,lt,decay}` cannot be pinned down independently of `emove`
// @param cfg  fit knobs: `emove_lo`/`emove_hi` search bracket, `max_iters`
//             cap, `wcen_floor` (passed through to `fit_term_curve_for_emove`
//             at every candidate `emove`), `tenor_T` (parametric `atm_cen`
//             sample grid; empty => `atm_cen` empty)
// @return  Ok(EarningsTermFit) with `expiry_count == obs.size()` and
//          `fit_error` equal to the optimum's `rms_resid`. `fit_code` is
//          `LeftBound`/`RightBound` if the optimum sits on a search bound,
//          `MaxSteps` if `cfg.max_iters` was exhausted before the
//          golden-section bracket converged, `CenterFlat` if the objective
//          does not meaningfully discriminate `emove` across the bracket
//          (event-under-identified, e.g. all `n` equal -- including the
//          all-`n==0` special case below), else `Minimum`.
//
//          Err(InvalidArgument) if `obs.size() < 2`, or any observation's `T`
//          or `w_dirty` is non-finite or <= 0.
//
//          SPECIAL CASE: if every observation has `n == 0` (no scheduled
//          event before any listed expiry), `emove` is not identifiable from
//          `obs` at all -- there is no event contribution to censor out.
//          Returns Ok with `emove == 0.0`, `fit_code == CenterFlat`, and
//          `{st,lt,decay,fit_error}` from `fit_term_curve_for_emove(obs, 0.0,
//          cfg)` directly (the caller's ex-event curve; still a valid Ok
//          result, per the brief -- not an error).
[[nodiscard]] Result<EarningsTermFit> fit_earnings_term(std::span<const CensorObsInput> obs,
                                                         const EarningsFitConfig &cfg);

} // namespace atx::vol
