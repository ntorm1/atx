# GOAL PROMPT — OPRA Full-Universe 10AM→11AM Surface Generalization Benchmark

> **Audience:** a fresh agent starting with zero conversation context in the `c:\atx` repo.
> **Nature of this goal:** long-running, recursive, self-improving. There is no single "done" —
> you run cycles of *research → review → implement → test → benchmark → identify next steps*
> and each cycle must leave the benchmark scoreboard measurably better or leave a written
> explanation of why it did not.

---

## 1. Mission

Build a new benchmark for `atx-vol` that answers one question:

> **If we fit American equity option vol surfaces on the entire OPRA universe at
> 2026-02-10 10:00 ET, and use our curve selector + auto config builder to choose
> per-name settings from that slice, how well do those frozen settings generalize
> when re-fit on the 11:00 ET slice?**

Concretely:

1. **Pull two full-universe OPRA snapshots**: 2026-02-10 at **10:00 ET (15:00 UTC)** and
   **11:00 ET (16:00 UTC)**. (Feb 10 is EST, UTC−5.)
2. **T0 pass (10:00):** fit every name; run the curve selector and auto config builder to
   produce a frozen per-symbol configuration (`SymbolFitConfig` — curve kind + knobs +
   admission posture) for every name we decide to serve.
3. **T1 pass (11:00):** re-fit every served name using the **frozen T0 config, no
   re-selection**, against the 11:00 quotes.
4. **Score T1:**
   - **RMSE in vol points** per name (model IV vs. mid-market IV), and universe aggregates.
   - **% of quotes within bid/ask** per name (model price inside the NBBO band), and
     universe aggregates.
   - **Coverage**: % of universe names served vs. explicitly declined, with a machine-readable
     reason for every declined name.

This is deliberately hard. Nearly all prior atx-vol work targeted liquid names (SPY, AMZN,
MAG7). The OPRA universe is thousands of names, most of them sparse, wide, and ugly. The
benchmark exists to force the fitting stack to grow breadth, robustness, throughput, and
honest failure handling.

### 1.1 The two-pronged objective — accuracy AND speed, jointly

This goal has **two prongs that must be optimized together**: maximum accuracy and minimum
fit/price wall-time. Neither wins alone:

- **Accuracy means economic accuracy, not numerical accuracy.** The yardstick is the
  bid/ask band and vol-point RMSE at trading-relevant precision — not 1e-10 decimal
  agreement. Chasing digits the market cannot trade is wasted work; an extra decimal of
  convergence that doubles fit time is a regression, not a win.
- **Past a threshold, accuracy gains are worthless if the stack is too slow to use.** The
  operating regime to design for is HFT / market-maker marking: a full-universe fit cycle
  fast enough to serve as a live mark, not an overnight batch. When accuracy and speed
  conflict, ask "would a market maker pay this latency for this precision?" — that is the
  tie-break.
- **The core deliverable is reasonable, tradeable accuracy at market-maker speeds.** A
  slightly wider RMSE that fits the universe in seconds beats a perfect surface that takes
  minutes. Every cycle's scoreboard entry must report both prongs side by side; a cycle
  that improves one prong by degrading the other must justify the trade explicitly in the
  cycle log.

---

## 2. Benchmark specification (build this first)

### 2.1 Data

- OPRA data is Databento `cbbo-1m` NBBO minute snapshots stored as parquet, one file per
  symbol per date, **not checked into the repo**. Expected hive root: `data/opra_universe`
  with layout `{symbol}/{date}.parquet`.
- **Data spend authorization: you are pre-approved for up to $100 of real Databento OPRA
  data pulls for this goal.** No need to ask before spending within that budget — but spend
  it deliberately: pull the smoke subset first, verify parsing end-to-end, then pull
  incrementally up the scale ladder. Track cumulative spend in the cycle log; stop and
  report if the full-universe pull would exceed the remaining budget.
- Pull tools: `atx-vol/tools/pull_opra_universe_snapshot.py` (has `--snap-utc`, schema
  `cbbo-1m`) and `atx-vol/tools/pull_opra_universe_batch.py`. Pull the 15:00 UTC and
  16:00 UTC minutes for 2026-02-10. Keep the two slices in separate roots (e.g.
  `data/opra_universe_1000` / `data/opra_universe_1100`) or disambiguate by snapshot suffix —
  the existing loaders treat *one file = one snapshot minute*.
- **Before anything else, verify data access actually works** (API key present, one symbol
  pulls cleanly, loader parses it). If you cannot obtain real data, stop and report — never
  substitute synthetic quotes and present benchmark numbers as real.
