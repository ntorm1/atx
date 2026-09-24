# Lane 3 (l3-search) status — Search throughput: rewrite, fingerprint, fidelity racing, sketch index

- Worktree: `C:/atx-wt/pool-4`  branch `feat/qps-l3-search`  head `8a1a1d5e`  base `334a7939`
- Spec: Lane 3 section of swarm plan (scratchpad plan.md; copied to pool-1 `SWARM_PLAN.md`)
- Builder: **finished**. Adversarial review: **not run / interrupted**. Not merged.

## Commits
```
8a1a1d5e Reuse per-worker rung engines and CPCV cache in the fidelity race; default 4x4/2x2 ladder
34e795db Add farthest-point behavioral archive eviction and the L3 throughput bench
ba9b0006 Wire multi-fidelity racing, semantic canon and output-fingerprint dedup into SearchDriver
be5bb066 Add bit-exact semantic rewrite, output fingerprint and PnL sketch index to the search factory
```

## Uncommitted working tree
```
(clean)
```

## Diff vs base
```
 19 files changed, 2567 insertions(+), 22 deletions(-)
```

## Builder report

Lane 3 (search factory throughput and quality) is built, tested and committed on feat/qps-l3-search in C:\atx-wt\pool-4. All paths below are under atx-engine/.
- factory/rewrite.hpp and src/factory/rewrite.cpp: `rewrite_ast` and `canonical_rewrite(const Genome&, const RewriteCfg&)` rewrite a genome into a normal form. Rules: neg(neg x)->x, abs(abs), sign(sign), rank(rank), abs(neg x)->abs x, and rank or sign of c*x, x*c or x/c with c>0. By default c must be a power of two, which keeps the rule bit-exact. Dead nodes are removed and the rewrite is idempotent. It checks the invariance flags below.
- factory/op_catalog.hpp: `OpInvariance op_invariance(OpCode)` gives the flags idempotent, involution, even, pos_scale and bit_exact.
- factory/canonical.hpp and .cpp: new `CanonCfg{semantic, rewrite}` and `canonical_hash(g, CanonCfg)`, which rewrites before hashing when `semantic` is set.
- factory/fingerprint.hpp: `output_fingerprint`, `signal_fingerprint`, `probe_rows` and `FingerprintIndex`. It hashes the signal after rank-quantizing it (average ranks for ties) on evenly spaced probe dates.
- factory/sketch_index.hpp: `SketchIndex` (z-normalised PnL, 128-dim ±1 random projection, flat scan, exact recheck of a shortlist, `topk` and `topk_exact`) and `FarthestPointArchive`.
- factory/fidelity.hpp and src/factory/fidelity.cpp: `Rung`, `FidelityCfg`, successive-halving `race()` (ties broken by canon hash, optional DetPool, every evaluation counted), `promote_count`, `first_full_rung` and `strided_panel`.
- factory/behavior.hpp: new `ArchiveEviction::FarthestPoint`.
- search_driver.hpp and .cpp: new `SearchConfig` fields `canon`, `output_dedup`, `fingerprint_rows`, `fingerprint_quant`, `fidelity` and `archive_eviction`. All default to off and reproduce today's results exactly. New `SearchResult` counters: `fingerprint_hits`, `fidelity_evals`, `fidelity_rejected`. Candidates rejected at a low rung go into the CanonSet, so they count as trials. The race reuses one Engine per rung and worker, plus a CpcvCache.
- bench/factory_throughput_bench.cpp: `BM_SearchThroughput` over modes 0-4 and 1, 4 and 8 workers.

### Tests

Debug build (equity-dev preset, factory group), with atx-engine-factory-tests and atx-shm-worker built.
- `powershell scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^(FactoryRewrite|FactoryFingerprint|FactorySketchIndex|FactoryCanonical)\.'`: 25/25 passed.
- `-R '^(FactoryFidelity)\.'`: 8/8 passed. `-R '^(FactorySearchL3)\.'`: 4/4 passed. FactoryArchiveL3: 3 tests, passed in a 28/28 run together with FactoryBehavior, FactorySearchL3, FactorySearchDriver and NsgaSearch.
- Full `-Ctest -Preset equity-dev -j 2` (every factory test, which covers the required suites FactorySearchDriver, NsgaSearch, FactoryCanonical, DeflateSelection, CascadeTrialCount and StagnationStop): 287/287 passed. The only 3 non-passes are the atx-core, atx-tsdb and atx-impl NOT_BUILT placeholders, which are outside this group.
- Before atx-shm-worker was built, the *SeqEqualsParallel tests failed as the house notes predict; after building it they all passed (55/55).

