# Lane W0-I0a review

## Verdict

APPROVE

There are no blocker or major findings. Four minor findings are listed below. None of them
changes a shipped number on the paths the plan names.

## Reviewed SHA

`172038881264d02299f1031fb5cb5e4eb3451c67`, the lane head (`17203888`, post-merge sync). The
lane changes were read as `git diff feat/w0-integration...HEAD`: 23 files, +3796/-424.

## Evidence

Each command below was run independently by the reviewer in `C:\atx-wt\pool-10`, with
`CMAKE_BUILD_PARALLEL_LEVEL=2`. Free RAM was 2.46 GB when checked.

```
Set-Location C:\atx-wt\pool-10; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker
[9/11] Linking CXX executable bin\atx-shm-worker.exe
[10/11] Linking CXX executable bin\atx-impl-tests.exe
exit=0
```

The anchored suites were run with the command below; only the suite name changes:

```
powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'
=== ImplNestedSplits          100% tests passed, 0 tests failed out of 7   exit=0
=== ImplCombineNoHoldoutRead  100% tests passed, 0 tests failed out of 5   exit=0
=== ImplOptimizePit           100% tests passed, 0 tests failed out of 5   exit=0
=== ImplDeadAlpha             100% tests passed, 0 tests failed out of 3   exit=0
=== ImplMetabookUsesCombo     100% tests passed, 0 tests failed out of 4   exit=0
```

The whole owning executable was run once, from the ctest working directory:

```
Set-Location C:\atx-wt\pool-10\build-equity\atx-impl\tests; ..\..\bin\atx-impl-tests.exe --gtest_brief=1
[  FAILED  ] FundamentalZoo.FixtureParsesTypechecksAndEvaluates
[  FAILED  ] StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput
[  FAILED  ] EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials
[==========] 557 tests from 106 test suites ran. (447479 ms total)
[  PASSED  ] 548 tests.
[  SKIPPED ] 6 tests.
```

The whole executable exits with code 1 because of these 3 failures. All 3 are outside this
lane's scope, as the orchestrator notes:

- `FundamentalZoo` is fixed by W0-FIXUP.
- `StageEquityIc` and `EquityMineCli` are owned by I0b.

No other test fails. That includes every pre-existing test the lane touched:

- `SignDeploy`
- `AtxImplOptimizeDeadAlphaWire`
- `MetabookDeadAlphaWire`
- `MetabookCloseBattery`
- `StageRunSyntheticSmoke`
- the E2E pipeline test
- `StageRunMegabook`, which runs `run_all` end to end, so it covers the nested split, the
  ledger guard and the V2 metabook weights sidecar on the real stage graph.

These are the measured lines from this run, after the merge:

```
[W0-I0a V1 leak] max |dw| under holdout mutation = 0.0632524
[W0-I0a I-08] stack walk-forward folds=2 embargo=2 oos_sharpes=0.663941,2.413094 mean=1.538518
[W0-I0a I-03] capacity AUM (rank(size), delta(close,1)): V2 trades=inf,17800.003404  V1 holdings=387899.144436,4886.626736
[W0-I0a I-07] SingleSleeve over the library: V2 rows identical to the combo book 24/24; V1 equal-weight max |dw| vs combo book = 0.283872
[W0-I0a I-04] metabook 1 sleeve / 3 sleeves: past rows identical under future mutation V2 16/16, V1 0/16
[W0-I0a I-01] discover prefix [0,96): admitted=21 factory_digest=78bca49224a4678f (identical with rows >= 96 mutated)
[W0-I0a I-04] expanding vs two-pass diagonal: max relative diff = 7.42e-16
[W0-I0a I-04/R-12] mvo-diagonal / mvo-participation-cap / position-mode-gp: V2 14/14, V1 0/14; mvo-factor V2 14/14, V1 14/14
```

## Findings

| path:line | severity | problem | required fix |
|---|---|---|---|
| `atx-impl/tests/w0i0a_optimize_pit_test.cpp:138,215-226` | minor | In the end-to-end participation-cap tests (`FutureMutationLeavesPastBooksIdentical`, `DelistedNameKeepsItsCapWhileListed`), the cap never binds on this fixture. The cap is 0.05·ADV·px/3e5, which is ≈0.8 or more per name, above `name_cap` 0.5. The delisted name's listed weight is also identical under V1 and V2. So the "mvo-participation-cap V2 14/14" result does not exercise a binding per-rebalance reference. Its V1 0/14 comes from `WholePanelV1`, not from `LastDateV1`. R-12's point-in-time property is still proven, at the function level (`ParticipationReferenceIsTrailingPit`, bit-identical for every d<70) and in the wiring (`PerStepLoopMirrorsTheDriver`: if the aliased buffers were never refreshed, every cap would be 0 and the digests would differ). | Add a case where the cap binds (lower `report_aum` or ADV for a few names) and the diagonal model is held at V2. Then show that V2 past books are identical and `LastDateV1` past books differ. Or record in the report that the solver limit makes this infeasible. |
| `atx-impl/src/dead_alpha_wire.hpp:382-406` | minor | `library_period_axis` maps library period p to panel date `holdout.begin + p` by raw index. It ignores the recorded `n_dates` and the session keys. A standalone optimize or metabook run on a different research panel (one with a different start date) would place the dead set on the wrong dates, which can be early and therefore look-ahead. This is latent today: there is no production lifecycle driver, so the V2 set is empty, and `run_all` uses one panel throughout. | Pass the deploy panel's session keys and date count. Validate them against the recorded holdout: match the keys when both sides are keyed, else require the same `n_dates`. On a mismatch, fail open (axis unknown). |
| `atx-impl/src/stage_metabook.cpp:425,463` | minor | `load_combo_weights` sums the weights of combo alphas that share a DSL SHA-256. `combo_weighted_sleeve_signal` then gives each library member with that SHA the summed weight. If a library holds two members with identical DSL, the metabook applies about 2·(w1+w2), where the combo applies w1+w2. So "one sleeve reproduces the combo" breaks in that case. This can only happen when the pool-correlation gate allows exact duplicates. | Either divide the summed weight by the member multiplicity, or key the weights by AlphaId label. At minimum, refuse duplicate-SHA members. |
| `.superpowers/sdd/w0/lane-i0a-report.md:77-90` | minor | The acceptance and defect tables quote numbers from before the merge. After the merge, the reviewer measured different values: V1 leak 0.0632524 (report: 0.0720029); stack out-of-sample Sharpes 0.663941 / 2.413094 (report: 0.684542 / 2.212081); discover `factory_digest` 78bca49224a4678f (report: 108d8eff5befc20a); I-07 V1 gap 0.283872 (report: 0.347627). The MET or CLOSED status is unaffected. | Refresh the quoted numbers in the next report revision, or label them as pre-merge. |

