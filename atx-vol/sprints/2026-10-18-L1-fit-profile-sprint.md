# L1 (lite) fit timing counters and per-expiry cost attribution (2026-10-18 / resumed 2026-10-19)

Lane L1-fit-profile of sprint 2026-10-18-vol-sota (plan
`docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md`, W1 "measure", rescoped to "lite" by
PM ruling 6: no sampling profiler, no nine-class harness, no baseline JSON — those move to
W4). Branch `feat/vol-L1-fit-profile-20261018`, base `7cebbbd3` (PM prebase = root after
W0); checkpoint `a1003ec1` (pool-35, user stop), resumed in pool-3 at `b6ba6482`.
`vol-rel`, `-Jobs 2`, `ATX_VOL_FIT_WORKERS=2`. Every timing number here is
**instrumented, unqualified (loaded host)**; counts are load-independent and are the
primary evidence.

## What was built

- `atx-vol/src/fitting/counters.hpp`, namespace `counters::fit_timing` (runtime-armed,
  not behind the compile-time counter gate):
  - `FitTimingCounters` per chain: the brief's fields (`al_price_calls`,
    `al_greeks_calls`, `certification_passes`, `channel_solves`, `jacobian_evals`,
    `rung_attempts`, `calendar_pairs`, `wall_us`, `deam_us`, `solve_us`, `price_us`,
    `certify_us`, `calendar_us`) plus `projections`, `iv_newton_iters`, `project_us`,
    `other_us`, `al_by_bucket[7]`, and the repair-rung cross-cut `repair_rung_visits` /
    `repair_rung_us`.
  - `ChainRecorder` (one slot per expiry), `ChainScope` (makes a slot current on this
    thread, counts rung attempts), `BucketScope`, `count()`, `RepairRungScope`, and
    `SurfaceTrace` (armed by the caller on its own thread; the fitter arms its slots only
    then). `ledger::this_thread(Solve)` reads the calling thread's solve-ledger cell.
- `quote_slice.cpp`: one-line bucket scopes (deam: point admission and channel
  observations; project: `project_in_channel`; price: certification grid reprice;
  certify: certificate, repairs, row re-implication; solve: point curve solve and channel
  solve; calendar: both calendar repairs) and counts.
- `quote_surface.cpp`: chain scopes in every per-expiry pass, rung counts, the
  repair-rung cross-cut, `publish_timing()`.
- `quote_bench_main.cpp`: opt-in `--timing-detail` (quote-slice only) →
  `graded_stats.slices[].timing`, `graded_stats.timing_detail{surfaces, surface_wall_us,
  chain_wall_us}`, `chain_latency_scope`, `timing_detail_semantics`; with the flag a graded
  chain's latency is its own fit wall, so `chain_latency_us_p50/p95/p99` are real
  percentiles (before: surface wall / expiries, p50 = p95 = p99).

## Semantics (binding for readers of the JSON)

- **Buckets are EXCLUSIVE, not inclusive** (PM-accepted deviation from the brief's
  "inclusive wall per bucket"): a scope opened inside another charges its own time and the
  outer bucket resumes on exit, so `deam + solve + project + price + certify + calendar +
  other = wall_us` exactly (per-bucket microsecond rounding). The report's
  `timing_detail_semantics` string says so.
- `repair_rung_us` is a cross-cut, not a bucket: the INCLUSIVE wall of the surface
  fitter's repair-rung visits (`repair_rung`, `ladder_repair_rung`); it overlaps the
  buckets their refits charge and is not part of `wall_us`. Added in the resume so the
  attribution can report the serial "ladder repair" stage the PM asked for.
- AL work = the fitting thread's own solve-ledger deltas (`AlBoundarySolves`,
  `IvNewtonIters`, Greek bundles). Work a fit hands to other threads is not counted.
- Zero cost off: one thread_local pointer load and a branch per scope; no clock read,
  atomic or allocation; no fitted value changes. Proven by the flag-OFF parity hashes.
- Slot-local (PM ruling for the L2d merge): slot i is written only while expiry i is
  fitted, by the thread fitting it; no shared counter across expiries, no serial-order
  assumption. L2d's per-expiry fan-out keeps the `ChainScope` as the first line of each
  per-expiry body.

