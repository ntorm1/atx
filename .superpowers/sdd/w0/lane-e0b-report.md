# Lane W0-E0b report — Trial accounting

## Outcome

DONE. All three plan acceptance items are met with named tests and measured numbers. E-01, E-16
(registry side) and E-17 are closed. L-08 is closed on the registry side. The learn-side parts of
L-08, the equity-mine recording site for E-16 and the embargo call sites for E-17 belong to other
lanes and are handed over below. The owning target `atx-engine-eval-tests` is fully green (221/221).

## Branch / SHA

- Branch: `feat/w0-e0b`. The code commit is `d0a37e02`. The final SHA is the report commit on top of
  it; the caller records it with `git -C C:\atx-wt\pool-6 rev-parse HEAD`.
- Base: `458d0bef480a624e258070c9d45174a9984466bf` (W0 base). `feat/w0-integration` is an ancestor
  of the lane (the pre-merge said "Already up to date", and I re-checked it before the report).
- Pool: `C:\atx-wt\pool-6` (lease run id `aes-w0-e0b`, held by the orchestrator). The tree is clean.

## Files changed

| File | Change |
|---|---|
| `atx-engine/include/atx/engine/eval/trial_clusters.hpp` | **new.** ONC-style clustering API, representative Sharpes, Monte-Carlo max-Sharpe null (`McMaxNull`). |
| `atx-engine/src/eval/trial_registry.cpp` | Registry V2 log (windows + metadata), V1 still read and appended, exported chain head, multi-writer OS lock, `refresh()`, `correlation()`, `mc_max_null()`, `accounting()`, chain-head sidecar codec. It also holds the trial_clusters implementation (the brief allows this; there is no new `.cpp`). |
| `atx-engine/include/atx/engine/eval/trial_registry.hpp` | The API above: `TrialMeta`, `TrialSample`, `TrialLogFormat`, `TrialInfo`, `TrialChainHead`, `TrialAccounting(Config)`. `TrialSummary` gains IS/OOS counts. |
| `atx-engine/include/atx/engine/eval/deflated_sharpe.hpp` | `SummaryDsrRule` (V1 kept, V2 default) on the `TrialSummary` overload. New `TrialAccounting` overload with `AccountingDsrRule` (cluster N, Monte-Carlo floor, Monte-Carlo CDF). |
| `atx-engine/include/atx/engine/eval/lockbox.hpp` | `EmbargoRule` / `LockboxEmbargo` / `lockbox_embargo_len`, plus the `reserve_lockbox` / `reserve_window` overloads (E-17). `LockboxChainHead`, `lockbox_chain_head` and `verify_lockbox_chain_head`. |
| `atx-engine/tests/eval/eval_trial_registry_test.cpp` | Only the cited E-01 pin: `RegistryFedDsrIsLessOverDeflatedOnCorrelatedTrials` is replaced by `RegistryFedDsrDiscountsCorrelationOnce`. |
| `atx-engine/tests/eval/eval_w0e0b_trial_clusters_test.cpp` | **new.** `EvalTrialClusters_*` (13 tests). |
| `atx-engine/tests/eval/eval_w0e0b_registry_windows_test.cpp` | **new.** `EvalRegistryWindows_*` (9 tests). |
| `atx-engine/tests/eval/eval_w0e0b_lockbox_embargo_test.cpp` | **new.** `EvalLockboxEmbargo_*` (4 tests). |

There are no CMake edits and no new `src/*.cpp`. The eval test folder is globbed.

## What was built (short)

- **Cluster-N DSR (E-01).** `TrialRegistry::accounting()` builds the trial correlation from the
  retained unit PnL sketches. It then runs ONC-style clustering: kernel k-means on the unit
  vectors, the ONC silhouette quality q = mean/sd choosing k, and the ONC recursive refinement of
  below-average clusters. Each cluster's representative Sharpe is the Sharpe of its equal-risk
  member portfolio, mean(SR) / sqrt(mean ρ). Finally it computes a Monte-Carlo null of max SR under
  the estimated correlation. `deflated_sharpe(sr, TrialAccounting, …)` offers three rules:
  - `ClusterV2`: N = clusters, V = variance of the representatives. This is the plan rule.
  - `ClusterMcFloorV2` (the default): SR* = max(cluster benchmark, Monte-Carlo E[max]).
  - `MonteCarloMaxV2`: dsr = the null CDF of the maximum at the observed Sharpe.

  Degenerate shapes: near-duplicates form one cluster; when there is no block structure every
  trial is a singleton, and the cluster benchmark then reduces to N = n_raw with the cross-trial
  V, which is the correct E[max] for equicorrelated and for independent trials.
