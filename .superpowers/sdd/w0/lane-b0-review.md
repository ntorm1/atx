# Lane W0-B0 review

Fresh adversarial review of lane W0-B0 ("Replay correctness"), pool-9, branch `feat/w0-b0`.
The reviewer was read-only on code: no source or test file was edited. The only file written is
this review.

## Verdict

**BLOCK.** There is one major finding. Under the new default `DelistingPolicy::TerminalReturn`,
the replay liquidates a held name at its **first** missing close, even when the name prints
again later (an interior gap, such as a halt or a data hole). The name is liquidated at the
Shumway fallback. A long loses 30 % or 55 %, and a **short books a windfall gain of 30 % or
55 %**. The lane's own legacy-report path treats the same interior gap as a mark at the last
print and not as a delisting. No test covers this case, and the report does not disclose it.

Everything else holds up. B-02, B-03 and B-05 are closed at the cited sites, and B-04 is closed
apart from the finding above. Both plan Accept items are proven by non-vacuous tests. All four
lane suites pass, and the whole book executable is green. The atx-impl red set matches the
report by name. These failures depend on I0b-owned call sites, so they go to waiver_needed.

## Reviewed SHA

`2762b8782a33f25d7f479bbd4e40e2baae458772` (lane head). The tree was clean before this review was
written (`git status --porcelain` empty). `feat/w0-integration` (`1a304619`) is merged into the
lane (merge `c957d6da`).

## Evidence

All commands were run by the reviewer from `C:\atx-wt\pool-9`. Free RAM was checked first
(3.29 GB), and `CMAKE_BUILD_PARALLEL_LEVEL=2`. `build-equity\CMakeCache.txt` has
`ATX_TEST_GROUPS:STRING=book;risk;combine`, so no reconfigure was needed.

```
atx-build.ps1 build -Preset equity-dev atx-engine-book-tests atx-impl-tests atx-shm-worker
  [9/12] Linking CXX executable bin\atx-shm-worker.exe
  [10/12] Linking CXX executable bin\atx-engine-book-tests.exe
  [11/12] Linking CXX executable bin\atx-impl-tests.exe                 exit=0
atx-build.ps1 check -Preset equity-dev atx-engine\src\book\replay.cpp  ninja: no work to do. exit=0
atx-build.ps1 check -Preset equity-dev atx-impl\src\stage_report.cpp   ninja: no work to do. exit=0
  (objects current at HEAD under the /W4 /WX build)

atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'   (each exit=0)
  ^BookReplayDelay        100% tests passed, 0 tests failed out of 4
  ^BookReplayDelist       100% tests passed, 0 tests failed out of 9
  ^BookBorrowSingleCount  100% tests passed, 0 tests failed out of 5
  ^BookLegacyReport       100% tests passed, 0 tests failed out of 11   (9 engine + 2 BookLegacyReportStage)

build-equity\bin\atx-engine-book-tests.exe --gtest_brief=1
  [==========] 117 tests from 18 test suites ran. (17550 ms total)
  [  PASSED  ] 117 tests.                                                exit=0

build-equity\bin\atx-impl-tests.exe --gtest_brief=1                        exit=1
  [==========] 535 tests from 102 test suites ran. (375274 ms total)
  [  PASSED  ] 514 tests.   [  SKIPPED ] 5 tests.   16 FAILED:
  B-02 (message "execution_delay_periods=0 fills at the decision close; set allow_same_close"):
    ReplayPolicyStage.BoundedIdentifiedWrapper..., ReplayPolicyStage.CoherentReplacementGraph...,
    ReplayReport.WeeklyCoverageDrift..., .DollarTradeFees..., .PolicyAllocationsBind...,
    .ExplicitIntentReport..., .LaterPolicyFailure..., StageEquityBaseline.ExplicitZeroCostsDelay...
  B-04 (expected an abort, got a completed run):
    ReplayReport.MissingHeldPriceMaps..., StageEquityBaseline.MissingHeldMarkPreserves...,
    StageEquityBaseline.ConstrainedBookPreservesMissingHeldMark...,
    StageEquityBaseline.ObservedCloseEntryConstraint...
  not book-related: FundamentalZoo.FixtureParses..., StageEquityIc.TwoRuns...,
    EquityMineCli.SmoothWindows..., TrialLedgerRepository.ExistingCp14Ledger (known CRLF)
```

The failure set is identical by name to the report's 16. The reviewer confirmed the root cause of
each book-related failure from its gtest message (8 B-02 + 4 B-04 = 12). The three
not-book-related failures are in stages that include no book header.

### Acceptance items (re-run and test code read)