## Attribution (six single runs, instrumented, unqualified host)

Folder `C:/atx-cache/quote-benchmark/L1-20261019-attribution/` (`RESULTS.md`,
`sha256.txt`, `attribution/per-chain.csv` for all 152 chains). Binary sha256 `d50fe401…`
(`b6ba6482`), policies graded-v1 `d7f89d13…` and knot-ladder-v2-point `61ec4c1a…`, inputs
`fit-timing-20261017-cohort5/input/quotes-{SPY,AAPL,CRWV}.csv` (whole symbol surface per
run), `ATX_VOL_FIT_WORKERS=2` (this base fits graded expiries serially). Host load 26-56%
before / 52-66% after (other lanes running). The six `--timing-detail` CSVs are
byte-identical to the 2026-10-17 `fit-timing-20261017-cohort5/runs2` CSVs (SPY graded-v1
`087e6a54`), so the flag changes no output.

Whole surface, % of chain wall (exclusive buckets; inversions = deam, certification =
price + certify, solve = solve + project; ladder repair = the inclusive cross-cut):

| policy | symbol | per-chain p50 / p95 / p99 ms | inversions | certification | solve | calendar | ladder repair | refused chains' wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| graded-v1 | SPY (32 chains, 4 served) | 3,018 / 5,596 / 6,003 | 91.4% | 8.3% | 0.2% | 0.0% | 0 | 95.6% |
| graded-v1 | AAPL (24, 16) | 688 / 2,173 / 3,022 | 87.2% | 11.8% | 0.2% | 0.7% | 0 | 61.5% |
| graded-v1 | CRWV (20, 20) | 22 / 42 / 43 | 51.9% | 43.9% | 0.5% | 3.7% | 0 | 0 |
| ladder | SPY (32, 6) | 4,756 / 10,989 / 11,724 | 87.9% | 9.6% | 2.4% | 0.2% | 15.8% | 90.9% |
| ladder | AAPL (24, 17) | 1,324 / 2,651 / 2,797 | 83.6% | 14.3% | 1.7% | 0.4% | 3.3% | 46.6% |
| ladder | CRWV (20, 20) | 202 / 841 / 1,339 | 87.6% | 10.1% | 2.1% | 0.2% | 8.2% | 0 |

"Other" is ≤ 0.1% everywhere, and the chain wall equals the surface wall to 0.01 s.

Target chains (wall; inversions / certification / solve ms; AL solves deam / price;
rung attempts):

| policy | chain | grade | wall ms | inv. | cert. | solve | AL deam / price | rungs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| graded-v1 | SPY 2026-11-06 (23 d, 334 quotes) | refused (arbitrage) | 2,952 | 2,664 | 275 | 12.5 | 5,729 / 1,002 | 3 |
| graded-v1 | AAPL 2026-11-20 (37 d) | refused (calendar) | 688 | 585 | 100 | 2.7 | 1,178 / 336 | 2 |
| graded-v1 | CRWV 2026-11-20 (37 d) | B | 26 | 8.7 | 8.4 | 0.0 | 18 / 41 | 1 |
| ladder | SPY 23 d | B | 1,384 | 1,187 | 190 | 7.0 | 3,024 / 668 | 2 |
| ladder | AAPL 37 d | B | 2,356 | 1,909 | 393 | 41.0 | 4,514 / 840 | 5 |
| ladder | CRWV 37 d | B | 217 | 190 | 26 | 0.5 | 289 / 82 | 2 |

Unit costs (unqualified): ~420-500 us of inversion wall per AL boundary solve; grid reprice
~210-260 us per AL solve; one certification pass 93-101 ms on SPY, 33-37 ms AAPL, 10 ms CRWV.
The certificate itself is cheap (`certify` 0.02 s of 8.2 s on SPY graded-v1).

## Hypotheses (counts decide; wall shares second)

- **The channel solve reprices no American option: CONFIRMED.** `al_by_bucket.solve` and
  `.project` are 0 on all 152 chains of the six runs (280 channel solves, 5,740 solver
  iterations, 127 projections). Plan §0 (a) "the channel solve iterates in American price
  space" is REFUTED: the solve is in vol space and costs 0.2-2.4% of the wall.