- **Summary overload fix (E-01).** `deflated_sharpe(sr, TrialSummary, …)` now defaults to
  `RawNCrossVarV2`: N = n_raw paired with the cross-trial V, so correlation is discounted once.
  `NEffCrossVarV1` reproduces the old rule.
- **Registry windows and metadata (E-16).** `cfg.pnl_len` is now a calendar length. Each trial
  records a window `[start, end]` (variable pnl length), a fidelity level, family and theme tags
  (`trial_tag(name)`) and a `TrialSample` flag (Unspecified / InSample / OutOfSample). These are
  stored in a V2 log. V1 logs keep their format; reading and legacy appends work. The legacy
  `record()` equals the full-calendar window bit-for-bit (same n_eff, `registry_hash` and chain
  head). The n_eff bias correction generalizes to partial windows.
- **Configuration counting (L-08, registry side).** Records are content-addressed on
  (kind, config_hash), so recording every fold of one configuration counts it once.
- **Label-horizon embargo (E-17).** `LockboxEmbargo{LabelHorizonV2 (default), max_label_horizon,
  delay}` gives an embargo of max_label_horizon + delay. `CpcvFractionV1` reproduces ⌈0.01·T⌉. An
  undeclared horizon is an error.
- **L4 gaps.**
  - `TrialRegistry::chain_head()` is a chained digest of every log record's bytes. It is exported
    outside the log (sidecar codec `write_chain_head` / `read_chain_head`) and verified by
    `open(path, cfg, anchor)` before any repair. Removing records, or editing a record and
    re-forging its checksum, gives `ParseError` and leaves the file untouched.
  - `lockbox_chain_head` / `verify_lockbox_chain_head` do the same for the lockbox audit chain.
  - Multi-writer lock: every durable registry operation runs under an exclusive OS lock
    (LockFileEx / flock) on one persistent handle. `record()` first ingests other writers'
    records. `refresh()` is exposed.

## Acceptance table

| Plan acceptance item | Test(s) | Measured result | Status |
|---|---|---|---|
| An equicorrelated null (ρ=0.5, N=2000) gives a Monte-Carlo false-positive rate of 5% ± 1% | `EvalTrialClusters_Mc.EquicorrelatedNullFalsePositiveRateIsFivePercent` | FPR = **0.0545** over 12,000 null experiments: 12 calibration samples of 2000 trials × T=252, each with its own estimated correlation and a 2000-draw Monte-Carlo null. The same experiments measure the pre-W0 rule (V1) at **0.5248** and the corrected summary rule (V2) at **0.0123**. A separate 16-calibration × 500 run gave 0.0549 (Diagnostics). | MET |
| A G-block model gives N ≈ G ± 10% | `EvalTrialClusters_Onc.GBlockModelGivesNWithinTenPercentOfG` | G=4 (n=59, ρ_within 0.6) → **N=4**; G=10 (n=119, ρ 0.5) → **N=10**; G=20 (n=199, ρ 0.5) → **N=20**. Every block lands in one cluster. Unequal block sizes. | MET |
| `RegistryFedDsrIsLessOverDeflatedOnCorrelatedTrials` is replaced by a test that asserts the correct behavior | `EvalTrialRegistry.RegistryFedDsrDiscountsCorrelationOnce` | Asserts that the default registry-fed SR* equals `expected_max_sharpe(n_raw, var_sr)` and lies within ±20% of the Monte-Carlo E[max] under the estimated correlation. It also asserts that the V1 rule's SR* is below 0.5 × that E[max] (it under-deflates: v1.dsr ≥ fed.dsr). | MET |

Supporting tests (all passing, listed under Evidence):
- ONC degenerate shapes: duplicates form one cluster, independent trials become singletons,
  equicorrelated trials show no blocks, tight clusters get canonical labels. There are also
  determinism and input-validation tests.
- The representative-Sharpe formula.
- Monte-Carlo null checks: independent trials vs. the closed form (ratio **0.9898**); identical
  trials show no premium.
- DSR rule wiring. The cross-check agrees where the cluster model holds: 20 tight blocks give
  SR*_cluster/SR*_mc = **0.978**.
- Windows, metadata, V1 compatibility, partial-window n_eff (40 staggered independent trials give
  **40.00**), fold counting, chain-head tamper tests, multi-writer tests, and the E-17 embargo
  tests including the leakage property.

## Defect table

