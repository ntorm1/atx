# Lane W0-I0b review

## Verdict
BLOCK. There is 1 major finding and no blockers. One waiver is needed: the D-12 residual for
cross-sectional operators inside the DSL.

## Reviewed SHA
`95ed4a8823c991195b8aa088cab32466ff08259e` (branch `feat/w0-i0b`, pool-11). The lane diff is
`feat/w0-integration...HEAD`, with merge-base `1a304619`.

## Evidence
All commands were run from `C:\atx-wt\pool-11` with `CMAKE_BUILD_PARALLEL_LEVEL=2`. Free RAM
was 3.19 GB before the build.

```
powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker
[9/11] Linking CXX executable bin\atx-shm-worker.exe
[10/11] Linking CXX executable bin\atx-impl-tests.exe
exit=0
```

Lane suites, run directly from the executable with
`--gtest_filter=ImplConfigBool_*:ImplConfigFinite_*:ImplIcAsOfMembership_*:ImplMineRequiresMembership_*:ImplPendingOrder_*:ImplDelayGuard_*`:
```
[ImplConfigFinite] 44 double flags x 5 non-finite spellings rejected: 220
[ImplIcAsOfMembership] joiner admitted cells: year-union 36, as-of 31 (rejected 5)
[ImplIcAsOfMembership] constant membership: 5 IC csv files byte-identical, evaluation payload 31f3c71cb1f50bcb
[ImplIcAsOfMembership] joiners: n_eligible row0 union 16 as-of 12; row 30 as-of 16
[ImplMineRequiresMembership] train member cells: as-of 20010 vs year-union 23280 (10 joiner ids)
[ImplMineRequiresMembership] trials n_raw=3 pnl_len=643 (train 388 + val 255) clusters=3 sr*_mc=0.0421
[ImplDelayGuard] replay seam: explicit-zero trade cost 0.000000, 5bps/365bps trade cost 0.010084 borrow 0.005994
[==========] 30 tests from 14 test suites ran. (132992 ms total)
[  PASSED  ] 30 tests.
exit=0
```

Whole owning executable (`build-equity\bin\atx-impl-tests.exe --gtest_brief=1`):
```
[  FAILED  ] FundamentalZoo.FixtureParsesTypechecksAndEvaluates (235 ms)
[  FAILED  ] TrialLedgerRepository.ExistingCp14Ledger_StillVerifies (2 ms)
[==========] 563 tests from 115 test suites ran. (463610 ms total)
[  PASSED  ] 556 tests.
[  SKIPPED ] 5 tests.
exit=1
```

Neither failure comes from this lane:
- `TrialLedgerRepository` fails with `CR in line 0`. This is the pre-existing CRLF hazard that
  w0b-integration-notes records as "not for W0b".
- `FundamentalZoo` fails with `acc_ts produced no finite cell`. The test includes only engine
  headers and `serialize_panel.hpp`, none of which this lane owns. The failure follows from
  A0's A-09 flat-window guard. These counts match the lane report exactly.

Regression probe for the major finding (grep of the repo):
```
scripts/canonical-acceptance-run.ps1:88  ... --top-n-by-adv 0 --require-sector 1 --compact-universe 1
scripts/build-tradeable-alphas.ps1:51    ... --adv-windows 5,10,20,60 --require-sector 1 --compact-universe 1
```
`parse_args` (config.cpp:743-751) now consumes only a literal `true` or `false` after a
boolean. The trailing `1` therefore becomes a stray token, and the run fails with "unexpected
argument '1'". The lane's own test `ImplConfigBool_Cli.ANonLiteralTokenAfterABooleanIsNotSwallowed`
asserts exactly this behaviour for `yes`.

