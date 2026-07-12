# atx-vol SpiderRock Integration Sprint — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the six SpiderRock-analytics modules shipped by the 2026-07-11 sprint (merge `0611660`) into the production fit/serve/greeks/backtest hot paths, end-to-end, behind config — so that every module is reachable from `SessionInputs`/`PricerConfig`/`SurfaceDb` and exercised by an end-to-end test on the canonical stack. No new standalone features.

**Architecture:** Every task takes an existing, fully-tested library module and threads it through an existing seam (`TimeModel`/`EvalRequest` for time and interpolation, `chain_parity`/`SessionDiagnostics` for fit stats, `default_selector_candidates`/ATXVSA for curve families, `PriceFrame` fill for greeks). Defaults stay bit-identical to current behavior; all new behavior is opt-in config. Worktree: create fresh from `main` (post-`0611660`).

**Tech stack:** C++20, `atx::core::Result`/`Status` (no exceptions), GTest, CMake preset `dev` (build dir `build/`), clang-cl + Ninja. House rules: no fast-math, no virtual on arithmetic hot path, `[[nodiscard]]`, `noexcept` on pure evaluators, aggregate value types.

---

## Provenance — what the 2026-07-11 sprint shipped and found

Plan: `docs/superpowers/plans/2026-07-11-atx-vol-spiderrock-analytics.md`. Branch `feat/atx-vol-spiderrock`, 11 commits `a284fb3..eabae15`, merged to `main` as `0611660`. Gate at merge: 1058/1061, only the 3 pre-existing quarantined `MultinamePipeline` bit-identity failures.

**Shipped (all additive, default-off):**
1. `vol_time` — hybrid volatility-time clock (`vol_time_years`, NYSE calendar 2024–2028, DST-correct civil math).
2. `event_vol` — earnings event-variance (`EventSchedule`, `censored_total_variance`, `implied_emove`, `event_aware_w`).
3. `SplineVol` — vol-multiple cubic-spline curve family (SR 29-pt grid, penalized WLS, flat wings) + `fit_slice_curve` dispatch.
4. `ShapeBlend` — FLEX-style vol-multiple time interpolation (`InterpMode::ShapeBlend`).
5. `adjusted_greeks` — skew-adjusted delta (`curve_skew_slope`, `vega_slope_per_spot`, `skew_adjusted`, sticky ω control).
6. `band_violation_stats` — SpiderRock-style fit-quality stats + the session failed-calendar-check honesty fix (`session.cpp:375-376`).

**Key findings recorded by that sprint:**
- **R3 reclassified (Task 6, BLOCKED-ACCEPTED):** the 2 surviving SPY calendar crossings sit at the flat-clamped wing boundary of the shorter slice (k≈−0.469/−0.431, T≈0.4y), not between interior nodes; 6 union/checker-grid floor-row variants all cascade into *worse* violations (up to 34). Wing region needs wing-aware enforcement (R2) first → folded into Sprint E.
- Integration audit (2026-07-12, post-merge): a repo-wide search for the modules' entry points (`vol_time_years`, `event_aware_w`, `skew_adjusted`, `band_violation_stats`, …) hits **only** their own sources and unit tests. Zero production TUs call them. `TimeModel` is threaded through `projection.cpp` and `portfolio_risk.cpp` only — never constructed non-Clock by production code; the fit path (`session.cpp`, `pricer_fitter.cpp`, `corpus.cpp`) has zero `TimeModel` references. This sprint exists to close exactly that gap.

