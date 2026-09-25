# Lane W0-FIXUP review (fresh adversarial)

## Verdict

**APPROVE** (0 blocker, 0 major, 3 minor).

## Reviewed SHA

`4d26defa319230809e9895d023f237a97c84bd9e` (branch `feat/w0-fixup`, merge base with
`feat/w0-integration` = `16a2ae35`). Lane changes (`diff feat/w0-integration...HEAD`):
`.gitattributes`, `atx-impl/tests/fundamental_zoo_test.cpp`, `.superpowers/sdd/w0/lane-fixup-{brief,report}.md`.

## Evidence

All commands were run in `C:\atx-wt\pool-2` with `CMAKE_BUILD_PARALLEL_LEVEL=2`. Free RAM was
checked before each build (2.25 GB and 2.23 GB).

- `atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker` exited 0 (/W4 /WX).
  `build ... atx-engine-{alpha,factory,learn,data,eval,combine,risk,book}-tests` exited 0.
- Run from the repo root: `atx-impl-tests.exe --gtest_filter=FundamentalZoo.*:TrialLedgerRepository.*`
  exited 0. `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` OK (295 ms).
  `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` OK. `RealDataIcReport` is an opt-in skip.
- `atx-impl-tests.exe --gtest_brief=1` (whole, repo root) exited 1. 533 ran: 526 passed,
  5 skipped, and 2 failed. The failures are `StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput`
  (ic.csv 31361 vs 32001 lines, `dates_emitted` 6 vs 7, `common_prefix_gaps` 1, `ic_mean` null)
  and `EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials` (`n_raw` 9 vs 12). The orchestrator
  exempts both as owned by I0b.
- Engine executables, whole, each with `--gtest_brief=1`, all exit 0:

  | Executable | Passed | Skipped |
  |---|---|---|
  | alpha | 704/704 | 0 |
  | factory | 299/299 | 0 |
  | learn | 193/193 | 0 |
  | data | 238 | 14 (environmental) |
  | eval | 251/251 | 0 |
  | combine | 183/183 | 0 |
  | risk | 470 | 1 (nightly) |
  | book | 90/90 | 0 |

- `git ls-files --eol -- '*.jsonl' CMakePresets.json` with `core.autocrlf=true`:
  - All 11 LF-blob jsonl files show `i/lf w/lf attr/text eol=lf`, including `atx-engine/reviews/trial-ledger.jsonl`.
  - Both tier1-parity archive files show `i/crlf w/crlf attr/-text`.
  - `CMakePresets.json` shows `i/lf w/crlf attr/` (its CRLF working copy is kept).
  - `git diff feat/w0-integration...HEAD --stat -- '*.jsonl'` is empty, so no committed blob changed.
- Independent IEEE-double restatement (Python, scratch) of `synthetic_records` against
  `fundamental_fields.hpp:325-327` (`accruals = (ni - cfo) / (0.5*(assets+assets_ly))`):
  - Old fixture (CFO = 12s): the worst per-instrument relative spread of accruals over the 5
    quarters is 6.0e-16. That is far below `kTsFlatRelTol = 1e-10` (`ts_ops.hpp:146,151-153`), so
    every 252-window is flat. `ts_zscore` is then NaN (0/0) and `rank(-1*...)` is NaN
    everywhere, which is the failure.
  - New fixture (CFO = (12+q)s): the relative spread is 0.059 to 0.354, a genuine variation.
- `stage_equity_ic.cpp:1171` still reads `common_sample_dates = dates - kHorizons.back()` with no
  label embargo, and the I0b brief owns `stage_equity_ic.*`. The I0b re-pin commit `a28a6c1e`
  exists on `feat/w0-i0b`.
- `git merge-tree --write-tree feat/w0-integration HEAD` is clean. Integration has advanced to
  `2170a259` (B0 merged). No B0 file overlaps this lane's files.

## Findings

