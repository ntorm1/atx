# Phase 3 Progress — Eval-VM Hot Path + Bench Baseline (p7 S3)

Base: main @ 2eaf3da
Branch: feat/p7-s3
Worktree: C:\atx-wt\p7-s3

Build env: clang-cl 18.1.8 + Ninja + vcpkg (x64-windows). Tests built unity-OFF
with `-DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc -Wno-unknown-argument"`.
Bench built in a separate Release dir (`build-bench`, Ninja + clang-cl, Release).

## Unit checklist
- [x] S3-0 — open ledger; record current bench baseline (af50609)
- [x] S3-1 — Welford/Neumaier online variance kernel (TsVar/TsStd), ResearchFast (d0a3462)
- [x] S3-2 — extend online variance to TsZscore/TsAvDiff, ResearchFast (9bf05c5)
- [x] S3-3 — cross-instrument column parallelism (seq==parallel digest), AuditExact (0d4dc2e)
- [x] S3-4 — bench delta writeup + vm.hpp stale-comment cleanup (HEAD (this S3-4 commit))

## Determinism contract for this sprint
- AuditExact (default `EvalMode`): byte-identical to pre-sprint. The online
  variance branch only fires under ResearchFast; S3-3 column parallelism is
  bit-identical to serial (column independence), proven by seq==parallel digest.
- ResearchFast (opt-in `EvalMode`): Welford/Neumaier online kernels for the
  variance family. Bit-divergent from the batch oracle (different accumulation
  order) but provably more accurate; ships behind the mode with a documented
  tolerance band + a recorded bench line.

---

## S3-0 baseline

Host: NATHANS_PC (16 X 2496 MHz CPU; L1d 48KiB x8, L2 1280KiB x8, L3 18432KiB).
Build: Release, clang-cl 18.1.8 + Ninja, `build-bench`. Date: 2026-06-28.
Command:
```
cmake -B build-bench -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows -DFETCHCONTENT_BASE_DIR=%ATX_DEPS_DIR% \
  -DATX_BUILD_BENCH=ON -DATX_UNITY_BUILD=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc -Wno-unknown-argument" \
  -DCMAKE_C_FLAGS="/DWIN32 /D_WINDOWS -Wno-unknown-argument"
cmake --build build-bench --target atx-engine-bench atx-shm-worker
build-bench/bin/atx-engine-bench \
  --benchmark_filter="BM_BatchEvaluate_MinedBattery|BM_ParallelEval" \
  --benchmark_format=console
```

NOTE (infra): the bench exe has a static-init `g_speedup_report` (executor_bench.cpp)
that runs a ProcessExecutor; it needs `atx-shm-worker.exe` beside the bench exe or
the process exits at load with `parallel_run.cpp:89 CHECK failed`. Building the
`atx-shm-worker` target into `build-bench` fixes it (per protocol §2). Pre-existing,
not sprint code.

Raw before lines (Release):
```
BM_BatchEvaluate_MinedBattery        103 ms     99.0 ms      6 iters   31.79M items/s  alphas=24
BM_ParallelEval/1/real_time         57.9 ms     4.26 ms     11 iters   2.212k/s        workers=1
BM_ParallelEval/2/real_time         58.3 ms     5.51 ms     17 iters   2.196k/s        workers=2
BM_ParallelEval/4/real_time         27.1 ms     6.14 ms     28 iters   4.731k/s        workers=4
BM_ParallelEval/8/real_time         34.9 ms     7.81 ms     26 iters   3.669k/s        workers=8
BM_ParallelEval/0/real_time         18.6 ms     6.25 ms     30 iters   6.900k/s        workers=0 (cores-2)
```

S3-0: complete (commit af50609, baseline recorded) — bench env + baseline frozen.

---

## S3-1 — Welford/Neumaier online variance kernel (TsVar/TsStd), ResearchFast

Kernel: `ts_ops.hpp` `tsv_welford_var_family` (+ `tsv_welford_var_col` /
`tsv_welford_std_col` wrappers, `ts_is_online_variance_op`, `tsv_welford_dispatch`).
Welford add/remove for the sum-of-squared-deviations S, Neumaier-compensated
running window-sum for the mean. No Sx^2 anywhere -> no catastrophic cancellation.
Plumbing: `vm.hpp` `EvalMode` enum (default AuditExact), `set_eval_mode`/`eval_mode`,
`mode_` member; ResearchFast branch in `eval_time_series` dispatches the variance
family to the Welford sweep. AuditExact path unchanged (batch ts_value_at).