### Bench

Release build (equity-rel preset plus -Bench, since this base has no equity-bench preset), BM_SearchThroughput, 756x500 panel, population 32, 6 generations, one iteration each. The machine is shared with other lanes, so expect noise. Final run, genomes/sec:
- 1 worker: baseline 16.5; semantic canon 18.4; plus output dedup 22.7; plus fidelity with the conservative ladder 88.8; plus fidelity with the default 4x4/2x2 ladder 93.7. That is 5.7x the baseline.
- 4 workers: baseline 61.3; all levers 201.8 (3.3x).
- 8 workers: baseline 101.7; all levers 222.6 (2.2x).
- Final best raw fitness: 112.546 with fidelity against 112.361 for the baseline, so quality holds.
- Other counters: dedup 30% to 38-41%, fingerprint hit rate 38.6% (mode 2), 87.5% of candidates rejected at a low rung, and 136 trials/sec at 8 workers.
- The target of 4x distinct genomes/sec was met at 1 worker but not at 4 or 8 workers (3.3x and 2.2x).
- Hypervolume was not measured; final best raw fitness is used as the quality check instead.
- The spec's 3000x2500 panel was not run for memory reasons; set ATX_L3_BENCH_DATES and ATX_L3_BENCH_INSTS to run it.

### Deferred
- Per-phase time split counters (compile / eval / fitness / pareto / novelty) inside SearchDriver; the bench reports only the rates listed
- Hypervolume-based quality guard in the bench (it uses final best raw fitness today)
- Baseline JSON and bench numbers at the full 3000x2500 panel size
- Using SketchIndex inside the driver's novelty pass for large archives (it is standalone and tested; the default archive cap of 64 does not need it)
- Racing directly on NSGA objectives in MultiObjective mode (low rungs score by pool_aware_fitness raw)
- Process-executor workers sweep in the bench (only DetPool thread counts are swept)

### Integration notes
- The Lane 0 scaffold commit 334a7939 says it adds an equity-bench preset, but CMakePresets.json on this base has none. I configured with `atx-build.ps1 configure -Preset equity-rel -Groups factory -Bench -DFETCHCONTENT_BASE_DIR=<worktree>/deps/equity-rel`. The benches also need atx-shm-worker built next to atx-engine-bench, or a check in parallel_run.cpp aborts (ATX_CHECK at line 89).
- L2 to L3: SearchConfig has no SubtreeCache* field yet, because the Lane 0 stub header is empty. At integration, add `alpha::SubtreeCache* subtree_cache{nullptr}` and pass it into both the evaluate_generation merged pass and SearchDriver::fidelity_reject (the rung engines), then report cache_hit_pct in SearchResult.
- L4 to L3: record trials into TrialRegistry. For each generation that is the new canon inserts, which already include candidates rejected at a low rung, plus res.fidelity_evals as extra low-fidelity looks. With deflate_selection, N is canon.size() plus prior_trial_count; switch it to TrialRegistry::summary() when that lands.
- The Factory, the atx-impl discover stage and the CLI do not pass the new SearchConfig knobs (canon.semantic, output_dedup, fidelity.enabled, archive_eviction). They default to off, so existing outputs are byte-identical. Enabling them in production needs flag wiring that Lane 8 or integration owns.
- Resume checkpoints do not serialize fp_index_ or the fidelity state, so a resumed run with output_dedup or fidelity on is not byte-identical to an uninterrupted one. Runs with these flags off are unaffected.
- The LEDGER.md line is left for integration: at 756x500 / 32x6 in Release, all L3 levers gave 93.7 against 16.5 genomes/s at 1 worker (5.7x) and 3.3x at 4 workers, with equal best raw fitness.

## Next step

Resume: review diff `git diff 334a7939..HEAD`, finish/verify uncommitted work, rerun lane suites, then integrate per plan §9.

