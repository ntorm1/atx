# L2d scheduler sprint: deterministic per-expiry fan-out (2026-10-18, resumed 2026-10-19)

Lane `L2d-scheduler` of the vol-sota sprint (`docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md`,
W2 "L2d"; workspace `.superpowers/sdd/2026-10-18-vol-sota/`). Branch
`feat/vol-L2d-scheduler-20261018`: implementation `b1dc4243`, tests `3391b6a1`, ctest registration
`c7e50f17`, this note on top. Base: PM prebase `feat/vol-w2-prebase-20261018 @ 7cebbbd3` (same source
tree as the W0-integrated root). Evidence folders: `C:/atx-cache/quote-benchmark/L2d-20261018-workers/`
(checkpoint runs) and `C:/atx-cache/quote-benchmark/L2d-20261019-fanout/` (resume runs, `RESULTS.md`,
`sha256.txt`).

## 1. Finding: the quote-bench graded path fitted expiries serially

The plan's "SPY surface, `fit_workers = 0`: 20-35 s (3 workers)" row was a serial number. `fit_graded`
-> `fit_quote_surface` -> `point_pass` / `knot_pass` looped the expiries one by one; nothing on that path
reached a `parallel_for`, so `ATX_VOL_FIT_WORKERS` had no effect on it (cohort5 at "3 workers" took
89.7 s against ~74 s summed at 1 worker). `fit_workers` reached only `curve_fit.cpp` (surface-db /
session path), which already resolved `0` to `hardware_concurrency` (`parallel_for.hpp`
`atx_resolve_fanout_workers`; env "0" pinned by `core/ParallelFor.AutoWorkerCountHonorsEnvCap`) and
already ran its prepass longest-first by strike count. The PM redirected the lane to build the
per-expiry fan-out in `quote_surface.cpp` (ruling 8 of the sprint).

## 2. What changed