| ID | Status | How / where |
|---|---|---|
| E-01 | **CLOSED** | `deflated_sharpe.hpp`: the `TrialSummary` overload now defaults to `SummaryDsrRule::RawNCrossVarV2`; the V1 rule is kept for re-derivation. The new cluster-N `TrialAccounting` overload (`AccountingDsrRule`, default `ClusterMcFloorV2`) is backed by `trial_clusters.hpp` and `TrialRegistry::accounting()` in `trial_registry.cpp`. The pin test is replaced (`RegistryFedDsrDiscountsCorrelationOnce`). On the equicorrelated null the V1 rule over-rejects at 52% and V2 is conservative at 1.2%. |
| E-16 | **CLOSED (registry side)**; recording site **DEFERRED → W0-I0b** | The registry API and V2 log store the window [start, end] on a calendar (variable pnl length), fidelity, family and theme tags, and the IS/OOS flag (`trial_registry.{hpp,cpp}`). Tests: `EvalRegistryWindows_*`. The recording call at `stage_equity_mine.cpp:742` (train PnL, fixed `pnl_len`) is I0b's file; see Integration notes. |
| E-17 | **CLOSED (lockbox API)**; call sites **DEFERRED → W0-I0a / A-track / W1** | `lockbox.hpp`: `LockboxEmbargo` (default `LabelHorizonV2` = max label horizon + delay; `CpcvFractionV1` legacy), `lockbox_embargo_len`, and the `reserve_lockbox` / `reserve_window` overloads. Tests: `EvalLockboxEmbargo_*`. The existing callers still pass CPCV or explicit widths (`stage_discover.cpp:638`, `stage_sweep.cpp:144`, `factory.cpp:1424,1830`, `robust_pipeline.hpp:181`); none is owned by this lane. |
| L-08 | **CLOSED (registry-side trial counting)**; learn items **DEFERRED → W0-L0 / W3-L4** (as the findings row assigns) | The registry counts configurations: records are content-addressed on (kind, config_hash), so any number of folds of one configuration counts once (`EvalRegistryWindows_Counting.FoldsOfOneConfigurationCountOnce`). The per-fold `trial_count++` sites (`linear_alpha.cpp:229`, `gbt.cpp:550`, `tcn_alpha.cpp:374`), the pooled-Pearson horizon blend, `IcLoss` batching and the autoencoder label are learn-track files. |

## Golden-digest old→new table

No golden digest in the repository changes. The pinned registry outputs for full-window histories
are unchanged bit-for-bit: `registry_hash` and `n_eff` (`EvalRegistryWindows_Legacy.V1LogStillReadsAndAppends`
asserts equality between a V1 log and a V2 in-memory registry).

Numeric defaults that moved, each tied to a defect and reproducible behind a versioned enum:

| Output | Old | New | Defect | Old behaviour via |
|---|---|---|---|---|
| `deflated_sharpe(sr, TrialSummary, …)` SR* / DSR (used by equity-mine `dsr_train`) | N = n_eff, V = var_sr | N = n_raw, V = var_sr. SR* is larger by E[M_n]/E[M_neff], about 2.7× for L9 per the findings. | E-01 | `SummaryDsrRule::NEffCrossVarV1` |
| Format of new durable registry logs | V1 (`ATXTRG01`, 40+8d byte records) | V2 (`ATXTRG02`, 72+8d byte records with metadata) | E-16 | `TrialRegistryConfig::format = TrialLogFormat::V1`. Existing V1 logs keep V1. |
| Lockbox embargo (new `LockboxEmbargo` API) | ⌈0.01·T⌉ | max label horizon + delay | E-17 | `EmbargoRule::CpcvFractionV1`. The old `reserve_lockbox` overloads are unchanged. |

## Evidence

All commands were run from `C:\atx-wt\pool-6` (each PowerShell call began with
`Set-Location C:\atx-wt\pool-6; $env:CMAKE_BUILD_PARALLEL_LEVEL='2';`). Free RAM before builds was
5.2 GB, 5.8 GB, 4.2 GB and 2.8 GB, all above the 2 GB floor.

1. Pre-merge: `git -C C:\atx-wt\pool-6 merge --no-ff feat/w0-integration` → `Already up to date.`
   (exit 0). Re-checked before the report: `git merge-base --is-ancestor feat/w0-integration HEAD`
   → `integration already contained` (exit 0).
2. Reconfigure (build-equity had `ATX_TEST_GROUPS=combine;library`; the brief needs `eval`):
   `powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev -Groups "eval"` → exit 0.
   ```
   -- Configuring done (25.2s)
   -- Generating done (0.7s)
   -- Build files have been written to: C:/atx-wt/pool-6/build-equity
   ```
   The CMakeCache now has `ATX_TEST_GROUPS:STRING=eval`.
3. Type-checks (exit 0, no diagnostics):
   - `scripts\atx-build.ps1 check -Preset equity-dev atx-engine\src\eval\trial_registry.cpp`
   - Headers used by atx-impl, which were not modified: `check … atx-impl\src\stage_equity_mine.cpp`,
     `… stage_discover.cpp` and `… stage_sweep.cpp`, all exit 0.