| Item | Test read | Proves it? |
|---|---|---|
| Reject `execution_delay=0` unless `allow_same_close` | `BookReplayDelay.*` (4) | Yes. All four entry points (fixed, policy, intent, claims) return InvalidArgument with the opt-in message. The opt-in run shows the look-ahead (133.1 vs 121.0). The test cannot compile on the old code, which has no field. The check is in the shared `validate_inputs` (`replay.cpp:202-206`). |
| `TerminalReturn` default, table then Shumway fallback, flagged; `Abort` kept | `BookReplayDelist.MissingCloseWithoutEvidence...`, `.SuppliedTerminalReturnTableWins...`, `.TableAndExchangeInputsAreValidated` | Yes for a vanishing name. All 6 venue × side cases give r ∈ {−0.30, −0.55}, never 0, flagged, with proceeds = last × (1+r) and the NAV identity checked. A table row wins and is not flagged. A NaN row falls back and is flagged. The PCS test reaches the Abort branch. It does not cover a name that prints again: see major finding 1. |
| Locate breach clips and reports | `BookReplayDelist.LocateBreachClips...`, `.ShrunkenLocateHolds...`, `.WorkingOrderIsClipped...` | Yes. The tests check the clip record values, carried-short hold, working-order re-clip before pricing, and `AbortV1` still rejecting. `locate_goal` (`replay.cpp:379-405`) is bounds-safe. Its min() handles a long→short flip and locate 0 correctly. |
| Borrow fee/rebate counted once | `BookBorrowSingleCount.*` (5) | Yes. The algebra was checked by hand. Fee-quoted `f·S − c·C` equals rebate-quoted `−(c−f)·S − c·(C−S)`. V1 exceeds both by exactly `S·f`. Both-quoted is rejected under V2. |
| Legacy report: delisted return, borrow per period length, horizon annualization | `BookLegacyReport.*` (9), `BookLegacyReportStage.*` (2) | Yes. The stage test drives the real `run_report` unidentified path and checks the pnl.tsv gross, the kvs (`legacy_report_rule=2`, `terminal_returns_flagged=1`), `total_pnl_borrow = 0.002`, and the `sqrt(252/5)` annualization. V1 is proven bit-for-bit against the verbatim pre-W0 loop. |
| **Accept:** PCS 2013-05-01 runs to the end | `BookReplayDelist.Pcs20130501FixtureRunsToTheEnd` | Yes. All 9 intervals run. PCS is liquidated at period 5 (r −0.30, NYSE, flagged). The 04-30 order is unfilled and stays in cash. Interval P&L is checked against the closed form. Explicit Abort fails at `period=5 instrument=0`. |
| **Accept:** missing close without evidence → −30 %/−55 % flagged, never 0, never abort | `...MissingCloseWithoutEvidence...`, `...IntentPolicyTargetOnAVanishedName...` | Met for a terminal disappearance. Exception: the claims entry point still aborts under the default config. This is a disclosed deviation, and that path has no production caller (grep). |

### Defect IDs

| ID | Reviewer status |
|---|---|
| B-02 (engine) | Closed. `replay.cpp:202-206` (the old `:184` site) is shared by every entry point, and the field is named `allow_same_close` for I0b. |
| B-03 | Closed. `report.hpp` `holding_interval_returns` / `report_holding_sessions` / `ReportAccrual` (the old `accumulate_period` `:172-178` now scales by `borrow_accrual`). `stage_report.cpp:493-549` uses a row-indexed returns panel whose universe row is `in_universe(periods[s])`, which is equivalent to the old date-indexed panel under V1. `:618-640` annualizes by mean holding length. The diag-risk site (`diagonal_risk_model`, `:513`) is untouched. |
| B-04 | Closed for aborts: `replay.cpp:978-1002` flags, `:1017-1044` liquidates, and `locate_goal` replaces `check_locate` under ClipV2. Correctness gap on interior gaps: major finding 1. |
| B-05 | Closed. `borrow_schedule.hpp:61,70,118-122` and `replay.cpp:546-555`. |

## Findings

