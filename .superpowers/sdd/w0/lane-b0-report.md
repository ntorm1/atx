# Lane W0-B0 report: Replay correctness

## Outcome

DONE for every Build item and both plan Accept items, with one open gate. The book target is
fully green (122/122 after fix pass 1; 117/117 before it). The atx-impl target is **not** fully
green: 12 existing atx-impl tests (8 B-02, 4 B-04) now fail because the engine defaults changed exactly as the plan requires (B-02: delay 0 is rejected;
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
| Must stay green: atx-impl-tests | `atx-impl-tests.exe --gtest_brief=1` | 535 ran: 514 passed, 5 skipped (real-data only), 16 failed: 12 (8 B-02, 4 B-04) caused by the plan-required engine defaults at I0b-owned call sites, 1 known CRLF, 3 arrived with the integration merge (see Evidence) | UNMET (needs I0b wiring) |

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
1. **B-02 consequence, 8 tests** (`ReplayPolicyStage` ×2, `ReplayReport` Weekly/Dollar/Policy/
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
   `result.allow_same_close = cfg.allow_same_close;`. Then the 8 group-1 tests above need
   `cfg.allow_same_close = true` (or the flag in `set_flags`) next to their
   `replay_execution_delay = 0` line; with that alone their existing numbers hold.
2. Decide the identified report's missing-close policy: either forward an explicit policy
   (e.g. `--replay-delisting-policy abort|terminal-return`, mapping to
   `book::DelistingPolicy::Abort/TerminalReturn`) so the 4 group-2 tests can select `Abort`, or
   keep the new default and update those tests to expect a completed run. In the second case
   `replay_report.cpp` should also publish `ReplayResult::delistings` (with `source`/`flagged`),
   `unfilled_targets`, `locate_clips`, `flagged_delistings`, `flagged_short_delistings`,
   `flagged_short_pnl` and `gap_carries` (fix pass 1) in its manifest so every liquidation and
   carry is disclosed, and pass `listing_exchange` once W2-D5 supplies venues.
3. Update the `--borrow-bps` help text (`config.hpp:444-446`, `dispatch.cpp`) to "annual rate on
   short weight, accrued over the sessions each book is held (legacy report)".
4. (Review minor 4, RULES §2 reproducibility at stage level.) Add
   `RunConfig::legacy_report_rule` (default 2) and `--legacy-report-rule 1|2`, and pass it to
   `stage_report.cpp` in place of the file-local `kLegacyReportRule` constant (`stage_report.cpp`
   is this lane's file, so the stage side is a one-line swap once the field exists; it is not
   done here because `RunConfig`/`config.cpp` are I0b's).

For the **owner / G0** (review minor 3, Shumway fallback on shorts): the plan's numbers are kept
(−30 % NYSE/AMEX, −55 % Nasdaq, adverse-for-the-side only for an Unknown venue), so on a known
venue a flagged fallback still credits a short +30 %/+55 % of its value, although many
unexplained disappearances are mergers on which a short loses. Fix pass 1 takes the reporting
option: `ReplayResult::flagged_short_delistings` (count) and `flagged_short_pnl` (dollars, sum of
proceeds − last value over flagged short rows) size that P&L apart. G0 should read them; if they
are material the owner should rule on applying adverse-for-the-side to every flagged fallback
(a one-line change in `shumway_terminal_return`, which would need a new versioned enum value).

For **W1-B1 / W2-B2** (Track B follow-ups): `BorrowSchedule` is now fee-quoted or rebate-quoted,
never both (B-06's annual-fraction adapter should target the fee form). W2-D2's terminal-return
table plugs straight into `ReplayConfig::delistings` (a finite `delist_return` wins over the
Shumway fallback) and D5's exchange into `ReplayConfig::listing_exchange`.

## Ledger candidates

1. W0-B0: replay defaults are now delay≥1 (0 needs `allow_same_close`), `DelistingPolicy::TerminalReturn` (table else flagged Shumway −30 % NYSE/AMEX, −55 % Nasdaq, adverse for unknown venue), `LocateBreach::ClipV2`, `ShortFinancing::FeeOnceV2`; V1 enums keep old numbers.
2. W0-B0: legacy report V2 on a weekly book: per-rebalance return 0.0510 (compounded week) vs 0.0100 under V1; 0.5 short at 252 bps/yr charged 2.5e-4/week vs 1.26e-2 under V1.
3. W0-B0: the identified report (`replay_report.cpp`) cannot yet opt into same-close fills or choose a delisting policy; 12 atx-impl tests (8 B-02, 4 B-04) stay red until I0b wires `allow_same_close` and the policy.

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

## Fix pass 1

Addresses `.superpowers/sdd/w0/lane-b0-review.md` (BLOCK: 1 major, 3 minor). Owned files only:
`book/replay.{hpp,cpp}`, `book_w0b0_replay_delist_test.cpp`, and this report.

### Major 1: interior gap liquidated under `TerminalReturn` — FIXED

What changed (`replay.cpp` `ReplayExtensions`, `mark_holdings`, `apply_target`; `replay.hpp`):
- Under `TerminalReturn`, a held name with no valid close at a valuation is **carried** and not
  liquidated when no `DelistingEvent` is due for it and either (a) the panel prints it again
  later (`last_print[i] > period`, where `last_print` is scanned once at init; this is the same
  "last bar" rule as `holding_interval_returns` / `gap_marks`) or (b) its table event says it is
  still listed (`last_valid_period >= period`, the related case the reviewer raised). Only a name
  that never prints again, or whose event is due, is liquidated (table, else flagged Shumway),
  as before.
- A carried name keeps its units and is valued at its **last valid close** at both the start and
  the end valuations. The carried seam is generalized to a per-name period (`carry_from`, passed
  to `mark_holdings`), so a gap of several sessions stays at the same close. The whole move lands
  in the interval where the name prints again. A pending liquidation uses the same per-name seam,
  so a table-listed name that is carried and then delisted is liquidated from its carried value.
- No trade executes on a carried name. A decision executing on it is not filled, and its
  resolved weight is carried value / NAV. Its working order is cancelled, because a new decision
  replaces every open order. A non-Hold instruction sets `trade_blocked` on the carry row. The
  payload is still validated. Eligibility is not checked, because nothing executes: a gap day
  commonly drops the name from the decision universe, and a Hold there must not abort.
- Each carried valuation is recorded in the new `ReplayResult::gap_carries`
  (`ReplayGapCarry{period, instrument, mark_period, tri_units, carried_value, trade_blocked}`).
  The `ReplayAllocationState` contract documents the carried name's missing current mark.
- `CrspDelistReturn` / `LastMarkZeroReturn` / `Abort` behave exactly as before: a gap under those
  policies still fails. The claims path still runs as `Abort`. With no gap, every path is
  bit-identical, and every pre-existing book test passes unchanged.

New tests (`BookReplayDelist`):
- `InteriorGapCarriesAHeldLongAndShortAtTheLastPrint`: a held long and a held short across a
  one-session gap (50, 50, NaN, 60, 60) produce no liquidation and no flag, and one carry row
  {period 2, mark_period 1, ±8 units, ±400}. The gap interval's P&L is 0 with NAV 1000 and
  assets 500 ± 400. The re-print interval books ±80. Final NAV is 1080 (long) / 920 (short). The
  identity holds, and explicit `Abort` fails at `period=2 instrument=1`.
- `GapCarryBlocksTradesUntilThePrintAndSpansSessions`: a two-session gap has two carry rows,
  both at mark_period 1 and both `trade_blocked`. Name 1 trades only at periods 1 and 4. At the
  re-print the name resizes to 3.6 units, and final NAV = 324 + 540 + 3.6×66.
- `TableStillListedCarriesThenLiquidatesAtTheTableReturn`: with table `{1, lvp 3, -0.9}` and no
  close from 2 on, the name is carried at 2 and 3, then liquidated at 4 at −0.9 (Table, not
  flagged) from the carried 400 → proceeds 40. Before the fix, period 2 took a flagged −55 %.
- `IntentOnAGapCarriedNameIsNotExecuted`: the name is ineligible on its gap days. A Hold does
  not abort and is not blocked, a Close is recorded as blocked, the resolved weight is 0.4 (the
  carried weight), the units stay 8, and final NAV is 1080.

Non-vacuity (mutation): forcing `gap_carried()` to return false restores the pre-fix
liquidate-at-first-gap behaviour. Under that mutation all 4 new gap tests fail and the other 10
pass. The source was restored and rebuilt afterwards.
```
build-equity\bin\atx-engine-book-tests.exe --gtest_filter=BookReplayDelist.* --gtest_brief=1   (mutant)
[  FAILED  ] BookReplayDelist.InteriorGapCarriesAHeldLongAndShortAtTheLastPrint
[  FAILED  ] BookReplayDelist.GapCarryBlocksTradesUntilThePrintAndSpansSessions
[  FAILED  ] BookReplayDelist.TableStillListedCarriesThenLiquidatesAtTheTableReturn
[  FAILED  ] BookReplayDelist.IntentOnAGapCarriedNameIsNotExecuted
[  PASSED  ] 10 tests.
```
Measured (verbatim):
```
[measured] gap short=0 carried=400.00 final_nav=1080.0000 (pre-fix final_nav 780.0000)
[measured] gap short=1 carried=-400.00 final_nav=920.0000 (pre-fix final_nav 1120.0000)
```
The short case is the reviewer's scenario. Before the fix, a short carried over a one-day halt
booked a spurious +120 (+12 % of NAV) windfall. It now ends at 920, which is the true −80 move.

### Minor 2: inconsistent failure counts — FIXED

The Outcome, the acceptance row, Evidence group 1 ("8 tests"), Integration note 1 and Ledger
candidate 3 now all say 12 (8 B-02 + 4 B-04). This matches the Post-merge sync section and the
reviewer's count.

### Minor 3: Shumway fallback credits shorts on known venues — reporting option implemented; rule is an owner decision

The plan's numbers are unchanged. New fields: `ReplayResult::flagged_short_delistings` (the count
of flagged rows whose position was short) and `flagged_short_pnl` (the sum of proceeds −
last_value over those rows; positive means a gain to the book). G0 can size this P&L separately.
Test `BookReplayDelist.FlaggedShortProceedsAreReportedApart`: a NYSE short gives count 1 and
pnl +120 (= proceeds − last_value); a long gives 0/0; a table-sourced short gives 0/0. The
owner decision (whether to apply adverse-for-the-side to every flagged fallback) is recorded in
Integration notes ("For the owner / G0"). Publishing these fields in the identified report is
I0b's (Integration note 2).

### Minor 4: stage-level V1 legacy report reachable only by recompiling — NOT FIXED (out of scope), Integration note kept

`RunConfig` and `--legacy-report-rule` live in I0b's `config.hpp`/`config.cpp`. The lane cannot
add the field, so `stage_report.cpp` keeps `kLegacyReportRule`. Integration note 4 now states the
exact I0b change (`RunConfig::legacy_report_rule` default 2 plus `--legacy-report-rule 1|2`,
passed instead of the constant). With it, the stage side is a one-line swap in this lane's file.

### Evidence (fix pass 1)

All commands ran from `C:\atx-wt\pool-9` with `CMAKE_BUILD_PARALLEL_LEVEL=2`. Free RAM was 2.76–3.54
GB, never below 2.0.
```
scripts\atx-build.ps1 check -Preset equity-dev atx-engine\src\book\replay.cpp
  [1/2] Building CXX object atx-engine\CMakeFiles\atx-engine.dir\src\book\replay.cpp.obj   exit=0 (/W4 /WX)
scripts\atx-build.ps1 build -Preset equity-dev atx-engine-book-tests atx-impl-tests atx-shm-worker
  [14/40] Linking CXX executable bin\atx-shm-worker.exe
  [38/40] Linking CXX executable bin\atx-impl-tests.exe
  [39/40] Linking CXX executable bin\atx-engine-book-tests.exe                              exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookReplayDelay'        100% tests passed, 0 tests failed out of 4    exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookReplayDelist'       100% tests passed, 0 tests failed out of 14   exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookBorrowSingleCount'  100% tests passed, 0 tests failed out of 5    exit=0
scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^BookLegacyReport'       100% tests passed, 0 tests failed out of 11   exit=0
build-equity\bin\atx-engine-book-tests.exe --gtest_brief=1
  [==========] 122 tests from 18 test suites ran.   [  PASSED  ] 122 tests.                 exit=0
build-equity\bin\atx-impl-tests.exe --gtest_brief=1
  [==========] 535 tests from 102 test suites ran. (306130 ms total)
  [  PASSED  ] 514 tests.  (5 skipped, 16 failed)                                           exit=1
```
The atx-impl failure set is identical by name to the 16 classified above: 8 B-02, 4 B-04, 3 that
arrived with the merge, and 1 CRLF. The 4 B-04 tests still assert a pre-W0 abort, which I0b owns
(Integration note 2). That whole-executable run was made on the fix source before one final
comment-only rewrap in `replay.hpp`. After the rewrap, both targets were rebuilt and relinked
(exit 0), and the book executable and all 4 anchored suites were re-run green on the final
source.

Book target: 117 → 122 tests, all passing (+5 `BookReplayDelist`: 4 gap tests and 1
flagged-short test). No existing test was changed.

Safety disclosure: during the mutation step, one `[IO.File]` call was first given a relative
path, which .NET resolved against the process cwd `C:\atx`. It read `C:\atx\...\replay.cpp`,
found no match, and wrote nothing. To confirm that, a read-only `git -C C:\atx status --short` was
run on that one file. It showed only the other session's pre-existing modification. No file under
`C:\atx` was written. The mutation was then redone with the absolute pool-9 path.

## Post-merge sync (final, before orchestrator merge)

- Base for this sync: `feat/w0-integration` had moved to `16a2ae35` ("w0: progress — R0 merged",
  bringing in `atx-engine/{include,src}/risk/{exposures.hpp,factor_model.hpp,factor_model.cpp}`
  and new `risk_w0r0_*` tests). Lane pre-sync head: `6d4e29e5`.
- `git -C C:\atx-wt\pool-9 status --porcelain` was empty, no `MERGE_HEAD`. Since
  `merge-base --is-ancestor feat/w0-integration HEAD` failed (integration had moved since the last
  sync recorded above), performed the merge:
  ```
  git -C C:\atx-wt\pool-9 merge --no-ff feat/w0-integration -m "w0-b0: merge feat/w0-integration" -m "Co-Authored-By: ..."
  ```
  exit 0, no conflicts (`Merge made by the 'ort' strategy`; 11 files changed, all under
  `atx-engine/{include,src,tests}/risk/` and `.superpowers/sdd/w0/` — none in this lane's owned
  files). New head: `b701c8bf`.
- Rebuild (RAM check: 2.9 GB free at time of build, `>= 2.0 GB` gate satisfied,
  `CMAKE_BUILD_PARALLEL_LEVEL=2`):
  ```
  scripts\atx-build.ps1 build -Preset equity-dev atx-engine-book-tests            # exit 0
  scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker    # exit 0
  ```
- Anchored suites (ctest `-R '^<Suite>\.'`, note the trailing dot — gtest suite names use `.`, not
  `_`, contrary to the brief's literal `_*` spelling):
  - `BookReplayDelay.*`: 4/4 passed.
  - `BookReplayDelist.*`: 14/14 passed (includes `Pcs20130501FixtureRunsToTheEnd` and
    `MissingCloseWithoutEvidenceIsShumwayFlaggedNeverZeroNeverAbort` — both Accept items MET).
  - `BookBorrowSingleCount.*`: 5/5 passed.
  - `BookLegacyReport.*`: 9/9 passed.
- Whole owning executables (`--gtest_brief=1`):
  - `atx-engine-book-tests.exe`: `[==========] 122 tests from 18 test suites ran.` `[  PASSED  ] 122 tests.`
  - `atx-impl-tests.exe`: `[==========] 535 tests from 102 test suites ran.` `[  PASSED  ] 514 tests.`
    `[  SKIPPED ] 5 tests.` 3 failures, none touching a file this lane owns or that the merge
    changed (`git diff 6d4e29e5 HEAD -- <file>` is empty for all three test files below):
    - `StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput`
    - `EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials`
    - `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies`
    All three match the pre-existing classification above (groups 3 and 4: "arrived with the
    integration merge" / owner I0b-A0, and "known pre-existing" CRLF `trial-ledger.jsonl` checkout
    issue) — the 12 group-1/group-2 (B-02/B-04 consequence) failures and `FundamentalZoo`
    previously listed there are now green, i.e. fixed upstream by other lanes since the earlier
    sync. No fix applied here: none of these 3 failures are caused by code this merge brought in
    (the risk/factor_model files), and none touch owned files.
- Tree clean and committed at `b701c8bf` (this block + the merge commit).
- No new defect IDs opened; no golden-digest changes beyond the table above.