4. Build: `scripts\atx-build.ps1 build -Preset equity-dev atx-engine-eval-tests` → exit 0.
   ```
   [65/88] Linking CXX static library lib\atx-engine.lib
   [85/88] Linking CXX executable bin\atx-engine-eval-tests.exe
   ```
   A later rebuild on the committed tree also exited 0. The whole `atx-engine` library compiles
   under `/W4 /WX`.
5. Anchored suites on the committed tree (`scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '<suite>'`):
   ```
   ^EvalTrialClusters   :: 100% tests passed, 0 tests failed out of 13   (Total Test time (real) = 34.90 sec)  exit=0
   ^EvalRegistryWindows :: 100% tests passed, 0 tests failed out of 9    (0.69 sec)                            exit=0
   ^EvalLockboxEmbargo  :: 100% tests passed, 0 tests failed out of 4    (0.15 sec)                            exit=0
   ^EvalTrialRegistry   :: 100% tests passed, 0 tests failed out of 13   (1.45 sec)                            exit=0
   ```
   Per test (verbatim ctest lines):
   ```
   EvalTrialClusters_Onc.GBlockModelGivesNWithinTenPercentOfG ...................   Passed    2.48 sec
   EvalTrialClusters_Onc.DuplicatesFormOneCluster ...............................   Passed    0.11 sec
   EvalTrialClusters_Onc.IndependentTrialsAreSingletons .........................   Passed    0.37 sec
   EvalTrialClusters_Onc.EquicorrelatedTrialsHaveNoBlockStructure ...............   Passed    1.18 sec
   EvalTrialClusters_Onc.TwoTightClustersWithCanonicalLabels ....................   Passed    0.21 sec
   EvalTrialClusters_Onc.IsDeterministicAndValidatesInput .......................   Passed    0.16 sec
   EvalTrialClusters_Rep.RepresentativeIsTheEqualRiskPortfolioSharpe ............   Passed    0.03 sec
   EvalTrialClusters_Mc.IndependentTrialsMatchTheClosedFormExpectedMax ..........   Passed    0.56 sec
   EvalTrialClusters_Mc.IdenticalTrialsHaveNoSelectionPremium ...................   Passed    0.26 sec
   EvalTrialClusters_Mc.RejectsBadInput .........................................   Passed    0.03 sec
   EvalTrialClusters_Mc.EquicorrelatedNullFalsePositiveRateIsFivePercent ........   Passed   28.51 sec
   EvalTrialClusters_Dsr.ClusterRuleUsesClusterCountAndRepresentativeVariance ...   Passed    0.53 sec
   EvalTrialClusters_Dsr.FloorTakesOverWhereClustersCannotExpressSelection ......   Passed    0.12 sec
   EvalRegistryWindows_Record.VariableWindowsShareOneCalendar .................   Passed    0.03 sec
   EvalRegistryWindows_Record.LegacyOverloadIsTheFullCalendarWindow ...........   Passed    0.03 sec
   EvalRegistryWindows_Record.MetadataRoundTripsThroughTheV2Log ...............   Passed    0.04 sec
   EvalRegistryWindows_Legacy.V1LogStillReadsAndAppends .......................   Passed    0.03 sec
   EvalRegistryWindows_Neff.PartialWindowsKeepTheBiasCorrection ...............   Passed    0.21 sec
   EvalRegistryWindows_Counting.FoldsOfOneConfigurationCountOnce ..............   Passed    0.03 sec
   EvalRegistryWindows_ChainHead.AnchorDetectsRemovedAndEditedRecords .........   Passed    0.19 sec
   EvalRegistryWindows_MultiWriter.TwoHandlesNeverDoubleCount .................   Passed    0.04 sec
   EvalRegistryWindows_MultiWriter.ConcurrentThreadsProduceOneConsistentLog ...   Passed    0.06 sec
   EvalLockboxEmbargo_Len.EmbargoIsLabelHorizonPlusDelay ..................   Passed    0.02 sec
   EvalLockboxEmbargo_Reserve.ReservationUsesTheDeclaredHorizon ...........   Passed    0.03 sec
   EvalLockboxEmbargo_Reserve.NoVisibleLabelReadsTheLockbox ...............   Passed    0.03 sec
   EvalLockboxEmbargo_ChainHead.AnchorDetectsRemovedAndReplacedReceipts ...   Passed    0.04 sec
   EvalTrialRegistry.RegistryFedDsrDiscountsCorrelationOnce ...........   Passed    0.25 sec
   (the other 12 EvalTrialRegistry tests: Passed)
   ```