- **Inversions + certification dominate: CONFIRMED**, 95.8-99.7% of the chain wall. The
  American IV inversions alone are 84-91% on SPY and AAPL under both policies (88% on
  CRWV ladder). Certification is 8-14% there, and it is the grid reprice, not the
  certificate. CRWV graded-v1 (thin, 1 rung) is the exception at 52% / 44%.
- Plan §0 (b) "nothing memoised between rungs": CONFIRMED. Inversion work scales with rung
  attempts (SPY graded-v1: 2,176 deam AL solves per rung attempt over 88 attempts; SPY 23 d
  inverts the same 334 quotes in each of its 3 rungs).
- Plan §0 (c) "refusals discovered only after the full solve": CONFIRMED. On SPY
  graded-v1, 28 of 32 chains are refused after 82 full certification passes, and the
  refused chains are 95.6% of the wall (ladder: 26 of 32, 90.9%).
- Plan §7 "projection vs AL pricing" on SPY 23 d: AL pricing. There were 0 projections
  (0 ms) against a 275 ms grid reprice under graded-v1, and 0 ms against 178 ms under the
  ladder. On the whole SPY ladder surface: projection 3.98 s, grid reprice 15.15 s,
  inversions 166 s. The decisive axis is the inversions, not projection vs reprice.

## Plan §1.1 budget paragraph (for the PM to lift into the plan at integration)

