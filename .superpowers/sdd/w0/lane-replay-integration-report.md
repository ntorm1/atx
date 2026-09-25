# W0 replay integration correction

## Outcome

DONE for this scoped correction: 128/128 book tests, 32/32 focused impl tests, and
all six changed production translation units compiled with PCH disabled. The whole
impl qualification remains the root's integrated gate, not a pass claimed by this lane.

- Branch: `feat/w0-replay-integration-codex-20260925`.
- Frozen base: `bc5cc646b46f6a7c23a60e87d28dfa9b972671ec`.
- Pool: `C:\atx-wt\pool-4`.
- Lease run: `aes-w0-replay-codex-20260925`, independent heartbeat keeper.
- Production SHA: `8ba15b0efeaff53587cc0ac36493cd159247b126`.
- Fixture-only correction SHA: `4f257729d2949f082c818f53b42c2c7aca3b9cf9`.
- Final report SHA: see the commit updating this report.

## Causal finding and implementation

The I0b final-sync report explicitly records pinning `Abort` in the single identified
replay adapter to restore four old failure fixtures. No CLI/config selector exposed
that choice. Thus B0's corrected engine default could not reach baseline or constrained
book consumers, including G0's unchanged baseline recipe.

The adapter now forwards `ReplayDelistingPolicy::{TerminalReturnV2,AbortV1}`. The default
is `terminal-return`; `--replay-delisting-policy abort` preserves strict legacy rejection.
Both CLI and config files parse the selector. Baseline forwards it, and constrained books
inherit an explicit baseline recipe policy unless overridden. Old recipes without a
policy use the corrected default. Other numerical recipe settings are unchanged.

The independent W0 audit also identified a causal defect inside B0: `scan_last_print`
examined all future prices and `gap_carried` used that result, or a future scheduled
delisting event, to decide today's holdings and NAV. Holding-window returns used the
same future-last-print classification. Appending a later valid close could therefore
change already-realized results.

The engine's default `TerminalReturn` now liquidates at the first missing held close,
using an already-due and available table return, or flagged Shumway if that evidenced
event has an unknown return. Without due and available evidence it applies a distinct
`AssumedMissingPriceAdverse` stress: negative mark return for a long, positive mark
return for a short. Either side loses; a missing data point never manufactures short alpha.
`DelistingEvent.available_period` explicitly gates evidence publication; zero asserts
the row was supplied before the replay began. A future print, future scheduled event,
or future publication cannot turn today's missing close into a carry. Legacy hindsight
behavior remains only as `TerminalReturnExPostV1`, documented as unsuitable for causal
trading replay. The legacy weight-report default is `HoldingIntervalV3`, applying the
same adverse first-missing rule inside the holding interval; `HoldingIntervalV2` retains the
old ex-post diagnostic for reproducibility.

Identified reports bind policy and interpretation into their recipe, write exact
security/session terminal-return evidence to `terminal_returns.csv`, and disclose total
flagged liquidations, signed short-side PnL, assumed liquidation count/PnL, gap carries
and unfilled targets. Assumption-affected reports explicitly declare
`performance_evidence_eligibility=ineligible-assumed-missing-price-liquidation` and
`usable_for_alpha_evidence=false`. Missing
data is explicitly described as a flagged assumption, not a proven delisting. There is
still no identified-report terminal-table or listing-exchange ingestion; the disclosed
stress return is unknown-exchange -55% long, +30% short. The legacy report also exposes
its assumed stress PnL in portfolio-weight units and marks alpha evidence unusable.

## Files changed

- Engine: `book/replay.hpp`, `book/replay.cpp`, `book/report.hpp`.
- Consumer: `config.hpp`, `config.cpp`, `replay_report.cpp`,
  `stage_equity_baseline.cpp`, `stage_equity_book.cpp`, `stage_report.cpp`.
- New tests: `book_w0replay_causal_test.cpp`, `w0replay_config_test.cpp`.
- Existing tests extended or explicitly versioned: `book_w0b0_replay_delist_test.cpp`,
  `replay_report_test.cpp`, `stage_equity_baseline_test.cpp`, `w0b0_legacy_report_test.cpp`.

## Acceptance table