6. Whole owning executable on the committed tree:
   `build-equity\bin\atx-engine-eval-tests.exe --gtest_brief=1` → exit 0. Verbatim tail with the
   measured numbers (the `CHECK failed` lines come from pre-existing death tests):
   ```
   [w0e0b] 40 independent trials on staggered windows: n_eff=40.00 (uncorrected 35.51)
   [w0e0b] 50 independent trials on one partial window: n_eff=50.00
   [w0e0b] G-block: G=4 n=59 rho_within=0.60 -> N_clusters=4 (n_eff=9.78, mean silhouette=0.369)
   [w0e0b] G-block: G=10 n=119 rho_within=0.50 -> N_clusters=10 (n_eff=33.44, mean silhouette=0.259)
   [w0e0b] G-block: G=20 n=199 rho_within=0.50 -> N_clusters=20 (n_eff=61.98, mean silhouette=0.262)
   [w0e0b] equicorrelated rho=0.5 n=300: shape=2 N_clusters=300
   [w0e0b] independent n=200: MC E[max]=0.17244 closed-form SR*=0.17421 ratio=0.9898
   [w0e0b] calibration 0: MC E[max]=0.1508 q95=0.2275 (sigma units 2.394 / 3.611); var_sr*T=0.478
   ... (calibrations 1-10) ...
   [w0e0b] calibration 11: MC E[max]=0.1552 q95=0.2290 (sigma units 2.464 / 3.635); var_sr*T=0.514
   [w0e0b] equicorrelated null rho=0.5 N=2000 (n_eff=3.77), 12000 experiments: FPR MonteCarloMaxV2=0.0545, summary NEffCrossVarV1 (pre-W0)=0.5248, summary RawNCrossVarV2=0.0123
   [w0e0b] tight 20-block: SR*_cluster=0.12950 SR*_mc=0.13244 ratio=0.978; V1 SR*=0.12742
   [==========] 221 tests from 38 test suites ran. (46746 ms total)
   [  PASSED  ] 221 tests.
   ```

### Diagnostics (failed attempts; these support no claim)

- The first `check` of `trial_registry.cpp` failed: `far` is a `windows.h` macro. I renamed it.
- On the first G-block run, G=10 at ρ_within 0.5 fell back to singletons (mean silhouette 0.000),
  because k-means++ seeding almost never puts one seed in each of 10 blocks. The fix is that
  restart 0 now uses farthest-first seeding and the others use greedy k-means++. After the fix,
  N = G exactly.
- The first FPR test design used T = 64 and gave FPR = 0.0640 (fail). The correlation estimated
  from 64 periods for 2000 trials has rank 63, and the maximum under it has a lighter tail than the
  truth. With T = 252 and 4 calibrations it gave 0.0597, at the edge of the band, because all four
  samples happened to have a realized ρ slightly above 0.5. An exploratory run (not committed)
  with 16 calibrations × 500 experiments gave **0.0549**. The committed design (12 × 1000) gives
  **0.0545**. The procedure's true FPR at T = 252 is about 5.5%. It is a little above 5% because
  the null is built from an estimated correlation matrix.

## Deviations from brief

1. **The default accounting rule is `ClusterMcFloorV2`, not the pure cluster rule.** The plan
   calls the Monte-Carlo E[max] a cross-check; I made it binding (it can only raise SR*). The
   reason: where ONC finds no clean blocks (equicorrelated or mixed sets), a cluster count cannot
   express the selection. The pure plan rule is available as `AccountingDsrRule::ClusterV2`, and
   both benchmarks are returned in `RegistryDsr` so the cross-check stays visible.
2. **Which rule meets the FPR acceptance item.** The "Monte-Carlo false-positive rate of 5% ± 1%"
   is met by `AccountingDsrRule::MonteCarloMaxV2`, whose dsr is the null CDF of the maximum. The
   PSR-based rules are conservative by construction on this null: the summary V2 rule measured
   1.23%. `ClusterMcFloorV2` has SR* ≥ E_mc[max], which is ≈ the V2 benchmark here, but its FPR at
   N = 2000 was **not measured**, because clustering 2000 trials in the debug preset is too slow
   for a unit test. On the equicorrelated n = 300 test, ONC returns singletons, where the cluster
   benchmark equals the V2 benchmark.
3. **FPR test mechanics.** Calibrations 1 to 11 compute the Monte-Carlo null from the standardized
   PnL rows directly. Calibration 0 goes through `TrialRegistry` and asserts the same draws
   bit-for-bit. The reason is debug-build cost: the Eigen Gram update dominates at /Od. The test
   still takes about 28-45 s in the debug preset.
4. **The lockbox already had a multi-writer lock** (`FileLockboxAudit`). For the lockbox, the L4
   gap was closed by the exported chain head only. The registry received both the chain head and
   the lock.
5. **The locked-file I/O helper is duplicated** in `trial_registry.cpp`, modelled on
   `lockbox_audit.cpp`'s anonymous-namespace `LockedFile`, because that file is not owned by this
   lane.
6. **Durability semantics are unchanged:** a record is handed to the OS before `record()` returns,
   which survives a process crash, and there is no fsync per record, as before.