> **Measured per-expiry cost split and the 30 ms budget (L1, 2026-10-19, instrumented,
> unqualified).** SPY 23 d (334 quotes) costs 2.95 s under graded-v1 and 1.38 s under the
> ladder. AAPL 37 d costs 0.69 s / 2.36 s, CRWV 37 d 26 / 217 ms. Of the chain wall,
> 84-91% is American IV inversion of the quotes, repeated in every rung (about 1,000
> inversions per channel rung at about 1 ms each). 8-14% is the certification grid
> reprice: 93-101 ms per pass on SPY, one pass per rung. The vol-space solve is ≤ 2.4%
> (0 American prices), calendar checks ≤ 3.7%, and the serial ladder repair rung is 15.8%
> of the SPY ladder surface. The 30 ms per-expiry budget, single thread, all rungs
> included:
> **inversions ≤ 12 ms** (each quote inverted once per snapshot and shared by every rung,
> ÷2-3; plus about 100x per inversion, ~1,000 inversions × ≤ 12 us, from the shared
> expiry geometry / boundary handle and batched inversion; owners: L2c rung memo, carried
> by L2a's per-quote IV cache, and L2b);
> **certification ≤ 8 ms** (one accurate pass per expiry; Fast screen → accurate confirm
> on the shared geometry; no re-certification of an unchanged candidate; pre-screen
> refusals before any solve, since 28 of 32 SPY chains are refused only after full passes;
> owners: L2b screen/pack, L2c memo/pre-screen);
> **solve + projection ≤ 5 ms** (already in vol space; 7-12.5 ms today on SPY 23 d; L2a
> keeps it there);
> **calendar ≤ 2 ms** (reads certified prices; no lane needed);
> **ladder repair ≤ 3 ms amortised** (its refits reuse the memoised inversions; L2c, with
> L2d fanning the rung out because it refits independent expiries).
> 12 + 8 + 5 + 2 + 3 = 30 ms. Thirty-two SPY expiries at 30 ms give 0.96 s single-thread,
> within the ≤ 1.0 s target. The all-core ≤ 150 ms target then needs ≥ 6.4x from L2d's
> per-expiry fan-out, with the serial stages (calendar pass, ladder repair rung, term
> priors) held to ≤ ~20 ms per surface. Today the ladder repair rung alone is 29.9 s
> serial on SPY, so the memo must make it cheap or L2d must fan it out. Order of
> magnitude: SPY 23 d is 46-98x over the median target and ~90% of the gap is inversions,
> so the single-thread levers are L2c + L2b and the multi-core lever is L2d.

## Gates (vol-rel, pool-3, lane tip `b6ba6482`)

| Gate | Result |
| --- | --- |
| `atx-vol-quote-slice-tests.exe` (all 124 cases, incl. `QuoteSurfaceLadder.*` and `QuoteSurfaceTiming.*`) | 124 passed |
| `--gtest_filter=QuoteSurfaceTiming.*` | 5 passed |
| ctest `^atx-vol-quote-slice$` + `^atx-vol-quote-benchmark-native$` | 2/2 passed |
| native CLI pytest (`test_quote_bench_cli.py`, `test_common_markov_bench_cli.py`) | 162 passed |
| PCH-off `vol-hygiene` `check` of quote_surface.cpp, quote_slice.cpp, tools/quote_bench_main.cpp, tests/quote_surface_test.cpp (cover `counters.hpp`) | 4/4 exit 0, no diagnostics |
| Flag-OFF parity, binary `d50fe401…` (`ATX_VOL_FIT_WORKERS=2`) | dev200 default `c89d0739` MATCH, dev200b default `4b273b08` MATCH, cohort5 graded-v1 raw `171fa2d2` MATCH |
| Flag-OFF report schema (key paths vs integrate-W0 reports) | dev200 268 = 268, dev200b 236 = 236, cohort5 graded-v1 312 = 312; 0 added, 0 removed |

The ctest `^atx-vol-quote-slice$` filter (`tests/CMakeLists.txt:37-38`) lists neither
`QuoteSurfaceLadder.*` (L2d's finding) nor `QuoteSurfaceTiming.*`: both suites run only
through the focused exe's direct invocation (above) and the monolith. The registration fix
belongs with L2d's ladder registration (one `string(JOIN …)` line both lanes would edit).

## Fail-on-revert (transient edits, each restored by inverse Edit; blob ids proven)

1. Counters armed only via `SurfaceTrace` + the repair-rung cross-cut: removing the
   point_pass `ChainScope` and both `RepairRungScope` lines fails
   `ArmedTraceCountsEachPointServedExpiryWithoutChangingTheFit` (rung_attempts 0 vs 1,
   certification_passes 0 vs 1, IV iterations 0, price-bucket AL 0) and
   `BothRepairRungsTimeOnlyTheExpiryTheyRefit` in BOTH iterations (graded `false` and
   knot-first `true`: repair_rung_visits 0 vs 1, repair_rung_us 0). Restored:
   `git hash-object` = `5d871ceb…` = `b6ba6482:quote_surface.cpp`.
2. Percentile fix (behind the flag): restoring `push_back(per_chain)` fails the CLI test
   (`assert 194.45 == 130.0 ± 1.0e-09`, test_quote_bench_cli.py:646). Restored:
   `git hash-object` = `f85ed74c…` = `b6ba6482:quote_bench_main.cpp`; `git status
   --porcelain` empty; rebuilt; gates above re-run green.

## Deviations from the brief

- Exclusive, not inclusive, bucket wall (PM-accepted). Projection is its own bucket so
  the §7 question has a number.
- The percentile fix is behind `--timing-detail` (real per-chain wall needs the
  in-library clocks, which must stay off by default); default JSON and CSV are unchanged.
- `al_price_calls` counts AL boundary solves on the fitting thread (every AL price,
  including those inside American IV inversions); `iv_newton_iters` sits beside it.
- Added in the resume: the `repair_rung_visits` / `repair_rung_us` cross-cut, so that
  "ladder repair" is a reported column.
- The budget paragraph lives here, not in `docs/plans/*` (PM ruling). The plan's sampled
  profile, nine-class `fit_latency` harness and baseline JSON stay deferred to W4
  (ruling 6).
- The six runs fit whole symbol surfaces at `ATX_VOL_FIT_WORKERS=2`, not `fit_workers = 1`.
  On this base the graded quote-bench path is serial over expiries and every AL count is
  on the fitting thread, so the counts do not depend on the cap.

## Open

- Register `QuoteSurfaceTiming.*` (and `QuoteSurfaceLadder.*`) in the
  `^atx-vol-quote-slice$` ctest filter when L2d fixes the ladder registration.
- After L2d's fan-out merges, the per-thread AL deltas still attribute correctly: each
  worker reads its own ledger cell. Work that a fit hands to a further pool, such as
  PricingExecutor, would not be counted. Re-check `al_by_bucket` totals against a
  1-worker run once at G2.