| Acceptance | Verification | Result |
|---|---|---|
| Corrected default reaches identified report, baseline and constrained book | `ReplayReport.DefaultMissingHeldCloseCompletesWithIdentifiedFlaggedTerminalReturn`, `StageEquityBaseline.CorrectedDefaultCompletesOriginalWindowAndConstrainedBook` | MET: final NAV 72.5 on the one-name fixture; original seven baseline/book intervals complete |
| Abort is explicitly selectable and keeps old failure/publication contracts | `ImplReplayPolicy.*`, existing four B-04 failure fixtures with `AbortV1` | MET: CLI/config precedence and original rejection/publication assertions pass |
| Causal prefix unchanged by future prices, future events and late publication | `BookReplayCausal.FuturePricesAndFutureEventsCannotAlterMissingClosePrefix` | MET: bitwise comparison of all tested economic prefix fields for long and short |
| Terminal table evidence only applies at/after publication | `BookReplayCausal.SuppliedReturnOnlyAppliesWhenAvailableAtTheMissingValuation` | MET: available-at-2 uses -90% table; available-at-3 uses -55% assumed stress at period 2 |
| Legacy report does not use future prints and does not undo first-missing liquidation | `BookLegacyReportCausal.*` | MET: future prints leave first-window return -0.505; a short with interior missing print receives +0.43 mark return and loses |
| Unknown missing marks never manufacture short gains, even with repeated re-entry | `BookReplayCausal.UnevidencedMissingPriceStressIsAdverseForEveryVenueAndPosition`, `.RepeatedMissingPricesAndShortReentryCannotManufactureAlpha`, `ReplayReport.DefaultMissingShortCloseSeparatelyDisclosesAssumedStressLoss` | MET: every side/venue loses; three short re-entries reduce NAV 1000 to 681.472; report fixture records -15 assumed PnL and ineligible evidence |
| Existing B0 delay, terminal fallback, borrow, and report accounting remain green | Whole `atx-engine-book-tests`; `BookLegacyReportStage.*` | MET: 128/128 book, 32/32 focused impl |

## Defect table

| Defect | Resolution |
|---|---|
| B-04 consumer integration silently pinned Abort | Corrected default and explicit versioned legacy selector, forwarded through all identified consumers |
| B-04 causal classification used future prints and not-yet-due table rows | First-missing default; future scan restricted to explicit ex-post legacy policy |
| B-03 legacy holding returns used future prints | Causal V3 default with V2 retained explicitly |
| B-04 literal unevidenced negative return creates unsupported short profits | Distinct adverse stress source, signed assumption PnL, explicit alpha-evidence ineligibility |

## Evidence

No TDD: production changes preceded the new acceptance tests. No real-data artifact
was read and no market performance claim is made.

The pool lease automatically attempted `dev` configure and failed because removed
non-equity modules do not exist. The lease remained held; no compile occurred in that
attempt. Explicit configure uses the required `equity-dev` preset and isolated
FetchContent:

```powershell
Set-Location C:\atx-wt\pool-4
$env:CMAKE_BUILD_PARALLEL_LEVEL='1'
& C:\atx-wt\pool-4\scripts\atx-build.ps1 configure -Preset equity-dev -Jobs 1 -Groups book -DFETCHCONTENT_BASE_DIR=C:/atx-wt/pool-4/deps/equity-dev
# exit=0
# -- Build files have been written to: C:/atx-wt/pool-4/build-equity
# Verified cache: FETCHCONTENT_BASE_DIR:PATH=C:/atx-wt/pool-4/deps/equity-dev
# Free RAM before build: 5.62656784057617 GiB.
& C:\atx-wt\pool-4\scripts\atx-build.ps1 build -Preset equity-dev -Jobs 1 atx-engine-book-tests atx-impl-tests atx-shm-worker
```

The cold build exited 0. Because the audit-driven refinement landed while earlier
objects were already compiled, the exact owning-target build was run again before any
tests. That synchronization build exited 0 and rebuilt replay/consumer objects before
linking both test executables. Logs: `w0-replay-build.log`, `w0-replay-sync-build.log`.

The first whole-book run passed 127/128. The sole failure was the newly added fixture
`ReappearanceInsideHoldingWindowDoesNotUndoFirstMissingFallback`: one schedule row
meant a one-session holding window, so its period-2 missing print was outside the
requested window. Test-only commit `4f257729` supplies `{0,4}` and asserts four holding
sessions. The production logic and expected economic result were unchanged.

```powershell
& C:\atx-wt\pool-4\scripts\atx-build.ps1 build -Preset equity-dev -Jobs 1 atx-engine-book-tests
# exit=0
& C:\atx-wt\pool-4\build-equity\bin\atx-engine-book-tests.exe --gtest_brief=1
[==========] 128 tests from 20 test suites ran. (13048 ms total)
[  PASSED  ] 128 tests.
# exit=0
& C:\atx-wt\pool-4\scripts\atx-build.ps1 -Ctest -Preset equity-dev -Jobs 1 -R '^(ReplayReport|StageEquityBaseline|BookLegacyReportStage|ImplReplayPolicy)'
100% tests passed, 0 tests failed out of 32
Total Test time (real) =  41.93 sec
# exit=0
```

