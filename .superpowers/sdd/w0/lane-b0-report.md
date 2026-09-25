# Lane W0-B0 report: Replay correctness

## Outcome

DONE for every Build item and both plan Accept items, with one open gate. The book target is
fully green (117/117). The atx-impl target is **not** fully green: 11 existing atx-impl tests now
fail because the engine defaults changed exactly as the plan requires (B-02: delay 0 is rejected;
B-04: a missing held close is liquidated instead of aborting), and the only call site that could
opt out or disclose the new behaviour, `atx-impl/src/replay_report.cpp` (plus `config.hpp` for the
CLI flag), belongs to W0-I0b, not to this lane. The exact fix for I0b is in "Integration notes".
Three more atx-impl failures arrived with the latest `feat/w0-integration` merge and do not touch
any book code; one is the known CRLF ledger failure.

## Branch / SHA

- Branch `feat/w0-b0`, pool `C:\atx-wt\pool-9`, lease run id `aes-w0-b0` (held by the orchestrator).
- W0 base `458d0bef480a624e258070c9d45174a9984466bf`.
- Lane commits: `7fa44bc8` (engine + stage), `w0-b0: legacy report stage tests...` (atx-impl tests),
  merge of `feat/w0-integration` (clean, no conflicts), then this report. The final SHA is the
  commit that adds this file (see `git log feat/w0-b0`).

## Files changed

Owned files:
- `atx-engine/include/atx/engine/book/replay.hpp`: `allow_same_close`; `DelistingPolicy::TerminalReturn`
  (new default); `ListingExchange`; the Shumway constants and `shumway_terminal_return()`;
  `TerminalReturnSource`; `LocateBreach {AbortV1, ClipV2}` (ClipV2 default); new result records
  `ReplayLocateClip`, `ReplayUnfilledTarget`, `ReplayDelisting::source/flagged`,
  `ReplayResult::locate_clips/unfilled_targets/flagged_delistings`.
- `atx-engine/src/book/replay.cpp`: the delay guard, terminal-return liquidation, locate clipping
  (target and working-order paths, clipped before pricing), unfillable-target handling, fee-once
  financing, claims-path compatibility.
- `atx-engine/include/atx/engine/book/borrow_schedule.hpp`: `ShortFinancing {FeeAndRebateV1,
  FeeOnceV2}` (FeeOnceV2 default), `quotes_fee()`, validation.
- `atx-engine/include/atx/engine/book/report.hpp`: `LegacyReportRule {OnePeriodV1,
  HoldingIntervalV2}`, `ReportBorrowAccrual {FlatPerRebalanceV1, AnnualBySessionsV2}`,
  `ReportAccrual`, `report_holding_sessions()`, `holding_interval_returns()`,
  `BookReport::holding_sessions`, the `accumulate_report(..., const ReportAccrual&)` overload (the
  scalar-borrow overload now forwards to it with the corrected default).
- `atx-impl/src/stage_report.cpp` (legacy/unidentified path only; the diag-risk site near the old
  line 499 is untouched): holding-interval returns through a row-indexed returns panel, annual
  borrow accrual, holding-length annualization, additive disclosure lines/kvs.

New tests: `atx-engine/tests/book/book_w0b0_replay_delay_test.cpp`,
`book_w0b0_replay_delist_test.cpp`, `book_w0b0_borrow_single_count_test.cpp`,
`book_w0b0_legacy_report_test.cpp`, `atx-impl/tests/w0b0_legacy_report_test.cpp`.

Existing tests changed (each one pins a cited defect; RULES §2):

| File | Change | Defect |
|---|---|---|
| `book_replay_test.cpp` `immediate()` | `allow_same_close = true`; `delisting_policy = Abort` | B-02, B-04 |
| `book_event_batch_test.cpp` `immediate()` | `allow_same_close = true`; `delisting_policy = Abort` (the case that set no policy pinned the old Abort default) | B-02, B-04 |
| `book_replay_cost_test.cpp` `base_config()` | `allow_same_close = true` | B-02 |
| `book_report_parity_test.cpp` (2 configs) | `allow_same_close = true` | B-02 |
| `book_borrow_schedule_test.cpp` helpers | `allow_same_close = true`; `locate_breach = AbortV1`; `financing = FeeAndRebateV1` | B-02, B-04, B-05 |
| `report_borrow_test.cpp` `accumulate_ok()` | passes `ReportAccrual{FlatPerRebalanceV1}` explicitly | B-03 |
| `atx-impl/tests/stage_report_borrow_test.cpp` | book moved from the final date (no holding window, yet charged a full flat period) to period 0 (held one session); rate 2520 bps/yr so the 1-session debit (0.0005) is exact in the 6-decimal kv; expectation `0.5*2520e-4/252` | B-03 |

