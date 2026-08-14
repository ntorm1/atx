#pragma once

// Task 7 — end-to-end earnings-repro pipeline: fitted `VolaSession` + a
// per-underlying earnings `EventSchedule` -> per-listed-expiry censoring
// (Task 1 `event_vol.hpp`) -> the joint {eMove, st, lt, decay} term-fit
// (Task 4 `fit_earnings_term`) -> the 12-point SR tenor grid's (Task 2
// `sr_tenor_grid.hpp`) PRIMARY reproduction target, `atmCenI_{Nd}`.
//
// ## DESIGN DECISION — one library entry point, not a CLI shell-out
//
// The plan's Task 7 file list is only the CLI + CMake; the smoke test was
// described as "running the pipeline". Shelling a slow test out to a built
// example binary is fragile (needs `ATX_BUILD_EXAMPLES=ON`, subprocess I/O
// parsing), and Task 9's batch validation driver needs the SAME pipeline.
// This header/`.cpp` factor the pipeline into ONE reusable library entry
// point, `run_earnings_repro`, so `examples/earnings_repro.cpp` (thin CLI:
// parse args, load parquet + events, call, print), the slow smoke test
// (`tests/earnings_repro_smoke_test.cpp`, links this function directly, no
// example-binary dependency), and Task 9 all share one implementation. IO
// (parquet load, TSV load) stays in the CLI; this module is pure computation
// over an already-built `VolaSession` + `EventSchedule`.
//
// ## Pipeline (exact recipe)
//
//   1. Per LISTED expiry `k` (the fitted eSSVI slices, `sess.surface().
//      essvi_slices()`, ascending T): forward-ATM total variance `w_dirty_k =
//      ps.total_variance(ps.forward_at(T_k), T_k)` off the session's
//      `PricedSurface` snapshot (`sess.to_priced_surface()`), and the
//      scheduled-event count `n_k = sched.count_between(now_ns,
//      expiry_ns_k)` against the slice's STAMPED `EssviParams::expiry_ns`
//      (Task 6). A slice that was never stamped (`expiry_ns_k == 0`) falls
//      back to the T-synthesized instant via `count_events_at` (same
//      fallback `SessionInputs::events`' own solve uses, session.hpp).
//      `CensorObsInput{T_k, w_dirty_k, n_k}` is this listed expiry's fit
//      observation.
//   2. The 12 SR tenor grid year-fractions (`SrTenorGrid::kTradingDays` via
//      `tenor_years`, under the SESSION's own T convention,
//      `sess.inputs().time`) become `EarningsFitConfig::tenor_T`.
//   3. `fit_earnings_term(obs, cfg)` (Task 4) jointly solves `{eMove, st, lt,
//      decay}` — `EarningsTermFit::emove` is `iEMove`; `EarningsTermFit::
//      atm_cen` (sampled at `cfg.tenor_T` by `fit_earnings_term` itself) is
//      the SECONDARY parametric read, retained on the result's `.fit` but
//      NOT the reproduction target below.
//   4. PRIMARY `atmCenI_{Nd}` (the reproduction target: RAW censored-space
//      interpolation, deliberately NOT the parametric-model read — the
//      acceptance argument for that choice is in
//      `sprints/2026-07-18-earnings-censored-atmvol-reproduction-sprint.md`,
//      and `EarningsReproResult::atm_cen_i` below carries the same split):
//      for each of the 12 grid tenors
//      `T_i`, bracket the listed expiries by `T` and reuse the EXISTING
//      `event_aware_w` (event_vol.hpp) with `n_query = 0` to censor both
//      bracketing pillars with THEIR OWN `n` and linearly interpolate the
//      censored total variance in `T` — no re-add, since `n_query == 0`.
//      `atm_cen_i[i] = sqrt(w_cen(T_i) / T_i)`. A tenor outside the listed
//      range (no bracket) clamps FLAT to the nearest pillar's own censored
//      ATM vol (`censored_atm_vol`, Task 1 — reusing the pillar's own T, not
//      re-scaled to `T_i`), matching a standard flat-extrapolation term-curve
//      convention and avoiding a `event_aware_w` extrapolation artifact
//      outside the region the censored-variance-linear-in-T assumption was
//      ever fit to.
//
// No hand-rolled censoring math anywhere in this module: every censored
// value comes from `event_vol.hpp`'s `censored_atm_vol`/`event_aware_w`
// (Task 1) or `earnings_term_fit.hpp`'s `fit_earnings_term` (Task 4) —
// this module only assembles their inputs/outputs.
//
// ## Thread-safety
//
// `run_earnings_repro` is a pure function of its arguments (reads-only off
// `sess`/`sched`, allocates its own locals) — safe to call concurrently from
// any number of threads on the same session/schedule.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "atx/vol/earnings_term_fit.hpp" // CensorObsInput, EarningsTermFit
#include "atx/vol/event_vol.hpp"         // EventSchedule
#include "atx/vol/types.hpp"             // Result

