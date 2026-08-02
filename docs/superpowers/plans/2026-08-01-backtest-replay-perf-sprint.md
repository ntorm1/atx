# Backtest Replay Performance Sprint — SP100 Dispersion Ladder

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the SP100 projection-strangle YTD replay (141 sessions, ~12.7k-lot steady-state book) from **175 s wall / 1,810 s CPU** toward **≤ 90 s wall / ≤ 1,000 s CPU** with **zero change to reported economics** — every task in Phases 1–3 must pass the economic-parity gate against the reference `track.tsv` (byte-identical preferred; ULP-class drift acceptable only with a measured perf win, see Global Constraints).

**Owner directives (2026-08-01, supersede the original draft):**
1. **Byte identity is negotiable; economics are not.** A task MAY break byte identity of `track.tsv` when — and only when — it delivers a measured performance gain and the difference is economically null (certified by the comparator defined below). Byte-identical remains the preferred and expected outcome for scheduling/laziness/memoization changes; numeric-path changes (summation order, FMA contraction, pack-width effects within the documented lane band) may land under the parity band.
2. **Prefer structural fixes over knob tuning.** Replacing the static scheduler outright, restructuring pack accumulation/flush ownership, redesigning per-step substrate lifetime are all in scope where they beat parameter tweaks. Thread-count bit-identity of the SAME build remains a hard engine contract regardless.

**Architecture:** The run's cost is fully characterized (four independent code reviews + empirical scaling probes, 2026-08-01). Per-session CPU ≈ 1.0 s fixed + 0.26 s × live cohorts; at the ~62-cohort plateau **~94% of CPU is the daily whole-book mark**: ~1,009 µs per contract-day ≈ one 5-solve analytic Andersen–Lake FullGreeks bundle (81%) + 1 mark + 1 IV resolve, on the cold `LegacyCompatible` tier. The wall/CPU gap (10.34× concurrency on 16 threads = 65%) is scheduling: a static equal-count `run_ranges` partition of ~100×-variance work on unpinned P/E hybrid cores. The SIMD greek kernel is 4-lane AVX2 (runtime-dispatched — already active in the shipped `_core`) but receives **1-lane packs**, because packing is gated on raw-bit-identical-T runs and 63 overlapping cohorts give every `(uid, side)` group 63 distinct expiries. The sprint therefore attacks, in order: measurement (Phase 0), scheduling (Phase 1), pack occupancy (Phase 2), load-lane waste (Phase 3), then the FMA-contraction build (Phase 4, measure-then-land under the parity band).

**Tech Stack:** C++20 (atx-vol engine), clang-cl 18 Release, AVX2 runtime-dispatch SIMD kernels, pybind11 `_core`, GoogleTest + repo bench harness (`bench/`, `compare_baseline.py`).

## Global Constraints