- Loaders: `load_opra_cbbo_parquet` (`atx-vol/include/atx/vol/opra_panel.hpp`) for one
  symbol; `load_opra_daterange` / `OpraBatchSpec` (`atx-vol/include/atx/vol/opra_batch.hpp`)
  for batch, with `snapshot_suffix` controlling which minute is loaded and missing files
  non-fatal. Spot is implied via put-call parity unless overridden.

### 2.2 Protocol

- **T0 (10:00 ET):** for each name, run selection via `select_curve` / `CurveSelector`
  (`atx-vol/include/atx/vol/curve_selector.hpp`) and the fit-policy auto-config
  (`select_fit_policy`, `atx-vol/include/atx/vol/fit_policy.hpp`), producing a
  `SymbolFitConfig` (`atx-vol/include/atx/vol/surface_db.hpp`). Persist the full frozen
  config set as a benchmark artifact (the existing `SurfaceDb` manifest already stores
  `SymbolFitConfig` fixed-width — reuse it, or emit a sidecar; either way it must be
  reloadable and diffable).
- **T1 (11:00 ET):** load the frozen config set, fit with `pin_curve`-style behavior (no
  re-selection, no silent fallback ladder unless the config explicitly allows it), score.
- A name is **served** only if it passes admission at T0. Names declined at T0 are excluded
  from T1 accuracy metrics but counted in coverage. Names that were served at T0 but fail at
  T1 count as failures in the scoreboard (report separately: `t1_fit_failed`).

### 2.3 Metrics (reuse existing conventions — do not invent parallel ones)

- Per-slice fit quality: `SliceFitMetrics` (`atx-vol/include/atx/vol/fit_metrics.hpp`) —
  `rmse_vol` is already defined in vol points; `n_within_band` exists.
- Bid/ask band: `BandViolationStats` (SpiderRock-style `cBidMiss`/`cAskMiss`) — "% within
  bid/ask" = quotes whose model price lies inside NBBO / total scored quotes.
- Admission and decline reasons: `FitAdmissionPolicy`, `SurfaceAdmissionReason` (20 reasons,
  e.g. `InsufficientQuoteCoverage`, `CalendarArbitrage`), `evaluate_surface_admission`
  (`atx-vol/include/atx/vol/fit_policy.hpp`).
- **Output artifact:** one machine-readable per-name row (symbol, tier, chosen curve kind,
  T0 metrics, T1 metrics, admission verdict + reason, fit wall-time) plus a universe roll-up
  (median/p90 RMSE, % within bid/ask, coverage %) — CSV or JSON, deterministic ordering,
  plus a short human-readable report. Aggregate **by liquidity tier** (e.g. quote count /
  spread buckets), not just globally: a universe average dominated by penny-wide megacaps
  hides everything interesting.

### 2.4 Scale ladder — build confidence before burning CPU

Do **not** start with the full universe. Every stage must pass before advancing:

1. **Smoke (~10 names):** SPY + a few MAG7 + a few deliberately illiquid names. Verify
   end-to-end plumbing, determinism (same inputs → bit-identical outputs), and that the
   report reads sanely.
2. **Top-50 (`data/universe/spy_top50_2026-01-01.csv`, builder
   `tools/build_spy_top50_universe.py`):** first real scoreboard baseline.
3. **~250–500 names** (e.g. via `tools/build_r3000_proxy_universe.py` /
   `tools/make_universe.py`): first serious exposure to sparse names; expect the decline
   machinery to matter here. Tune before scaling further.
4. **Full OPRA universe:** only when stage 3 runs clean, fast, and the decline reasons look
   sensible rather than being a dumping ground.

If a run is producing garbage (NaNs, absurd RMSE, mass unexplained declines), **stop the
run, diagnose, fix, and re-run the smallest stage that reproduces it**. Wasted full-universe
cycles are the main failure mode to avoid.

---

## 3. Workstreams

The benchmark will expose weaknesses; these four workstreams are where you fix them. Expect
to iterate on all four across cycles, guided by what the scoreboard says is the current
bottleneck.

**Gains must land in the library, not the benchmark.** The benchmark driver is a thin shell;
every improvement — new curve families, selector hardening, throughput work, failure
handling — belongs in the `atx-vol` library proper so it automatically benefits every
consumer: the backtest engine (`src/dispersion_backtest.cpp`, `src/dispersion_workflow.cpp`),
the portfolio pricer (`src/portfolio_pricer.cpp`), `surface_db_populate`, `PricerFitter`,
and the example/CLI drivers. If a fix only works because the benchmark driver special-cases
something, it isn't done. Prefer extending the existing seams (`PricerConfig`,
`SymbolFitConfig`, `CurveSelector`, `FitPolicyConfig`, `FitAdmissionPolicy`) over
benchmark-private code paths, and when a cycle improves fitting or pricing, check that the
backtest and portfolio-pricing paths pick the gain up for free (their tests and benches
still pass, ideally faster).