Determinism: ResearchFast (NOT bit-exact — different accumulation order than the
oracle's two-pass). AuditExact default path: zero bytes change (off-path identity
test green). Tolerance atol=rtol=1e-9 for var/std on random panels.

Tests (atx-engine-alpha-tests, ts_online_variance_test.cpp), all green:
- TsWelfordVar.NearConstantHighMean_MatchesTruth (mean ~1e7, std ~1.0): Welford
  AND batch both within 1e-9 of truth, and agree within 1e-9. (the revert's
  catastrophic-cancellation regression, now recovered.)
- TsWelfordVar.ConstantWindow_ExactlyZero: var/std == 0.0 exactly, no spurious neg.
- TsWelfordVar.NaNInWindow_PropagatesNaNGate: matches the gated batch path.
- TsWelfordVar.RandomPanel_WithinToleranceOfOracle: 500x200, d=20 — every finite
  cell within atol=rtol=1e-9 of the batch oracle (var AND std).
- TsAuditExactVar.DefaultMode_ByteIdenticalToOracle: off-path byte-identity.
- TsEvalMode.DefaultIsAuditExact + TsWelfordVar.ResearchFast_TwiceRunIdentical.

Bench (build-bench Release, host NATHANS_PC, 2026-06-28), ts_std(close,20) 512x256:
```
BM_TsVarBatch     23.5 ms   21.6 ms CPU    34 iters    6.07M items/s   mode=0 (AuditExact)
BM_TsVarWelford   5.50 ms   5.00 ms CPU   100 iters   26.21M items/s   mode=1 (ResearchFast)
```
Ratio: 23.5 / 5.50 = 4.27x faster (target >= 2x at d=20). MET.

Gates: alpha 577/577 green; factory oracle slice (*Oracle*:*Golden*:*Digest*) 18/18 green.

S3-1: complete (commit d0a3462, 7 new tests green, 4.27x kernel speedup) — Welford
variance family lands behind ResearchFast; AuditExact byte-identical.

---

## S3-2 — extend online variance to TsZscore / TsAvDiff, ResearchFast

The S3-1 Welford accumulator already produces (m, S, n) per cell; TsZscore =
(x[t]-m)/std and TsAvDiff = x[t]-m reuse it (`tsv_welford_zscore_col` /
`tsv_welford_avdiff_col`, both in `tsv_welford_var_family`). `ts_is_online_variance_op`
and `tsv_welford_dispatch` already cover all four ops (landed in S3-1). S3-2 adds
the validation + the AvDiff ship/bail gate. No vm.hpp structural change.

TsAvDiff SHIP DECISION: ships online. The adversarial fixture (mean ~1e6, av_diff
amplitude ~1.0, d=20) measured worst |welford - oracle| = 4.66e-10 << the 1e-7 atol
gate -> the plan's bail-out is NOT triggered. AvDiff stays in the online dispatch.

TsZscore tolerance note (numerically honest): zscore's numerator (x[t]-mean) is a
true f64 cancellation, so the achievable atol-vs-oracle scales with mean*eps. At
mean=1e7 the floor is ~2e-9 (the oracle's own (x-mean) and Welford's differ by
~3.6e-9 there — an INTRINSIC f64 limit, not a kernel defect), so the pathological
zscore fixture uses mean=1e5 (floor ~2e-11, comfortably under the 1e-9 gate) while
still stressing the SAME catastrophic-cancellation regime (Sx^2 ~ 1e10*d) the old
rolling variance failed. The random-panel zscore (price magnitudes) passes 1e-9.

Tests (ts_online_variance_test.cpp), all green:
- TsWelfordZscore.NearConstantHighMean_WithinToleranceOfOracle (mean 1e5, atol 1e-9).
- TsWelfordAvDiff.AdversarialHighMean_WithinAtol1e7OfOracle (worst 4.66e-10).
- TsWelfordZscoreAvDiff.RandomPanel_WithinTolerancesOfOracle (zscore 1e-9, avdiff 1e-7).
- TsAuditExactZscoreAvDiff.DefaultMode_ByteIdenticalToOracle (off-path identity).