## Adversarial review (2026-09-23, reviewer)
- Verdict: fix-required (no code edited by reviewer).
- Reproduced: build atx-engine-factory-tests (equity-dev) OK; lane + must-stay-green suites 77/77; full factory group 287/287 (only NOT_BUILT placeholders fail).
- Bench (equity-rel, existing binary): 1 worker mode0 11.5 -> mode4 56.2 genomes/s (4.9x; distinct trials/s 8.0 -> 34.6 = 4.3x); 4 workers 40.8 -> 124.8 (3.1x; trials/s 2.7x). best_raw 112.361 vs 112.546. Claims roughly reproduced; 4x only at 1 worker.
- Findings:
  1. MAJOR fidelity_reject passes full-panel `pool` to pool_aware_fitness on strided sub-panel -> corr_to_pool length mismatch (ATX_ASSERT abort in Debug, misaligned corr in Release) whenever pool non-empty. All tests use empty pool.
  2. MAJOR fidelity-rejected candidates keep default CachedScore (raw 0, all objectives 0) -> outrank negative-raw evaluated candidates in ScalarRaw and sit on Pareto front 0 when parsimony/cost objectives active.
  3. MAJOR output_dedup reuses owner's full CachedScore on a lossy fingerprint (32 dates x 32 buckets, monotone-invariant) regardless of WeightPolicy transform (ZScore/Raw configurable) -> wrong scores; parsimony objective copied from owner.
  4. MINOR SketchIndex: +-inf PnL -> NaN z -> NaN comparator in nth_element/partial_sort (strict weak ordering UB); "exact" recheck is f32.
  5. MINOR bench headline counts candidates_generated (not distinct); quality guard is final best_raw over 6 gens (no hypervolume), LEDGER line missing.
- Next: builder fixes 1-3 (+tests with non-empty pool / MultiObjective+parsimony / ZScore policy), then re-review.

## Fix pass (2026-09-23, builder wave 2) — DONE, committed 9d37a8fa
- Head: 9d37a8fa on feat/qps-l3-search (on top of sync 68a0aab1). Working tree clean except this file.
- Findings 1-5 all addressed (none rebutted):
  1. fidelity_reject scores low rungs against an empty AlphaStore (pool-free wq*robust); full pass still applies real pool. Test FactoryFidelity.NonEmptyPoolDoesNotMisalignLowRungs (Debug, full-length pool, 1 vs 4 workers).
  2. ScoreOrigin{Full,FingerprintBorrowed,FidelityRejected} on CachedScore+Scored; rejected -> rejected_score() (raw=-inf sentinel, 0 objectives); assign_pareto_ranks excludes rejected rows (trailing front, byte-identical when none); op-credit skips rejected; finalize emits only Full; deserialize_cache restores rejected origin from -inf. Tests RejectedRanksBelowNegativeRawInScalarMode, RejectedStaysOffFrontZeroWithParsimony, MultiObjectiveParsimonyRunIsDeterministic.
  3. output_dedup gated on policy_.transform==Rank; borrowed hits marked FingerprintBorrowed, parsimony recomputed per genome, never emitted; SearchConfig doc says approximate. Tests OutputDedupIsInertUnderZScoreTransform, BorrowedFingerprintScoresAreNotEmitted.
  4. sketch_znorm uses isfinite (+ overflow guard); recheck/archive dist use f64-accumulated dot; docs corrected. Test FactorySketchIndex.InfiniteDaysAreTreatedAsMissing.
  5. Bench: trials_per_sec documented as acceptance metric; front_topk_raw/front_hv (full-fidelity re-score of top-8 admitted, 2-D HV over wq,robust) + ATX_L3_BENCH_GENS. LEDGER line left for integration.
- Tests (equity-dev Debug): lane suites FactoryFidelity|FactorySketchIndex|FactorySearchL3|FactoryArchiveL3|FactoryRewrite|FactoryFingerprint|FactoryCanonical 47/47; full factory group -j2 294/294 (only 3 NOT_BUILT placeholders "fail").
- Bench (equity-rel, 756x500, pop 32): 6 gens: 1w trials/s 9.23 -> 31.8 (3.4x), 4w 29.5 -> 75.2 (2.55x); front_hv 112.36 -> 112.61; front_topk_raw 27.4 -> 112.6. 20 gens 4w: trials/s 24.4 -> 64.8 (2.66x), front_hv 112.36 -> 112.69 (+0.3%, within 5%).
- Honest gap: 4x distinct/s target met at neither 1w (3.4x) nor 4w on this rerun (the earlier 5.7x was genomes/s incl. duplicates). HV guard is degenerate-ish here (robust=1 without weak panel -> HV ~= max wq).
- Next: re-review, then integrate into feat/quant-platform-swarm-20260922; add LEDGER line at integration.