### WS-1: Breadth of surface configuration types

Current `VolCurveKind` inventory (`atx-vol/include/atx/vol/vol_curve.hpp`): `ConvexDense`,
`Essvi`, `Svi`, `LinearVariance`, `C8`, `SplineVol`, plus `CStar` outside the enum's
selector path. These were built for liquid names. Illiquid names need:

- Low-DoF, heavily-regularized families that stay sane on 5–15 quotes per expiry
  (priors/shrinkage toward a reference surface, monotone-wing constraints, spread-weighted
  objectives).
- Explicit sparse-regime variants of existing families rather than only new families —
  e.g. eSSVI with tightened parameter boxes and vega/spread weighting.
- Wire new types through the whole stack: `CurveConfig`, calibrator, selector candidate
  list (`default_selector_candidates()` currently omits `SplineVol`/`CStar` — decide
  deliberately what belongs in the candidate set), admission, tests.

### WS-2: Curve selector and auto config builder

- `CurveSelector` scores by held-out price-in-band with a parsimony tie-break
  (`SelectorConfig`: `oos_max_expiries`, `parsimony_margin`, `time_budget_ms`). It has been
  validated on liquid names (SPY→ConvexDense, XOM→eSSVI). Harden it for names where
  held-out data barely exists: minimum-evidence floors, tier-aware candidate lists,
  fallback-to-prior behavior, and stable selection (selector flapping between T0-adjacent
  slices is itself a defect — consider a selection-stability diagnostic).
- `select_fit_policy` + `FitPolicyConfig` + the seven `ProfileKind` underlier profiles
  (`atx-vol/include/atx/vol/profile.hpp`) are the auto-config layer. Extend profiling to
  classify the long tail (quote density, spread width, expiry count, underlying type) and
  emit tier-appropriate `SymbolFitConfig`s, including admission posture (`band_k`, policy
  strictness).
- The frozen-config replay path (T1) must be first-class: config in, fit out, zero hidden
  re-selection. Audit `fallback_curve_rungs` and `apply_symbol_config` for silent behavior.

### WS-3: Massively parallel fit/price throughput

- Existing infra: `parallel_for` (deterministic, bit-identical across thread counts,
  `ATX_VOL_FIT_WORKERS`), `PricingExecutor` (process-wide pool, P-core pinning),
  `fit_scheduler`, `surface_db_populate` dynamic board queue, AVX2 batch kernels under
  `atx-vol/src/simd/` (American boundary/greeks, Black-76, IV, eSSVI batches), bench harness
  under `atx-vol/bench/` (Google Benchmark, `rel-avx2` preset, baselines +
  `compare_baseline.py`).
- Full-universe × two slices is thousands of names × dozens of expiries. Targets: saturate
  cores on the universe fit loop, keep per-name fit cost bounded (selector `time_budget_ms`
  honored), and keep everything deterministic — bit-identical reruns are an existing repo
  invariant; do not trade it away for speed.
- Measure before optimizing: add a `universe_generalization_bench` (or extend
  `universe_cycle_bench.cpp` / `fitting_throughput_bench.cpp`) so throughput claims come
  from the bench harness with baselines, not from wall-clock anecdotes.
- Track fit wall-time per name in the benchmark artifact so throughput regressions show up
  on the same scoreboard as accuracy.

### WS-4: Failure handling — deciding we cannot fit a name

- Declining must be a **first-class, honest outcome**, not an exception path. Every decline
  carries a `SurfaceAdmissionReason`-style structured reason; extend the enum if the
  existing 20 reasons don't cover what you see (e.g. no valid parity spot, single-expiry
  board, crossed/locked markets universe-wide).
- Policy questions to resolve deliberately (and document): when do we retry with a simpler
  family vs. decline outright? Is a partial surface (front expiries only) servable under a
  `Mark`-grade contract while declined for `Risk`? (`SurfaceConsumer` grades already exist.)
- No crashes, no hangs, no NaN propagation on any input the OPRA universe can produce. A
  malformed name must cost bounded CPU and produce a decline row, never poison the batch.
- The decline distribution is itself a scoreboard metric: mass declines with vague reasons =
  failing grade even if RMSE on survivors looks great.