## Findings
path:line | severity | problem | required fix
---|---|---|---
atx-impl/src/config.cpp:743-751, :88 | major | I-10 regression, not disclosed. `require-sector`, `compact-universe` and `exclude-no-sector` used to consume the next token as a value. Two committed runbooks rely on that: the canonical acceptance run (`scripts/canonical-acceptance-run.ps1:88`) and the dev-panel recipe (`scripts/build-tradeable-alphas.ps1:51`), both passing `--require-sector 1 --compact-universe 1`. Both now fail at parse time with exit 2. The report's Integration notes and Deviations do not mention it. | Either accept `1`/`0` as boolean literals, on the CLI and in files, in `parse_bool_flag_value` and the `parse_args` look-ahead, with a test pinning `--require-sector 1 --compact-universe 1`. Or, if the flag contract must stay true/false only, add an Integration note naming both scripts so their owner changes them to `true`, and have the orchestrator confirm that before merge.
atx-impl/src/equity_baseline_views.cpp:606-637 (families VM), :235 (feature mask) | waiver | Build item "cross-sectional ops are masked by as-of membership" is only partly met. The as-of mask gates admission: baseline, IC cross-section, families' admitted cells and the mine book. But `rank`, `normalize` and `group_neutralize` inside `kEquityFamilyDsl`, for example `idio_vol_63`, `residual_momentum_252`, `mom_resid_vs_blend` and the earnings families, still normalise over every observed context instrument, which is the year-union set. The report discloses this honestly as DEFERRED to W4-A6. It is infeasible in the owned files: A-12 (`vm.hpp` applies the mask at LoadField, so it resets history) is W4-A6's item. | Owner waiver needed. W4-A6 must feed the `EquityAsOfMembership` flags as the tradable mask for CS ops once the VM splits its data and tradable masks.
atx-impl/src/stage_equity_ic.cpp:1354-1363, :1570-1581 | minor | The published `unclassified_id_count` (previously 29) was removed from `request.json` (`required_mark_audit`) and from `manifest.json` (`terminal_evidence`). Only the ledger entry carries the computed count (:1059). A reader of the published outputs loses the R-A partition count without notice. | Publish the computed `unclassified` count in both places, not just in the ledger.
atx-impl/src/stage_equity_ic.cpp:640-652 | minor | A `terminal_return` row is priced on the security's last finite raw close anywhere in the evaluated view. The engine applies the terminal leg to any forward gap (cross_section_ic.cpp:392-404). If a security has an interior halt and then resumes trading, a cell whose horizon ends inside the halt gets a leg priced off a close after that horizon, which is a look-ahead. Frozen value rows do not depend on a later close. This is latent until W2-D2 supplies real rows. | Refuse (or skip) return rows when the view has a finite raw close after an interior gap, or state the "final observation is the delisting" precondition in the table contract and check it.
atx-impl/src/config.cpp:770 + atx-impl/src/dispatch.cpp:137-153 | minor | `parse_args` runs `validate_cross_flags` before the `--config` merge. A CLI `--replay-execution-delay 0` together with `allow-same-close=true` in the config file is therefore refused, even though the merged result is valid. This fails closed, and it matches the pre-existing `--resume`/`--run-db` pattern. | Defer the cross-flag pass until after the merge when `--config` is present, or document that the opt-in must be on the CLI.
atx-impl/src/stage_equity_mine.cpp:1283 | minor | In equity-mine, `--allow-same-close` takes no value, so `--allow-same-close true` is rejected ("unexpected argument 'true'"). The dispatch usage text says boolean flags take an optional `true`/`false`. It fails closed. | Accept an optional literal `true`/`false` here as the main parser does, or scope the usage text.
atx-impl/src/stage_equity_mine.cpp:787-805 | minor | The train DSR moved from raw-N to cluster-N (as the E0b note requires), but the stage offers no switch to reproduce the pre-W0 raw-N DSR. RULES §2 asks for old behaviour behind a versioned selector. `dsr_train` is report-only and is not used for selection. | Add a `--dsr-rule` (or similar) that selects `summary-raw-n` explicitly, or record a waiver.
atx-impl/tests/w0i0b_pending_order_test.cpp:166-176 | minor | The I-17 stage-level test checks only the final state, which old code also produced. The publish-before-release order is proven only by the helper unit test, plus code reading of the three call sites. | Optional: inject a manifest-write failure at stage level, or accept it on the strength of the helper test.
atx-impl/tests/stage_equity_mine_cli_test.cpp:224-229 | minor | This existing expectation (n_raw 12 → 9) was re-pinned for A-01, which is A0's defect, not one cited to I0b. The change adds an assertion (`degenerate == 3`) rather than loosening one, and the report discloses it. | None required. The orchestrator should confirm the A0 attribution on `feat/w0-integration`.