Final logs: `build-equity/w0-replay-book-final.log` and
`build-equity/w0-replay-impl-focused.log`. Test-only fixture rebuild log:
`build-equity/w0-replay-fixture-build.log`.

Scoped include verification, after the tests, also passed:

```powershell
& C:\atx-wt\pool-4\scripts\atx-build.ps1 configure -Preset equity-dev -Jobs 1 -Groups book -DFETCHCONTENT_BASE_DIR=C:/atx-wt/pool-4/deps/equity-dev -DATX_USE_PCH=OFF
# exit=0
# ATX_USE_PCH:BOOL=OFF
# FETCHCONTENT_BASE_DIR:PATH=C:/atx-wt/pool-4/deps/equity-dev
# Free RAM before check: 4.73868179321289 GiB.
& C:\atx-wt\pool-4\scripts\atx-build.ps1 check -Preset equity-dev -Jobs 1 atx-engine/src/book/replay.cpp atx-impl/src/config.cpp atx-impl/src/replay_report.cpp atx-impl/src/stage_report.cpp atx-impl/src/stage_equity_baseline.cpp atx-impl/src/stage_equity_book.cpp
[1/7] Building CXX object atx-impl\CMakeFiles\atx-impl-core.dir\src\config.cpp.obj
[2/7] Building CXX object atx-impl\CMakeFiles\atx-impl-core.dir\src\replay_report.cpp.obj
[3/7] Building CXX object atx-impl\CMakeFiles\atx-impl-core.dir\src\stage_equity_book.cpp.obj
[4/7] Building CXX object atx-impl\CMakeFiles\atx-impl-core.dir\src\stage_equity_baseline.cpp.obj
[5/7] Building CXX object atx-impl\CMakeFiles\atx-impl-core.dir\src\stage_report.cpp.obj
[6/7] Building CXX object atx-engine\CMakeFiles\atx-engine.dir\src\book\replay.cpp.obj
# exit=0
```

Logs: `build-equity/w0-replay-pch-off-configure.log` and
`build-equity/w0-replay-pch-off-check.log`. This is a scoped compile of six production
consumers, not a full hygiene build. No new test-only production hooks were added.

Qualification boundary: per the orchestrator's explicit instruction, this lane runs
the whole book executable and focused impl suites. The whole impl executable belongs
to the final integrated root gate with D12 and inference fixes; it is neither claimed
passed here nor waived. The scoped PCH-off compile checked the changed production
header consumers; no full hygiene build or sanitizer claim is made.

## Golden digests and existing expectations

No opaque golden digest was re-pinned. Recipe hashes intentionally change because they
now disclose policy and bind terminal evidence. Existing four B-04 failure fixtures
select explicit `AbortV1`; all their failure/security/date/publication assertions remain.
Four previous B0 gap-carry fixtures select `TerminalReturnExPostV1`, preserving every
legacy arithmetic/trade assertion. The six original unevidenced Shumway cases likewise
remain tested under the explicit ex-post legacy enum. The default PCS synthetic long
keeps the same -30% arithmetic but records source `AssumedMissingPriceAdverse`. The
default short disclosure fixture changes +120 to -120 stress PnL, closing the discovered
unsupported-profit defect. Legacy stage rule disclosure changes from numeric
`2` / `holding_interval_v2` to `3` / `holding_interval_v3_first_missing`; its tested
numerical delisting and borrow results stay unchanged.

## Deviations and integration notes

The orchestrator explicitly expanded ownership to the replay engine, legacy report
header, stage default and related tests after the audit finding. No CMakeLists edits,
no raw worktree creation, no edits to the live checkout, no real data, and no TDD.

**Numerical acceptance deviation, explicitly ruled by the orchestrator:** the literal
B0 requirement that *all* unexplained missing closes receive -30%/-55%, including shorts,
would create profits from data holes. It is retained only in explicit ex-post diagnostic
mode and is NOT claimed met by the corrected default. User priority for tradeable engine
correctness takes precedence: unsupported short marks receive a positive adverse stress
return, always a loss. Causal carry pending actual terminal evidence is a possible future
policy; it is not silently substituted into this first-missing correction.

The first-missing fallback is an adverse modeled liquidation, not verified event
economics. Actual terminal evidence and listing-exchange ingestion are still needed
to remove these flagged assumptions. Ex-post legacy policies must never support a
causal alpha or trading claim. Existing claims-aware replay semantics were not changed.

## Ledger candidates

- I0b's hard-coded Abort suppressed B0's default at every identified replay consumer.
- Future final-print classification changes prefix NAV; only explicit ex-post policies may use it.
- Isolated FetchContent was verified in the completed CMake cache before compilation.