No assertion was removed or weakened; every changed case still checks the same V1 behaviour
through its explicit versioned enum.

## Acceptance table

| Item | Test(s) | Measured result | Status |
|---|---|---|---|
| Reject `execution_delay=0` unless `allow_same_close` | `BookReplayDelay.ZeroDelayIsRejectedWithoutTheOptIn`, `.EveryEntryPointSharesTheRejection` (fixed, policy, intent and claims entry points), `.OptInAdmitsSameCloseAndMeasuresItsLookAhead`, `.DefaultConfigIsDelayOneWithoutSameClose` | delay 0 → `InvalidArgument` "set allow_same_close to opt in" on all 4 entry points; with the opt-in, a +10 %/day name ends at NAV 133.1 (fills at the close the signal saw) vs 121.0 at the default delay 1 | MET |
| `TerminalReturn` default: table, else Shumway −30 % NYSE/AMEX / −55 % Nasdaq, flagged; `Abort` still available | `BookReplayDelist.MissingCloseWithoutEvidenceIsShumwayFlaggedNeverZeroNeverAbort`, `.SuppliedTerminalReturnTableWinsAndIsNotFlagged`, `.TableAndExchangeInputsAreValidated`, `.Pcs20130501FixtureRunsToTheEnd` (Abort branch) | NYSE long/short r=−0.30, Nasdaq long/short r=−0.55, unknown venue long −0.55 / short −0.30 (adverse for the side), all flagged; table row −0.9 wins (source Table, not flagged); NaN table row falls back to −0.55 flagged; explicit Abort fails at `period=5 instrument=0` | MET |
| Locate breach clips and reports, no abort | `BookReplayDelist.LocateBreachClipsToTheLocateAndReports`, `.ShrunkenLocateHoldsTheCarriedShortWithoutGrowingOrCovering`, `.WorkingOrderIsClippedToTheCurrentLocateBeforePricing` | 200 $ short vs 150 $ locate → clip record {requested −200, allowed −150, locate 150}, units −1.5; carried 100 $ short with locate shrunk to 50 is held (no growth, no forced cover); capped working order re-clipped 250 → 150 at the next period; `AbortV1` still rejects | MET |
| Borrow fee and rebate counted once | `BookBorrowSingleCount.*` (5 tests) | fee+rebate quoted together: V1 = −0.0200004000, V2 fee-quoted = −0.0230004600, V2 rebate-quoted = −0.0230004600 for one D360 day; the V1 excess equals short dollars × fee exactly; V2 rejects a schedule quoting both | MET |
| Legacy report: delisted return applied, borrow per period length, horizon-correct annualization | `BookLegacyReport.*` (9 engine tests), `BookLegacyReportStage.*` (2 stage tests) | weekly name return V2 0.051010 vs V1 0.010000 per rebalance; delisted held name −0.55 (long, unknown venue) instead of 0; weekly 0.5 short at 252 bps/yr charged 2.5e-4 per week (V1: 1.26e-2); stage fixture week-1 pnl_gross V2 −0.136495 vs V1 +0.011667; annualization sqrt(252/5); daily schedules bit-identical to the pre-W0 arithmetic | MET |
| **Accept: the PCS 2013-05-01 fixture runs to the end** | `BookReplayDelist.Pcs20130501FixtureRunsToTheEnd` | synthetic NYSE sessions 2013-04-24..2013-05-07, PCS stops printing after 04-30 with no delisting record; default config runs all 9 intervals; PCS liquidated at 05-01 with r=−0.30 (ShumwayNyseAmex, flagged): last_value=309613.47, proceeds=216729.43, interval P&L −86691.77, final NAV 992931.34; the 04-30 PCS order is reported as unfilled (stays in cash); explicit Abort fails at `period=5 instrument=0` | MET |
| **Accept: missing close without evidence yields −30 %/−55 % (flagged), never 0 and never an abort** | `BookReplayDelist.MissingCloseWithoutEvidenceIsShumwayFlaggedNeverZeroNeverAbort` (6 venue × side cases), `.IntentPolicyTargetOnAVanishedNameIsUnfilledNotFatal` | every case completes all intervals, r ∈ {−0.30, −0.55}, r ≠ 0, flagged; proceeds = last value × (1 + r) (e.g. 400 $ long Nasdaq → 180 $) | MET |
| Must stay green: whole book target | `atx-engine-book-tests.exe --gtest_brief=1` | 117/117 passed | MET |
| Must stay green: atx-impl-tests | `atx-impl-tests.exe --gtest_brief=1` | 535 ran: 514 passed, 5 skipped (real-data only), 16 failed: 11 caused by the plan-required engine defaults at I0b-owned call sites, 1 known CRLF, 3 arrived with the integration merge (see Evidence) | UNMET (needs I0b wiring) |