7. **Python bindings** (`python/src/_bindings/*`) are not part of `equity-dev` and were not built.
   They use only the unchanged `deflated_sharpe(sr, T, skew, exkurt, N, var)` overload.

## Integration notes

- **W0-I0b (`atx-impl/src/stage_equity_mine.cpp`), E-16 recording side:**
  - At `:1739`, set `TrialRegistryConfig::pnl_len` to the calendar covering train and validation
    sessions.
  - At `:742`, record with
    `registry.record(kind, hash, TrialMeta{window_start, window_end, fidelity, TrialSample::InSample, trial_tag(family), trial_tag(theme)}, row.train.net, sr)`.
    Train PnL is IS; state that explicitly rather than leaving it Unspecified.
  - `dsr_train` (`:755`) already moves to `SummaryDsrRule::RawNCrossVarV2` through the default,
    so new runs are more deflated. For the plan's cluster N, call `registry.accounting({})` and
    `deflated_sharpe(sr, acct, T, skew, kurt)`.
  - Export `registry.chain_head()` in the run report and manifest (`write_chain_head`).
  - The mine report's `registry_hash` is unchanged in meaning.
- **W1-I1 (one registry, run manifest):**
  - Import the sidecars into a V2 registry with `family_tag` / `theme_tag = trial_tag(name)`.
  - DSR everywhere through `accounting()` + the `TrialAccounting` overload. The `max_trials`
    guard defaults to 4096 (the n×n correlation is O(n²)); L9's 2065 fits.
  - Bind `TrialChainHead` and `LockboxChainHead` into the manifest, and verify them with
    `TrialRegistry::open(path, cfg, anchor)` / `verify_lockbox_chain_head`.
- **E-17 call sites** (W0-I0a for `stage_discover.cpp:638` and `stage_sweep.cpp:144`; A-track or
  W1 for `factory.cpp:1424,1830` and `robust_pipeline.hpp:181`): replace
  `detail::embargo_len_from_cpcv(...)` with
  `lockbox_embargo_len(LockboxEmbargo{EmbargoRule::LabelHorizonV2, max_label_horizon, delay}, T)`,
  or call the `reserve_lockbox(panel, frac, LockboxEmbargo)` overload.
- **W0-L0 (L-08 learn side):** record one registry entry per configuration (a config_hash without
  the fold). Registry dedup makes per-fold calls harmless, but `trial_count++` per fold must go.
- **Later cleanup:** factor the locked-file helper shared by `lockbox_audit.cpp` and
  `trial_registry.cpp` into one internal header.

## Ledger candidates

1. ONC via kernel k-means needs farthest-first seeding. Plain k-means++ puts one seed in each of 10
   blocks with probability of about 1% when within:across distance² is 1:2, so N = G recovery
   fails. With a farthest-first restart, G = 4/10/20 is recovered exactly.
2. The Monte-Carlo max-Sharpe null under an estimated correlation (ρ = 0.5, N = 2000) gives
   FPR 5.45% at T = 252 but 6.4% at T = 64 (rank-deficient R̂). Feed it at least a year of PnL per
   trial.
3. On the same equicorrelated null, the pre-W0 registry DSR (N_eff + cross-trial V, E-01) rejects
   52% of the time; N = n_raw with the cross-trial V rejects 1.2%.

## Fix pass 1

Review: `.superpowers/sdd/w0/lane-e0b-review.md` (APPROVE at `d6b6df2f`, three minor findings, no
blocker or major). All three are fixed in owned files: documentation where the finding asked for
it, plus new tests that pin the documented behaviour with measured numbers. No existing test was
changed or weakened. No production code path changed; the header edits are comments only.

