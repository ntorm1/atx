# Lane 4 (l4-mtest) status — Trial registry, FDR/Romano-Wolf/SPA, lockbox, MinTRL

- Worktree: `C:/atx-wt/pool-5`  branch `feat/qps-l4-mtest`  head `80a49268`  base `334a7939`
- Spec: Lane 4 section of swarm plan (scratchpad plan.md; copied to pool-1 `SWARM_PLAN.md`)
- Builder: **finished**. Adversarial review: **not run / interrupted**. Not merged.

## Commits
```
80a49268 Tile bootstrap means by series and batch registry Gram updates via SYRK
b69ba6b9 Stream registry replay record-by-record; add multiple-testing throughput bench
7b9aebe1 Add single-use audited open_lockbox with hash-chained receipts and durable audit log
3bd7a0b2 Add MinTRL, Harvey-Liu haircut and jackknife weight-stability diagnostics
ebcac294 Add durable content-addressed TrialRegistry with streaming N_eff and registry-fed DSR
057ab8a1 Add FDR/FWER adjustments, Romano-Wolf stepdown and Hansen SPA over a counter-seeded stationary bootstrap
```

## Uncommitted working tree
```
(clean)
```

## Diff vs base
```
 14 files changed, 2967 insertions(+), 11 deletions(-)
```

## Builder report

Lane 4 is finished and committed in C:\atx-wt\pool-5 on branch feat/qps-l4-mtest, 6 commits on top of 334a7939. All code sits under atx-engine; include paths are under include/atx/engine/eval/.
- multiple_testing.hpp and src/eval/multiple_testing.cpp: p_adjust_bh, p_adjust_by, p_adjust_holm and p_adjust_bonferroni follow R p.adjust and return adjusted p-values. benjamini_hochberg, benjamini_yekutieli and holm return the matching reject masks. Also PnlMatrix (a strategy-major view), BootstrapCfg (n_boot, mean_block, seed, threads) and stationary_bootstrap_blocks, which uses a counter-based splitmix RNG. romano_wolf gives a studentized stepdown with adjusted p-values. hansen_spa gives the lower, consistent and upper SPA p-values plus White's Reality Check. Replicate means come from per-series prefix sums over blocks, grouped by series within each replicate chunk. Results are bit-identical at any thread count.
- trial_registry.hpp and src/eval/trial_registry.cpp: TrialRegistry::open(path, cfg) and in_memory(cfg). record(kind, config_hash, pnl, sharpe) returns Result<RecordOutcome>; trial IDs are stable content addresses, so recording the same trial twice does not add to N. summary() returns n_raw, n_eff, n_eff_uncorrected, mean/var/max Sharpe and registry_hash. n_eff is a bias-corrected participation ratio, streamed through a d×d Gram. The Gram is exact when d ≥ T and uses a count-sketch otherwise, with batched SYRK updates. The log is append-only, with a checksum per record; a torn tail is truncated on reopen and a config mismatch is refused.
- deflated_sharpe.hpp: new overload deflated_sharpe(sr, const TrialSummary&, T, skew, kurt) and new expected_max_sharpe_eff for a non-integer N.
- lockbox.hpp: open_lockbox(SealedPanel&&, full panel, OpenRequest, LockboxAuditSink&) is single-use per content address and returns Err AlreadyExists on a second open. A panel that doesn't match the seal gets PermissionDenied, and a missing commitment hash gets InvalidArgument. Receipts form a hash chain. Includes an InMemoryLockboxAudit sink and a durable FileLockboxAudit sink that detects edits to the log.
- min_trl.hpp (min_track_record_length), haircut.hpp (Harvey-Liu with Bonferroni or Sidak and a TrialSummary overload) and weight_stability.hpp (jackknife_weight_stability, refit_turnover).

### Tests

Configured once with: powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev -Groups 'eval'. Built with: scripts\atx-build.ps1 build atx-engine-eval-tests -Preset equity-dev. Ran: scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^(Eval|Cluster|Conviction|Robustness)'. Result: 181/181 passed, 0 failed. That covers the new suites EvalFdr, EvalBootstrap, EvalRomanoWolf, EvalSpa, EvalTrialRegistry, EvalMinTrl, EvalHaircut, EvalWeightStability and EvalLockboxOpen, plus the existing EvalDsr, EvalDsrNetCost and EvalLockbox (still green, outputs unchanged). Check-compiled atx-impl/src/stage_combine.cpp and stage_sweep.cpp, which include the modified headers: both clean. What the tests show: BH/BY/Holm match values from the R p.adjust algorithm (computed with a Python reimplementation, not R itself); MinTRL matches the Bailey-LdP paper figures (2.73, 2.83 and 3.24 years); the Harvey-Liu example gives 0.324 (Sidak); Romano-Wolf null FWER stays within alpha + 3 standard errors over 200 Monte Carlo runs; 50 perfectly correlated trials give N_eff = 1 and 50 independent trials give about 50; the registry survives a crash and reopen; registry-fed DSR ≥ raw-N DSR; a second lockbox open is refused.

### Bench

Release build (equity-rel with -Bench and isolated FetchContent deps) on a machine shared with the other lanes, so timings are noisy. bench/eval_multiple_testing_bench.cpp, run with --benchmark_filter. HansenSpa, 1000 candidates × 2520 days × 1000 bootstraps: 1166 ms on 1 thread, 367 ms on 4 threads (4699 ms before the series-tiling change). RomanoWolf at the same size: 599 ms on 1 thread, 183 ms on 4. TrialRegistry append, 10^6 in-memory trials at T=252: 2.03 s at d=16 (500k/s), 2.99 s at d=64 (376k/s), and 10^5 trials exact at d=252 in 1.20 s (95k/s). summary() on 10^6 trials: 3.6 us. Durable flushed append: 55k records/s.