## Defect table

| ID | Status | How / where |
|---|---|---|
| B-02 (engine half) | CLOSED | `replay.cpp` `validate_inputs`: `execution_delay_periods == 0 && !allow_same_close` → `InvalidArgument`; shared by all four entry points. Field named `allow_same_close` for I0b. The CLI flag and the equity-mine/config sites are W0-I0b's (the other half of B-02). |
| B-03 | CLOSED | `report.hpp` `holding_interval_returns` (HoldingIntervalV2 compounds over the holding window and prices a vanished held name at last print × (1 + Shumway), flagged), `ReportBorrowAccrual::AnnualBySessionsV2` (annual rate × sessions held / 252), `stage_report.cpp` annualizes by mean holding length. V1 rules reproduce the old numbers (bit-for-bit test). |
| B-04 | CLOSED | `replay.cpp` `ReplayExtensions` (TerminalReturn default, table → Shumway fallback, flagged records) and `locate_goal` (ClipV2 default, clip records). Abort / AbortV1 remain selectable. |
| B-05 | CLOSED | `borrow_schedule.hpp` `ShortFinancing::FeeOnceV2` default + `replay.cpp` `scheduled_financing`: a fee-quoted schedule charges the fee and lets proceeds earn the cash rate; a rebate-quoted schedule uses the rebate only; quoting both is rejected. FeeAndRebateV1 kept. |

## Evidence

All commands ran from `C:\atx-wt\pool-9` with `CMAKE_BUILD_PARALLEL_LEVEL=2` and ≥ 2 GB free
(2.22 – 4.03 GB observed). `build-equity\CMakeCache.txt` has `ATX_TEST_GROUPS=book;risk;combine`,
which includes `book`, so no reconfigure was needed.

Type checks:
```
powershell -NoProfile -File scripts\atx-build.ps1 check -Preset equity-dev atx-engine\src\book\replay.cpp
[1/4] Building CXX object atx-engine\CMakeFiles\atx-engine.dir\src\book\replay.cpp.obj      (exit 0)
powershell -NoProfile -File scripts\atx-build.ps1 check -Preset equity-dev atx-impl\src\stage_report.cpp
[1/2] Building CXX object atx-impl\CMakeFiles\atx-impl-core.dir\src\stage_report.cpp.obj   (exit 0)
```

Build after the final integration merge:
```
powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-engine-book-tests atx-impl-tests atx-shm-worker
[66/147] Linking CXX executable bin\atx-shm-worker.exe
[142/147] Linking CXX executable bin\atx-engine-book-tests.exe
[144/147] Linking CXX executable bin\atx-impl-tests.exe
exit=0
```

Anchored suites (after the merge):
```
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookReplayDelay'        100% tests passed, 0 tests failed out of 4    exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookReplayDelist'       100% tests passed, 0 tests failed out of 9    exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookBorrowSingleCount'  100% tests passed, 0 tests failed out of 5    exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookLegacyReport'       100% tests passed, 0 tests failed out of 11   exit=0
```
(`^BookLegacyReport` covers the 9 engine tests and the 2 `BookLegacyReportStage` atx-impl tests.)

Whole book executable (after the merge):
```
build-equity\bin\atx-engine-book-tests.exe --gtest_brief=1
[==========] 117 tests from 18 test suites ran. (17722 ms total)
[  PASSED  ] 117 tests.
exit=0
```

