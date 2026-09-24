# Lane 2 (l2-vmcache) status — VM subtree cache, fusion, Cs pool, strategy-B CSE, bench gate

- Worktree `C:/atx-wt/pool-3`, branch `feat/qps-l2-vmcache`, base `334a7939`; synced with integration `8c42689a`.
- Resumed 2026-09-23 by continuation builder.

## Done / committed
- efbcd08d subtree cache, fusion, Cs pool, strategy-B union DAG; 5cd6b7ce WQ101 bench + bench-gate.ps1;
  fa6dc20b cache-aware strategy-A overload; 33db6c9f publish roots last (LRU); 7d5fd4e6 WIP = baseline JSON
  (recorded BEFORE 33db6c9f -> warm hits 72%; needs re-baseline).
- Verified: equity-dev alpha+parallel groups build; ctest -R '^(Alpha|Parallel|SubtreeCache|...)' 592/592 pass.

## In progress
- Acceptance gaps: warm-cache >=90% hits (re-run bench), 3x speedup vs strategy A @8 threads (baseline: 1.16x).

## Continuation progress (2026-09-23)
- 134dc266 test ParallelGlobalDag_Cache.TightBudgetKeepsRootsWarm (verified red on in-level publish: 13% hits)
- c4397ba4 GlobalDagOptions::panel_digest hint (skip 42 ms panel hash per call)
- 278814f4 parallel root copy-out: warm WQ101 pass 477 -> 165 ms @8T, 100% hits
- e2384278 uninitialized scratch buffers + pooled root sizing: caller-thread CPU 1.5 s -> 0.4 s per cold pass
- Profiling: every WQ101 alpha costs 50-970 ms serial; Ts/Cs kernels (Lane 1 files) are ~95% of time.
  Serial A/B ratio is only ~1.3x -> 3x cold target needs Lane 1 kernels (radix rank, sliding corr, ordstat ts_rank).
- Host is at 100% CPU (other lanes): absolute bench numbers are contended.
- 481c891a re-baselined alpha_throughput.json (WarmCache 100% hits, 178 ms); bench-gate self-check: old-vs-new FAIL exit 1, new-vs-new PASS.
- Final verification: equity-dev ctest -R '^(Alpha|Parallel|SubtreeCache|Wq101|Vm|Batch)' 594/594 pass.

## Review-fix pass (2026-09-23)
- c47b480d bench-gate: MISSING baseline rows -> exit 1 (opt-out -AllowMissing); empty current -> exit 2;
  --benchmark_repetitions files compare _median rows. Verified with fixtures: empty=2, partial=1,
  partial -AllowMissing=0, self=0, reps with a 1.5x noisy repetition=0 (median used).
- c47b480d global_dag_eval.hpp: two cp1252 0x97 bytes -> "--"; header now valid UTF-8.
- ctest -R '^(Alpha|Parallel|SubtreeCache|Wq101|Vm|Batch)' 594/594 pass after fixes.
- 72509ff7 baseline re-recorded as medians of 3 reps (CV 1-13%, host ~60% load). 8T: A 5664, B 3776,
  BFused 3481 ms (1.63x vs A); WarmCache 159 ms @100% hits. Gate new-vs-old PASS, self PASS.
  Re-record cmd: build-equity-rel\bin\atx-engine-bench.exe --benchmark_filter=Wq101 --benchmark_repetitions=3 --benchmark_out=<f> --benchmark_out_format=json, then bench-gate.ps1 -Update
- Spec deviations (record in acceptance): battery is 70/101 WQ alphas; bench 500 instruments (not 3000);
  3x cold target open (1.15-1.35x).

## State: lane COMPLETE for this wave (head 72509ff7). Not merged to integration.
## Deferred
- 3x cold speedup vs strategy A @8T: blocked on Lane 1 kernels (serial A/B ratio ~1.3x; kernels ~95% of time).
  Re-measure after merging feat/qps-l1-kernels.
- Quiet-host re-baseline of cold numbers; LEDGER line (orchestrator appends, lanes may not).
- Zero-copy LoadField in strategy B (alias panel column when out-of-universe cells already hold kVmNaN bits): <1% of cold pass.
- Plan asked 3000 instruments; bench defaults to 500 (ATX_WQ101_INSTRUMENTS env) for 16 GB shared host.