| Finding | What changed | Evidence |
|---|---|---|
| `deflated_sharpe.hpp:257`: acceptance 1 (FPR 5% ± 1%) holds only for opt-in `MonteCarloMaxV2`, and the default FPR was not measured | The `AccountingDsrRule` doc block now has a "Which rule for an α-level selection GATE" paragraph. A calibrated gate must use `MonteCarloMaxV2` with `dsr > 1 − α`. `ClusterV2` / `ClusterMcFloorV2` are PSR-based: conservative, not calibrated. Use them for ranking or haircut reporting. `EvalTrialClusters_Mc.EquicorrelatedNullFalsePositiveRateIsFivePercent` now also measures an upper bound on the default rule's FPR on the same 12,000 null experiments. It runs the default `ClusterMcFloorV2` with no clusters, so SR* = SR*_mc (asserted exactly). A real partition can only raise SR*, and PSR falls as SR* rises, so this bounds the default rule's FPR for every clustering outcome. The test asserts the bound is ≤ 0.05. Integration note below for W1-I1. | `[w0e0b] same null: FPR bound of the default ClusterMcFloorV2 (PSR at SR*_mc)=0.0141`. `MonteCarloMaxV2` is unchanged at 0.0545. |
| `trial_registry.hpp:199`: memory is now O(n·d) by default, not O(d²) | `trial_registry.hpp` header note: new "Memory" bullet. Memory is O(d² + n·d) with sketches and O(d² + n) without; the TrialInfo (72 B) and the dedup-set entry are always kept. 10^6 trials at d = 64 take about 0.6 GB with sketches and about 0.1 GB without. 10^5+-trial registries should set `keep_sketches = false`. The `keep_sketches` field comment is corrected: the old text said "false keeps memory O(d²)". I did not change the default: `accounting()` is the E-01 path and needs the sketches. New test `EvalRegistryWindows_Memory.LeanRegistryKeepsTheSummaryBitForBit` shows that a lean registry keeps `summary()` (n_eff, n_eff_uncorrected, var_sr, registry_hash), the chain head and `trials()` bit-for-bit. Only `correlation` / `accounting` refuse. Ledger candidate 4 and an integration note for the bench owner are below. | `[w0e0b] per-trial retained bytes: TrialInfo=72, sketch=8*d (d=64 -> 512); 10^6 trials at d=64 ~ 0.58 GB with sketches, 0.07 GB without` (sketch and TrialInfo bytes only; the dedup set adds about 16-24 B per trial) |
| `trial_clusters.hpp:70`: the ONC base-stage cap `kOncDefaultMaxK = 64` can under-count N under `ClusterV2` | The cap is documented at `kOncDefaultMaxK`, in the `ClusterV2` rule text in `deflated_sharpe.hpp`, and on `TrialAccountingConfig::onc`. The docs say how to raise it (`onc.max_k`) and what it costs (linear in max_k). I did not raise the default: the base stage costs O(max_k · n_init · max_iter · n²), and the default rule's Monte-Carlo floor does not depend on the partition. Measuring the behaviour refined the reviewer's premise. With a cap just below G, the base stage under-counts (N = cap). With a cap far below G, no capped partition passes `min_silhouette`, so ONC returns singletons (N = n_raw, the conservative direction). An under-count can move SR*_cluster either way, because V_c changes too. The docs say exactly this. New test `EvalTrialClusters_Dsr.BaseStageCapBoundsClusterCountUntilRaised` uses G = 8 blocks, n = 79, scaled down so it fits the debug preset. It pins every case, and asserts default SR* ≥ max(SR*_mc, SR*_cluster) for every partition. | `max_k=7 depth=0 -> N=7`; `max_k=7 depth=2 -> N=8`; `max_k=4 depth=0/2 -> N=79 (singletons)`; `max_k=16 depth=0 -> N=8`; SR*_mc = 0.14135, and default SR* ≥ SR*_mc in every case |

### Fix pass 1 evidence

Every call ran from `C:\atx-wt\pool-6` with `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'`. Free RAM before
the builds was 3.80, 5.85, 5.45 and 5.78 GB.

The first run of the new cap test failed: it assumed that cap 4 gives N ≤ 4 and that ClusterV2
under-deflates. The measured result was singletons, N = 79. A diagnostic sweep over caps 4-7 with
depth 0 and 2 established the behaviour in the table above. I then rewrote the test to assert the
measured behaviour and corrected the docs. The final results, on the final tree:

1. `scripts\atx-build.ps1 build -Preset equity-dev atx-engine-eval-tests` → exit 0
   (`[20/21] Linking CXX executable bin\atx-engine-eval-tests.exe`), clean under /W4 /WX.
2. `scripts\atx-build.ps1 check -Preset equity-dev atx-impl\src\stage_equity_mine.cpp` → exit 0
   (a consumer of the edited headers).
3. Anchored suites (`-Ctest -Preset equity-dev -R ...`):
   ```
   ^EvalTrialClusters   100% tests passed, 0 tests failed out of 14   exit=0
   ^EvalRegistryWindows 100% tests passed, 0 tests failed out of 10   exit=0
   ^EvalLockboxEmbargo  100% tests passed, 0 tests failed out of 4    exit=0
   ^EvalTrialRegistry   100% tests passed, 0 tests failed out of 13   exit=0
   ```
4. Whole owning executable, `build-equity\bin\atx-engine-eval-tests.exe --gtest_brief=1` → exit 0:
   ```
   [w0e0b] equicorrelated null rho=0.5 N=2000 (n_eff=3.77), 12000 experiments: FPR MonteCarloMaxV2=0.0545, summary NEffCrossVarV1 (pre-W0)=0.5248, summary RawNCrossVarV2=0.0123
   [w0e0b] same null: FPR bound of the default ClusterMcFloorV2 (PSR at SR*_mc)=0.0141
   [==========] 223 tests from 39 test suites ran. (40440 ms total)
   [  PASSED  ] 223 tests.
   ```
   The count went from 221 to 223 because of the two new tests. The `CHECK failed` lines come from
   pre-existing death tests.