Measured lines printed by the tests (verbatim):
```
[measured] fee-quoted 1-day financing: V2=0.1840036800 V1=0.1900038000
[measured] 1-day financing: V1 fee+rebate=-0.0200004000 V2 fee=-0.0230004600 V2 rebate=-0.0230004600
[measured] weekly book, name 0: V2 0.051010 vs V1 0.010000 per rebalance
[measured] weekly 0.5 short @252bps/yr: V2 2.500e-04 vs V1 1.260e-02 per rebalance
[measured] delisting week pnl_gross: V2 -0.189394 vs survivor-only 0.030606
[measured] PCS last_value=309613.47 proceeds=216729.43 pnl(04-30->05-01)=-86691.77 final_nav=992931.34
[measured] venue=1 short=0 r=-0.30 proceeds=280.00 final_nav=880.0000
[measured] venue=1 short=1 r=-0.30 proceeds=-280.00 final_nav=1120.0000
[measured] venue=2 short=0 r=-0.55 proceeds=180.00 final_nav=780.0000
[measured] venue=2 short=1 r=-0.55 proceeds=-180.00 final_nav=1220.0000
[measured] venue=0 short=0 r=-0.55 proceeds=180.00 final_nav=780.0000
[measured] venue=0 short=1 r=-0.30 proceeds=-280.00 final_nav=1120.0000
[measured] pnl_gross week 1: V2 -0.136495 (delisting priced) vs V1 0.011667
```

Whole atx-impl executable (after the merge), exit 1:
```
build-equity\bin\atx-impl-tests.exe --gtest_brief=1
[==========] 535 tests from 102 test suites ran. (343173 ms total)
[  PASSED  ] 514 tests.
[  FAILED  ] EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials
[  FAILED  ] FundamentalZoo.FixtureParsesTypechecksAndEvaluates
[  FAILED  ] ReplayPolicyStage.BoundedIdentifiedWrapperRetainsScheduleAndExplicitFeeValidation
[  FAILED  ] ReplayPolicyStage.CoherentReplacementGraphMustMatchEveryAdmittedArtifactIdentity
[  FAILED  ] ReplayReport.DollarTradeFeesAndActualCalendarBorrowReconcileCashAndAssets
[  FAILED  ] ReplayReport.ExplicitIntentReportBindsHoldCloseAndActualCashFlows
[  FAILED  ] ReplayReport.LaterPolicyFailurePublishesNoAcceptedAllocationOrCompleteManifest
[  FAILED  ] ReplayReport.MissingHeldPriceMapsExactSecurityAndDateWithoutCompletePublication
[  FAILED  ] ReplayReport.PolicyAllocationsBindActualDollarRowsAndModelProvenance
[  FAILED  ] ReplayReport.WeeklyCoverageDriftExactAxesAndDeterministicManifest
[  FAILED  ] StageEquityBaseline.ConstrainedBookPreservesMissingHeldMarkFailureOnOriginalWindow
[  FAILED  ] StageEquityBaseline.ExplicitZeroCostsDelayAndGlobalDefaultAumArePreserved
[  FAILED  ] StageEquityBaseline.MissingHeldMarkPreservesBoundFailureAndNoCompleteManifest
[  FAILED  ] StageEquityBaseline.ObservedCloseEntryConstraintBindsAvailabilityWithoutShrinkingUnion
[  FAILED  ] StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput
[  FAILED  ] TrialLedgerRepository.ExistingCp14Ledger_StillVerifies
```
(535 ran = 514 passed + 5 skipped real-data cases + 16 failed.)

Classification of the 16 failures:
1. **B-02 consequence, 7 tests** (`ReplayPolicyStage` ×2, `ReplayReport` Weekly/Dollar/Policy/
   ExplicitIntent/LaterPolicy, `StageEquityBaseline.ExplicitZeroCostsDelay...`): these fixtures
   run the identified report with `replay_execution_delay = 0`; the engine now answers
   `replay: execution_delay_periods=0 fills at the decision close; set allow_same_close to opt in`.
   `replay_report.cpp:394` builds the `ReplayConfig` and has no way to pass the opt-in, and there
   is no `--allow-same-close` flag yet (`config.hpp`, I0b).
2. **B-04 consequence, 4 tests** (`ReplayReport.MissingHeldPrice...`, `StageEquityBaseline`
   MissingHeldMark / ConstrainedBook... / ObservedCloseEntry...): they assert the pre-W0 abort on a
   missing held close (or on an unpriceable entry). Under the new default the name is liquidated
   at a flagged terminal return, or the entry is reported unfilled, and the run completes.
3. **Arrived with the `feat/w0-integration` merge, 3 tests** (`EquityMineCli.SmoothWindows...`
   n_raw 9 vs 12, `FundamentalZoo.FixtureParsesTypechecksAndEvaluates`, `StageEquityIc.TwoRuns...`
   31361 vs 32001 dates and `summary_reportable`): they passed in this lane's pre-merge run and
   the stages involved (`stage_equity_mine`, `stage_equity_ic`, the fundamental zoo) include no
   book header (only `replay_report.cpp` and `stage_report.cpp` do). They match the A0 / E0a notes
   in `w0b-integration-notes.md` and the G0 runbook (A-03 vs `FundamentalZoo`); owner I0b / A0.