## Checked

- [x] I applied the `.agents/cpp/agent.md` §10 checklist to the diff:
  - **Undefined behaviour and bounds.** I found no undefined behaviour and no out-of-bounds access:
    - The Welford loops, trailing-ADV and liquidity panels index rows ≤ d < D.
    - `library_as_of` is clamped.
    - `resolve_nested_split` is guarded against underflow.
  - **Lifetimes.** The `CapacityRef` spans alias buffers at function scope that are only rewritten
    in place. `ShipInputs` and `LiquidityPanels` reference objects in the enclosing scope.
    Discover's `session_keys` span points into `identity`, which the prefix slice does not
    touch.
  - **Switches and error paths.** `split_role_name` is exhaustive. Every new `Result` is
    propagated. The build is `/W4 /WX`-clean (it links at exit 0).
- [x] **The diff stays inside the brief's scope.**
  - The src files are exactly the owned set.
  - The `stage_report.cpp` change is confined to the I-04 call site. `accumulate_report` reads
    only `V.exposures()` and `n_instruments()` (verified in `book/report.hpp:263-317`), so the
    1-row fit leaves the output unchanged.
  - The new tests are the `w0i0a_*` files plus the lane-prefixed fixture header.
  - No CMake file changed.
- [x] **Pre-existing tests are not weakened.**
  - `sign_deploy_test`: re-pinned under I-04. It adds an assertion that `WholePanelV1` reproduces
    the old pin 5744281451106956152.
  - The two dead-alpha wire tests: only the fixture changed (Live→Dead marks and a holdout
    ledger, I-06). The assertions are unchanged.
  - `metabook_test`: selects `EqualWeightV1` explicitly (I-07). The fixture has no sidecar.
  - The staged helpers in the smoke and e2e tests now mirror `run_all`'s nested split (I-01).
    The assertions are unchanged.
  - No test was changed to `DISABLED_` or `GTEST_SKIP`, and no tolerance was loosened.
- [x] **Acceptance items were re-run and the test code read.**
  - *Holdout mutation leaves the weights byte-identical.* The mutation starts exactly at
    `fit_end`. The 17-digit sidecar lines match under conviction, crowding, capacity, Kelly,
    walk-forward and stack. There is a non-vacuity check on the combo digest, a V1 teeth
    test, and the nested discover+combine chain.
  - *Future mutation leaves past books identical.* This holds for optimize under MVO, GP and
    Factor, and for metabook with 1 and 3 sleeves; every V2 case is identical and V1 differs.
    See finding 1 on how weak the participation-cap case is.
  - *Walk-forward `--method stack` runs.* The `"stack"` CLI string is parsed. The run gives
    2 finite folds with embargo 2, and V1 still errs.
- [x] **Every defect ID was verified at its cited site.** All eight are closed:
  - **I-01**: `stage_run.cpp` nested split, the ledger guards in discover and combine.
  - **I-02**: the fit-window conviction.
  - **I-03**: the trailing liquidity panels and the cost on trades (Δw).
  - **I-04**:
    - optimize, MVO and GP: one fit per step through `diagonal_risk_models_expanding`;
    - metabook: per step;
    - report: the lens is inert.
  - **I-06**: `dead_set_at` returns the Dead or Decaying alphas as of each step.
  - **I-07**: the combo sidecar weights, hash-checked with `require_pipeline_file`.
  - **I-08**: one shared dispatch in `fit_shipped_weights`, with the h+delay embargo inside the
    fit window.
  - **R-12**: a per-rebalance trailing reference. The per-step loop was checked line by line
    against `MultiPeriodOptimizer::run` in `multi_period.hpp:131-183`.
- [x] **Versioned enums are in place.** Each changed default has a V1 enumerator:
  - `ConvictionWindowRule`
  - `CapacityRule`
  - `WalkForwardRule`
  - `HoldoutGuardRule`
  - `DiagRiskRule`
  - `DeadAlphaRule`
  - `ParticipationAdvRule`
  - `SleeveSignalRule`

  The only golden digest that moved is `SignDeploy`, and it is tied to I-04 in an old→new
  table.
- [x] **Causality harness: not applicable.** Registration is required only from W1 on (W1-X1).
- [x] **The work is real and wired.**
  - `run_all` uses the nested split.
  - Optimize and metabook use the per-step models and dead sets by default.
  - Metabook uses the combo weights by default.
  - `StageRunMegabook` and the e2e tests pass through `run_all`.
- [x] The report's evidence matches its claims, apart from the stale numbers in finding 4.
  The discover geometry mirrors `factory::mine_into_oos` and `eval::reserve_lockbox`.