## Checked
- [x] .agents/cpp/agent.md §10 checklist applied to the diff. UB/bounds: `asof_member_cells` indexes stay within the validated context shape. The `equity_membership_cut_index` band maths fits u32. The `build_cells` reverse loop is well formed. Lifetimes: the as-of set is moved into `profile.views` before evaluation. Error paths: JSON `.at()` throws are caught by the stage wrappers (stage_equity_ic.cpp:1886). The one enum switch (`equity_membership_rule_label`) is exhaustive. `[[nodiscard]]`/`noexcept` are in place. The build is /W4 /WX clean (exit 0).
- [x] Every plan acceptance item was re-run and its test code read. Mid-year joiner: the views and IC-stage tests would fail on the old code, whose union admitted all 16. Constant membership is identical to the year-union path: the evaluation payload and 5 CSVs are byte-compared across the two paths. The `as-of` labels are asserted in the baseline recipe and in the IC summary and manifest. All three are MET.
- [x] Every cited ID was checked in the code:
  - D-12: CLOSED for admission. The DSL CS-op residual is the waiver above.
  - I-10: CLOSED. There is a script regression, the major finding.
  - I-11: CLOSED on the identified path. The legacy `stage_report.cpp` path belongs to B0 and has an Integration note.
  - I-12: CLOSED. All 44 double flags go through `parse_double`, which has the `isfinite` guard.
  - I-15: interface CLOSED. The data is DEFERRED to W2-D2.
  - I-16: CLOSED.
  - I-17: CLOSED at all three commit sites.
  - I-23: CLOSED. The knobs are recorded but still hardcoded, as the brief asks.
  - E-18: CLOSED.
  - B-02: impl half CLOSED at config, the replay seam, baseline, book, IC and mine. The engine forward goes through the `requires` expression, pending B0.
  - D-02: default CLOSED. It has no consumer in atx-impl; the report discloses this.
- [x] Diff stays inside the brief's files in scope, plus new `w0i0b_*` tests and existing-test expectation edits. The `replay_report.cpp` delay guard goes beyond the I-11 site. It is disclosed, sits in the same function, and has no other W0 owner, so it is not treated as a breach.
- [x] No test was weakened. The pre-existing test diffs were each checked. The one removed block (the missing audit failing, `stage_equity_ic_test.cpp:535`) is tied to I-15, and its replacement asserts that the absent audit is recorded. The rest are explicit opt-ins tied to B-02/I-11/E-18/D-02/E-09, or strengthened assertions. There are no DISABLED_ tests and no new GTEST_SKIP.
- [x] Numeric default changes stay reproducible: `--min-names-per-date 2`, `--ic-execution-delay 0 --allow-same-close`, `--ic-block-len-rule half-horizon-v1`, `--membership-rule year-union-v1`, `--si-publication-lag-rule calendar-days-v1`. The one exception is the mine DSR rule (minor). No golden digest was re-baselined.
- [x] Causality-harness registration: n/a in W0.
- [x] The work is wired into the real stages: `run_equity_baseline`, `run_equity_ic`, the equity-mine dispatch, `replay_config` and `dispatch`. The owning executable runs whole, with only the two failures above, which are not from this lane.
- [x] The report's evidence matches the claims. The build, the 30/30 lane suites, the 563/556/5/2 whole-executable counts and every printed measurement were reproduced.

## Re-review 1

Reviewed SHA: `190c2fb4c0832d60a11fe40b75e1c91e92b705b2` (fix commit `190c2fb4` over `95ed4a88`).
Verdict: **APPROVE**. The one major is fixed, all but one of the minors are fixed, and nothing regressed.

### Evidence
- Build: `atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker`, with `CMAKE_BUILD_PARALLEL_LEVEL=2` and 2.08 GB free RAM at the start. Exit 0. The atx-impl objects were already up to date for the clean tree (/W4 /WX), so only a relink ran.
- Affected suites, run directly from the exe. The filter was `ImplConfigBool_*:ImplConfigFinite_*:ImplIcAsOfMembership_*:ImplMineRequiresMembership_*:ImplPendingOrder_*:ImplDelayGuard_*:StageEquityIc*:StageEquityMine*:CliSmoke*:ConfigEquity*`. Result: **53/53 passed**. It printed `dsr rules: n_raw=4 n_eff=1.6143, 4 scored rows compared, 1 raised`.
- Whole owning executable (`atx-impl-tests.exe --gtest_brief=1`): **567 tests ran: 560 passed, 5 skipped, 2 failed**, exit 1. Before the fix the counts were 563 ran and 556 passed, with the same 5 skipped and 2 failed. The 4 extra tests are the new fix-pass tests. The two failures are the same pre-existing, non-lane ones as before: `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` and `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` (the CRLF issue).
- Test diff `95ed4a88..HEAD`: 239 lines added and 1 line removed. The removed line is the `write_context` signature, which gained a defaulted `halt` parameter. No assertion was loosened, no test was disabled or skipped, and no golden value was re-baselined.