4. **Known pre-existing, 1 test** (`TrialLedgerRepository.ExistingCp14Ledger_StillVerifies`): the
   CRLF checkout of `trial-ledger.jsonl` recorded in `w0b-integration-notes.md`.

Pre-merge whole-executable run for comparison: 535 ran, 516 passed; failures were groups 1, 2 and 4
plus `StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard`, which my first
version caused (a text-valued report kv); fixed by making `legacy_report_rule` numeric (`2`), and
it passes after the merge.

## Golden-digest old → new

No test in the repository pins a literal legacy-report stage digest or replay digest (grep for
64-bit hex literals in `atx-impl/tests` finds only RNG seeds and fixture fingerprints), so there is
no golden to re-baseline. What changes:

| Output | Old → new | Defect |
|---|---|---|
| `report` stage digest (unidentified path) | unchanged for daily schedules with no vanished held name (V2 arithmetic is bit-identical there, proven by `BookLegacyReportStage.DailyScheduleIsUnchangedByTheHoldingIntervalRule`); changes for multi-session schedules (compounded window) and for held names that stop printing (terminal return instead of 0) | B-03 |
| `report` `total_pnl_borrow` / `pnl_net` with `--borrow-bps > 0` | flat per rebalance → annual rate × sessions held / 252 (fixture: 0.0025 flat → 0.0005 for one session at 2520 bps/yr) | B-03 |
| Replay results on panels where a held name loses its close | error → completed run with a flagged `ReplayDelisting` | B-04 |
| Replay financing with a fee grid and a nonzero cash rate | proceeds now earn the cash rate (fixture: 0.1900038 → 0.1840037 per day) | B-05 |

## Deviations from brief

1. **Claims-aware entry point keeps Abort semantics.** `replay_scheduled_intents_with_events`
   admits the default `TerminalReturn` policy (so existing callers with `ReplayConfig{}` still
   work) but runs it as `Abort`, and rejects a delisting table, exchange list or other policies.
   That path is synthetic-fixture only and its terminal mechanism is the admitted mandatory event;
   mixing delisting liquidation into its carried-predecessor valuation would have needed a
   redesign outside this lane. Tested by `BookReplayDelist.ClaimsPathKeepsAbortSemanticsAndRejectsExtensions`.
2. **Unfillable targets.** A nonzero target on an unheld name that has no close at execution
   (the name left the panel between decision and execution) is reported in
   `ReplayResult::unfilled_targets` and stays in cash under `TerminalReturn`, instead of failing.
   The PCS acceptance item needs this (the 04-30 decision executes on 05-01). Under `Abort` the
   old error is kept.
3. **Legacy report V1 is reachable only in code.** `stage_report.cpp` selects the rule through a
   file-local constant (`kLegacyReportRule = HoldingIntervalV2`); there is no CLI flag because
   `config.hpp` belongs to I0b. The engine API (`holding_interval_returns`, `ReportAccrual`) exposes
   V1 fully and a test proves it is bit-identical to the old loop.
4. **`--borrow-bps` unit.** In the legacy report it is now an annual rate accrued over sessions
   held (the fix B-03 asks for). The help text in `config.hpp:444-446` still says "flat per-period";
   see integration notes.
5. **atx-impl-tests not fully green** (see Outcome and Evidence). Nothing was skipped or weakened.

## Integration notes

For **W0-I0b** (owns `config.hpp`/`config.cpp`, `dispatch.cpp`, the `replay_report.cpp` I-11 site,
`stage_equity_baseline.cpp`):
1. Add `bool allow_same_close = false;` to `RunConfig` and the `--allow-same-close` flag, and in
   `replay_report.cpp` `replay_config()` (line ~394) set
   `result.allow_same_close = cfg.allow_same_close;`. Then the 7 group-1 tests above need
   `cfg.allow_same_close = true` (or the flag in `set_flags`) next to their
   `replay_execution_delay = 0` line; with that alone their existing numbers hold.