**Routed elsewhere (NOT in this sprint):**
- R2 Lee wings + wing-aware calendar enforcement, and R3 wing-boundary crossings → **Sprint E** (`2026-07-07-atx-vol-sota-engine-workmodule.md` §Sprint E; R3 evidence updates E's scope: the wing-boundary coupling is the same defect class E1/E3 address).
- R4 SPY cold-AL test-perf pass → **Sprint G** (per `2026-07-07-atx-vol-noarb-followups.md` §R4).
- sdiv EMA / strike-dependent cpAdj spline / uPrcRatio futures calibration; per-expiry rate-curve carry — new features, fail this sprint's no-dead-code bar; stay on the deferred list.

---

## Global Constraints

- Build: `cmake --preset dev` then `cmake --build build -j16`. Test gate: `ctest --test-dir build -L atx_vol -j16 --output-on-failure --timeout 900`. Baseline: **1058/1061 pass; the only acceptable failures are the 3 quarantined `MultinamePipeline` bit-identity tests** (fail on clean main; hardcoded pins drift ULPs on this box). Gate for every task: no NEW failures vs this set.
- **Never** `git add -A` — stage explicit paths only.
- Commit trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Default-off / bit-identical: with all new config fields at their defaults, existing tests pass unchanged and served numbers are bit-identical. Any task that discovers it cannot reach its production seam without breaking this must STOP and report — do not land a library-only approximation.
- **No-dead-code acceptance (sprint-wide):** each task ends with (a) the module reachable from a production config struct (`SessionInputs`, `PricerConfig`, `CurveConfig`, `SymbolFitConfig`) or a production data path, and (b) at least one end-to-end test that exercises the module *through* that production entry point (session fit / surface eval / backtest run / db round-trip) — not only through the module's own header.
- New config fields follow existing header documentation style (file-comment what/why/thread-safety, `@param/@return` on entry points); tests register as source lines in `atx-vol/tests/CMakeLists.txt`'s single `add_executable(atx-vol-tests …)` list.
- SPY-parquet fixtures: `data/` is gitignored; SPY-dependent tests `GTEST_SKIP` cleanly when absent. No paid data pulls.

---

## Task order & parallelism

Sequential spine (session/projection files overlap): **I1 → I2 → I3 → I4**.
**I5** (archive/selector) and **I6** (portfolio greeks/backtest) are file-disjoint from the spine and from each other — may run in parallel worktrees, but must rebase onto the spine before merge if run concurrently.

| Task | Module | Files touched (production) |
|---|---|---|
| I1 | ShapeBlend | `projection.hpp/.cpp`, `session.hpp/.cpp` |
| I2 | band stats | `parity.hpp/.cpp`, `session.hpp/.cpp` |
| I3 | vol_time | `data.cpp`, `opra_panel.cpp`, `session.hpp/.cpp`, `pricer_fitter.hpp` |
| I4 | event_vol | `projection.hpp/.cpp`, `session.hpp/.cpp` |
| I5 | SplineVol | `surface_archive.cpp`, `surface_db.*`, `curve_selector.cpp`, `vol_curve.hpp` |
| I6 | adjusted delta | `portfolio_greeks.cpp`, `portfolio_pricer.cpp`, `pricer_fitter.hpp`, `backtest.*` |

---

### Task I1: ShapeBlend — hot-path de-duplication + production config exposure

**Files:**
- Modify: `atx-vol/src/projection.cpp` (eval hot path), `atx-vol/include/atx/vol/projection.hpp` (doc only if signatures change)
- Modify: `atx-vol/include/atx/vol/session.hpp` (`SessionInputs`), `atx-vol/src/session.cpp`
- Test: extend `atx-vol/tests/projection_test.cpp`, `atx-vol/tests/session_test.cpp` (or the session test file that exercises eval — locate via `grep -l "SessionInputs" atx-vol/tests/`)

**Why:** review finding from the 07-11 sprint (Task 4 Minor): the `ShapeBlend` branch of `surface_eval_ex` redoes work. `convert_coord` resolves the forward at `projection.cpp:453` (`curve_forward_T(curves, request.T_clock, request.extrap_policy)`); the ShapeBlend branch at `projection.cpp:568-572` then calls `surface_insert_vol_slice`, which repeats the identical `curve_forward_T` at `projection.cpp:366-367` and redoes the slice-T bracket search (`find_exact_T` at `:315` + binary search `:339-353`). Additionally `InterpMode` is only selectable per-call on `EvalRequest` (`projection.hpp:335`) — no production config carries it, so ShapeBlend is unreachable from the session path.

**Design:**
1. Extract a file-local helper in `projection.cpp` that builds the inserted-slice handle from an *already-resolved* forward: `build_inserted_slice(surface, curves, T_clock, tm, extrap, interp, const ForwardAtT* pre)` where `ForwardAtT` is whatever struct `curve_forward_T` returns (read the code; reuse its exact type). `surface_insert_vol_slice` keeps its public signature and forwards with `pre = nullptr`; `surface_eval_ex`'s ShapeBlend branch passes the forward it already resolved in `convert_coord`. The bracket search stays inside the helper (it is needed to build the handle) — the duplicated *forward* lookup is the target; if profiling shows the bracket search is also duplicated against work `convert_coord` already did, hoist that too, else leave it.
2. Add `InterpMode interp{InterpMode::PiecewiseTotalVariance};` to `SessionInputs` (`session.hpp:69-110`). Thread it to every `EvalRequest` the session constructs (locate: `grep -n "EvalRequest" atx-vol/src/session.cpp atx-vol/src/portfolio_risk.cpp`). If `session.cpp` never constructs an `EvalRequest` (serving goes straight through `surface.w()`), then the production seam is `portfolio_risk.cpp`'s group insert (`:358`) — in that case expose the field on the risk-group config that reaches `resolve_group`, and record in the task report which seam was real.

**Interfaces (Produces):**
```cpp
// session.hpp — SessionInputs gains:
  // Cross-expiry interpolation mode for arbitrary-T queries served off this
  // session's surface. PiecewiseTotalVariance (default) is bit-identical to
  // current behavior; ShapeBlend is the FLEX-style vol-multiple blend.
  InterpMode interp{InterpMode::PiecewiseTotalVariance};
```

**Test cases (write first):**
```cpp
TEST(Projection, ShapeBlendEvalBitIdenticalAfterForwardReuse) {
  // Golden pin BEFORE refactor: build a 2-slice surface + rate curves, evaluate
  // surface_eval_ex with InterpMode::ShapeBlend on a 16-point (k,T) grid,
  // record values in the test. After refactor: identical to the pinned values
  // (exact ==, not NEAR — same arithmetic must be preserved).
}
TEST(Session, InterpModeReachesEval) {
  // Two synthetic slices with deliberately different shapes (skewed lo, flat hi).
  // SessionInputs{.interp = ShapeBlend} => arbitrary-T query differs from the
  // PiecewiseTotalVariance default at a skewed strike; default inputs =>
  // bit-identical to a session built before this task (pin one value).
}
```

**Steps:**
- [ ] **I1.1** Write `ShapeBlendEvalBitIdenticalAfterForwardReuse` with golden values captured from current `main`; commit the pins with the test (green pre-refactor).
- [ ] **I1.2** Refactor: `build_inserted_slice` + forward reuse in the ShapeBlend eval branch. Test stays green (bit-identical).
- [ ] **I1.3** Write `Session.InterpModeReachesEval` (RED — field doesn't exist), add `SessionInputs::interp`, thread to the eval seam, GREEN. If the seam turns out to be `portfolio_risk` only, adjust the test to go through that entry point and say so in the commit body.
- [ ] **I1.4** Full gate; commit `perf(atx-vol): reuse resolved forward in ShapeBlend eval; expose InterpMode on SessionInputs`.

**Acceptance:** golden ShapeBlend values bit-identical across the refactor; `interp` default bit-identical; ShapeBlend reachable from a production config struct with an e2e test proving it.

---

### Task I2: band-violation stats — wire into the parity/fit-quality pipeline

**Files:**
- Modify: `atx-vol/include/atx/vol/parity.hpp` (`ParityReport`, `parity.hpp:74-83`), `atx-vol/src/parity.cpp` (`chain_parity`)
- Modify: `atx-vol/include/atx/vol/session.hpp` (`SessionDiagnostics`, `session.hpp:152-164`), `atx-vol/src/session.cpp` (aggregation loops `:378-404` CurveSurface path, `:423-434` eSSVI path)
- Test: extend `atx-vol/tests/parity_test.cpp` (locate exact name: `grep -l chain_parity atx-vol/tests/`) and the session diagnostics test

**Why:** `band_violation_stats` (`fit_metrics.hpp:192`) is called only by its unit test. The natural producer is `chain_parity` (`parity.hpp:101`) — it already scores model price against bid/ask per expiry (`frac_fv_within_bidask`); the natural aggregate consumer is `SessionDiagnostics`, which already rolls up `worst/mean_frac_within_bidask` in `session.cpp:378-404`. SpiderRock exposes exactly these fields (`cBidMiss/cAskMiss/fitMaxPrcErr`) per fit; ~90% of their fits have `fitMaxPrcErr == 0` — that is the competitive bar this makes measurable.

**Interfaces (Produces):**
```cpp
// parity.hpp — ParityReport gains:
  // SpiderRock-style surface/quote band-violation stats for this expiry
  // (model price vs bid/ask): miss counts, worst premium violation, signed bias.
  BandViolationStats band{};

// session.hpp — SessionDiagnostics gains:
  std::size_t n_bid_miss{};   // sum over slices
  std::size_t n_ask_miss{};   // sum over slices
  double max_prc_err{};       // max over slices (premium units)
```

**Design:** inside `chain_parity`, where the per-quote model-vs-band comparison already runs, collect the three parallel spans (model, bid, ask) it already has and call `band_violation_stats`; on `Err` (length mismatch — impossible by construction) propagate the existing error convention of `chain_parity`. In both `session.cpp` aggregation loops, sum `n_bid_miss`/`n_ask_miss` and max `max_prc_err` across slices. Do NOT gate selection on these v1 — record-only (the selection signal already flows through `slice_fit_metrics` chi²; adding a second gate without OOS evidence is scope creep).

**Test cases (write first):**
```cpp
TEST(Parity, BandStatsInBandQuotesAreZero) {
  // Synthetic chain whose model prices sit strictly inside [bid, ask]:
  // report.band.n_bid_miss == 0, n_ask_miss == 0, max_prc_err == 0.0, n == quote count.
}
TEST(Parity, BandStatsCountsCrossings) {
  // Force one model price below bid by 0.07 and one above ask by 0.03:
  // n_bid_miss == 1, n_ask_miss == 1, max_prc_err ≈ 0.07, max_err_idx points at the bid miss.
}
TEST(Session, DiagnosticsAggregateBandStats) {
  // Multi-expiry synthetic session fit: diagnostics n_bid_miss/n_ask_miss equal
  // the sum of per-slice ParityReport values; max_prc_err is the max.
}
```

**Steps:**
- [ ] **I2.1** Write the two parity tests (RED — no `band` field), extend `ParityReport`, compute in `chain_parity`, GREEN.
- [ ] **I2.2** Write the session aggregation test (RED), extend `SessionDiagnostics`, fill in both aggregation loops (CurveSurface AND eSSVI paths — the 07-11 sprint's session guard fix history shows these two paths drift when only one is edited), GREEN.
- [ ] **I2.3** Run the SPY fixture test family locally if `data/` present (`ctest -R Spy`) and eyeball reported `max_prc_err` for sanity (expect small, nonzero); record the number in the task report. GTEST_SKIP-safe otherwise.
- [ ] **I2.4** Full gate; commit `feat(atx-vol): band-violation stats wired through chain_parity into SessionDiagnostics`.

**Acceptance:** every session fit now produces band stats with zero config; existing `ParityReport`/`SessionDiagnostics` fields unchanged; gate green.

---

### Task I3: vol_time — opt-in T convention on the production fit/serve path

**Files:**
- Modify: `atx-vol/src/data.cpp` (`year_fraction`, `:247-255`, consumed `:444`), `atx-vol/src/opra_panel.cpp` (`:181`, `:399`, `:492`)
- Modify: `atx-vol/include/atx/vol/session.hpp` (`SessionInputs`), `atx-vol/src/session.cpp`; `atx-vol/include/atx/vol/pricer_fitter.hpp` (`PricerConfig`, `:110-146`) if the panel builders take their knobs from there (locate the actual plumbing: `grep -n "year_fraction" atx-vol/src/`)
- Test: extend `atx-vol/tests/vol_time_test.cpp` (e2e case), plus the panel/chain test file that pins `Chain::T` (locate: `grep -ln "Chain" atx-vol/tests/*panel*`)

**Why:** T is produced ONCE, at chain construction — `year_fraction` in `data.cpp:247-255` (`365.25 * 86400e9` ns) — and flows as `Chain::T` into everything (`curve_selector.cpp:143`, `SliceContext::T`, fitters, greeks). `vol_time.hpp:9-12` documents the wiring as the follow-up. Wiring at the *source* means fit, serve, and greeks all see the same convention with no per-consumer changes, and `TimeMode::Clock` in projection remains correct as the identity (T is already in the chosen convention when it reaches projection).

**Design:**
```cpp
// New enum + carrier, in vol_time.hpp (it owns the convention):
enum class TimeConvention : std::uint8_t {
  Calendar365 = 0,   // (to-from)/365.25y — current behavior, default
  VolTime = 1,       // SpiderRock hybrid clock: vol_time_years(from, to, params, cal)
};
struct TimeSpec {
  TimeConvention convention{TimeConvention::Calendar365};
  VolTimeParams vol_time{};                       // used when convention == VolTime
  // calendar: VolTimeCalendar::us_default() v1; field reserved for a custom table later.
};
// Single conversion entry point, replaces direct year_fraction calls:
[[nodiscard]] double time_to_expiry_years(std::int64_t from_ns, std::int64_t to_ns,
                                          const TimeSpec& spec) noexcept;
```
- `time_to_expiry_years` with a default `TimeSpec` must be bit-identical to `year_fraction` (delegate to the same expression — do not re-derive the constant).
- Replace the four production call sites (`data.cpp:444`, `opra_panel.cpp:181/:399/:492`) with `time_to_expiry_years(from, to, spec)`, threading `TimeSpec` down from the panel/chain builder's config. `SessionInputs` gains `TimeSpec time{};` and passes it wherever the session builds chains; if chains are built by callers *before* `SessionInputs` exists (inspect the call graph from `opra_panel.cpp` upward), put `TimeSpec` on the panel-builder options struct instead and mirror it on `SessionInputs` for session-internal refits — the task report must state where the single source of truth landed.
- **Consistency guard:** the same `TimeSpec` must govern fit-time T and any T the session computes later (refits, ladders). Add a debug assertion or a stored copy on the session so a mixed-convention session is impossible.

**Test cases (write first):**
```cpp
TEST(VolTime, TimeToExpiryDefaultBitIdenticalToYearFraction) {
  // 20 random (from, to) pairs: time_to_expiry_years(f, t, {}) == year_fraction result
  // exactly (==, same expression).
}
TEST(VolTime, ChainCarriesVolTimeT) {
  // Build a synthetic panel/chain with TimeSpec{VolTime}: for a Friday-16:00-ET
  // anchor and a Monday expiry, Chain::T equals vol_time_years(...) (weekend
  // compresses), and is strictly less than the Calendar365 T for the same pair.
}
TEST(VolTime, SessionFitUnderVolTimeServesConsistentGreeks) {
  // End-to-end: synthetic board (flat 20% smile), fit a session under
  // TimeSpec{VolTime}, then price + greeks a contract; assert (a) fit converges,
  // (b) served iv ≈ 20% at ATM, (c) theta sign sane, (d) the SAME test under
  // default TimeSpec is bit-identical to a pinned pre-task value.
}
```

**Steps:**
- [ ] **I3.1** Write `TimeToExpiryDefaultBitIdenticalToYearFraction` (RED — function missing), implement `TimeSpec`/`time_to_expiry_years` in `vol_time.hpp/.cpp`, GREEN.
- [ ] **I3.2** Trace the production plumbing from `opra_panel.cpp` call sites up to whoever owns config; write `ChainCarriesVolTimeT` (RED), replace the four call sites + thread `TimeSpec`, GREEN. Default path bit-identity: run the full gate here — any drift in any existing test means the default delegation is wrong; STOP and fix before proceeding.
- [ ] **I3.3** Write `SessionFitUnderVolTimeServesConsistentGreeks` (RED at the `SessionInputs::time` field), add the field + session threading + consistency guard, GREEN.
- [ ] **I3.4** Evidence run (record-only, no gate): if `data/spy_ytd` present, fit one SPY day under both conventions; record `SessionDiagnostics` quality (frac-within-bidask, band stats from I2) and ATM theta at 3 intraday timestamps in the task report. This is the input to the *separate, future* default-flip decision (needs corpus rebaseline — explicitly out of scope, per the 07-11 deferred list).
- [ ] **I3.5** Full gate; commit `feat(atx-vol): opt-in vol-time T convention (TimeSpec) on the chain/fit/serve path`.

**Acceptance:** default `TimeSpec` bit-identical everywhere (full gate proves it); `VolTime` reachable from `SessionInputs` with an e2e fit/serve/greeks test; evidence numbers recorded; no default flip.

---

### Task I4: event_vol — event-aware cross-expiry interpolation in the query path

**Files:**
- Modify: `atx-vol/include/atx/vol/projection.hpp` (`EvalRequest` `:326-340`; `TimeModel` docs `:175-180`), `atx-vol/src/projection.cpp` (the linear blend in `w_on_inserted_slice`/eval, `:393-397`)
- Modify: `atx-vol/include/atx/vol/session.hpp` (`SessionInputs`, `SessionDiagnostics`), `atx-vol/src/session.cpp`
- Test: extend `atx-vol/tests/projection_test.cpp`, `atx-vol/tests/event_vol_test.cpp` (session e2e)

**Why:** cross-expiry interpolation is linear-in-total-variance with no event awareness (`projection.cpp:393-397`); an expiry pair straddling earnings mis-prices every T between them. `event_aware_w` (`event_vol.hpp:202`) is purpose-built as the drop-in replacement for exactly that blend: censor both bracketing slices, interpolate censored variance, re-add `n_query·eMove²`. The only missing pieces are (a) a carrier for the schedule + eMove and (b) the call.

**Design:**
```cpp
// projection.hpp — EvalRequest gains (nullable, default off => bit-identical):
  // Earnings/event awareness for cross-expiry interpolation. When `events` is
  // non-null and emove > 0, the T-blend runs in censored variance via
  // event_aware_w. Applies to BOTH InterpMode paths (the censoring wraps the
  // blend, whatever the blend is).
  const EventSchedule* events{nullptr};
  double emove{0.0};

// session.hpp — SessionInputs gains:
  std::shared_ptr<const EventSchedule> events{};  // symbol event schedule (may be null)
  // eMove policy v1: solve implied_emove from the two fitted expiries bracketing
  // the next event when schedule present; expose the result:
// SessionDiagnostics gains:
  double implied_emove{std::numeric_limits<double>::quiet_NaN()};  // NaN = not solved
```
- In `projection.cpp`, factor the current two-point blend into a helper and wrap it: when events active, compute `n_lo/n_hi/n_q` via `EventSchedule::count_between(now, ·)` — `EvalRequest` must therefore also carry `now_ns` if it doesn't already (locate; the session knows `now_ts_ns`) — then `event_aware_w(w_lo, T_lo, n_lo, w_hi, T_hi, n_hi, T_q, n_q, emove)`. Fallback semantics (emove ≤ 0, all-n-zero) are already inside `event_aware_w` — do not duplicate them at the call site.
- In `session.cpp`: after fit, if `inputs.events` non-null, pick the first event in `(now, last_expiry]`, find the two fitted expiries bracketing it, call `implied_emove(w1,T1,n1,w2,T2,n2)` (ATM total variances from the fitted slices); store into `SessionDiagnostics::implied_emove` on `Ok`, leave NaN on any `Err` (mirror the conservative reporting convention from the calendar guard at `session.cpp:375-376` — a failed solve must never fabricate 0). Serve path: session-constructed `EvalRequest`s carry `events.get()` + the solved eMove.

**Test cases (write first):**
```cpp
TEST(Projection, EventAwareBlendJumpsAcrossEventDay) {
  // Two slices, censored vol flat 20%, one event between them, emove 5%
  // (the exact synthetic from event_vol_test.cpp RoundTripKnownEmove):
  // eval at T just below vs just above the event day differs by ≈ emove²
  // in w; both sides match the closed-form censored interpolation to 1e-12.
}
TEST(Projection, NullScheduleBitIdentical) {
  // events == nullptr: eval on a 16-pt grid bit-identical to pinned pre-task values.
}
TEST(Session, ImpliedEmoveSolvedAndServed) {
  // E2e: synthetic board built from a known (σ_C, emove) pair with an event
  // between expiries 1 and 2; SessionInputs carries the schedule. After fit:
  // diagnostics.implied_emove ≈ true emove (tol from fit noise, e.g. 10%);
  // an arbitrary-T query between the expiries reflects the event variance.
}
TEST(Session, NoBracketingExpiriesLeavesEmoveNaN) {
  // Event after the last expiry: implied_emove stays NaN; serving falls back
  // to plain blend; no error.
}
```

**Steps:**
- [ ] **I4.1** Write the two projection tests (RED), add `EvalRequest` fields + the wrapped blend, GREEN. Full gate mid-task (bit-identity of the null path is the risk).
- [ ] **I4.2** Write the two session tests (RED), add `SessionInputs::events` + post-fit `implied_emove` solve + diagnostics field + serve threading, GREEN.
- [ ] **I4.3** Full gate; commit `feat(atx-vol): event-aware cross-expiry interpolation wired through session serve path (EventSchedule, implied eMove)`.

**Acceptance:** with a schedule present the served surface is event-aware end-to-end; without one, bit-identical; solve failures report NaN, never 0; gate green.

---

### Task I5: SplineVol — ATXVSA persistence + gated selector candidacy + OOS evidence

**Files:**
- Modify: `atx-vol/src/surface_archive.cpp` (rejection sites `:127-131` size stub, `:284-289` write reject, `:458-463` write stub, `:874-880` reconstruct reject)
- Modify: `atx-vol/src/curve_selector.cpp` (`default_selector_candidates` `:25-44`, `slice_butterfly_violations` `:63-82`, `select_curve` `:108`)
- Modify: `atx-vol/include/atx/vol/vol_curve.hpp` (`CurveConfig` `:319-324` — selector flag)
- Test: extend `atx-vol/tests/surface_archive_test.cpp` (locate the LinearVariance round-trip pattern), `atx-vol/tests/surface_db_test.cpp`, `atx-vol/tests/curve_selector_test.cpp` (locate: `grep -ln select_curve atx-vol/tests/`)

**Why:** SplineVol is fittable when pinned (`vol_curve.cpp:380-384`) but (a) persistence is rejected on both paths (write: `"SplineVol serialization not supported"`, `surface_archive.cpp:284-289`; reconstruct: `:874-880`), so a SplineVol fit cannot reach `SurfaceDb` — it dies with the process; and (b) it is not a selector candidate and the per-kind butterfly gate has no SplineVol case (`curve_selector.cpp:63-82` falls through to `return 0u`, i.e. a SplineVol slice would pass the gate *unchecked* — a latent correctness trap the moment anyone adds it to the list). The 07-11 sprint gated candidacy on OOS proof; this task builds the proof harness into the selector config rather than flipping the default blind.

**Design:**
1. **Wire format (additive ATXVSA payload):** follow the LinearVariance payload precedent exactly (find its write/reconstruct/size trio in `surface_archive.cpp` and mirror the structure). Payload: `atm_vol` (f64), `z_lo_valid`, `z_hi_valid` (f64), knot count `n` (u32), then `z[n]`, `mult[n]` (f64 arrays), `n_butterfly_viol` (u32). Bump whatever per-kind versioning the format uses additively — reconstruct of old archives must be unaffected (they cannot contain SplineVol by construction of the old reject). Round-trip test asserts byte-equality of re-serialized payload AND `w()` equality on a k-grid (both patterns exist in `surface_archive_test.cpp`).
2. **Butterfly gate case:** add `case VolCurveKind::SplineVol:` to `slice_butterfly_violations` returning `params().n_butterfly_viol` (already carried, `spline_curve.hpp:104`) via the `SplineVolCurve` adapter (`vol_curve.hpp:238`).
3. **Gated candidacy:** `CurveConfig` gains `bool spline_candidate{false};`. `select_curve` appends `VolCurveKind::SplineVol` to its working candidate list when set. `default_selector_candidates()` stays untouched (ConvexDense, LinearVariance, Essvi, Svi, C8) — the default flip is a separate future decision that requires the evidence below.
4. **OOS evidence run (record-only):** run the existing whole-universe harness (`examples/universe_autofit.cpp` + `tools/analyze_universe_autofit.py`) twice on the cached OPRA snapshot — `spline_candidate` off vs on — and record: SplineVol win-rate under the existing OOS in-band → chi² → parsimony scoring, plus band stats (I2) deltas on the boards it wins. If the artifact cache lacks the snapshot, record the exact command line and SKIP (no paid pulls).

**Test cases (write first):**
```cpp
TEST(SurfaceArchive, SplineVolRoundTripBitExact) {
  // fit_spline_vol_slice on the SVI-generated board from spline_curve_test.cpp,
  // write archive, reconstruct: kind()==SplineVol, params byte-compare via
  // re-serialization, w() equal on 64-pt k-grid (==, not NEAR).
}
TEST(SurfaceDb, SplineVolPartitionRoundTrip) {
  // write_partition with a SplineVol surface (mirrors the Task-4 kind-coverage
  // pattern in surface_db_test.cpp), reopen db, read, byte-equal payload.
}
TEST(CurveSelector, ButterflyGateReadsSplineViolations) {
  // Hand-build a SplineVolCurve with n_butterfly_viol = 3:
  // slice_butterfly_violations returns 3 (not the fall-through 0).
}
TEST(CurveSelector, SplineCandidateFlagAddsCandidate) {
  // Synthetic board where SplineVol fits well: spline_candidate=false selects
  // from the current 5 families (pin: result kind is one of them);
  // spline_candidate=true on a spline-favorable board CAN return SplineVol.
  // Also: flag=false selection is bit-identical to pre-task (pin one board).
}
```

**Steps:**
- [ ] **I5.1** Write archive round-trip test (RED at the reject), implement payload write/size/reconstruct, GREEN.
- [ ] **I5.2** Write db round-trip test (RED), confirm the db path needs no code (it delegates to archive — the 07-11 db review verified no kind switches in the db path); if it passes immediately after I5.1, keep it as the regression pin and note that.
- [ ] **I5.3** Write butterfly-gate test (RED — falls through to 0), add the case, GREEN.
- [ ] **I5.4** Write candidacy tests (RED — no flag), add `CurveConfig::spline_candidate` + `select_curve` wiring, GREEN.
- [ ] **I5.5** Evidence run (§4 above); write results into the task report and `atx-vol/docs/` if substantive.
- [ ] **I5.6** Full gate; commit `feat(atx-vol): SplineVol ATXVSA persistence, butterfly-gate case, config-gated selector candidacy + OOS evidence`.

**Acceptance:** SplineVol survives a full fit → archive → SurfaceDb → reconstruct → serve cycle; selector can be asked for it and gates it honestly; default selection bit-identical with the flag off; evidence recorded for the future default decision.

---

### Task I6: adjusted delta — production greeks serving + backtest hedging

**Files:**
- Modify: `atx-vol/src/portfolio_greeks.cpp` (`:82-87` Black-76 seam), `atx-vol/src/portfolio_pricer.cpp` (`:323-327` FullGreeks fill, `:362-363` totals)
- Modify: `atx-vol/include/atx/vol/pricer_fitter.hpp` (`PricerConfig` `:110-146`), possibly `atx-vol/include/atx/vol/adjusted_greeks.hpp` (surface-level slope helper)
- Test: extend `atx-vol/tests/adjusted_greeks_test.cpp`, `atx-vol/tests/backtest_test.cpp` (hedge overlay; locate the hedge-band test: `grep -n "hedge" atx-vol/tests/backtest_test.cpp`)

**Why:** every production path serves raw analytic delta: `portfolio_greeks.cpp:82-87` (`black76_greeks(...)` → qty-weighted accumulate), `portfolio_pricer.cpp:323-327` (`out.delta[i] = w * g.delta`), and the backtest hedger trades on exactly that number (`backtest.cpp:789-807`: `option_delta += current_risk->delta[i]`, trades `-net` when `|net| > hedge_spec.band`). Under sticky-delta dynamics the raw delta systematically mis-hedges skewed books — the entire point of `skew_adjusted` (`adjusted_greeks.hpp:73`). `curve_skew_slope` needs an `IVolCurve&` but the greeks seams hold surfaces — one thin overload closes that.

**Design:**
```cpp
// adjusted_greeks.hpp — surface-level overload (same FD scheme as curve_skew_slope):
// ∂σ/∂k at (k_log, T) off a served surface; central FD h=1e-4 on the surface's
// w-query (locate VolSurface's w/iv accessor in portfolio_greeks.cpp's ctx usage).
[[nodiscard]] double surface_skew_slope(const VolSurface& s, double k_log, double T) noexcept;

// pricer_fitter.hpp — PricerConfig gains:
  // Skew-adjusted (SpiderRock) delta: delta + VegaSlope·vega, with
  // sticky.ref_uprc_weight ω ∈ [0,1] (0 = sticky-delta). Off by default.
  bool skew_adjusted_delta{false};
  StickyParams sticky{};
```
- `portfolio_greeks.cpp`: between `:83` and the accumulate at `:87`, when enabled: `slope = surface_skew_slope(ctx.surface, k, ctx.T)`; `vs = (1−ω)·(−slope/S)` — reuse `vega_slope_per_spot`'s arithmetic by exposing a slope-input variant if needed rather than duplicating the formula; `g = skew_adjusted(g, vs)`. Spot S: whatever the ctx carries (F and df are there; use the same S the delta convention of this path is quoted against — read the header contract of the path first and match it; if the path is F-quoted, document the ω mapping in the field comment).
- `portfolio_pricer.cpp` FullGreeks fill: same adjustment on `g.delta` using `g.vega` before the `w *` scaling at `:323-327` and totals `:362-363`. The slope needs the contract's (k,T) and served surface — both reachable from the pricing context that produced `c.g` (trace it; if the surface is genuinely out of reach at that point, compute the slope where the context IS available and carry it alongside — the task report must document the choice).
- Backtest: **no backtest code change** — the hedger consumes `PriceFrame.delta`, which is now adjusted when the run's `PricerConfig` enables it. That is the e2e proof.

**Test cases (write first):**
```cpp
TEST(AdjustedGreeks, SurfaceSlopeMatchesCurveSlopeOnSingleSlice) {
  // Surface built from one SVI slice: surface_skew_slope(s, k, T) ==
  // curve_skew_slope(curve, k) within 1e-8 across k ∈ {-.3,-.1,0,.1,.3}.
}
TEST(PortfolioGreeks, SkewAdjustedDeltaRaisesCallDeltaOnPutSkew) {
  // Book of 1 OTM call on a put-skewed synthetic surface, ω=0:
  // enabled delta > raw delta (sign law from the 07-11 sprint's corrected
  // test 4); ω=1 => exactly raw; flag off => bit-identical to pinned raw.
}
TEST(Backtest, HedgeTradesOnAdjustedDelta) {
  // Deterministic 2-day synthetic backtest with hedge band ~0 on a skewed
  // surface: shares traded with skew_adjusted_delta on differ from off in the
  // pinned direction (put skew => larger short-share hedge for a long-call book);
  // off-run bit-identical to a pre-task pinned run (full-frame compare).
}
```

**Steps:**
- [ ] **I6.1** Write the surface-slope test (RED), implement `surface_skew_slope`, GREEN.
- [ ] **I6.2** Write the portfolio-greeks test (RED at the config field), add `PricerConfig` fields + the `portfolio_greeks.cpp` seam, GREEN.
- [ ] **I6.3** Extend to `portfolio_pricer.cpp` FullGreeks (American path) with the same law; the backtest test (RED until this lands) goes GREEN here — it exercises pricer → PriceFrame → hedger with zero hedger changes.
- [ ] **I6.4** Full gate (the 3 quarantined MultinamePipeline pins stay quarantined; everything else must be bit-identical with the flag off — this task touches the hottest path in the library, treat any off-flag drift as a defect).
- [ ] **I6.5** Commit `feat(atx-vol): skew-adjusted delta on the portfolio greeks/pricer path; backtest hedging inherits via PriceFrame`.

**Acceptance:** adjusted delta reachable from `PricerConfig` through both greeks paths and consumed by the backtest hedger with no hedger changes; off-flag bit-identity proven by full-frame pins; sign law tested on both ω extremes.

---

## Self-review notes

- Spec coverage: all six shipped modules get a production wiring task (I1–I6); every 07-11 deferred item is either a task here, routed (R2/R3→E, R4→G), or explicitly re-deferred with reason (sdiv/cpAdj/uPrcRatio/rate-carry — new features, not wiring).
- The two riskiest bit-identity surfaces (I3 default T source, I6 off-flag hot path) both carry an explicit mid-task full-gate step, not just the end-of-task gate.
- Type names cross-checked against the seam audit: `SessionInputs` (session.hpp:69), `PricerConfig` (pricer_fitter.hpp:110), `CurveConfig` (vol_curve.hpp:319), `EvalRequest` (projection.hpp:326), `ParityReport` (parity.hpp:74), `SessionDiagnostics` (session.hpp:152), `BandViolationStats` (fit_metrics.hpp:156), `StickyParams` (adjusted_greeks.hpp:45), `SplineVolParams::n_butterfly_viol` (spline_curve.hpp:104). Line numbers are as of merge `0611660` and may drift — implementers re-locate by symbol, not line.