| path:line | severity | problem | required fix |
|---|---|---|---|
| `.superpowers/sdd/w0/lane-fixup-report.md:40-47` | minor | The report has no RULES §4 "Acceptance table" (brief item → test → measured → MET/UNMET) and no "Defect table". A per-failure table covers the same content, but acceptance items 1-5 are never marked MET one by one. | Add a 5-row acceptance table and mark A-09 as "not reopened (guard correct)" in a defect row. |
| `.superpowers/sdd/w0/lane-fixup-report.md:21-27` | minor | The lane did not do the RULES §3 pre-merge of the current `feat/w0-integration` (now `2170a259`, B0 merged). Its results are measured on `16a2ae35`. `merge-tree` is clean and no files overlap. | Merge `feat/w0-integration` and re-run `atx-impl-tests` whole at integration time (orchestrator gate). |
| `atx-impl/tests/fundamental_zoo_test.cpp:165-186` | minor | The root-cause proof ("old fixture flat → NaN under RelativeV2, finite under NoneV1") was a deleted diagnostic. Nothing committed pins that the zoo fixture's time-series ratio inputs are non-degenerate, so a future fixture edit could silently reintroduce flat windows and pass only through other lines. The A-09 guard itself is pinned in the alpha exe. | Optional: add an assertion that `ts_std(accruals, 252)` has a finite cell greater than 0 on the fixture panel. |

## Checked

1. **agent.md §10 on the diff.** The only code change is a test-fixture constant:
   `(12.0 + static_cast<f64>(q)) * s`. It has an explicit usize→f64 cast, no narrowing, no UB and
   no lifetime or bounds issues, and it builds clean under /W4 /WX. `.gitattributes` has no C++
   impact.
2. **Acceptance items.**
   1. MET. The ledger test passes from the repo root with autocrlf=true, the LF checkout is
      verified with `ls-files --eol`, no blob changed, and CMakePresets.json stays `w/crlf`. The
      `-text` exception for the two CRLF-blob archive files is justified: without it they would
      show as modified.
   2. MET. The failure's root cause is fixture degeneracy, which I reproduced independently. The
      guard is only consulted in var, lin_fit, zscore, skew, kurt, corr and regression, so it is
      not over-broad here. The expectation is unchanged, the zoo expression is unchanged, and the
      fixture fix gives accruals genuine variation.
   3. MET. Both I0b failures are reproduced with the stated root cause (`stage_equity_ic.cpp:1171`;
      the all-tie `rank(volume)` from A-01). I0b owns them, and the lane did not touch them.
   4. MET. All 8 engine executables are green whole. `atx-impl-tests` has only the 2 I0b failures.
   5. MET. No test was weakened, and the report exists.
3. **Defects.** A-09 was correctly not reopened: the guard at `ts_ops.hpp:151-153` is sound for
   this case. The report's note on the ResearchFast Welford path is plausible and is honestly
   deferred to W1-A1.