### Per finding
1. **major, bool 1/0 literals: FIXED.**
   - `is_bool_literal` (true/false/1/0) drives the `parse_args` look-ahead, and `parse_bool_flag_value` maps 1 to true and 0 to false for both the CLI and files.
   - `ImplConfigBool_Cli.CommittedRunbooksNumericBooleanLiteralsStillParse` pins both runbooks' exact flag sets (`--require-sector 1 --compact-universe 1`). It also covers `0` meaning false, the refusal of `2` and `yes`, and the literals in a file. It passes.
   - A scan of the committed scripts turned up no other boolean flag followed by a non-literal value.
2. **minor, unclassified count: FIXED.**
   - A single helper, `unclassified_mark_count`, feeds the ledger, `request.json` (`required_mark_audit.unclassified_id_count`) and `manifest.json` (`terminal_evidence.unclassified_id_count`).
   - `StageEquityIc` pins 29 in both files, and the as-of test pins 0 in both files and in the ledger.
3. **minor, resumed-halt look-ahead: FIXED (refused).**
   - `build_cells` finds the last priced close for a `terminal_return` row. It then returns Err if an earlier priced close exists before an interior gap.
   - Leading NaNs from a mid-window listing are correctly not treated as a gap.
   - The precondition is stated in the `stage_equity_ic.hpp` table contract.
   - Test `TerminalReturnRowAfterAResumedHaltIsRefused` passes. The halted return row is refused, while a real delisting's return row and a value row both run.
   - The default frozen 2013 table has value rows only (`equity_ic_frozen_terminal_table` sets `terminal_value` only), so the canonical path is unaffected.
4. **minor, cross-flag pass vs `--config`: FIXED.**
   - `parse_args` skips the pass only when `config_file` is set. `dispatch.cpp:137-153` always runs `validate_cross_flags` after the merge. Subcommands that reject `--config` still exit 2 before stage routing.
   - `dispatch` is the only non-test caller of `parse_args`; equity-mine has its own parser.
   - Test `AConfigFileOptInCountsForACliZeroDelay` covers three cases: the file opt-in is accepted, a missing opt-in is refused, and a CLI `false` beats the file's `true`.
5. **minor, equity-mine `--allow-same-close [true|false|1|0]`: FIXED.**
   - The flag consumes one literal, validated through `parse_bool_flag_value`, and the usage text now says `true|false|1|0`.
   - The extended `DelayZeroNeedsAllowSameClose` test covers `true` and `1`, `false` and `0`, a following `--quiet` that is not swallowed, and `maybe` refused as a stray argument.
6. **minor, versioned DSR selector: FIXED.**
   - The new flag is `--dsr-rule cluster-mc-floor-v2|summary-raw-n-v2|summary-n-eff-v1`. I checked `summary-n-eff-v1` against base `458d0bef`: the pre-W0 `deflated_sharpe(sr, TrialSummary, ...)` used `expected_max_sharpe_eff(n_eff, var_sr)`, which is exactly `eval::SummaryDsrRule::NEffCrossVarV1`. So the pre-W0 rule really was n_eff (the original finding's "raw-N" wording was inexact), and it is now reproducible.
   - The default path (cluster rule, falling back to raw-N) is unchanged from `95ed4a88`.
   - The requested rule and the applied rule are both recorded.
   - The test proves that only `dsr_train` differs and that the admitted set is identical.
   - Disclosed caveat: E-16 widened the registry calendar, so n_eff inputs can move. The rule itself is reproducible.
7. **minor (optional), I-17 stage-level test: NOT FIXED. Accepted.** A stage-level test would need a failure seam in production code. The helper unit test and the three call sites carry the proof. It was optional.
8. **minor, A-01 re-pin: no change needed.** The orchestrator still needs to confirm the A0 attribution on `feat/w0-integration`.

### New issues from the fix
None blocking. One observation, which is not a finding because this fix did not introduce it: the halt guard refuses the whole run rather than skipping the row. With real W2-D2 data, one vendor gap in a return-row security's closes stops the stage. This fails closed. W2-D2 should know about it when it supplies real rows.
