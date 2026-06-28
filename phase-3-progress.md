# Phase 3 Progress — Eval-VM Hot Path + Bench Baseline (p7 S3)

Base: main @ 2eaf3da
Branch: feat/p7-s3
Worktree: C:\atx-wt\p7-s3

Build env: clang-cl 18.1.8 + Ninja + vcpkg (x64-windows). Tests built unity-OFF
with `-DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc -Wno-unknown-argument"`.
Bench built in a separate Release dir (`build-bench`, Ninja + clang-cl, Release).

## Unit checklist
- [x] S3-0 — open ledger; record current bench baseline (af50609)
- [x] S3-1 — Welford/Neumaier online variance kernel (TsVar/TsStd), ResearchFast
- [ ] S3-2 — extend online variance to TsZscore/TsAvDiff, ResearchFast
- [ ] S3-3 — cross-instrument column parallelism (seq==parallel digest), AuditExact
- [ ] S3-4 — bench delta writeup + vm.hpp stale-comment cleanup

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

S3-1: complete (commit <pending>, 7 new tests green, 4.27x kernel speedup) — Welford
variance family lands behind ResearchFast; AuditExact byte-identical.