2. Decide the identified report's missing-close policy: either forward an explicit policy
   (e.g. `--replay-delisting-policy abort|terminal-return`, mapping to
   `book::DelistingPolicy::Abort/TerminalReturn`) so the 4 group-2 tests can select `Abort`, or
   keep the new default and update those tests to expect a completed run. In the second case
   `replay_report.cpp` should also publish `ReplayResult::delistings` (with `source`/`flagged`),
   `unfilled_targets`, `locate_clips` and `flagged_delistings` in its manifest so the liquidation
   is disclosed, and pass `listing_exchange` once W2-D5 supplies venues.
3. Update the `--borrow-bps` help text (`config.hpp:444-446`, `dispatch.cpp`) to "annual rate on
   short weight, accrued over the sessions each book is held (legacy report)". Optionally expose
   `--legacy-report-rule 1|2` and pass it to `stage_report.cpp`'s constant (make it a parameter).

For **W1-B1 / W2-B2** (Track B follow-ups): `BorrowSchedule` is now fee-quoted or rebate-quoted,
never both (B-06's annual-fraction adapter should target the fee form). W2-D2's terminal-return
table plugs straight into `ReplayConfig::delistings` (a finite `delist_return` wins over the
Shumway fallback) and D5's exchange into `ReplayConfig::listing_exchange`.

## Ledger candidates

1. W0-B0: replay defaults are now delay≥1 (0 needs `allow_same_close`), `DelistingPolicy::TerminalReturn` (table else flagged Shumway −30 % NYSE/AMEX, −55 % Nasdaq, adverse for unknown venue), `LocateBreach::ClipV2`, `ShortFinancing::FeeOnceV2`; V1 enums keep old numbers.
2. W0-B0: legacy report V2 on a weekly book: per-rebalance return 0.0510 (compounded week) vs 0.0100 under V1; 0.5 short at 252 bps/yr charged 2.5e-4/week vs 1.26e-2 under V1.
3. W0-B0: the identified report (`replay_report.cpp`) cannot yet opt into same-close fills or choose a delisting policy; 11 atx-impl tests stay red until I0b wires `allow_same_close` and the policy.

## Post-merge sync

Sync check on `2026-09-25`: `feat/w0-integration` (`1a304619`) was already an ancestor of
`feat/w0-b0` HEAD (`git -C C:\atx-wt\pool-9 merge-base --is-ancestor feat/w0-integration HEAD`,
exit 0) — the merge recorded above (`c957d6da`) already carries it, so no new merge was made. No
"Post-merge sync" block existed yet for this exact head, so per the sync task the owning targets
were rebuilt and every lane suite re-run on head `6381c3558f7560fd1f40372bde3980e63775ee31` to
confirm reproducibility before review; working tree was clean (`git status --porcelain` empty), no
`MERGE_HEAD`.

Commands (from `C:\atx-wt\pool-9`, `CMAKE_BUILD_PARALLEL_LEVEL=2`, free RAM 3.2-4.1 GB observed,
threshold 2.0 GB never hit):
```
scripts\atx-build.ps1 build -Preset equity-dev atx-engine-book-tests            exit=0 (relink only, no source changes)
scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker    exit=0 (relink only, no source changes)
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookReplayDelay\.'        100% tests passed, 0 tests failed out of 4   exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookReplayDelist\.'       100% tests passed, 0 tests failed out of 9   exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookBorrowSingleCount\.'  100% tests passed, 0 tests failed out of 5   exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookLegacyReport\.'       100% tests passed, 0 tests failed out of 9   exit=0
build-equity\bin\atx-engine-book-tests.exe --gtest_brief=1   [==========] 117 tests from 18 test suites ran.  [PASSED] 117 tests.   exit=0
build-equity\bin\atx-impl-tests.exe --gtest_brief=1          [==========] 535 tests from 102 test suites ran. [PASSED] 514 tests. [SKIPPED] 5 tests.   exit=1 (16 failed)
```

The `atx-impl-tests` failure set is byte-identical, by name, to the 16 tests already classified in
this report's Evidence/Outcome sections (groups 1-4: 8 B-02-consequence + 4 B-04-consequence + 3
merge-arrived zoo/ic + 1 known CRLF ledger) — no new failure, no prior failure now passing, no
regression introduced by re-syncing this exact head. Still UNMET pending W0-I0b's
`allow_same_close`/delisting-policy wiring per Integration notes; no owned-file fix applies (the
call sites are outside this lane's scope). Tree left clean; this file added with `git add -f` and
committed at the head below.

Result: **UP_TO_DATE / re-verified**, head `6381c3558f7560fd1f40372bde3980e63775ee31` plus this
commit.