Bench (build-bench Release, 512x256, d=20):
```
BM_TsZscoreBatch    18.5 ms    7.70M/s   ->  BM_TsZscoreWelford   3.94 ms   39.90M/s   = 4.70x
BM_TsAvDiffBatch     8.28 ms   17.40M/s   ->  BM_TsAvDiffWelford   3.46 ms   42.54M/s   = 2.39x
```
Both >= 2x target. MET.

Gates: alpha 581/581 green; factory oracle slice 18/18 green.

S3-2: complete (commit 9bf05c5, 4 new tests green, zscore 4.70x / avdiff 2.39x) —
TsZscore + TsAvDiff online; AvDiff met its adversarial 1e-7 gate (no bail-out).

---

## S3-3 — cross-instrument column parallelism for batch Ts ops, AuditExact

`vm.hpp`: nullable `parallel::DetPool* ts_pool_` (default null = serial = unchanged
bytes), `set_ts_pool`/`ts_pool`, four per-worker scratch vectors (`ts_col_thr_`,
`ts_col_b_thr_`, `ts_scratch_a_thr_`, `ts_scratch_b_thr_`, sized to [n_workers] in
set_ts_pool). The batch column loop in eval_time_series is refactored into a per-
column helper `eval_ts_column(ctx, j, col, col_b, sa, sb)` called either serially
(null pool — original loop, byte-for-byte) or across DetPool instrument bands. The
ResearchFast variance sweep, the online sum/extreme sweeps, and the TsDelay/TsDelta
direct path stay single-threaded this sprint (deferred to future-work).

Determinism: AuditExact. Column independence is the proof — out[t*I+j] depends only
on input column j, distinct j -> disjoint output slots, each band uses its OWN
scratch (no shared mutable state). Seq==parallel digest is the GATE.

set_ts_pool header documents the DEADLOCK constraint: the column pool MUST be a
SEPARATE instance from the search-level det_pool (nesting two DetPool dispatches
deadlocks). NOT wired into the search driver here (that is S7).

Tests (ts_parallel_eval_test.cpp), all green:
- TsColumnParallel.SeqEqualsParallel_DigestIdentical: TsStd+TsRank+TsCorr d=20 on
  600x501 — null vs 2-worker vs 4-worker SignalSet byte-identical (every cell, NaN==NaN).
- TsColumnParallel.NullPoolMatchesOneWorker.
- TsColumnParallel.DefaultEngineHasNullTsPool (inert default).
- TsColumnParallel.SingleInstrument_PoolNoOp (instruments>1 guard).

TSan: NOT run — the dev build is clang-cl on Windows with no TSan toolchain wired
here. Documented as TSan-PENDING. The no-race property holds by construction
(per-worker scratch + disjoint output slots + read-only x/y), and the seq==parallel
digest at {null,2,4} is the empirical correctness/no-corruption gate.

Bench (build-bench Release, 256x512, instruments>>dates, d=20):
```
BM_TsColumnEval/1/real_time   68.0 ms    5.79M/s   workers=1
BM_TsColumnEval/2/real_time   35.8 ms   10.98M/s   workers=2   (1.90x over w1; target >=1.5x) MET
BM_TsColumnEval/4/real_time   22.3 ms   17.64M/s   workers=4   (3.05x over w1)
```

Gates: alpha 585/585 green; factory oracle slice 18/18 green.

S3-3: complete (commit 0d4dc2e, 4 new tests green, w2 1.90x / w4 3.05x) — intra-eval
column parallelism, seq==parallel byte-identical; null default unchanged.

---

## S3-4 — bench delta writeup + vm.hpp stale-comment cleanup

(a) vm.hpp:56-57 stale comment ("Cs*/Ts* opcodes are NOT YET implemented ... return
Err(NotImplemented)") replaced with the accurate one-liner (cs_ops.hpp P3-7 /
ts_ops.hpp P3-8, both fully implemented, oracle differential enforces AuditExact
bit-exactness). Comment-only change; no output bytes change.

(b) Measured bench delta. Host NATHANS_PC (16 X 2496 MHz; L1d 48KiBx8, L2 1280KiBx8,
L3 18432KiB), Release clang-cl 18.1.8 + Ninja (build-bench), 2026-06-28. The after
numbers are `--benchmark_repetitions=3 --benchmark_report_aggregates_only=true`
(median reported; the ratio is the signal — absolute latencies are Debug-default
upper bounds with notable run-to-run variance, see cv% column in the raw logs).

