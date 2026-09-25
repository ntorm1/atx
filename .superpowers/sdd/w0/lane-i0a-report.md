# Lane W0-I0a report: pipeline look-ahead fixes (discover, combine, optimize, metabook)

## Outcome

**DONE.** All three plan acceptance items are MET, and all eight cited defects are CLOSED.

- Branch: `feat/w0-i0a`. The final SHA is the commit that adds this report; see `git log`.
  The implementation is in `6c31e0fe` and the tests are in `38f64385`.
- Base: `458d0bef480a624e258070c9d45174a9984466bf`, the W0 base. The lane merged
  `feat/w0-integration` at `e488ac16` first.
- Pool: `C:\atx-wt\pool-10`. Run id: `aes-w0-i0a`. Preset: `equity-dev` (`build-equity\`).
  The test groups were unchanged, so no reconfigure was needed.

## Files changed

The only src files touched are owned ones:

- `atx-impl/src/dead_alpha_wire.hpp` gains:
  - the `DeadAlphaRule` enum;
  - the per-date dead set (`collect_dead_or_decaying_ids`, `dead_set_at`, `library_period_axis`,
    `library_as_of`);
  - the split-range ledger (`SplitRange`, `read_split_ranges`, `append_split_range`,
    `split_ranges_overlap`).
- `atx-impl/src/diag_risk.hpp` gains:
  - the `DiagRiskRule` and `ParticipationAdvRule` enums;
  - `diagonal_risk_models_expanding`, which fits per-step PIT diagonal models in one Welford
    pass;
  - `trailing_participation_reference`;
  - `DeployPitConfig`;
  - a 3-argument `run_optimize` overload.
- `atx-impl/src/stage_optimize.cpp`:
  - fits the risk models per step and uses the per-step dead set;
  - refreshes the participation reference at each rebalance through a per-step loop that
    mirrors `MultiPeriodOptimizer::run`;
  - in GP position mode, uses the per-step models.
- `atx-impl/src/stage_metabook.{hpp,cpp}`:
  - adds `SleeveSignalRule`, so sleeve signals come from the combo's fitted weights (read from
    the `weights` sidecar);
  - uses per-step diagonal models and the per-step dead set.
- `atx-impl/src/stage_combine.{hpp,cpp}` adds:
  - the enums `ConvictionWindowRule`, `CapacityRule`, `WalkForwardRule` and
    `HoldoutGuardRule`;
  - `CombinePitConfig`, `NestedSplitConfig`/`NestedSplit` and `resolve_nested_split`;
  - a shared `fit_shipped_weights` dispatch, used by the shipped weights and by walk-forward;
  - trailing PIT liquidity panels and trade-based capacity;
  - a final-test guard and ledger record;
  - weights written to the sidecar at 17 significant digits.
- `atx-impl/src/stage_discover.cpp` adds `run_discover_window(cfg, discover_end)`, which slices
  the panel to `[0, discover_end)`. It also adds the split-range guard and record, which refuses
  a recorded final test or a shifted holdout.
- `atx-impl/src/stage_run.cpp`: `run_all` now uses nested splits. Discover gets `[0, D)`, combine
  fits on `[F0, F1)`, and the final test is `[T0, n)`, with an embargo between the windows.
- `atx-impl/src/stage_report.cpp`: only the I-04 call site near line 499.

New tests are all in `atx-impl/tests/`:

- `w0i0a_fixtures.hpp`
- `w0i0a_nested_splits_test.cpp`
- `w0i0a_combine_no_holdout_read_test.cpp`
- `w0i0a_optimize_pit_test.cpp`
- `w0i0a_dead_alpha_test.cpp`
- `w0i0a_metabook_uses_combo_test.cpp`

Existing tests were changed only where they pinned a cited defect:

| Test | Defect | Change |
|---|---|---|
| `sign_deploy_test.cpp` (SignDeploy digest pin) | I-04 | The pin moves to the per-step PIT digest. A new check shows that `DiagRiskRule::WholePanelV1` still reproduces the old digest exactly. |
| `stage_optimize_dead_alpha_wire_test.cpp` and `stage_metabook_dead_alpha_wire_test.cpp` | I-06 | The fixture library used to mark its alphas *admitted*, which V1 wrongly called "dead". It now drives them Live→Decaying→Dead in the lifecycle journal and records a discover-holdout range, so the library axis is known. The crowding assertions are unchanged. |
| `metabook_test.cpp` `MultiSleeveByCorrCluster` | I-07 | This test pins the equal-weight sleeve signal, so it now selects `SleeveSignalRule::EqualWeightV1` explicitly. |
| `stage_run_synthetic_smoke_test.cpp` and `e2e_pipeline_test.cpp` (`run_staged` helpers) | I-01 | The staged helpers used the old same-window discover/combine wiring. The new combine guard refuses that wiring by design. The helpers now mirror `run_all`'s nested split; the assertions are unchanged. |

## Acceptance table

| Plan acceptance item | Test(s) | Measured | Status |
|---|---|---|---|
| Mutating holdout PnL leaves the shipped weights byte-identical | `ImplCombineNoHoldoutRead.ShippedWeightsIgnoreHoldoutMutation`, `ImplNestedSplits.FinalTestMutationLeavesShippedWeightsIdentical` | Under V2 the `weights.txt` sidecar (17 significant digits) is byte-identical when every row from the final test on is mutated. The V1 rules (full-stream conviction, full-period capacity) move the weights by up to 0.0632524 (`LegacyRulesReadTheHoldout`; post-merge, fix pass 1 run). | MET |
| Mutating future volumes or prices leaves past books byte-identical | `ImplOptimizePit.FutureMutationLeavesPastBooksIdentical`, `ImplMetabookUsesCombo.FutureMutationLeavesPastBooksIdentical`, `ImplOptimizePit.ParticipationReferenceIsTrailingPit`, `ImplNestedSplits.DiscoverWindowIgnoresLaterRows` | Past optimize books identical under mutation of rows 66+, V2 vs V1: mvo-diagonal 14/14 vs 0/14; mvo-participation-cap 14/14 vs 0/14 (V1 there is `WholePanelV1` + `LastDateV1`; the cap does not bind at NAV 3e5); **binding** participation cap (NAV 2e6, 13 past cells on their %ADV box, diagonal lens held at `PerStepPitV2`): `TrailingPitPerRebalanceV2` 14/14 vs `LastDateV1` 0/14 (`ImplOptimizePit.BindingParticipationCapIsPitPerRebalance`, fix pass 1); position-mode-gp 14/14 vs 0/14; mvo-factor 14/14 vs 14/14. Metabook past rows identical: V2 16/16, V1 0/16, with both 1 and 3 sleeves. The participation reference at every date d < 66 is bit-identical under the mutation. The discover prefix `[0,96)` gives the same `factory_digest` (`78bca49224a4678f` post-merge; `108d8eff5befc20a` pre-merge; 21 admitted) when rows 96+ are mutated. | MET |
| Walk-forward with `--method stack` runs | `ImplCombineNoHoldoutRead.WalkForwardStackRuns`, `ImplCombineNoHoldoutRead.WalkForwardFoldsStayInsideTheFitWindow` | The stack walk-forward runs: 2 folds, embargo 2 (h=1 + delay 1), OOS Sharpes 0.663941 and 2.413094 (mean 1.538518; post-merge, fix pass 1 run). Every fold's train and test windows lie inside `[fit_begin, fit_end)`, and each test begins at least one embargo after its train window. | MET |

## Defect table

| ID | Status | How, and where |
|---|---|---|
| I-01 | CLOSED | **Nested splits.** `resolve_nested_split` (`stage_combine.cpp`) splits the timeline into discover `[0,D)`, fit `[F0,F1)` and final test `[T0,n)`, with an embargo of 1 + execution delay between the windows. `run_all` (`stage_run.cpp`) wires discover through `run_discover_window`, which slices the panel before mining.<br>**Split ledger.** Split ranges are recorded in the library's `_split_ranges.txt` (`dead_alpha_wire.hpp`). Combine refuses a final test that overlaps any discover range. Discover refuses a panel window that overlaps a recorded final test, and refuses a holdout at a different place.<br>**Tests.** `ImplNestedSplits.*` (7 tests), including `LegacySameWindowIsRefused`, which rejects with the message "combine: final test [150,200) overlaps the discover_holdout range [150,200)". |
| I-02 | CLOSED | `ConvictionWindowRule::FitWindowV2` (the default) scores DSR and stability over the fit window only. This is done inside `fit_shipped_weights` (`stage_combine.cpp`). `FullStreamV1` reproduces the old behaviour. |
| I-03 | CLOSED | `CapacityRule::TrailingPitTradesV2` computes:<br>• edge = mean PnL over the fit window;<br>• cost from \|Δw\| trades, not holdings;<br>• a trailing 20-day dollar ADV and 60-return volatility per date, PIT (`build_liquidity_panels`, `alpha_capacity_trades`).<br>Test: `CapacityChargesTradesNotHoldings`. For (rank(size), delta(close,1)), V2 gives inf and 17800.0 while V1 gives 387899.1 and 4886.6. The low-turnover alpha is no longer penalized. |
| I-04 | CLOSED | `DiagRiskRule::PerStepPitV2` fits one expanding PIT diagonal model per rebalance (`diagonal_risk_models_expanding`, `diag_risk.hpp`). It is used in optimize (MVO and GP) and in metabook. The report's call site uses a 1-row fit; its V is never read, so its output is unchanged. `ImplOptimizePit.ExpandingDiagonalMatchesTwoPass` gives a maximum relative difference of 7.42e-16. |
| I-06 | CLOSED | `DeadAlphaRule::DeadOrDecayingPerStepV2`: an alpha is "dead" when the lifecycle journal says it was Dead or Decaying as of each step's date, mapped onto the library's period axis. If the axis is unrecorded, the rule fails open and returns an empty set. Tests `ImplDeadAlpha.*`:<br>• `DeadSetFollowsTheLifecycleAsOfEachDate`: V2 gives {}, {}, {}, {1}, {0,1}, …, while V1 gives {0,1,2} on every date.<br>• `CrowdingStartsWhenTheAlphasDie`: under V2 the centre weight stays 0.100000 until the step at date 15 and is 0.090216 from then on. Under V1 it is 0.090216 from step 0. |
| I-07 | CLOSED | `SleeveSignalRule::ComboWeightsV2`: a sleeve signal is Σ w_j·pos_j, using the combo's fitted weights (read from `<combo>.weights.txt` and matched by DSL sha256). It uses the same weight policy and sector map as combine. A member with no fitted weight is refused. Tests `ImplMetabookUsesCombo.*`:<br>• With one sleeve, the metabook reproduces the combo book: 24/24 rows identical, against a V1 gap of 0.283872 (post-merge).<br>• With 3 sleeves, the gap between the shrinkage-mv and ic combos is 0.0420907 under V2 (post-merge) and 0 under V1.<br>• Fix pass 1: a library holding the same DSL twice shares the combo's weight on that DSL between the duplicates (`member_combo_weights`); one sleeve still reproduces the combo 24/24 (`DuplicateDslMembersSplitTheirComboWeight`). |
| I-08 | CLOSED | `WalkForwardRule::ShippedFitEmbargoedV2`: the walk-forward folds run inside the fit window, through the same `fit_shipped_weights` dispatch as the shipped weights: stack, the cleaned-cov methods, and every AlphaCombiner method. They use an embargo of h + execution delay, and the rule errors when a segment is too short. Tests: `WalkForwardStackRuns`, `WalkForwardFoldsStayInsideTheFitWindow`. |
| R-12 | CLOSED | `ParticipationAdvRule::TrailingPitPerRebalanceV2` refreshes the reference at each rebalance to the mean volume over `[d-19,d]` and the close at d (`trailing_participation_reference`, `diag_risk.hpp`). The optimizer loop is per step (`run_multi_period_step_ref`).<br>**Tests.**<br>• `ParticipationReferenceIsTrailingPit`: the reference is PIT. The delisted name 11 has adv=1384831 and px=70.69 at d=59, and cap 0 after delisting. V1 gave it adv=0 and px=0 for its whole history.<br>• `PerStepLoopMirrorsTheDriver`: when the reference is static, the V2 digest equals the V1 digest.<br>• `DelistedNameKeepsItsCapWhileListed`: the delisted name holds weight while listed and none after delisting. |

## Evidence

### Build

The build ran with `CMAKE_BUILD_PARALLEL_LEVEL=2`; free RAM was 4.04 GB when it was checked.

```
Set-Location C:\atx-wt\pool-10; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker
[9/11] Linking CXX executable bin\atx-shm-worker.exe
[10/11] Linking CXX executable bin\atx-impl-tests.exe
exit=0
```

### Anchored suites

Each suite was run with the command below; only the suite name changes:

```
Set-Location C:\atx-wt\pool-10; powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'
```

The results are as follows:

```
=== ImplNestedSplits          100% tests passed, 0 tests failed out of 7   exit=0
=== ImplCombineNoHoldoutRead  100% tests passed, 0 tests failed out of 5   exit=0
=== ImplOptimizePit           100% tests passed, 0 tests failed out of 5   exit=0
=== ImplDeadAlpha             100% tests passed, 0 tests failed out of 3   exit=0
=== ImplMetabookUsesCombo     100% tests passed, 0 tests failed out of 4   exit=0
```

In total, 24 of the 24 new tests pass.

### Whole owning executable (run once)

```
Set-Location C:\atx-wt\pool-10; .\build-equity\bin\atx-impl-tests.exe --gtest_brief=1
..\atx-impl\tests\trial_ledger_test.cpp(634): error: Value of: head.has_value()
ParseError: trial ledger: CR in line 0 (LF endings only)
[  FAILED  ] TrialLedgerRepository.ExistingCp14Ledger_StillVerifies (3 ms)
[==========] 557 tests from 106 test suites ran. (368587 ms total)
[  PASSED  ] 551 tests.
[  SKIPPED ] 5 tests.
exit=1
```

The single failure is **pre-existing and environmental**. It is the same one lane O1's report
documents: `core.autocrlf=true` checks out `atx-engine/reviews/trial-ledger.jsonl` as CRLF
(`git ls-files --eol` gives `i/lf w/crlf attr/`), and this failure only happens when the exe runs
from the repository root. Run from the ctest working directory, the test skips:

```
Set-Location C:\atx-wt\pool-10\build-equity\atx-impl\tests; ..\..\bin\atx-impl-tests.exe --gtest_brief=1 --gtest_filter='TrialLedgerRepository.ExistingCp14Ledger_StillVerifies'
[  PASSED  ] 0 tests.
[  SKIPPED ] 1 test.
exit=0
```

Every other test passes. This includes the existing tests that were changed (`SignDeploy`,
both dead-alpha wire suites, `MetaBook`, `StageRunSyntheticSmoke` 3/3 and the E2E pipeline) and
all 24 new tests.

### Verbatim measured output (from the whole run, PRE-MERGE: first implementation run)

The numbers in this block and the paragraph after it are from before the `feat/w0-integration`
merge. The merged kernels move several of them; the post-merge values are in the tables above
and in "Fix pass 1" below.

```
[W0-I0a I-06] date 22: V2 dead={} (as_of 2)  V1 'dead'={0,1,2} (as_of 9)
[W0-I0a I-06] date 23: V2 dead={1} (as_of 3)  V1 'dead'={0,1,2} (as_of 9)
[W0-I0a I-06] date 25: V2 dead={0,1} (as_of 5)  V1 'dead'={0,1,2} (as_of 9)
[W0-I0a I-06] step 2 (date 10): |w_center| off=0.100000 V2=0.100000 V1=0.090216
[W0-I0a I-06] step 3 (date 15): |w_center| off=0.100000 V2=0.090216 V1=0.090216
[W0-I0a I-07] SingleSleeve over the library: V2 rows identical to the combo book 24/24; V1 equal-weight max |dw| vs combo book = 0.347627
[W0-I0a I-07] 3 sleeves: max |dw| between shrinkage-mv and ic combos: V2=0.0460013 V1=0
[W0-I0a I-04] metabook 1 sleeve: past rows identical under future mutation V2 16/16, V1 0/16
[W0-I0a I-04] metabook 3 sleeves: past rows identical under future mutation V2 16/16, V1 0/16
[W0-I0a I-01] legacy wiring refused: combine: final test [150,200) overlaps the discover_holdout range [150,200) recorded in the library -- nested splits require discover < fit < test (the alphas were selected on those dates)
[W0-I0a I-01] ledger discover_train [0,72) of 200
[W0-I0a I-01] ledger discover_holdout [72,96) of 200
[W0-I0a I-01] ledger final_test [150,200) of 200
[W0-I0a I-01] discover prefix [0,96): admitted=21 factory_digest=108d8eff5befc20a (identical with rows >= 96 mutated)
[W0-I0a I-04] expanding vs two-pass diagonal: max relative diff = 7.42e-16
[W0-I0a I-04/R-12] mvo-diagonal           past steps identical under future mutation: V2 14/14, V1 0/14
[W0-I0a I-04/R-12] mvo-participation-cap  past steps identical under future mutation: V2 14/14, V1 0/14
[W0-I0a I-04/R-12] position-mode-gp       past steps identical under future mutation: V2 14/14, V1 0/14
[W0-I0a I-04/R-12] mvo-factor             past steps identical under future mutation: V2 14/14, V1 14/14
[W0-I0a R-12] delisted name 11: V2 reference at d=59 adv=1384831 px=70.69; V1 (last-date) reference adv=0 px=0.00 for every rebalance
[W0-I0a R-12] delisted name 11: sum|w| while listed V2=1.000000 V1=1.000000; after delisting V2=2.04e-08
```

The combine lines (holdout-mutation weights, V1 max |dw| 0.0720029, stack walk-forward with 2
folds, embargo 2 and OOS Sharpes 0.684542 / 2.212081, capacity V2 inf / 17800.003404 against V1
387899.144436 / 4886.626736, max_part V2 0.539954 against V1 0.515169) appear in the same log
under `[W0-I0a I-02/I-03/I-08]`.

The factor case shows V1 14/14 as well. This is expected: the factor model was already
per-step before W0, and V2 only changes which alphas count as dead there.

## Golden digests (old → new)

| Pin | Old | New | Defect | V1 reproduces old? |
|---|---|---|---|---|
| `SignDeploy` optimize digest (`sign_deploy_test.cpp`) | 5744281451106956152 | 4943992197170640678 | I-04 | Yes: `DeployPitConfig{diag = WholePanelV1}` gives 5744281451106956152 (asserted). |

`run_all` and staged-pipeline OOS numbers change by design under I-01, because the OOS window is
now a true final test that discover never saw. Those tests assert structure, not digests, so no
other pin moved. The combo `weights.txt` sidecar is now written at `setprecision(17)`. This is a
format change, not a digest.

## Deviations from brief

1. **Where the ledger lives.** The split-range ledger is a sidecar file
   (`<library>/_split_ranges.txt`) and is implemented in the owned `dead_alpha_wire.hpp`. The
   `Library` class is in atx-engine, which this lane does not own. The shared PIT helpers live in
   the owned `diag_risk.hpp`. No new src `.cpp` or header was added.
2. **Test fixture header.** A test-only header, `atx-impl/tests/w0i0a_fixtures.hpp`, is shared by
   the five new test files. It is not a `_test.cpp`, but it is test-only and lane-prefixed.
3. **Participation cap and the QP solver.** When participation caps bind tightly,
   `ConstrainedQpSolver`'s augmented path hits its iteration budget under both V1 and V2. This is
   a pre-existing solver limit, not caused by this lane. The end-to-end participation tests use a
   cap the solver converges on (0.05 at a NAV of 3e5). R-12 PIT-ness is proven directly on the
   reference function (`ParticipationReferenceIsTrailingPit`) and end to end
   (`FutureMutationLeavesPastBooksIdentical`, participation case: V2 14/14 vs V1 0/14).
4. **Delisted name under V1.** On this fixture the V1 last-date NaN price does not bind the
   delisted name's cap, so its V1 weight while listed is printed but not asserted to be lower. The
   V1 reference being zero is asserted on the reference function itself.
5. **Report call site (I-04).** The stage_report V matrix is only used with zero exposures, so the
   PIT fix (a 1-row fit) does not change report output.
6. **Embargo length.** The `run_all` nested-split embargo is 1 + `replay_execution_delay`, which
   is 2 dates by default. The combine walk-forward embargo is `stack_horizon` (or 1) plus the
   execution delay.

## Integration notes

- **I0b / W1-I1 (config and CLI).** New stage-private knobs need config and CLI plumbing:
  - `CombinePitConfig` rules, `test_begin` and `execution_delay`;
  - `DeployPitConfig` (diag, dead, participation rules);
  - `MetaBookStageConfig::sleeve_signal`;
  - `NestedSplitConfig::{test_frac, combine_frac, embargo}`.

  Today, `run_all` derives them from `holdout-frac` and `replay_execution_delay`. Nothing in
  `config.*` or `dispatch.*` was edited.
- **B0 (report).** The report's in-sample region now includes the `[fit_end, test_begin)`
  embargo gap. The OOS region begins at `combo.meta holdout_begin` = `test_begin`.
- **Lifecycle driver.** There is no production writer of Decaying/Dead transitions yet. Until one
  lands, the V2 dead set is empty in production, so crowding augmentation is off. This is
  correct, not inverted.
- **Track A (library).** The split-range sidecar could move into `Library` as first-class
  metadata.
- **R track.** The `ConstrainedQpSolver` augmented path does not converge on tight binding
  participation caps (pre-existing).
- **Metabook input.** Under V2 the metabook requires the combo `weights` sidecar, which combine
  already writes.
- **G0.** `run_all` headline OOS numbers change: they are now a true final test.
- **Pre-existing failure.** `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` fails
  because of CRLF, which the W0 integration notes already record. It is not in scope.

## Ledger candidates

- W0-I0a: `run_all` nested split is discover `[0,D)` < fit `[F0,F1)` < final test `[T0,n)`, with
  a 1+delay embargo. The split ranges live in `<library>/_split_ranges.txt`, and overlap is
  refused.
- W0-I0a: the expanding Welford diagonal risk (one pass for all rebalances) matches two-pass
  within a 7.42e-16 relative difference.
- W0-I0a: `ConstrainedQpSolver`'s augmented path hits its iteration budget on tightly binding
  participation caps, with either reference rule. Use cap 0.05 at a NAV of 3e5 on synthetic
  fixtures.

## Post-merge sync

- Merge: `git merge --no-ff feat/w0-integration` — clean (ORT strategy), no conflicts. The merge
  touched only `atx-engine/{include,src,tests}/**` (alpha/factory/eval/combine kernels from lanes
  A0/E0a) and `.superpowers/sdd/w0/*` progress/report files; no file in this lane's owned scope
  (`atx-impl/src/stage_discover.cpp`, `stage_run.cpp`, `stage_combine.{hpp,cpp}`,
  `stage_optimize.cpp`, `stage_metabook.{hpp,cpp}`, `dead_alpha_wire.hpp`, `diag_risk.hpp`,
  `stage_report.cpp`) was touched.
- Head after merge: `2ec4e7a1e2a2c031f0db6df04eb9893a6a440f56`.
- Build: `Set-Location C:\atx-wt\pool-10; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker`
  — exit 0 (134/134; reconfigure ran because of a glob mismatch, no `CMakeLists.txt` edits made).
- Anchored suites (`-Ctest -Preset equity-dev -R '^<Suite>'`), all green:
  - `ImplNestedSplits` 7/7 passed (10.76s)
  - `ImplCombineNoHoldoutRead` 5/5 passed (11.38s)
  - `ImplOptimizePit` 5/5 passed (12.74s)
  - `ImplDeadAlpha` 3/3 passed (1.13s)
  - `ImplMetabookUsesCombo` 4/4 passed (2.31s)
  - Total: 24/24 lane-suite tests passed.
- Whole owning executable (`build-equity\bin\atx-impl-tests.exe --gtest_brief=1`, `atx-shm-worker`
  already built alongside): exit 1. 557 tests ran, 548 passed, 5 skipped, **4 failed**:
  - `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` — pre-existing CRLF issue, already
    recorded above as not in scope.
  - `StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput` — NEW after this
    merge. `stage_equity_ic_test.cpp` is not in this lane's owned scope.
  - `EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials` — NEW after this merge (`n_raw` 9 vs
    expected 12). `stage_equity_mine_cli_test.cpp` is not in this lane's owned scope; the test's
    expectation was itself re-pinned by `a28a6c1e` ("w0-i0b: re-pin mine smooth-window trial
    count after A0 average rank ties (A-01)"), a commit that reached this tree only through this
    merge, and it is still red against the current `atx-engine` kernels.
  - `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` — NEW after this merge (`acc_ts produced
    no finite cell`). `fundamental_zoo_test.cpp` is not in this lane's owned scope; the failure
    traces to `atx-engine/include/atx/engine/alpha/ts_ops.hpp`, which the merge changed by +167/-
    lines (lane A0).
  - Verified these 3 are new, not pre-existing: `a28a6c1e` (which touches the equity-mine test's
    expectation) does not merge-base-ancestor this lane's pre-merge head
    (`e488ac16`), so it — and the `ts_ops.hpp`/`cross_section_ic.cpp` kernel changes feeding the
    other two — arrived only via `feat/w0-integration` in this sync.
- Disposition: all 3 new failures are outside this lane's owned-files scope (RULES §2 "Owned
  files only"; brief "Files forbidden: everything else"), so per RULES §3 they are reported, not
  fixed here. Lane suites (this lane's actual acceptance surface) stay 24/24 green; the whole-
  executable "must stay green" bar is not met because of out-of-scope regressions from lanes
  A0/E0a/I0b landing on `feat/w0-integration`.

## Fix pass 1

Addresses the four minor findings in `lane-i0a-review.md` (verdict APPROVE, no blocker or major).
Files changed: `atx-impl/src/dead_alpha_wire.hpp`, `atx-impl/src/stage_optimize.cpp`,
`atx-impl/src/stage_metabook.cpp` (all owned), `atx-impl/tests/w0i0a_optimize_pit_test.cpp`,
`atx-impl/tests/w0i0a_dead_alpha_test.cpp`, `atx-impl/tests/w0i0a_metabook_uses_combo_test.cpp`
(lane tests), this report. No existing assertion was weakened; the only edit to an existing test
is the two `library_period_axis` call sites in `w0i0a_dead_alpha_test.cpp`, which now pass the
deploy panel (60 dates, unkeyed: the recorded panel) because the signature gained it.

### Finding 1 (participation cap never binds end to end) -- FIXED

New test `ImplOptimizePit.BindingParticipationCapIsPitPerRebalance`. At NAV 2e6 the %ADV box
0.05·ADV·px/NAV is about 0.125 for the least liquid name, below `name_cap` 0.5 and below the
uncapped weights (up to 0.22). The diagonal lens is held at `PerStepPitV2` in both runs, so only
`ParticipationAdvRule` differs. Binding is shown two ways: past (step, name) cells sitting on
their per-rebalance box, and past books that differ from the same augmented QP run with a slack
cap (NAV 3e5).

```
[W0-I0a R-12] binding cap (NAV 2e+06): 13 past (step,name) cells on their %ADV box, 14/14 past steps differ from the slack-cap run; past books identical under future mutation: TrailingPitPerRebalanceV2 14/14, LastDateV1 0/14 (diag PerStepPitV2 in both)
```

So the per-rebalance reference alone keeps past books identical, and `LastDateV1` alone carries
the future into all 14 past books. Solver note (pre-existing, recorded, not fixed here): a
one-off NAV x cap sweep (not committed) showed that `ConstrainedQpSolver`'s augmented path
does not converge at many binding points, including NAV 5e5, 6e5, 1e6, 1.3e6, 1.6e6 and 3e6
at cap 0.05. Convergence is not monotone in NAV; 8e5 at cap 0.02 and 2e6 at cap 0.05 converge
for all four runs. The test uses 2e6/0.05. This refines the earlier ledger candidate about the
solver.

### Finding 2 (library period axis placed by raw index) -- FIXED

`library_period_axis(lib_dir, lib, deploy_n_dates, deploy_session_keys)` now places the recorded
holdout on the deploy panel. `stage_optimize.cpp` and `stage_metabook.cpp` pass `D` and the
research panel's session keys (empty for an unidentified panel).

- When both sides are session-keyed, the recorded `begin_key` must be a deploy key at index b,
  and the deploy key at b+len-1 must equal the recorded `last_key`. The holdout is then re-mapped
  to b, so a panel with a different start gets the correct dates.
- When both sides are unkeyed, the deploy panel must have the recorded `n_dates`.
- In every other case the axis is unknown and the dead set is empty (fail open): mixed axes, a
  key axis whose length is not `deploy_n_dates`, a missing first session, or a different session
  count inside the holdout.

New test `ImplDeadAlpha.DeployPanelMustMatchTheRecordedHoldout`:

```
[W0-I0a I-06] deploy panel starting 5 sessions earlier: key-mapped dead sets match 65/65 dates; raw-index placement reads the future on 5 dates
```

The test also asserts each fail-open case above. The existing wire tests
(`AtxImplOptimizeDeadAlphaWire`, `MetabookDeadAlphaWire`) record index-axis holdouts on their
own panel length, so their axis is still known and they still pass (whole run below).

### Finding 3 (duplicate-DSL members double-counted) -- FIXED

New `member_combo_weights` in `stage_metabook.cpp`. The combo weight per DSL sha, W(sha), is
still the sum over the combo's lines, which is the combo's total exposure to that expression.
Each of the k library members with that sha now carries W(sha)/k, so the members add back to
W(sha) in whichever sleeves they land. The multiplicity is computed once over the whole library.
`combo_weighted_sleeve_signal` takes the per-AlphaId weights, and a member with no weight is
still refused with the same message.

New test `ImplMetabookUsesCombo.DuplicateDslMembersSplitTheirComboWeight`: the library holds
`rank(close)` twice, the combo is fitted from the library with the equal method, and the combo's
two lines share one sha.

- Fixed code: 24/24 rows identical to the combo book.
- Teeth check, with the pre-fix weighting (`out[a] = it->w`) temporarily restored, rebuilt and
  then reverted: 1/24 rows identical, and the test FAILED.

```
[W0-I0a I-07] duplicate-DSL library (w=0.25,0.25 on rank(close)): one sleeve rows identical to the combo book 24/24
```

### Finding 4 (stale pre-merge numbers) -- FIXED

The acceptance and defect tables now quote the post-merge values measured in this pass:

| Quantity | Post-merge value (was, pre-merge) |
|---|---|
| V1 leak | 0.0632524 (was 0.0720029) |
| Stack OOS Sharpes | 0.663941 / 2.413094, mean 1.538518 (was 0.684542 / 2.212081) |
| `factory_digest` | 78bca49224a4678f (was 108d8eff5befc20a) |
| I-07 V1 gap | 0.283872 (was 0.347627) |
| 3-sleeve V2 gap | 0.0420907 (was 0.0460013) |

Unchanged values: capacity V2 inf / 17800.003404 against V1 387899.144436 / 4886.626736, and
max_part V2 0.539954 against V1 0.515169. The verbatim block under Evidence is now labelled
PRE-MERGE. No MET/CLOSED status changed.

### Evidence (fix pass 1)

Every build ran with `CMAKE_BUILD_PARALLEL_LEVEL=2` after a free-RAM check (2.26 to 4.06 GB).
One exploratory build started at 1.61 GB, below the RULES §1 2 GB floor; it completed without
incident. The final build ran after the last source edit:

```
Set-Location C:\atx-wt\pool-10; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker
[21/22] Linking CXX executable bin\atx-impl-tests.exe
exit=0
```

Anchored suites (`-Ctest -Preset equity-dev -R '^<Suite>'`):

```
=== ImplNestedSplits          100% tests passed, 0 tests failed out of 7 exit=0
=== ImplCombineNoHoldoutRead  100% tests passed, 0 tests failed out of 5 exit=0
=== ImplOptimizePit           100% tests passed, 0 tests failed out of 6 exit=0
=== ImplDeadAlpha             100% tests passed, 0 tests failed out of 4 exit=0
=== ImplMetabookUsesCombo     100% tests passed, 0 tests failed out of 5 exit=0
```

In total 27 of the 27 lane tests pass: the 24 existing tests and the 3 new ones. The
`ImplDeadAlpha`, `ImplOptimizePit` and `ImplMetabookUsesCombo` suites were re-run after the
final build.

The whole owning executable was run from the ctest working directory:
`Set-Location C:\atx-wt\pool-10\build-equity\atx-impl\tests; ..\..\bin\atx-impl-tests.exe --gtest_brief=1`

```
[  FAILED  ] FundamentalZoo.FixtureParsesTypechecksAndEvaluates (403 ms)
[  FAILED  ] StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput (49487 ms)
[  FAILED  ] EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials (4382 ms)
[==========] 560 tests from 106 test suites ran. (500896 ms total)
[  PASSED  ] 551 tests.
[  SKIPPED ] 6 tests.
exit=1
```

The only failures are the 3 out-of-scope ones the orchestrator recorded: `FundamentalZoo` is
fixed by W0-FIXUP, and `StageEquityIc` and `EquityMineCli` are owned by I0b. Every other test
passes, including the `SignDeploy` pin, the two dead-alpha wire suites, `MetaBook`,
`StageRunMegabook`, `StageRunSyntheticSmoke` and the e2e pipeline test. That run was built
before the last edit, a comment-only change to `dead_alpha_wire.hpp`; the three suites that
depend on that header were re-run after the final build and pass.

Golden digests: none moved in this pass.

## Post-merge sync (final, before orchestrator merge)

Head before sync: `2a86af12` (re-review 1, APPROVE). Integration (`feat/w0-integration`) was
not yet an ancestor of the lane head (`merge-base --is-ancestor` exit 1), so the lane merged it.

```
git -C C:\atx-wt\pool-10 merge --no-ff feat/w0-integration -m "w0-i0a: merge feat/w0-integration" ...
Merge made by the 'ort' strategy. (auto-merged atx-impl/src/stage_report.cpp, no conflicts)
```

No files in this lane's owned scope conflicted; `stage_report.cpp` (the one file this lane
partially owns, only the I-04 diagonal-risk call site near line 499) auto-merged cleanly against
B0's changes to the rest of that file. Ledger refresh per instructions:

```
rm atx-engine/reviews/*.jsonl ; git checkout -- atx-engine/reviews
git status --porcelain -> clean
```

Head after merge: `69d5138c`.

Build (RAM check: 4.1 GB free before build, >= 2 GB floor; `CMAKE_BUILD_PARALLEL_LEVEL=2`):

```
Set-Location C:\atx-wt\pool-10; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker
[138/141] Linking CXX executable bin\atx-impl-tests.exe
exit=0
```

Anchored suites (`-Ctest -Preset equity-dev -R '^<Suite>'`):

```
=== ImplNestedSplits          100% tests passed, 0 tests failed out of 7  exit=0
=== ImplCombineNoHoldoutRead  100% tests passed, 0 tests failed out of 5  exit=0
=== ImplOptimizePit           100% tests passed, 0 tests failed out of 6  exit=0
=== ImplDeadAlpha             100% tests passed, 0 tests failed out of 4  exit=0
=== ImplMetabookUsesCombo     100% tests passed, 0 tests failed out of 5  exit=0
```

All 27 lane tests still pass post-merge.

Whole owning executable (`atx-impl-tests.exe --gtest_brief=1` from the ctest working directory):

```
[==========] 597 tests from 121 test suites ran. (432698 ms total)
[  PASSED  ] 591 tests.
[  SKIPPED ] 6 tests.
exit=0
```

Zero failures. Notably `FundamentalZoo.FixtureParsesTypechecksAndEvaluates`,
`StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput` and
`EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials` — the 3 out-of-scope failures recorded
before this sync (W0-FIXUP and I0b owned) — now pass, since `feat/w0-integration` already
carries those fixes. No new failures were introduced by the merge. Head at commit time:
`69d5138c8d06352cba0ad7252c29a3a5107b7ef2` (before this report commit).