- `src/fitting/quote_surface.cpp`: `SurfaceFitter::for_each_expiry(tasks, body)` runs
  `parallel_for_dynamic` over `QuoteSurfaceConfig::fit_workers`, tasks ordered densest first by the
  strictly two-sided quote count (`cost_`, computed in the constructor; ties by index). Used by
  `point_pass`, `knot_pass` (the ladder's pass A), and the channel stages B1 and B2. Serial and
  unchanged: priors, `calendar_pass`, `ladder_calendar_pass`, `repair_rung`, `ladder_repair_rung`,
  `dominance_pass`, `measure_containment`.
- `include/atx/vol/api/fitting/quote_slice.hpp`: `QuoteSurfaceConfig::fit_workers` (last member,
  default 1 = the historical serial path on the calling thread; N = N workers; 0 = AUTO:
  `ATX_VOL_FIT_WORKERS` when a positive integer, else `hardware_concurrency`).
- `src/fitting/curve_fit.cpp`: one helper `chains_longest_first` (stable sort by strike count,
  descending) for all three per-chain fan-outs: the prepass (same order as before), the phase-1.5
  carry repair and the phase-1.75 starved retry (both index order before).
- `src/fitting/expiry_schedule.hpp` (new, internal): `detail::longest_first` and the test-only
  thread_local dispatch trace (`note_expiry_dispatch`, `ScopedExpiryDispatchTrace`, the counters.hpp
  `t_step_sink` pattern; unarmed cost one thread_local load per expiry).
- `tools/quote_bench_main.cpp` (one hunk): the graded fit runs at `fit_workers = 0` (AUTO).
- `tests/CMakeLists.txt`: the `^atx-vol-quote-slice$` suite list now includes `QuoteSurfaceLadder.*`
  (section 5).

Determinism contract (`for_each_expiry`'s comment): a body writes only expiry i's slots (`out_[i]`,
`breadth_[i]`, `two_sided_[i]`, `held_point_[i]`; distinct vector elements, no `vector<bool>`) and
reads only state no task writes (`expiries_`, the configs, a `TermPrior` built before the fan-out and
immutable after construction). The fan-out joins before returning; `parallel_for_dynamic` rethrows the
first exception after the join, as the serial loop propagated it. Thread-local state on the path
(`t_alo_slot`, `AloPricer::reset` consumes no prior-contract state) and counters never feed the fit.
The carry memo is not on this path (graded admission requires supplied carry). No lock was added.

## 3. Deviations (all three ACCEPTED by PM ruling, 2026-10-19) and their hash proofs

| Deviation | Why | Proof that bytes did not change |
| --- | --- | --- |
| B1 / B2 channel refits fanned out too (the ruling named pass A and knot_pass) | Each stage's prior is built before its loop and only read inside it; each body writes only `out_[i]` | cohort5 graded-v1 at workers {1,2,7,0} = `171fa2d2` (W0 raw); SPY graded-v1 at {1,4,8,0} = `087e6a54`; cohort5 ladder point / strict at {1,0} = W0 keys-ON hashes (section 4) |
| `fit_workers` member added to `quote_slice.hpp` (file was on the forbidden list) | The struct the ruling asked to extend lives there | Library default 1 = serial historical path; PCH-off clean on every direct includer (section 5) |
| quote-bench graded fit uses all cores when `ATX_VOL_FIT_WORKERS` is unset | Matches the house AUTO convention (surface_db_build, chain_export); every parity/benchmark script sets the variable | Identity at 0 = identity at 1 on every graded / ladder output above; default parity at 0: dev200 auto `c89d0739`, dev200b auto `4b273b08` MATCH |

## 4. Hash identity and parity

Resume binary built at `c7e50f17` (sha256 `f22e9cfd...`; library and tool source identical to
`b1dc4243`), runner `L2d-20261019-fanout/scripts/run_l2d_resume.ps1`, `--dividends funding-pv`, one run
per row. Every row exit 0, stderr empty.

| Output | `ATX_VOL_FIT_WORKERS` | sha256 | Expected | Verdict |
| --- | --- | --- | --- | --- |
| dev200 default (auto) | 0 | `c89d0739` | W0 G0 `c89d0739` | MATCH |
| dev200b default (auto) | 0 | `4b273b08` | W0 G0 `4b273b08` | MATCH |
| cohort5 knot-ladder-v2-point | 1, 0 | `232d2c0d` | W0 keys-ON `232d2c0d` | MATCH x2 |
| cohort5 knot-ladder-v2-strict | 1, 0 | `21f4ee58` | W0 keys-ON `21f4ee58` | MATCH x2 |
| cohort5 graded-v1 (checkpoint) | 1, 2, 7, 0 | `171fa2d2` | W0 raw `171fa2d2` | MATCH x4 |
| SPY graded-v1 surface (checkpoint) | 1, 4, 8, 0 | `087e6a54` | `087e6a54` | MATCH x4 |

The ladder rows exercise `knot_pass`, B1 and B2 on worker threads (the W0 hashes came from the serial
root binary), so they are the end-to-end proof of the B1/B2 deviation under the ladder policies too.
Wall through the runner (unqualified): ladder point 187 -> 78 s, strict 197 -> 65 s at 1 -> 0 workers.

## 5. Tests, revert checks, ctest gap, hygiene

- `QuoteSurface.FanOut*` (4, in `quote_surface_test.cpp`): bit-identity for workers {2,7,0} vs 1 with
  and without the knot-first ladder; densest-first dispatch for the point pass and the ladder's knot
  pass (`{3,2,1,0}`); densest-first for pass A + B1 + B2 (`{2,1,0,2,1,0,2,1,0}`); work leaves the
  calling thread above one worker. Revert checks (checkpoint): dropping `longest_first` in
  `for_each_expiry` fails the two order tests (`{0,1,2,3}`, `{0,1,2,0,1,2,0,1,2}`); a serial loop in
  place of `parallel_for_dynamic` fails the off-thread test; blob restored `9c9bbc26`.
- curve_fit (4): `CurveFitCarryFallback.RepairPassDispatchesTheDensestExpiryFirst` (`{0,3,2,1,3,2,1}`),
  `RepairPassBitIdenticalAcrossWorkers`, `CurveFitParallel.StarvedRetryDispatchesTheDensestChainFirst`
  (`{3,2,1,0,3,2,1,0}`), `StarvedRetryBoardBitIdenticalAcrossWorkers`. Revert (resume): removing the two
  `chains_longest_first` calls gives `{0,3,2,1,1,2,3}` and `{3,2,1,0,0,1,2,3}` (both order tests fail,
  both identity tests stay green as they must); restored blob `58e0c019`.
- Gates (vol-rel): the nine curve_fit suites 32/33 with the one PRE-EXISTING ledgered failure
  `CurveFitSliceFallback.LinearFallbackRecoversForcedSplineFailures` (same message as the 2026-10-16
  base log); `^core/ParallelFor\.` 8/8; `^atx-vol-quote-slice$` 123/123.
- **ctest gap closed:** `^atx-vol-quote-slice$` runs the exe with a `--gtest_filter` suite list that
  omitted `QuoteSurfaceLadder.*`, so the 17 knot-ladder tests (integrated in W0) never ran under it:
  106 cases from 8 suites before, 123 from 9 after (`c7e50f17`).
- PCH-off (`vol-hygiene`) on the touched TUs and every direct `.cpp` includer of `quote_slice.hpp` and
  `expiry_schedule.hpp`: 16 objects, exit 0, 0 diagnostics. `tools/surface_archive_bench_main.cpp` is
  not configured in the hygiene tree.

## 6. Numbers (unqualified, loaded shared host)

SPY graded-v1 surface (33 chains, `fit-timing-20261017-cohort5/input/quotes-SPY.csv`, direct binary,
one run each, checkpoint binary from `b1dc4243`, host load 39-81% with other lanes building):

| `ATX_VOL_FIT_WORKERS` | wall s | fit+reprice s | speed-up vs 1 |
| --- | --- | --- | --- |
| 1 | 87.79 | 87.38 | 1.0x |
| 4 | 30.53 | 30.26 | 2.9x |
| 8 | 21.36 | 21.07 | 4.1x |
| 0 (16 threads) | 15.15 | 14.70 | 5.8x |

All four prediction files sha256 `087e6a54`. cohort5 graded-v1 through `quote_benchmark.py`: 128 / 79 /
37 / 29 s at {1,2,7,0}. The plan target "SPY surface ≤ 150 ms all cores" still needs the per-expiry
levers (L2a/L2b/L2c): the fan-out removes the serial-expiry factor only, and the 16-thread host has
8 E-cores, so the all-core gain past 8 workers is small.

## 7. Follow-ups (not in this lane)

- L2c: `repair_rung` and `ladder_repair_rung` are slot-local per expiry too and could fan out the same
  way; kept serial per the PM ruling (cross-expiry calendar order).
- W2 merge with L1 (counters): `quote_surface.cpp` lines this lane owns at `c7e50f17` (new-file
  numbers) 26-29, 43-44, 196-200, 231-255, 257, 264, 273, 327, 338, 360, 687-689, 691, 696, 699,
  702-703, 716, 1116. Any L1 counter placed inside point_pass / knot_pass / B1 / B2 now runs on a
  worker thread and must stay slot-local.