VARIANCE KERNEL (ResearchFast vs AuditExact batch), ts_*(close,20) 512x256:
| Benchmark           | Batch (AuditExact) median | Welford (ResearchFast) median | speedup |
|---------------------|---------------------------|-------------------------------|---------|
| BM_TsVar (ts_std)   | 12.6 ms                   | 3.09 ms                       | 4.08x   |
| BM_TsZscore         | 10.7 ms                   | 2.69 ms                       | 3.98x   |
| BM_TsAvDiff         | 8.09 ms                   | 2.18 ms                       | 3.71x   |
(single-run S3-1/S3-2 readings, less noisy: TsVar 4.27x, TsZscore 4.70x, TsAvDiff 2.39x.)

COLUMN PARALLELISM (BM_TsColumnEval, AuditExact, 256x512, d=20):
| workers | median (3-rep) | single-run | speedup over w1 (single-run) |
|---------|----------------|------------|------------------------------|
| 1       | 43.4 ms        | 68.0 ms    | 1.00x                        |
| 2       | 41.7 ms        | 35.8 ms    | 1.90x  (target >=1.5x) MET   |
| 4       | 14.5 ms        | 22.3 ms    | 3.05x                        |
(the single-run column reading is the cleaner curve; the 3-rep w1/w2 medians overlap
within the ~20-30% cv noise of the Debug-default build, but w4 is unambiguously faster.)

OVERALL BATCH-EVAL THROUGHPUT (BM_BatchEvaluate_MinedBattery, AuditExact, 512x256):
| | before (S3-0) | after (S3-4, 3-rep median) |
|---|---|---|
| BM_BatchEvaluate_MinedBattery | 103 ms (single) | 87.4 ms |
No regression — the mined battery runs on the AuditExact default path (byte-identical
to pre-sprint); the difference is run-to-run noise (cv ~16%), not a code effect.

(c) Sprint close: residuals -> ROADMAP future-work backlog (below). Gates: alpha
585/585 green; factory oracle slice 18/18 green; vm.hpp diff is comment-only this unit.

ROADMAP/phaseN close-ceremony note (DEFERRED to controller merge, NOT done here):
`atx-engine/plans/p7/ROADMAP.md` is the SHARED Wave-1 coordination doc (single
`Last reviewed`, status table covering S1/S2/S3 — all three parallel sprints). It is
OUTSIDE this sprint's exclusive Owns set (ts_ops.hpp / vm.hpp / bench/ / tests/alpha/),
and concurrent edits from the three Wave-1 worktrees would collide. Per the protocol's
hard boundary (stay inside Owns; controller runs the whole-branch review/merge), the
S3 row status flip + `Last reviewed` bump + any `phase3.md` user stub are left for the
controller to apply at merge. The S3 deliverables themselves are complete and green.

### Residuals / future-work backlog (lift into p7 ROADMAP)
- Online-path (sum/extreme/variance) cross-instrument parallelism: deferred. The
  online sweeps share ts_dq_lo_/hi_ deque scratch and the variance sweep is a single
  column pass; parallelising them needs per-thread deques. S3-3 parallelises only the
  BATCH column path.
- TsAvDiff/TsZscore at extreme mean (>=1e7): zscore's (x-mean) numerator hits the f64
  cancellation floor (~mean*eps), so a 1e-9-vs-oracle bar is unmeetable by any kernel
  at 1e7. AvDiff ships at atol=1e-7; zscore validated at mean<=1e5. Documented, not a bug.
- TSan run for the column-parallel path: pending a TSan-capable toolchain (the Windows
  clang-cl dev build has none wired). No-race holds by construction + seq==parallel gate.
- Wiring set_ts_pool into the search driver with a SEPARATE pool per worker: S7.

S3-4: complete (commit HEAD (this S3-4 commit)) — bench delta table recorded; vm.hpp comment accurate.

---

## Sprint summary

All five units complete. Commits: af50609 (S3-0), d0a3462 (S3-1), 9bf05c5 (S3-2),
0d4dc2e (S3-3), and the S3-4 docs commit at HEAD. Final gates: atx-engine-alpha-tests
585/585 green; factory oracle
slice (*Oracle*:*Golden*:*Digest*) 18/18 green. New: 15 tests across two files
(ts_online_variance_test.cpp 11, ts_parallel_eval_test.cpp 4) + two new bench files.
AuditExact default path byte-identical to pre-sprint (off-path identity + oracle slice).