### Fix pass 1 integration notes (additions)

- **W1-I1 and any selection gate at α.** Gate with
  `deflated_sharpe(sr, acct, T, skew, kurt, AccountingDsrRule::MonteCarloMaxV2).result.dsr > 1 − α`.
  It is the only calibrated rule: 5.45% at α = 5%. The default `ClusterMcFloorV2` is conservative,
  at ≤ 1.41% on the same null. Keep it for ranking and for `haircut_sharpe` / SR* reporting. The
  earlier note "DSR everywhere through the default overload" applies to reporting, not to the
  α-gate. If the owner reads acceptance 1 as applying to the default rule, it is UNMET for the
  default: that rule is conservative by construction, and only the owner can decide.
- **Owner of `atx-engine/bench/eval_multiple_testing_bench.cpp`** (not owned by this lane): the
  10^6-trial registry there (d = 64) now retains about 0.6 GB by default. Set
  `TrialRegistryConfig::keep_sketches = false`. It uses only `summary()`, which a lean registry
  keeps unchanged.
- **Registries that may hold more than 64 genuine families**, if they use `ClusterV2` alone: set
  `TrialAccountingConfig::onc.max_k` to at least the expected family count.

### Ledger candidates (additions)

4. By default the TrialRegistry now keeps 8·d + 72 B per trial for `accounting()`: about 0.6 GB for
   10^6 trials at d = 64. Bulk or bench registries set `keep_sketches = false`, which is lossless
   for `summary()`.
5. On the ρ = 0.5, N = 2000 null, the PSR-based DSR rules are conservative: the default
   accounting rule has FPR ≤ 1.41%. Only `MonteCarloMaxV2` (the null CDF of the max) is calibrated,
   at 5.45%. An α-gate must use it.

## Post-merge sync (2026-09-25)

Final sync before orchestrator merge. Note: a stale MERGE_HEAD had been cleared earlier with
`merge --quit`; the post-merge suites had never been run on that head (35198851), so this sync
merges again (integration had moved) and runs the full suite set below.

- Pre-sync head: `35198851f9c139c0f439b4bf2fb4f4055b1fd7ae` (was already a merge of
  `feat/w0-integration`, but integration had advanced two more commits since).
- `git -C C:\atx-wt\pool-6 status --porcelain` -> empty; no `MERGE_HEAD` present.
- `git -C C:\atx-wt\pool-6 merge-base --is-ancestor feat/w0-integration HEAD` -> failed (not an
  ancestor), so proceeded to merge.
- `git -C C:\atx-wt\pool-6 merge --no-ff feat/w0-integration -m "w0-e0b: merge feat/w0-integration" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"`
  -> exit 0, merge made by the 'ort' strategy, no conflicts. Only orchestration/docs files
  changed (`.superpowers/sdd/aes-wave-workflow.js`, `.superpowers/sdd/w0/aes-w0a-workflow.js`,
  `.superpowers/sdd/w0/g0-runbook.md`, `.superpowers/sdd/w0/gen_briefs.py`,
  `.superpowers/sdd/w0/progress.md`, `.superpowers/sdd/w0/w0b-integration-notes.md`,
  `docs/superpowers/handoffs/2026-09-24-alpha-engine-w0-handoff.md`) — none in this lane's owned
  scope (`atx-engine/include/atx/engine/eval/*`, `atx-engine/src/eval/trial_registry.cpp`).
- Merge commit sha: `76b9d135e64c349aba4a6abc14172e87dbe5714a`.
- Rebuild: `Set-Location C:\atx-wt\pool-6; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-eval-tests`
  -> exit 0. Free RAM checked first: 2.35 GB (>= 2.0 GB gate, proceeded without waiting).
- Anchored suites (`-Ctest -Preset equity-dev -R '^<Suite>'`):
  - `EvalTrialClusters_` -> 14/14 passed (47.66 s).
  - `EvalRegistryWindows_` -> 10/10 passed (1.05 s).
  - `EvalLockboxEmbargo_` -> 4/4 passed (0.34 s).
- Whole owning executable: `.\build-equity\bin\atx-engine-eval-tests.exe --gtest_brief=1` ->
  `[==========] 223 tests from 39 test suites ran. (56626 ms total)` / `[  PASSED  ] 223 tests.`
  (same 223-test count as the last fix pass; no regressions from the merged integration docs).
- No code changes were required by the merge — merged content was orchestration/docs only, no
  owned source file was touched.
- Tree clean after sync; report committed with `git add -f`.