### Deferred
- ONC clustering as an alternative N_eff estimator (only the participation ratio from breadth.hpp is implemented)
- Harvey-Liu Holm/BHY haircut variants, which need the full p-value cross-section (use p_adjust_* from multiple_testing.hpp); Lo (2002) autocorrelation adjustment of the Sharpe before the haircut
- Registry thread-safety (single writer by contract); fsync durability (a flush covers a process crash, not power loss)
- Fixed per-record overhead of about 2 us in TrialRegistry::record, cause not identified; the Gram term was fixed with SYRK

### Integration notes
- L4 -> L3/L5/gate/learn: SearchDriver, gate.hpp, the combiner and stacker, the regime count and optimizer tuning should call TrialRegistry::record with the right TrialKind, and admission DSR should use deflated_sharpe(sr, registry.summary(), T, skew, kurt). var_sr is the variance of the per-period Sharpe across trials.
- The final-evaluation path (S8.2) should call open_lockbox with a FileLockboxAudit and a non-zero candidate_hash commitment; the receipt's content_address can feed cross_section_ic's content_address field.
- Lane 0 scaffold issue: commit 334a7939 says it adds an equity-bench preset but CMakePresets.json has none. I used equity-rel with the -Bench switch.
- The Release bench build hits a spdlog _ITERATOR_DEBUG_LEVEL link mismatch because the Debug/Release FetchContent tree at C:/atx-cache/deps is shared. Fixed here with -DFETCHCONTENT_BASE_DIR=C:/atx-wt/pool-5/deps/equity-rel plus FETCHCONTENT_SOURCE_DIR_<DEP> pointed at the shared sources.
- atx-engine-bench aborts in executor_bench's static initializer unless atx-shm-worker is built beside it: build both targets.
- deflated_sharpe.hpp now includes trial_registry.hpp (a small PIMPL header). lockbox.hpp now includes filesystem, fstream, charconv and unordered_set.

## Next step

Resume: review diff `git diff 334a7939..HEAD`, finish/verify uncommitted work, rerun lane suites, then integrate per plan §9.

## Adversarial review (2026-09-23) — verdict: fix-required
- Rebuilt atx-engine-eval-tests (equity-dev, eval group): ctest ^(Eval|Cluster|Conviction|Robustness) 181/181 pass; full eval binary 188/188 pass.
- Verified by a temporary probe test (deleted afterwards, tree clean):
  1. MAJOR lockbox: same holdout [80,100) opened twice by re-reserving with embargo 2 vs 3 (content_address keys on geometry).
  2. MAJOR lockbox: two FileLockboxAudit handles on one log both open the lockbox; the forked chain then fails to load (ParseError) forever.
  3. MAJOR registry: 1-byte flip in record 3/10 -> reopen silently truncates to n_raw=2 (undercounted N).
  4. MAJOR SPA: all-constant differentials -> statistic 0 and p_lower=p_consistent=p_upper=0 (null rejected).
  5. minor: sketch-mode n_eff inflates on identical trials (sketch norm not renormalized): d=64 p95 1.64, max 2.66 (python sim).
- Next: builder fixes 1-4, add regression tests, rerun eval suites.

## Review fixes (2026-09-23, wave 2) — commit 88eb752d on feat/qps-l4-mtest (base ca0c9619 = synced integration)
All 6 reviewer findings verified real and fixed:
1. MAJOR lockbox re-open via geometry: single-use now keyed on geometry-free per-date holdout digests
   (`detail::holdout_date_digests`) + reservation address. Tests: ReReservingWithOtherGeometryCannotReopenDates
   (embargo 2 vs 3, frac 0.2 vs 0.21), ExtendedPanelCannotReopenOldDatesButNewDatesOpen (appended dates).
2. MAJOR FileLockboxAudit two-writer fork: new src/eval/lockbox_audit.cpp; open()/commit() under exclusive
   LockFileEx/flock, commit ingests other writers' lines then re-checks, FlushFileBuffers/fsync. Sink API is now
   `commit(draft) -> Result<LockboxReceipt>` (was append). Log format ATXLBX2 (adds digests column).
   Test: TwoFileHandlesCannotBothOpen (second handle refused AlreadyExists, chain reloads with 2 receipts).
3. MAJOR registry mid-log corruption: ParseError + file untouched unless bad record is the last complete one.
   Test: MidLogCorruptionIsRefusedNotTruncated.
4. MAJOR SPA degenerate: statistic 0 -> p_lower/p_consistent/p_upper = 1. Test: EvalSpa.DegenerateZeroStatisticNeverRejects.
5. minor sketch norm: sketch renormalized to unit length. Bias constant c kept (numpy sim, T=2520 N=50 indep:
   mean n_eff 48.6 @d=32, 49.5 @d=64, 49.8 @d=256). Test: SketchModeIdenticalTrialsCollapseAcrossSeeds (12 seeds x d=32,64).
6. minor unstable hash: content_address uses StableHasher (golden values pinned; matched a Python reimpl).
   Truncation of trailing log lines documented as NOT detected.
Tests: ctest -R '^(Eval|Cluster|Conviction|Robustness)' 188/188 pass; ctest -R '.' in equity-dev(eval group): 195 pass,
3 NOT_BUILT placeholders (atx-core/tsdb/impl test exes not in the eval group). Check-compiled stage_equity_ic/
stage_sweep/stage_discover/cross_section_ic/factory.cpp clean.
Next: integrate feat/qps-l4-mtest into feat/quant-platform-swarm-20260922 (new TU in atx-engine/CMakeLists.txt).