4. **Ownership.** Every changed file is within the brief (`.gitattributes`, a test fixture shown
   to be wrong, and the lane's sdd files).
5. **Tests weakened.** None. The only edit to a pre-existing test file changes synthetic input
   data. No assertion, tolerance or skip changed.
6. **Numeric behaviour.** No production numeric behaviour changed and there is no golden-digest
   re-baseline. The legacy `FlatGuard::NoneV1` path is untouched.
7. **Causality harness.** Not applicable (W0).
8. **Real work.** The fix is on the real test path, and the owning executables pass whole (see
   Evidence).

## Re-review 1

### Verdict

**APPROVE** (0 blocker, 0 major, 0 minor open). All three original minors are FIXED, and the fix
commits introduce no regression and weaken no test.

### Reviewed SHA

`a8eff5318a10b554107f23b886a947a56e5916c4` (`feat/w0-fixup`). Fix commits since `4d26defa`:
`dfcb1721` (original review), `dc821303` (merge of `feat/w0-integration` @ `2170a259`, B0),
`a8eff531` (fix pass 1: new test + report tables). `a8eff531` touches only
`atx-impl/tests/fundamental_zoo_test.cpp` (+38, additions only) and `lane-fixup-report.md`.

### Evidence

All commands were run in `C:\atx-wt\pool-2` with `CMAKE_BUILD_PARALLEL_LEVEL=2`. Free RAM before
the build was 3.34 GB.

- Merge purity: `git merge-tree --write-tree dfcb1721 2170a259` = `a0f53aa1`, which equals
  `dc821303^{tree}`. The merge has no hand edits. `2170a259` is an ancestor of HEAD. The working
  tree is clean.
- `atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker atx-engine-{alpha,factory,learn,data,eval,combine,risk,book}-tests`
  exited 0 (/W4 /WX).
- Eight engine executables, whole, `--gtest_brief=1`, all exit 0: alpha 704/704, factory 299/299,
  learn 193/193, data 238 + 14 environmental skips, eval 251/251, combine 183/183, risk 470 + 1
  nightly skip, book 122/122.
- `atx-impl-tests.exe --gtest_brief=1` (whole, repo root) exited 1. 536 ran: 517 passed, 5
  skipped, 14 failed. The failing set exactly matches the report's fix-pass-1 table:
  - The 2 I0b failures that the orchestrator exempts (`StageEquityIc.TwoRuns...`,
    `EquityMineCli.SmoothWindows...`).
  - 12 failures that arrived with the B0 merge: 8 B-02 (`ReplayPolicyStage` x2, `ReplayReport` x5,
    `StageEquityBaseline.ExplicitZeroCosts...`) and 4 B-04 (`ReplayReport.MissingHeldPrice...`,
    `StageEquityBaseline` x3).

  B0's own report (`lane-b0-report.md:75,270`) already records these 12 as UNMET pending I0b
  wiring of `allow_same_close` and the delisting policy, and the orchestrator merged B0 into
  `feat/w0-integration` with them present. They are not caused by this lane: its code delta against
  integration is only `.gitattributes` plus test-fixture data and one new assertion-only test in
  `fundamental_zoo_test.cpp`.
- `atx-impl-tests.exe --gtest_filter=FundamentalZoo.*:TrialLedgerRepository.*` (repo root)
  exited 0. Results: `FixtureParsesTypechecksAndEvaluates` OK, `FixtureAccrualsVaryOverTime` OK,
  `EpochDayMatchesKnownDates` OK, `ExistingCp14Ledger_StillVerifies` OK, and `RealDataIcReport` an
  opt-in skip.
- `git ls-files --eol`: `trial-ledger.jsonl` is `i/lf w/lf attr/text eol=lf` and
  `CMakePresets.json` is `i/lf w/crlf`, both unchanged.

### Per-finding

| # | Finding | Status | Evidence |
|---|---|---|---|
| 1 | No RULES Sec. 4 acceptance/defect tables | **FIXED** | `lane-fixup-report.md` "Acceptance table" has 5 rows, each with its test, a measured result and MET. Item 4 honestly lists the 14 `atx-impl-tests` failures, each attributed. "Defect table" has A-09 NOT REOPENED (guard correct; ResearchFast Welford path deferred to W1-A1) and CRLF CLOSED. |
| 2 | No pre-merge of current `feat/w0-integration` | **FIXED** | `dc821303` merges `2170a259` with no conflicts and no hand edits (tree matches `merge-tree`). All ten targets rebuilt and every executable re-run whole on the merged tree. My re-run reproduces the report's numbers exactly (engine exes above; impl 536/517/5/14). |
| 3 | Nothing pinned fixture non-degeneracy (optional) | **FIXED** | New `FundamentalZoo.FixtureAccrualsVaryOverTime` (`fundamental_zoo_test.cpp:388-424`). It requires `ts_std(accruals,252)` to have a finite cell and a cell > 0, the maximum `ts_std/|ts_mean|` to exceed 1e-3, and `ts_zscore(accruals,252)` to have a finite cell. It evaluates on the same `extended(synthetic_panel, ..., synthetic_records)` panel as the zoo test. It would fail on the degenerate fixture: the diagnostic in the original report shows `ts_std` there is exactly 0 in all 336 finite cells under the default policy, so `positive_sd` = 0. This agrees with the lane's recorded mutation run. `<algorithm>`/`<cmath>` are included, and it builds under /W4 /WX. |

### Regression / weakening check

The diff of `a8eff531` is additions only: one new TEST, with no existing assertion, tolerance or
skip changed. The merge carries B0's already-reviewed and approved content (B0 re-review 1
APPROVE) unchanged. No new findings.