| path:line | severity | problem | required fix |
|---|---|---|---|
| `atx-engine/src/book/replay.cpp:985-999` (`flag_delistings`), `atx-engine/include/atx/engine/book/replay.hpp:30-44` | major | Under the default `TerminalReturn`, **every** held name with a missing or nonpositive close at a valuation is liquidated at a terminal return, even when it prints again later (an interior gap such as a halt, a no-trade day or a data hole). With no table row the name takes the Shumway fallback. A long loses 30 % or 55 %. A **short gains 30 % or 55 %** (value × 0.70 or 0.45), and the name can be re-shorted at the next rebalance. Before W0 this case aborted, so no completed run ever contained it. Now it completes silently: the flag lives only in `ReplayResult`, which `replay_report.cpp` does not publish. This gives a new route to spurious short-side alpha on unfiltered or illiquid universes, which is exactly what B-04 exists to enable. The lane's own legacy path handles the same panel differently: `report.hpp:283-287` marks an interior gap at the last print (`gap_marks`) and applies Shumway only when the name never prints again. That matches the plan's terminal evidence (W2-D2, "each ID's last bar"). A related case: a supplied table event whose `last_valid_period >= period` (the table says the name is still listed) also falls through to a flagged Shumway liquidation. No replay test covers a gap, and the report does not mention it. | Under `TerminalReturn`, liquidate only a name with no later valid close in the panel (the same last-print rule as `holding_interval_returns`), or one whose table event is due. Carry a held name over an interior gap at its last accounted mark, using the existing `carried` valuation seam at both the start and end valuations. It gets no trade until it prints again, and each carry is recorded in a result field (e.g. `gap_carries`). Add `BookReplayDelist` tests with a held long and a held short across a one-session gap that assert no liquidation, the carried NAV, and the identity. If the owner instead rules that a gap is a delisting, record that ruling in the report and disclose the short-side gain. |
| `.superpowers/sdd/w0/lane-b0-report.md:6,168,256` | minor | The counts are inconsistent. The Outcome and Ledger say "11" atx-impl tests fail from the engine defaults, and Evidence group 1 says "7 tests" while listing 8 names. The reviewer measured 8 B-02 + 4 B-04 = 12, which the Post-merge sync section already states as "8 + 4". | Correct the Outcome, group 1 and the Ledger candidate to 12 (8 B-02, 4 B-04). |
| `atx-engine/include/atx/engine/book/replay.hpp:74-88` (`shumway_terminal_return`) | minor | The adverse-for-the-side choice applies only to `Unknown` venues. On a known venue, a flagged fallback credits a short +30 % or +55 %. Many unexplained disappearances are mergers (PCS itself was a combination), and on those shorts lose. The code follows the plan, but a flagged fallback is still a source of short P&L. | Owner decision, not a lane fix. Either apply adverse-for-the-side to every flagged fallback, or report flagged short-side proceeds separately (count and dollars) so G0 can size them. Add this to Integration notes. |
| `atx-impl/src/stage_report.cpp:226` (`kLegacyReportRule`) | minor | In the stage, the pre-W0 legacy report (V1) can be reached only by recompiling. RULES §2 wants the old behaviour reproducible behind the versioned enum. The engine API satisfies this, but the stage does not. The report discloses it (Deviation 3). | Keep the Integration note to I0b: add `RunConfig::legacy_report_rule` and `--legacy-report-rule 1\|2`, and pass it through instead of the constant. |

No blocker was found. UB and bounds: in `holding_interval_returns`, `end = d + h <= dates-1` by the
`report_holding_sessions` cap, `close.size()/instruments` is guarded by `instruments == 0`, and the
gap scan `end-k` stays at or above `d+1`. `locate_goal` pushes to `ctx.clips` only when
`borrow != nullptr && ClipV2`, which is exactly when `init` wires `clips`. `resolve_terminal`
indexes `delistings[delist_event[i]]` only when the policy guarantees an event (Crsp/LastMark
require `event_due`). There is no narrowing and no uninitialised state. The new enums are
validated at every entry.

## Checked

- [x] `.agents/cpp/agent.md` §10 was applied to the diff: UB, bounds, narrowing, lifetimes (the
  `listing_exchange` and `delistings` spans are borrowed as documented), error paths, the
  `resolve_terminal` switch plus the Shumway fallthrough, and the contracts in the headers.
  /W4 /WX is clean: the objects are current at HEAD and the build exits 0.
- [x] Every plan acceptance item was re-run through anchored ctest and the whole book executable,
  and the test code was read. Every test is non-vacuous. The new fields and enums do not exist on
  the old code, and the behaviour tests (never-abort, clip, fee-once, compounded week) assert
  values that the old code contradicts. The interior-gap case is not covered: major finding 1.
- [x] Every cited defect ID was checked at the code. B-02 (engine), B-03 and B-05 are closed.
  B-04 is closed for aborts, with the gap-handling defect above.
- [x] File ownership. `git diff --name-only feat/w0-integration...HEAD` shows the 5 owned sources,
  4 new book tests, 1 new atx-impl test, 7 pre-existing tests and the lane report. The
  diag-risk site in `stage_report.cpp` is untouched. New files follow the RULES naming and
  namespaces.
- [x] No test was weakened. Each pre-existing test edit pins a cited defect (B-02 `allow_same_close`;
  B-04 `Abort`/`AbortV1`; B-05 `FeeAndRebateV1`; B-03 `FlatPerRebalanceV1` and the
  stage_report_borrow fixture). The borrow fixture was moved off the final date, where under V2
  the book has no holding window. That change is justified, and the tolerance is unchanged. There
  are no deleted assertions and no DISABLED_ or GTEST_SKIP additions.
- [x] Changed numeric behaviour has a versioned enum (`LocateBreach::AbortV1`,
  `ShortFinancing::FeeAndRebateV1`, `LegacyReportRule::OnePeriodV1`,
  `ReportBorrowAccrual::FlatPerRebalanceV1`, `DelistingPolicy::Abort`, and the `allow_same_close`
  opt-in). No golden digest pins these outputs, and the report's old→new table ties each change
  to a defect ID. The stage-level V1 selector is missing (minor finding 4).
- [n/a] Causality-harness registration is required from W1 on.
- [~] The work is real and wired into the engine replay (the default config) and the unidentified
  report stage. The book executable is green whole (117/117). atx-impl-tests is red: 12 failures
  are caused by the plan-required defaults at I0b-owned call sites, and 4 are not related to
  this lane. See waiver_needed.