- Builds ONLY via `.\scripts\atx-build.ps1 build atx-vol-tests --parallel 8` (never bare cmake/ninja for the monorepo; the standalone python project's documented cmake route is the accepted exception). Release preset: add `-Preset rel`. ctest via `.\scripts\atx-build.ps1 -Ctest -R "<regex>"`.
- **Economic-parity gate (the accuracy contract of this sprint, amended by owner directive):** the reference run is `run_sp100_strangle_backtest.py --db C:\atx-scratch\surface-db\sp100-2026 --from 2026-01-02 --to 2026-07-31 --exclude BK` at the sprint base commit; its `track.tsv` is the golden. After EVERY task in Phases 1–4, re-run and compare `track.tsv` with `atx-vol/tools/compare_track.py` (built in Task 1). Outcomes:
  - **PASS/byte:** files byte-identical — the default expectation for scheduling, laziness, memoization, and dead-code tasks.
  - **PASS/parity:** bytes differ but ALL of: identical row count, identical column set, identical values in every integer/count/date/id column, every float column within relative 1e-9 (absolute 1e-12 where the reference value is 0). Allowed ONLY when the task's ledger entry records (a) the measured perf delta, (b) per-column max abs and rel drift, (c) one sentence on the numeric mechanism (e.g. "flush-order summation regroups within kLanedGreeksRelBand"). A parity pass without a measured perf win is a FAILED task — revert to the byte-identical formulation.
  - **FAIL:** anything outside the band, any row/column/count difference — failed task, no exceptions.
- These four gates stay green after every task: `SurfaceDbDispersionBacktest.BitIdenticalAcrossThreadCounts` (bit_cast-exact, same-build thread-count determinism — HARD, not relaxed by the parity amendment), `PricedSurface.EvaluateBatchLanedGreeksPackCompositionInvariant` (as amended by Task 7), `SpyDispersionPnl.*`, `UnpricedTolerance.*`.
- New scheduling must keep pack/tile membership book-determined, never thread-determined.
- **Forbidden (economics-changing — rejected with evidence, do not re-propose; NOT covered by the parity amendment):**
  - AL preset change `al_fast_opts{7,16,4}` → `al_bulk_opts` (1.81× but materially different marks; american.hpp:102-106).
  - `QueryPricingTier::RepresentativeFast` (29× but final_nav_delta −14,567 on a −167k NAV; e2e-hotpath baseline).
  - Narrowing `PriceOptions::greek_needs` on the risk frame (zeroes emitted `pnl_rho`/`pnl_charm` attribution) — PM decision, not a perf patch.
  - Loosening the strike-solve tolerance (strategy.cpp:186) or SoA delta wavefront (books different strikes).
  - `/fp:fast` (banned repo-wide), LTO on atx-vol (baseline policy), `adjoint_greeks` (no base-risk stamp ⇒ net loss).
- `C:\atx-data` is READ-ONLY. The scratch corpus `C:\atx-scratch\surface-db\sp100-2026` is the parity corpus — read-only during this sprint.
- Never print environment variables or API keys.
- Commit messages end with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Measured baseline (2026-08-01, i7-1260P 4P+8E/16T, Release clang-cl `_core`)

| Window | Sessions | Wall | CPU | CPU/session |
|---|---|---|---|---|
| Jan | 20 | 9.2 s | 74.9 s | 3.7 s |
| Jan–Mar | 61 | 59.8 s | 552 s | 9.1 s |
| YTD | 141 | **175.0 s** | **1,810 s** | 12.8 s |

Cost model (fits all three within 6%): `CPU/session ≈ 1.0 + 0.26 × live_cohorts`. Solve accounting: 1,009 µs/contract-day ≈ 6.2 cold-AL solve-equivalents at the measured ~165 µs/solve; bundle : mark : IV ≈ 5 : 1 : 1. Concurrency 1,810/175 = 10.34 of 16 (65%).

Key bench anchors (committed baselines): `amer/american_delta` ≈ 59 µs, `amer/american_greeks_al` ≈ 300 µs (= 5×delta ✓), `al_fast_opts` ladder rung 47.1 µs/solve, `port/floor/greeks_analytic/u2688` = 636 µs/unique, ConvexDense anchor ≈ 4.13 µs (65 black76 calls, 64-iteration bisection).

---

## Phase 0 — Measurement substrate (no behavior change)

### Task 1: Solve-ledger + phase visibility + parity comparator

**Files:**
- Modify: `atx-vol/python/src/bindings/backtest.cpp` (bind the always-on solve-ledger counters)
- Modify: `atx-vol/tools/run_sp100_strangle_backtest.py` (print/emit ledger deltas around `run_backtest`)
- Create: `atx-vol/tools/compare_track.py` (the economic-parity comparator — the sprint's gate tool)
- Test: `atx-vol/python/tests/test_run_sp100_strangle_backtest.py`, `atx-vol/python/tests/test_compare_track.py`

The per-thread solve ledger (`counters.hpp:700-765`: `sl_al_boundary_solves`, `sl_greeks_analytic`, `sl_greeks_fd`, `sl_duplicate_mark_solves`) is compiled into Release unconditionally and is the ground truth every later task is judged against. Bind a snapshot/reset pair (e.g. `solve_ledger() -> dict`, `reset_solve_ledger()`), have the driver emit the per-run totals into `tearsheet.tsv` (keys `sl_*`) and stdout. `track.tsv` untouched.

`compare_track.py` contract: `python compare_track.py GOLDEN CANDIDATE` → exit 0 + line `PASS byte` when files byte-equal; exit 0 + `PASS parity max_rel=<x> max_abs=<y> worst_col=<name>` when the parity conditions hold (identical row count, identical header, integer/date/id columns byte-equal per cell, float columns within rel 1e-9 / abs 1e-12-at-zero); exit 1 + `FAIL <reason>` otherwise, naming the first offending column/row. Column type classification: a column is float iff any cell parses as float with `.`/`e` present; else exact-match. Report per-column max drift on parity passes.

- [ ] Failing pytest: `tearsheet.tsv` carries `sl_al_boundary_solves > 0` after a fixture run; two identical runs report identical ledger totals (determinism of the count itself).
- [ ] Failing pytest for `compare_track.py`: byte-equal → `PASS byte`; float drift 1e-12 → `PASS parity`; float drift 1e-6 → `FAIL`; integer cell differs → `FAIL`; row count differs → `FAIL`.
- [ ] Bind + emit + comparator. Run the pinned file-scoped test lane. PASS.
- [ ] Run the reference YTD replay once at the sprint base; record the ledger totals in the sprint workspace as `baseline-ledger.txt` and stash the golden `track.tsv` path in the ledger. Sanity: `sl_al_boundary_solves / 141 sessions / 12,726 uniques ≈ 6.2`.
- [ ] Economic-parity gate (trivially PASS/byte — no engine change). Commit.

### Task 2: A/B instrumentation runs (no code)

Controller-executed, results recorded in the sprint workspace:
- [ ] `ATX_SIMD_ISA=ForceScalar` vs unset on the 61-session probe — quantifies what the laned AVX2 greek kernel is worth on this exact workload today (expected band from `boundary_batch` benches: 1.5–1.7×; results differ by the documented `kLanedGreeksRelBand=1e-9`, so compare timing only, not track bytes).
- [ ] `-DATX_VOL_PROFILE=ON` build (`build-relprof/` precedent exists) of the C++ test binary; run the dispersion e2e fixture and read `strategy_step` / `snapshot_load` phase shares.
- [ ] Capture the missing solver bench baselines (`strangle/eval/resolve`, `strangle/eval/delta` have code but no baseline rows) under the quiet-host protocol.

### Task 3: Close the bench compare-gate ISA hole

**Files:** Modify: `bench/compare_baseline.py` (~line 106-138), `bench/bench_main.cpp` (~line 61-70). Test: extend the bench harness's own gate test if present, else a small pytest over `compare_baseline.py`.

`compare_baseline.py` host check carries no ISA: an `-sse2-` baseline silently gates a `rel-avx2` run. Add build-ISA to the bench `context` on write and refuse mismatched compares. Prerequisite for Phase 4 to be measurable at all.

- [ ] Failing test: comparing an `-sse2-` baseline against a context tagged avx2 exits with a named error.
- [ ] Implement, gate, commit. (No parity gate needed — tooling only.)

---

## Phase 1 — Scheduling: close the 65% → ~90% utilization gap (expect PASS/byte)

### Task 4: Dynamic tile schedule for the FullGreeks pass

**Files:**
- Modify: `atx-vol/src/portfolio_pricer.cpp` (~line 932-943: the `run_ranges(n_unique, ...)` fall-through), `atx-vol/src/prepared_portfolio.cpp` (add greek tiles), `atx-vol/include/atx/vol/prepared_portfolio.hpp`
- Test: `atx-vol/tests/portfolio_pricer_test.cpp`, `atx-vol/tests/prepared_portfolio_test.cpp`

The dominant pass (5-solve bundle, 83% of solve volume) is statically partitioned into fixed contiguous per-worker ranges over work whose per-contract cost spans ~100× (American put bundle ~0.6-0.9 ms vs q=0 European-shortcircuit call ~4 µs), grouped side-homogeneously — a classic straggler tail, worsened by unpinned P/E cores and `ceil`-block starvation (`n = k·nt+1` idles 44% of the pool). The Marks arm already has the correct pattern: **immutable tiles + `run_blocks`** (portfolio_pricer.cpp:914-926, :1763-1781 — the comment there names this exact fix). Add `greek_tiles_` to `PreparedPortfolio` — `(uid, side)` groups chunked at a fixed width, NOT T-subdivided at the tile level — and schedule the greek pass over tiles dynamically (`run_dynamic` over tile indices, or `run_blocks` if measurement shows it suffices). **Structural replacement of the static partition is preferred over tuning its block size** (owner directive 2). Tile membership must be book-determined so pack composition never depends on worker count — strictly stronger than today.

- [ ] Failing/pinning tests first: (a) greek tiles are identical for `n_threads ∈ {1, 4, 0}` (bit_cast-compare the tile table); (b) a mixed put/call fixture's FullGreeks results are bit-identical to the pre-change path (golden captured before the refactor lands).
- [ ] Implement. Gates: full `portfolio_pricer_test`, `BitIdenticalAcrossThreadCounts`, pack-composition invariance.
- [ ] Measure: 61-session probe wall before/after (expect 15-35% step-wall reduction; CPU roughly flat). Record.
- [ ] **Economic-parity gate** (expected PASS/byte — scheduling only). Commit.

### Task 5: P-core topology binding for the python driver

**Files:**
- Modify: `atx-vol/python/src/bindings/` (new binding: `configure_pricing_executor(ExecutorConfig)`, `Topology` enum), `atx-vol/tools/run_sp100_strangle_backtest.py` (call it before first pool touch; assert the `false`-if-late return)
- Test: python binding test + driver test asserting the call happens before any pricing

`Topology::Auto` never pins; 16 equal blocks land on threads with ~2× speed spread (4P+8E). `configure_pricing_executor(PerformanceCores)` exists (pricing_executor.cpp:221-231) but is unbound — the driver structurally cannot comply with the repo's own bench protocol. Bind it; driver gains `--topology {auto,pcores}` defaulting to `pcores`. Worker count/affinity is certified perf-only (`BitIdenticalAcrossThreadCounts`).

- [ ] Failing pytest → bind → driver flag → gates.
- [ ] Measure both topologies on the 61-session probe; keep the winner as default, record both numbers (the review flags this could lose if total throughput beats tail latency — decide on data).
- [ ] **Economic-parity gate** (both topologies must produce byte-identical `track.tsv` — that IS the certification). Commit.

### Task 6: Retain the per-step prepared substrate

**Files:**
- Modify: `atx-vol/src/backtest.cpp` (`positions_at` ~113-124, step loop), `atx-vol/src/portfolio_pricer.cpp` (`Portfolio::create` ~280-308, `carry_base_risk_subset` ~2209-2213)
- Test: `atx-vol/tests/backtest_test.cpp`

Each step rebuilds, serially: a fresh 12.7k `positions` vector, a fresh dedup `unordered_map` in `Portfolio::create` (+ sort/unique), a fresh `stable_sort` + 3 aligned columns in `PreparedPortfolio::create`, and ANOTHER throwaway 12.7k map in `carry_base_risk_subset` — 4 hash builds + 2 sorts + ~6 large allocations per step, all on the critical path. Move the maps/vectors into grow-only workspace members (`clear()` per use — the neighbouring `carry_px`/`carry_instances` already model this). **If measurement shows the rebuild itself (not just its allocations) dominates, a structural step-to-step incremental substrate (apply position deltas instead of full rebuild) is in scope** (owner directive 2) — but only with the same determinism guarantees and the parity gate. Mind the R-35 self-alias note at backtest.cpp:1218-1223.

- [ ] Pin first (allocation-free-when-warm assertion or counter), implement, gates, measure (expect 3-6% CPU, more wall — it is all serial).
- [ ] **Economic-parity gate** (expected PASS/byte). Commit.

---

## Phase 2 — Pack occupancy: the CPU payload

### Task 7: Extend pack-composition invariance to mixed-T packs (gate first)

**Files:** Test: `atx-vol/tests/priced_surface_test.cpp` (~line 852, `EvaluateBatchLanedGreeksPackCompositionInvariant`)

Task 8's bit-identity claim rests on "a 1-lane pack returns exactly what a 4-lane pack returns" (laned_greek_run.hpp:22-26). Today's test pins that for same-T packs. Extend it: packs mixing distinct T (and distinct surfaces per the kernel's per-lane `S,K,T,σ,r,q` signature, simd/american_boundary_batch.hpp:98-118) must return per-lane results bit-identical to 1-lane evaluation.

**Amended stop condition (owner directive 1):** if mixed-T packs are NOT bit-identical to 1-lane evaluation, Task 8 is no longer dead. Record the observed deviation; if it sits within the documented `kLanedGreeksRelBand=1e-9`, Task 8 proceeds targeting **PASS/parity** instead of PASS/byte, and the invariance test pins the band (`golden_close` with the lane band) instead of bit equality. Only if the deviation exceeds the band does the sprint re-plan here.

- [ ] Write the extended test against the CURRENT kernel (no production change). Expected: PASS bit-exact (the kernels are documented pack-composition invariant). If bit-exact fails, pin the band per the amended stop condition and record which columns move.
- [ ] Commit the test.

### Task 8: Widen greek packs across T-runs within a (uid, side) group

**Files:**
- Modify: `atx-vol/src/priced_surface.cpp` (~1007-1014 run loop, ~1132-1154 laned dispatch), `atx-vol/src/laned_greek_run.hpp` (accumulate resolved lanes across T-runs, flush at group boundary / kGreekChunk), `atx-vol/src/prepared_portfolio.cpp` (~114-132 T-run subdivision)
- Test: `atx-vol/tests/priced_surface_test.cpp`, `atx-vol/tests/portfolio_pricer_test.cpp`

The single largest CPU lever. 63 cohorts ⇒ every `(uid, side)` group holds ~63 contracts with 63 distinct expiries ⇒ every laned flush is 1-lane in a 4-lane kernel: ~25% occupancy on 83% of solve volume. Each lane already resolves against its own `t`/`ForwardCarry`/`bracket` (priced_surface.cpp:1016-1018) and the kernel takes per-lane T — only the flush gate requires bit-equal T. Keep per-T resolution; accumulate resolved lanes across T-runs inside the group; flush at group boundary or 128. **Restructuring flush ownership (moving accumulation out of the per-run loop into a group-scoped accumulator object) is the preferred shape if the minimal gate-widening turns into special-case soup** (owner directive 2). Depends on Task 4 (tiles make pack membership book-determined) and Task 7 (the gate).

- [ ] Red-first: a 63-distinct-T fixture whose solve-ledger `sl_greeks_analytic` flush count drops ~4× post-change (write the pinning test against the ledger).
- [ ] Implement. Gates: Task 7's (possibly band-pinned) mixed-T invariance, `BitIdenticalAcrossThreadCounts`, full portfolio/surface suites.
- [ ] Measure on the 61-session probe: expect the greek-bundle share (~81% of 12.8 CPU-s/session) to compress toward the kernel's laned ratio — target ≥1.5× session CPU overall.
- [ ] **Economic-parity gate** (PASS/byte if Task 7 proved bit-exact; else PASS/parity with drift + perf recorded). Commit.

### Task 9: Small bit-identical redundancy kills

**Files:** `atx-vol/src/strategy.cpp` (~212-217), `atx-vol/src/backtest.cpp` (StepMarkMemo ~208-232, 890-892, 1160-1167), tests alongside.

Three independent, cheap, bit-identical fixes bundled as one task:
1. **Entry lane:** drop the provably redundant post-loop revalidation reprice in `resolve_strike_by_delta_routed` — `Kroot`'s expression and `gm.value` are byte-for-byte the values recomputed (~404 wasted cold deltas/session). Handle all three loop exits.
2. **StepMarkMemo repair (latent defect):** `populate_from`'s only caller is `book_greeks`, which is skipped whenever entry or hedge fired — i.e. always on this run, so L2 mark reuse is dead AND the `DuplicateMarkSolves == 0` gate passes vacuously (only bumped when the memo has a value). Repopulate from `execute`'s frame (`w.px` already holds the marks — portfolio_pricer.cpp:2253-2256); fix the gate so it counts genuinely.
3. **Dead API:** delete `resolve_strikes_by_delta_batched` (zero production callers; header claims it is the hot path) and correct the stale "17 solves" comments to "7 unique boundaries (17 stencils), measured 7.07×" at strategy.cpp:125-127, priced_surface.cpp:795-798, american.hpp:477-478.

- [ ] Red-first per item where testable, implement, gates, **economic-parity gate** (expected PASS/byte), commit.

---

## Phase 3 — Load lane + driver wall (expect PASS/byte)

### Task 10: Lazy heavy-curve materialization

**Files:**
- Modify: `atx-vol/src/priced_surface_view.cpp` (~273-341 `create_over_record`), `atx-vol/src/vol_curve.cpp` (~95-152 `ConvexDenseCurve` ctor/`w()`)
- Test: `atx-vol/tests/priced_surface_test.cpp` (or the view's own test file), plus a concurrency test

Every session load eagerly builds ConvexDense anchor tables — 65 `black76_price` calls per anchor × ~43k anchors/partition ≈ 25 s CPU over the run — of which ~70% of slices are never in the queried tenor bracket, and the table itself is a wing-fallback that in-band queries never read. Two composable fixes: (a) defer `ConvexDenseCurve`/`SplineVolCurve` construction to first `slice_w(i,·)` per slice (validation stays eager — it is the untrusted-input parser); (b) build `finite_k_`/`finite_w_` lazily inside `ConvexDenseCurve` on the first fallback hit. Both need a per-slice `std::once_flag`/atomic — the view is consumed const from the parallel fan-out.

- [ ] Red-first: counter-based test showing anchors built ≤ slices actually queried; thread-hammer test on lazy init; bit-compare queried values pre/post.
- [ ] Implement (a), then (b). Gates + **economic-parity gate** (also compare a settlement-heavy window — wings do get hit at expiry intrinsics).
- [ ] Measure: expect ~15-25 s CPU off the run; small wall (load is mostly hidden behind the async prefetch).
- [ ] Commit.

### Task 11: Driver pre-pass fast path + prefetch depth binding

**Files:**
- Modify: `atx-vol/src/surface_db.cpp` / `include/atx/vol/surface_db.hpp` (new `session_ts(key)` reading the first directory entry's record header `now_ts_ns` only — uniqueness is guaranteed by `MarketSnapshot::load`'s agreeing-ts check), `atx-vol/python/src/bindings/surface_db.cpp` (bind it), `atx-vol/python/src/bindings/backtest.cpp` (one `def_readwrite` for `RunConfig::prefetch_depth`), `atx-vol/tools/run_sp100_strangle_backtest.py` (use `session_ts`; fix `corpus_rate`'s false warm-cache docstring and its 20 ms re-read)
- Test: python binding tests + driver test

The pre-pass reconstructs the heaviest surface (SPY, ~18 ms) per session to read one int64 — ~2.8 s of serial wall, 100% discarded. `prefetch_depth` is pinned at 1 because the binding doesn't exist (insurance for when compute gets faster).

- [ ] Red-first pytest (session_ts equals the engine's ts on a fixture db; prefetch_depth round-trips), implement, gates, **economic-parity gate**, commit.

---

## Phase 4 — `/arch:AVX2` `_core` (ULP class — measure, then land under the parity gate)

### Task 12: `/arch:AVX2` `_core` — measure, then decide

The remaining lever after Phases 1-3 is FMA contraction in the scalar AL boundary solver (`/arch:AVX2` on the whole build). This is **ULP-class**: it flips `kFmaContraction`, every `golden_close()` loses its byte-exact branch (57 call sites), expected drift ~13 ULP/value, ~5.5e-13 relative on accumulated NAV — well inside the parity band. The repo has **zero ISA-paired baselines**, so the gain is unmeasured (plausibly single-digit %).

**Amended decision rule (owner directive 1 pre-authorizes ULP-class changes with measured wins):** measure first; land only on evidence. No separate human sign-off round needed, but the numbers go in the ledger and the final report.

- [ ] Prereqs: Task 3 (ISA-aware compare gate) done; Phases 1-3 landed and measured.
- [ ] Build a side-by-side `_core` with `CXXFLAGS`/`CFLAGS` env injection (NOT `cmake.define` — CMakePresets.json:81 warns it drops `/EHsc`); measure the YTD replay; run `compare_track.py` and quantify the drift (max abs/rel per column).
- [ ] **Land iff** wall gain ≥ 3% on the YTD replay AND `compare_track.py` reports PASS/parity AND the full C++ suite is green under the flag (golden_close sites take their tolerance branch). If landed: regenerate the `-avx2-` bench baseline set under the quiet-host protocol (Task 3's gate makes the pairing enforceable). If the gain is under threshold: delete the experiment, record the measurement, done.

---

## Projected outcome (honest ranges, to be replaced by Phase 0 measurements)

| Lever | Class | CPU | Wall |
|---|---|---|---|
| Task 4 dynamic greek tiles | byte | ~flat | −15-35% of step wall |
| Task 5 P-core pinning | byte | ~flat | −10-30% (measure; may lose) |
| Task 6 substrate retention | byte | −3-6% | more (serial section) |
| Task 8 mixed-T packs | byte or parity | −30-45% | follows CPU |
| Task 9 redundancy kills | byte | −1-2% | small |
| Task 10 lazy heavy curves | byte | −15-25 s abs | small |
| Task 11 pre-pass fast path | byte | — | −2.8 s abs |
| Task 12 /arch:AVX2 | **parity (ULP)** | unknown | unknown |

Compounded conservative estimate for Phases 1-3: **CPU 1,810 → ~900-1,100 s; wall 175 → ~60-95 s**. Stretch (all levers at top of range + Task 12 landing): wall < 60 s.

## Self-review notes

- Spec coverage: "why it takes so long" = the measured baseline + cost model section (94% book-marking, 5-solve bundle, 1-lane packs, 65% utilization); "minimize while maintaining same economic accuracy" = the economic-parity gate on every task, the byte/parity/forbidden taxonomy, and Task 12's measure-then-land rule.
- Every task names exact files/lines from the four lane reviews; savings figures carry their evidence class (bench-anchored vs estimated) and Phase 0 exists precisely to firm them up before the expensive tasks land.
- Sequencing honors hard dependencies: Task 4 before Task 8 (tile determinism prerequisite), Task 7 before Task 8 (gate first), Task 3 before Task 12 (measurable at all), Task 1 before everything (the ledger and the comparator are the judges).