namespace atx::vol {

class VolaSession;

// Convention knob-carrier (atx/vol/earnings_repro_config.hpp). Forward-declared
// here so the 3-arg `run_earnings_repro` consumers (the Task 7 smoke test, the
// `earnings-repro` CLI) stay decoupled from the config header's heavier include
// graph; a caller of the 4-arg overload includes the config header itself.
struct EarningsReproConfig;

// Result of the full earnings-repro pipeline for one underlying's fitted
// session at one valuation instant.
struct EarningsReproResult {
  // The joint {eMove, st, lt, decay} term-fit (Task 4). `.emove` is iEMove;
  // `.atm_cen` is the SECONDARY parametric-model atmCen read (sampled at the
  // same 12 tenors as `atm_cen_i` below, but via the fitted curve, not raw
  // censored-space interpolation).
  EarningsTermFit fit;
  // PRIMARY reproduction target: raw censored-space interpolation of the
  // listed-expiry ATM total variance at each of the 12 SR tenor
  // year-fractions (`tenor_T` below), aligned with `SrTenorGrid::
  // kTradingDays` (5,10,21,...,504 trading days) index-for-index.
  std::array<double, 12> atm_cen_i{};
  // Scheduled-event count at each of the 12 query tenors themselves (debug/
  // diagnostic parity with SpiderRock's `nEarnCnt_Nd` columns; NOT what
  // censors atm_cen_i[i] -- that uses the bracketing LISTED pillars' own
  // counts, per event_aware_w's contract).
  std::array<std::size_t, 12> n_earn{};
  // The 12 SR tenor year-fractions actually used (`tenor_years`, under the
  // session's own T convention) -- `atm_cen_i[i]`/`n_earn[i]` are both at
  // `tenor_T[i]`.
  std::array<double, 12> tenor_T{};
  // Per-listed-expiry raw (dirty) observations fed into `fit_earnings_term`
  // and used to build `atm_cen_i` -- ascending T, one entry per fitted eSSVI
  // slice with a positive T/forward. Debug/introspection for the CLI and
  // Task 9's batch driver (e.g. printing the raw board this run fit against).
  std::vector<CensorObsInput> listed_obs;
};

// Runs the full earnings-repro pipeline (see the module doc above) over an
// already-built `sess` and an already-loaded `sched`. Pure computation: does
// no IO (parquet/TSV loading is the caller's job -- the CLI and the smoke
// test each build `sess`/`sched` themselves via `load_opra_cbbo_parquet` +
// `VolaSession::from_frame` + `load_earnings_events`).
//
// @param sess    a session built via `VolaSession::build`/`from_frame`, with
//                at least 2 fitted expiries carrying a positive T and a
//                positive forward (fewer is not enough to pin down the
//                3-parameter term curve independently of eMove; see
//                `fit_earnings_term`'s own precondition)
// @param sched   the underlying's earnings-event schedule
// @param now_ns  valuation instant, epoch nanoseconds (UTC) -- the schedule
//                is counted from here, and the 12 tenor year-fractions are
//                measured from here
// @return  Ok(EarningsReproResult) on a successful fit.
//          Err(InvalidArgument) if `sess.to_priced_surface()` fails (no
//          fitted slice) or fewer than 2 listed expiries have a positive
//          T/forward. Otherwise propagates `fit_earnings_term`'s own error
//          (obs.size() < 2 after filtering, or a non-finite/non-positive
//          per-expiry T/w_dirty).
[[nodiscard]] Result<EarningsReproResult> run_earnings_repro(const VolaSession &sess,
                                                              const EventSchedule &sched,
                                                              std::int64_t now_ns);

// Config-driven overload (Task 9): runs the same pipeline but under an explicit
// `EarningsReproConfig` so the cohort-validation harness + Task 10 sweep can
// vary convention knobs. Only the WIRED knobs affect the result:
//   - `cfg.time`               -> the 12 SR tenor year-fractions' time
//                                 convention (`tenor_years`), replacing the
//                                 session's own `sess.inputs().time`.
//   - `cfg.clock_days_per_year`-> when > 0, the tenor year-fraction is the
//                                 fixed-clock `N_trading_days /
//                                 clock_days_per_year` instead of the
//                                 calendar-aware `tenor_years` advance (0 = the
//                                 calendar-aware default).
//   - `cfg.censor_space`       -> true: censor each bracketing pillar BEFORE
//                                 interpolating (SR FLEX); false: interpolate a
//                                 single plain cross-pillar variance/vol, then
//                                 censor once with the query's own event count.
//   - `cfg.interp`             -> Variance: interpolate (censored) TOTAL
//                                 VARIANCE linearly in T; Vol: interpolate
//                                 (censored) VOL linearly in T.
// The remaining `EarningsReproConfig` fields (`atm_mode`, `deam_pricer`,
// `implied_borrow`) are carried for Task 10/M5 but have no wiring seam in this
// pipeline yet -- they DO NOT affect the result (see the config header).
//
// The 3-arg overload above is exactly this one called with a config whose
// `time == sess.inputs().time` and every other field default -- i.e. the
// historical behavior is bit-preserved.
[[nodiscard]] Result<EarningsReproResult> run_earnings_repro(const VolaSession &sess,
                                                              const EventSchedule &sched,
                                                              std::int64_t now_ns,
                                                              const EarningsReproConfig &cfg);

} // namespace atx::vol
