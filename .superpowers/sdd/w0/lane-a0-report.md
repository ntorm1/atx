# Lane W0-A0 report — Alpha kernel correctness

## Outcome

**DONE.** Every Build item of plan §7 W0-A0 is implemented, every Accept item is MET with a
named test and a measured number, and all six cited defects are closed. Both owning test
executables are fully green (alpha 703/703, factory 299/299).

## Branch / SHA / base / pool

- Branch `feat/w0-a0` in `C:\atx-wt\pool-2` (lease run id `aes-w0-a0`, held by the orchestrator;
  this lane never ran `lease-worktree.ps1`).
- Base: `458d0bef480a624e258070c9d45174a9984466bf` (W0 base). `feat/w0-integration` was merged
  before work and again before this report: "Already up to date" both times.
- Code commits: `cb3b4e78` (kernel fixes + tests), `3a1197b5` (factory golden re-baselines). This
  report is committed on top of them; the final HEAD is the commit that adds this file.
- Tree is clean after the report commit.

## Files changed

Production (owned by the brief unless marked):

| File | Change |
|---|---|
| `atx-engine/include/atx/engine/alpha/cs_ops.hpp` | `RankTies {Average, OrdinalV1}`; `cs_for_each_rank`; average-rank ties in `cs_rank_row`, `cs_quantile_row`, `cs_group_row` (new defaulted `ties` parameter). |
| `atx-engine/include/atx/engine/alpha/state_ops.hpp` | `HumpNaN {SeedCapV2, StickyV1}`, `kHumpMaxStaleDates = 5`, `HumpState`, new `hump_step(HumpState&, x, thr)`; the old rule kept verbatim as `hump_step_v1`. |
| `atx-engine/include/atx/engine/alpha/ts_ops.hpp` | Only the flat-window guard and the ts_sum/ts_mean routing: `FlatGuard {RelativeV2, NoneV1}`, `kTsFlatRelTol = 1e-10`, `tsv_is_flat`; guard threaded (defaulted parameter) through `tsv_var`, `tsv_lin_fit`, `ts_value_at`, `ts_pair_at`; `TsSumPath {WindowedV2, OnlineV1}`; Neumaier `TsvRunSum` and a `compensated` flag on `ts_online_sum_family`. |
| `atx-engine/include/atx/engine/alpha/vm.hpp` | A-02/A-03/A-13 sites plus the policy plumbing they need (see Deviations): `KernelPolicy` + `set_kernel_policy`, rank-tie/flat policy threaded into the Cs and Ts contexts, AuditExact ts_sum/ts_mean routed to the batch recompute, new hump scan, SubtreeCache bypass for a non-default policy. |
| `atx-engine/include/atx/engine/alpha/oracle.hpp`, `atx-engine/src/alpha/oracle.cpp` | Independent restatements: average-rank ties (rank / group_rank / quantile), `window_is_flat` guard in `sample_var`, `pearson`, `lin_fit`, ts_zscore and ts_regression, the new hump rule. Contract comments updated. |
| `atx-engine/include/atx/engine/alpha/typecheck.hpp`, `atx-engine/src/alpha/typecheck.cpp` | `has_scalar_literal_slot`, `validate_scalar_literal_operand` (called from `analyze_call`). |
| `atx-engine/include/atx/engine/factory/crossover.hpp`, `atx-engine/src/factory/crossover.cpp` | A cut in a scalar-literal slot only accepts finite Literal donors. |
| `atx-engine/include/atx/engine/factory/canonical.hpp`, `atx-engine/src/factory/canonical.cpp` | `canonical_string` (exact canonical form behind the hash); `CanonSet` verified API `insert(h, form)` / `contains(h, form)`, `collisions()`. |
| `atx-engine/include/atx/engine/alpha/streaming_engine.hpp` | **Not in the owned list — see Deviations.** Follows the kernel changes so the stream==batch contract holds: hump state, AuditExact ts_sum/ts_mean as the Generic window recompute, ResearchFast RunSum on `TsvRunSum`. |