---

## 4. Operating loop (recursive self-improvement)

Run repeated cycles. Each cycle:

1. **Research** — read the relevant code and the latest scoreboard; pick the current
   bottleneck (worst tier, worst decline reason, slowest stage). Write down a hypothesis.
2. **Review** — before implementing, review the design against existing seams
   (`atx-vol/docs/`, `atx-vol/docs/seams/`); prefer extending existing structs/APIs over
   parallel systems.
3. **Implement** — via subagents (see §5), TDD, smallest coherent change.
4. **Test** — full relevant test suite (`ctest` labels `atx_vol_fast` inner loop,
   full `atx_vol` before claiming a cycle done). The suite has ~1,867 cases across 131
   files; breaking existing tests is a cycle-blocker.
5. **Benchmark** — re-run the appropriate scale-ladder stage; record the scoreboard delta.
6. **Next steps** — append a cycle log entry (see below) with results, deltas, and the
   ranked list of next bottlenecks. Then start the next cycle.

**Cycle log:** keep a running log file in the worktree (e.g.
`atx-vol/sprints/opra-universe-goal-cycles.md`) — one dated entry per cycle: hypothesis,
what changed (commits), test status, scoreboard before/after, next steps. This log is the
project's memory; a fresh reader must be able to resume from it alone.

**Scoreboard (north star, tracked every cycle):**

| Metric | Definition |
|---|---|
| Coverage | % of universe names served at T0 |
| Honest declines | % of declines with specific (non-catch-all) reasons |
| T1 RMSE (vol pts) | median + p90, per tier and overall |
| T1 % within bid/ask | median + p90, per tier and overall |
| T1 fit failures | count of T0-served names that failed at T1 |
| Throughput | full-stage fit wall-time; bench-harness numbers vs. baseline |
| Determinism | rerun bit-identity check passes |

Cycle 1's job is to **establish the baseline**, not to hit targets. Set improvement targets
yourself from the baseline and revise them in the cycle log as evidence accumulates.

---

## 5. Process constraints (non-negotiable)

- **Fresh git worktree; never merge into local `main`.** Create it with
  `scripts/new-worktree.ps1` (repo convention). All commits stay on the worktree branch.
  Commit frequently with descriptive messages; the branch is the deliverable.
- **Subagent-driven development.** Use the `superpowers:subagent-driven-development` skill
  (and `superpowers:writing-plans` / `superpowers:test-driven-development` /
  `superpowers:systematic-debugging` as applicable). Keep one writer per translation unit
  when dispatching parallel subagents; review subagent output before integrating.
- **Verification before completion.** Never claim a cycle, fix, or benchmark result without
  having run the command and looked at the output (`superpowers:verification-before-completion`).
- **Build system:** CMake presets (`dev` for iteration, `rel-avx2` for perf/bench runs);
  helper `scripts/atx-build.ps1` (e.g. `pwsh scripts/atx-build.ps1 configure`, and
  `pwsh scripts/atx-build.ps1 -Ctest -R <regex>` for tests). Bench runs need
  `ATX_BUILD_BENCH`; examples/CLIs need `ATX_BUILD_EXAMPLES`.
- **Determinism is an invariant.** `parallel_for` is bit-identical for any worker count;
  benchmark outputs must be deterministically ordered; reruns must be bit-identical.
- **Library-first improvements.** No benchmark-only forks of fitting/pricing logic (§3).
  The benchmark measures the library; it must not become a private better version of it.
- **No garbage cycles.** Small runs until confidence is earned (§2.4). If you can't explain
  a number, don't scale it up.
- **Honest reporting.** Failed tests, partial results, and skipped stages get reported as
  such in the cycle log. Fabricated or extrapolated benchmark numbers are the one
  unforgivable sin here.

---

## 6. Starting checklist (cycle 0)

1. Create the worktree and branch.
2. Verify OPRA data access; pull the two 2026-02-10 slices for the smoke subset only
   (~10 names). Confirm loaders parse them and the snapshot timestamps are correct
   (15:00 UTC / 16:00 UTC).
3. Build the benchmark driver skeleton (new example/tool alongside
   `examples/universe_autofit.cpp` — likely named `universe_generalization_bench` or
   similar): T0 select+fit → freeze configs → T1 replay → score → emit artifact + report.
4. Run smoke stage end-to-end; check determinism; write the first cycle log entry with the
   smoke scoreboard.
5. Pull the full-universe slices (both minutes) while iterating on stage 2 of the ladder.
6. Begin cycle 1 against the Top-50 baseline.
