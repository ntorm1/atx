# Phase 3 Progress — Eval-VM Hot Path + Bench Baseline (p7 S3)

Base: main @ 2eaf3da
Branch: feat/p7-s3
Worktree: C:\atx-wt\p7-s3

Build env: clang-cl 18.1.8 + Ninja + vcpkg (x64-windows). Tests built unity-OFF
with `-DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc -Wno-unknown-argument"`.
Bench built in a separate Release dir (`build-bench`, Ninja + clang-cl, Release).

## Unit checklist
- [ ] S3-0 — open ledger; record current bench baseline
- [ ] S3-1 — Welford/Neumaier online variance kernel (TsVar/TsStd), ResearchFast
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

S3-0: complete (commit <pending>, baseline recorded) — bench env + baseline frozen.