Tests:

| File | Change |
|---|---|
| `atx-engine/tests/alpha/alpha_w0a0_kernels_test.cpp` (new) | Suites `AlphaCsRankTies_*`, `AlphaHumpWarmup_*`, `AlphaTypecheckScalarLiteral_Analyze`, `AlphaFlatWindow_*`, `AlphaAuditExactParity_*`. |
| `atx-engine/tests/alpha/alpha_w0a0_digest_test.cpp` (new) | Golden-digest battery: `AlphaCsRankTies_Digest`, `AlphaHumpWarmup_Digest`, `AlphaFlatWindow_Digest`, `AlphaAuditExactParity_Digest`. |
| `atx-engine/tests/factory/factory_w0a0_canon_test.cpp` (new) | `FactoryCanonCollision_*`, `AlphaTypecheckScalarLiteral_CrossoverStress`. |
| `atx-engine/tests/alpha/alpha_cs_test.cpp` | A-01 pin re-pinned (`AlphaCs_Rank.AllEqual_DeterministicOrdinal`, the brief's cited 256-272). |
| `atx-engine/tests/alpha/alpha_oracle_test.cpp` | A-01 pin re-pinned (`AlphaOracle_Rank.AllEqual_DeterministicOrdinalTieBreak`). |
| `atx-engine/tests/alpha/alpha_eval_perf_test.cpp` | A-01 pin (`CsValidSet_KernelDirect_TiedValues.RankTiebreakByAscendingIndex`): ordinal blocks now select `OrdinalV1` explicitly, expectations unchanged; Average block added. |
| `atx-engine/tests/alpha/cs_radix_rank_test.cpp` | A-01 pins (`CsRadixRank_Row.*`): ordinal references checked against `OrdinalV1`, expectations unchanged; Average references added. |
| `atx-engine/tests/alpha/alpha_trade_when_test.cpp` | A-02 pin re-pinned (`AlphaHump_NaN.NaNInputPropagatesPerOracle`). |
| `atx-engine/tests/factory/factory_nsga_search_test.cpp`, `atx-engine/tests/factory/factory_oos_test.cpp` | Golden digests re-baselined (table below); old values kept in comments. |

## What changed, in plain terms

- **A-01 rank ties.** `rank`, `group_rank` and `quantile` now give tied values the average of
  their ordinal positions (a fully tied row is 0.5 everywhere, like `ts_rank`). The old
  index tie-break is still available as `RankTies::OrdinalV1`.
- **A-02 hump.** A NaN input now emits NaN instead of silently holding the last value; a missing
  prior (warm-up, or state dropped) is seeded by the next finite input; the hidden prior survives
  at most 5 consecutive NaN dates (`kHumpMaxStaleDates`) so a short data hole keeps the
  suppression state but a universe exit does not resurrect a stale value. Old rule:
  `HumpNaN::StickyV1`.
- **A-03 scalar slots.** `analyze()` rejects anything but a finite literal in arg 2 of
  `scale`, `winsorize`, `quantile` and `hump`; crossover only offers literal donors there. The VM
  read sites carry a SAFETY note pointing at that guarantee.
- **A-09 flat windows.** A window whose population std is at most `1e-10 * |mean|` is treated as
  exactly flat on the batch path (VM and oracle use the identical test): var/std are 0,
  ts_zscore/skew/kurt/corr/rsquare are NaN, slope and resid are 0, a regression on a flat predictor
  is NaN. Old behaviour: `FlatGuard::NoneV1`.
- **A-13 ts_sum/ts_mean.** Under AuditExact they now use the per-window recompute, bit-exact with
  the oracle and independent of where the panel starts. ResearchFast keeps the O(T) slide, now
  Neumaier-compensated. Old behaviour: `TsSumPath::OnlineV1`.
- **A-18 CanonSet.** The set stores each canonical string under its hash and compares it on a
  hash hit; a different string under the same hash is admitted as a new structure and counted as
  a collision.
- `KernelPolicy::legacy_v1()` selects all four pre-W0 numeric rules at once.

## Acceptance table

| # | Plan Accept item | Test(s) | Measured result | Status |
|---|---|---|---|---|
| 1 | A tied row ranks to 0.5 for every name | `AlphaCsRankTies_Row.AllTiedRowRanksHalfForEveryName` (n = 2, 5, 6, 7; VM and oracle); `AlphaCsRankTies_Radix.WideTiedRowsMatchOracleAboveRadixThreshold` (200 names, radix path); `AlphaCs_Rank.AllEqual_DeterministicOrdinal` | every name exactly 0.5 in VM and oracle; OrdinalV1 still gives 0 … 1 | MET |
| 2 | `hump(ts_mean(x,5),0.1)` is finite from t=4 | `AlphaHumpWarmup_TsMean.FiniteFromT4` | NaN for t < 4, finite for all 48 cells t ≥ 4 (of 60); VM == oracle == stream bit-exact; old rule: **0** finite cells | MET |
| 3 | `winsorize(x, close)` returns Err | `AlphaTypecheckScalarLiteral_Analyze.WinsorizeWithPanelScalarIsErr` (+ `EveryScalarSlotRejectsNonLiteral`, `LiteralsAndDefaultsAccepted`) | `winsorize(open, close)` → `Err(InvalidArgument)`, message names the literal rule | MET |
| 4 | A 10k-child crossover stress run produces no non-literal scalar slot | `AlphaTypecheckScalarLiteral_CrossoverStress.TenThousandChildrenHaveOnlyLiteralScalarSlots` | children=10000 (attempts 12045), scalar slots audited 13591, **non-literal 0**, analyze rejects for a scalar slot 0 | MET |
| 5 | `ts_zscore` over a constant 0.1 window gives NaN in both VM and oracle | `AlphaFlatWindow_Zscore.ConstantPointOneWindowIsNaNInVmAndOracle` (d = 3, 5, 7, 10, 20) | all cells NaN in VM and oracle; old behaviour: 372 finite noise cells, max \|z\| = **0.974679** | MET |
| 6 | AuditExact VM output is bit-exact against the oracle for ts_sum and ts_mean | `AlphaAuditExactParity_Sum.TsSumTsMeanBitExactVsOracle` (2 fields × 6 windows × 2 ops, 300×6 panel with 1e8 magnitudes, NaN and ±inf holes); `AlphaAuditExactParity_Sum.OutputIndependentOfPanelStart` | **0** mismatching cells of 43200 (old online path: 27306 mismatches); panel-start dependence over 1192 cells: new 0, old 474 | MET |
| 7 | The full VM↔oracle differential stays green | whole `atx-engine-alpha-tests.exe` (all `AlphaCs_Differential*`, conformance, trade_when/hump differential, `StreamingEngine_Batch*`, …) + `AlphaAuditExactParity_Differential.W0SurfaceBitExact` (23 exprs) | 703/703 passed | MET |
| 8 | Golden digests re-baselined, with an old→new table | `Alpha*_Digest` battery; factory goldens | table below; every `*V1` enum reproduces its pre-W0 digest bit-exactly | MET |

Also verified: ResearchFast Neumaier slide vs an accurate per-window compensated reference:
max relative error **0.000e+00** (old uncompensated slide 2.771e-12)
(`AlphaAuditExactParity_ResearchFast.NeumaierSlideTighterThanUncompensated`); stream == batch
bit-exact for every new behaviour (ties, hump, flat, sums, both modes).

## Defect table

| ID | Status | How / where |
|---|---|---|
| A-01 | CLOSED | Average-rank ties by default in `cs_ops.hpp` (`cs_for_each_rank`; rank, group_rank, quantile) and in `oracle.cpp` (`average_rank_order`); `RankTies::OrdinalV1` reproduces the old digests (`AlphaCsRankTies_Digest`). |
| A-02 | CLOSED | `state_ops.hpp` `hump_step(HumpState&, …)` + `vm.hpp` hump scan + oracle restatement: NaN prior seeds, NaN x emits NaN, 5-date staleness cap; `HumpNaN::StickyV1` reproduces old digests. |
| A-03 | CLOSED | `typecheck.cpp` `validate_scalar_literal_operand` (finite Literal required in scale/winsorize/quantile/hump arg 2); `crossover.cpp` literal-only donors for such a cut; VM read sites documented. |
| A-09 | CLOSED | `ts_ops.hpp` `tsv_is_flat` guard in var/std/zscore/skew/kurt/slope/rsquare/resid/corr/regression on the batch path; identical `window_is_flat` in the oracle; `FlatGuard::NoneV1` reproduces old digests. |
| A-13 | CLOSED | `vm.hpp` routes AuditExact TsSum/TsMean to the batch recompute (oracle-exact); ResearchFast uses the Neumaier `TsvRunSum`; `TsSumPath::OnlineV1` reproduces old digests. |
| A-18 | CLOSED (lane scope) | `CanonSet` stores canonical strings and compares them on a hash hit (`insert(h, form)`, `contains(h, form)`, `collisions()`); `canonical_string` is the exact form. The search driver (`search_driver.cpp`, not owned) still calls the hash-only overloads, so wiring it onto the verified API is an integration item (see Integration notes). |

## Existing tests changed (each pins a cited defect)

| Test | Defect | Change |
|---|---|---|
| `AlphaCs_Rank.AllEqual_DeterministicOrdinal` (alpha_cs_test.cpp, brief-cited 256-272) | A-01 | default now expects 0.5 for every name + VM==oracle; the old ordinal values are still asserted under `KernelPolicy::legacy_v1()`. |
| `AlphaOracle_Rank.AllEqual_DeterministicOrdinalTieBreak` | A-01 | oracle now expects 0.5 for every name. |
| `CsValidSet_KernelDirect_TiedValues.RankTiebreakByAscendingIndex` | A-01 | ordinal blocks pass `OrdinalV1` explicitly (expectations unchanged); Average block added. |
| `CsRadixRank_Row.{RankRow,QuantileRow,GroupRank}BitIdenticalToStableSortReference` | A-01 | ordinal references checked under `OrdinalV1` (unchanged); Average references added. |
| `AlphaHump_NaN.NaNInputPropagatesPerOracle` | A-02 | the NaN date now expects NaN (was "hold 2.0"); t=0 and t=2 unchanged. |

No test was skipped, disabled or deleted.

## Golden-digest old → new

Alpha battery (`alpha_w0a0_digest_test.cpp`, frozen 80×16 fixture; old values captured on
the unmodified base build before any change):

| Group | Defect | Old (pre-W0) | New | Old reproduced by |
|---|---|---|---|---|
| rank / group_rank / quantile on ties | A-01 | `0xa50ec3743580856b` | `0xdd31545d3a5ad696` | `RankTies::OrdinalV1` alone, and `legacy_v1()` |
| hump (incl. nested ts_mean) | A-02 (+A-13) | `0x0a9ce0c6fb27f23e` | `0x3159e020352402f8` | `HumpNaN::StickyV1` + `TsSumPath::OnlineV1`, and `legacy_v1()` |
| flat-window family | A-09 | `0x1c75499cdd019337` | `0xc87fd2b6cc8ceb63` | `FlatGuard::NoneV1` alone, and `legacy_v1()` |
| ts_sum/ts_mean, AuditExact | A-13 | `0xfbd765ddbfee7d5c` | `0xda7bd655e7f69444` | `TsSumPath::OnlineV1` alone, and `legacy_v1()` |
| ts_sum/ts_mean, ResearchFast | A-13 | `0xfbd765ddbfee7d5c` | `0xca905bf2dfb89bb4` | `TsSumPath::OnlineV1` alone |

Factory goldens (re-baselined in the test files; old values kept in comments):

| Pin | Old | New | Defects | Attribution evidence |
|---|---|---|---|---|
| `factory_nsga_search_test` `kGoldenDigest` | `0xff95ac12512e0e91` | `0x889874a3b9b29c55` | A-01, A-13 | Experiment 1 (Engine defaults set to `OrdinalV1` + `OnlineV1`, rebuilt, not committed): `ScalarRaw_ReproducesGoldenDigest` passed with the OLD value. |
| `factory_nsga_search_test` `kGoldenMultiObjectiveOffPath` | `0x1763d356dfa4fbce` | `0x1968d9ad03e424b0` | A-01, A-13, A-03 | Not restored by any kernel policy (experiments 1 and 2 both gave `17091996207805464219`); the only other change on this path is the A-03 rule — attributed by elimination. |
| `factory_oos_test` R3b `kPinnedDigest` / admitted / version_id | `14354626274288095608` / 29 / `2670205213` | `100871560902752353` / 28 / `4049056013` | A-01, A-02/A-09, A-13, A-03 | Exp 1 and exp 2 (all four kernel enums V1) each moved it but did not restore it; residual attributed to A-03 by elimination. |
| `factory_oos_test` `kPinnedSubwindowDigest` | `6368737882721888739` | `14814588614960253351` | A-01, A-13 | Experiment 1 reproduced the old pin exactly, so the single-pass sub-window path is unchanged — only kernel semantics moved. |
| `factory_oos_test` HoldoutEngineReuse `kPinnedDigest` / version_id | `10909738412604108776` / `3846488092` | `5867665479471522971` / `703512706` | A-01, A-02/A-09, A-13, A-03 | As R3b (admitted unchanged at 5). |

The experiments were temporary edits of the `KernelPolicy` default initializers in `vm.hpp`,
rebuilt with `atx-shm-worker`, run, then reverted with `git checkout` (tree verified clean). A
direct experiment switching the A-03 rule off was not run (a temporary edit of the typecheck
rule was refused by the session's permission classifier), so the A-03 share is by elimination:
every other change on the factory path is either one of the four policy enums or inert there
(CanonSet's hash-only overloads keep their old semantics; the oracle/streaming changes are not
on the search path).

## Evidence

All commands were run from `C:\atx-wt\pool-2` with `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'` after a
free-RAM check (≥ 2.8 GB every time).

Configure (groups differed: tree had `alpha`, brief needs `alpha;factory`):

```
powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev -Groups "alpha;factory"
-- Build files have been written to: C:/atx-wt/pool-2/build-equity
exit=0
```

Baseline before any change (base build, for comparison):

```
build-equity\bin\atx-engine-alpha-tests.exe --gtest_brief=1      -> [==========] 679 tests from 256 test suites ran.  [  PASSED  ] 679 tests.  (exit 0; includes the 1 temporary digest-capture test)
build-equity\bin\atx-engine-factory-tests.exe --gtest_brief=1    -> [==========] 294 tests from 54 test suites ran.   [  PASSED  ] 294 tests.  (exit 0)
baseline digests: rank 0xa50ec3743580856b  hump 0x0a9ce0c6fb27f23e  flat 0x1c75499cdd019337  sumAE 0xfbd765ddbfee7d5c  sumRF 0xfbd765ddbfee7d5c
```

Single-TU checks:

```
atx-build.ps1 check -Preset equity-dev atx-engine\src\alpha\oracle.cpp          exit=0
atx-build.ps1 check -Preset equity-dev atx-engine\src\alpha\typecheck.cpp       exit=0
atx-build.ps1 check -Preset equity-dev atx-engine\src\factory\crossover.cpp     exit=0
atx-build.ps1 check -Preset equity-dev atx-engine\src\factory\canonical.cpp     exit=0
atx-build.ps1 check -Preset equity-dev atx-engine\tests\alpha\streaming_engine_test.cpp  exit=0
atx-build.ps1 check -Preset equity-dev atx-engine\tests\alpha\alpha_cs_test.cpp          exit=0
```

Final build (warnings-as-errors, no warnings beyond the toolchain's `/MP` notice):

```
atx-build.ps1 build -Preset equity-dev atx-engine-alpha-tests atx-engine-factory-tests atx-shm-worker
build exit=0
```

Whole owning executables (final code):

```
build-equity\bin\atx-engine-alpha-tests.exe --gtest_brief=1        alpha exit=0
[==========] 703 tests from 273 test suites ran. (55732 ms total)
[  PASSED  ] 703 tests.

build-equity\bin\atx-engine-factory-tests.exe --gtest_brief=1      factory exit=0
[w0a0] crossover stress: children=10000 attempts=12045 scalar_slots=13591 non_literal=0 scalar_rejects=0
[==========] 299 tests from 57 test suites ran. (89553 ms total)
[  PASSED  ] 299 tests.
```

Measured lines printed by the alpha run:

```
[w0a0-digest] rank (A-01)        old=0xa50ec3743580856b new=0xdd31545d3a5ad696
[w0a0-digest] hump (A-02)        old=0x0a9ce0c6fb27f23e new=0x3159e020352402f8
[w0a0-digest] flat (A-09)        old=0x1c75499cdd019337 new=0xc87fd2b6cc8ceb63
[w0a0-digest] sum AuditExact     old=0xfbd765ddbfee7d5c new=0xda7bd655e7f69444
[w0a0-digest] sum ResearchFast   old=0xfbd765ddbfee7d5c new=0xca905bf2dfb89bb4
[w0a0] hump(ts_mean(x,5),0.1): finite cells new=48 old=0 of 60
[w0a0] ts_zscore(fund, 3) on constant 0.1: legacy finite cells=114
[w0a0] ts_zscore(fund, 5) on constant 0.1: legacy finite cells=0
[w0a0] ts_zscore(fund, 7) on constant 0.1: legacy finite cells=102
[w0a0] ts_zscore(fund, 10) on constant 0.1: legacy finite cells=93
[w0a0] ts_zscore(fund, 20) on constant 0.1: legacy finite cells=63
[w0a0] legacy ts_zscore noise: 372 finite cells, max |z| = 0.974679
[w0a0] legacy flat corr finite cells=26, max |slope| = 1.421e-16
[w0a0] AuditExact ts_sum/ts_mean: new mismatches=0, legacy online mismatches=27306 of 43200 cells
[w0a0] panel-start dependence over 1192 cells: new=0, legacy=474
[w0a0] ResearchFast max rel err vs compensated reference: neumaier=0.000e+00 uncompensated=2.771e-12
```

Anchored suite runs (`atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'`), all exit 0:

```
^AlphaCsRankTies_              100% tests passed, 0 tests failed out of 7
^AlphaHumpWarmup_              100% tests passed, 0 tests failed out of 4
^AlphaTypecheckScalarLiteral_  100% tests passed, 0 tests failed out of 5
^AlphaFlatWindow_              100% tests passed, 0 tests failed out of 5
^AlphaAuditExactParity_        100% tests passed, 0 tests failed out of 5
^FactoryCanonCollision_        100% tests passed, 0 tests failed out of 4
```

Diagnostics (failed intermediate runs, not used as success evidence): the first factory run
after the change showed 14 failures, 7 of which were seq-vs-parallel digest mismatches caused by
a stale `atx-shm-worker.exe` (built 2026-09-22 with the old kernels); rebuilding the worker with
the owning targets fixed them. The remaining 7 were the golden pins re-baselined above.

## Deviations from the brief

1. **`streaming_engine.hpp` edited (not in the owned list).** The StreamingEngine re-uses the
   changed kernels and has a bit-exact stream==batch contract tested inside the owning alpha
   target (`StreamingEngine_Batch*`, which covers `ts_sum` and `hump`). Without following the
   kernel changes that contract (and the alpha target) would break. Edits are limited to the hump
   state, the ts_sum/ts_mean classification and the RunSum step. No W0 lane owns this file.
2. **`vm.hpp` beyond the literal cited lines.** Keeping the old behaviour reproducible behind
   versioned enums (RULES §2) needs a way to select them on the Engine: the `KernelPolicy` struct
   sits next to `EvalMode` (the A-13 cited site), plus `set_kernel_policy`/`kernel_policy`, one
   member, one policy field in each of `CsRowsCtx`/`TsBatchCtx`, the `ties` argument through
   `cs_one_date`, and a one-line SubtreeCache bypass when the policy is not the default (the cache
   key has no policy field). W1-A1 should expect these small hunks.
3. **Rank-tie policy applied to `group_rank` and `quantile` too**, not only `rank`: they share the
   same sort and had the same index-proxy defect; one policy keeps the family consistent.
4. **Choices the plan left open:** staleness cap = 5 dates (`kHumpMaxStaleDates`); flat tolerance
   = `1e-10` relative std (about 1000× the rounding noise of a 500-term mean). The flat guard is
   applied on the batch path only (per the brief); the ResearchFast Welford/sliding lanes are
   unchanged.
5. **The oracle pins the default policy only**; the `*V1` rules are a VM digest-reproduction path.
6. A-03 share of the factory digest shifts is attributed by elimination (see above).

## Integration notes

- **A-18 driver wiring (factory track, `search_driver.cpp`):** call `canon.insert(h,
  canonical_string(g, canon_cfg_))` / `canon.contains(h, form)` instead of the hash-only
  overloads, and key `fitness_cache` by (hash, form) or check the form on a hit. Until then the
  driver keeps the pre-W0 collision-blind behaviour. Resume snapshots persist hashes only; a
  hash-only entry is treated as "seen" by the verified API.
- **Other consumers of the Engine do not thread `KernelPolicy`** (SearchDriver, global DAG eval,
  process workers): they run the corrected default, which is intended. Re-deriving a pre-W0
  search result would need the policy plumbed through `SearchConfig` and the shm worker.
- **Rebuild `atx-shm-worker` whenever alpha kernels change**: the factory/parallel seq==parallel
  tests spawn it, and a stale worker gives digest divergence that looks like nondeterminism.
- **Other test groups not built in this lane may carry goldens that move** with these kernel
  changes (for example `data_real_panel_e2e_test` `kGoldenDigest`, `atx-impl` alpha101 tests,
  parallel workload digests). The integration gate should run them and re-baseline with this
  report's attribution.
- `combine/combined_source.hpp` has its own copy of the ordinal rank kernel (E track). It still
  breaks ties by index; the E lane may want to adopt average ties for consistency.
- Pre-existing, not changed: `cs_quantile_row` casts the bucket-count literal to `int` without a
  range check (a huge literal such as `quantile(x, 1e20)` is undefined behaviour). Typecheck now
  guarantees a finite literal; a range rail belongs to whoever next owns the typecheck rules.

## Ledger candidates

- W0-A0: pre-W0 ts_zscore over a constant 0.1 window returned noise up to |z| = 0.975 (372 of
  600 cells finite); the 1e-10 relative flat guard makes it NaN in VM and oracle.
- W0-A0: the pre-W0 AuditExact online ts_sum/ts_mean mismatched the oracle on 27306 of 43200
  cells (1e8-scale data); the windowed recompute is 0/43200 and panel-start independent.
- Factory seq==parallel digest failures after an alpha-kernel change are usually a stale
  `atx-shm-worker.exe` — build it with the owning test targets.
