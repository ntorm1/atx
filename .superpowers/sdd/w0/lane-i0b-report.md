# Lane W0-I0b report — Equity stages and config hygiene

## Outcome

**DONE**, with one residual deferred. All three plan Accept items are MET with named tests and
measured numbers. Ten of the eleven cited IDs are CLOSED inside the owned files. The eleventh,
D-12, is CLOSED at every place where the equity stages decide eligibility: baseline admission,
the IC cross-section and the mine book. One residual is DEFERRED to W4-A6 (owner of `alpha/vm.hpp`):
cross-sectional operators *inside* a DSL expression, such as `rank(x)`, still normalise over the
context's inherited mask, which is the year-union.

Masking the VM data loads with the as-of mask would blank the time-series history that joiners
need, so it cannot be done from the owned files. The VM first needs separate data and tradable
masks, which is W4-A6's Build item.

Owning targets `atx-impl-tests` and `atx-shm-worker` build clean under /W4 /WX. The 30 lane
tests pass, both in the anchored ctest run and in the whole-executable run.

The whole executable has two failures, and neither comes from this lane:

- `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` already failed before this lane. It is
  the CRLF hazard named in the W0b integration notes.
- `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` started failing only when the
  post-review pre-merge brought in W0-A0's kernel fixes. `acc_ts = rank(-1 * ts_zscore(accruals,
  252))` now yields no finite cell under A-09's flat-window guard. The file is not owned by this
  lane; see Integration notes.

## Branch / SHA / base / pool

- Branch `feat/w0-i0b`, worktree `C:\atx-wt\pool-11`, lease run id `aes-w0-i0b`. The orchestrator
  holds the lease, and this lane never ran `lease-worktree.ps1`.
- W0 base: `458d0bef480a624e258070c9d45174a9984466bf`.
- Commits:
  - `420138c0`: first merge of `feat/w0-integration` (E0a, E0b, D0, L0, O1).
  - `4b4a48c6`: implementation plus tests.
  - `2cf7d63f`: 100-column wraps.
  - `856bb2a4`: pre-review merge of `feat/w0-integration` at `1a304619` (A0).
  - `a28a6c1e`: re-pin of one mine test after A0's A-01 fix.
  - The report commit on top of these. The final HEAD is the SHA the lane returns.
- Test groups: the `build-equity` cache is `data`, and the lane's groups are n/a (atx-impl), so no
  reconfigure was needed.

## Files changed

Only owned files, plus existing-test expectation edits (listed below with defect IDs) and new
`w0i0b_*` test files. No CMake edits. No new `src/*.cpp`.

| File | Change |
|---|---|
| `atx-impl/src/config.hpp` | `si_publication_lag` default 2 → **7** (NYSE sessions) + `si_publication_lag_rule="nyse-sessions-v2"` (D-02). New fields `allow_same_close`, `equity_membership`, `equity_membership_rule="as-of-v2"`, `equity_ic_min_names_per_date=50`, `equity_ic_execution_delay=1`, `equity_ic_block_len_rule="two-horizon-v2"`, `equity_terminal_returns`. Declarations for `validate_execution_delay`, `validate_cross_flags`, `parse_bool_flag_value`, `subcommand_rejects_config`. |
| `atx-impl/src/config.cpp` | One `kBoolFlags` table (35 flags). Booleans parse `true`/`false`: on the CLI a bare flag means true and only a literal `true`/`false` is consumed as its value; in files the value is required and anything else is rejected (I-10). Every double flag rejects NaN and ±inf (I-12). New flags `--membership`, `--membership-rule`, `--terminal-returns`, `--ic-block-len-rule`, `--min-names-per-date` (≥2), `--ic-execution-delay`, `--si-publication-lag`, `--si-publication-lag-rule`, `--allow-same-close`. A nested `config=` is refused. `validate_cross_flags` runs at the end of parsing and after a config merge. `validate_execution_delay` refuses delay < 1 without `--allow-same-close` (B-02). |
| `atx-impl/src/dispatch.cpp` | `--config` is merged for every subcommand. `equity-ic` and `equity-universe`, the frozen-recipe stages, refuse it (exit 2, "--config is not accepted for this subcommand") (I-10). Cross-flag validation runs after the merge. Usage text updated. |
| `atx-impl/src/replay_report.cpp` | I-11 site. An identified report requires both rates to be explicit choices (nonzero, or named in `set_flags`), so 0 is allowed only when it is deliberate. Delay 0 needs `allow_same_close`, which is forwarded to `ReplayConfig::allow_same_close` through a `requires`-expression once B0's field exists. |
| `atx-impl/src/equity_baseline_views.{hpp,cpp}` | `EquityMembershipRule {ContextYearUnionV1=1, AsOfV2=2}`; `EquityAsOfMembership` (upper-bound lookup on effective session keys); `equity_asof_membership`, `equity_membership_cut_index`, `load_equity_membership` (sha256 of bytes), `resolve_equity_membership` (hash-bound to the context recipe's `universe_membership_sha256` / `universe_cut`); `publish_manifest_then_release_pending` (I-17). Eligibility = context mask ∧ as-of member ∧ readiness; `membership_rejected_cells` counted; the memory budget includes the as-of flags (D-12). |
| `atx-impl/src/stage_equity_baseline.cpp` | Flags `--membership`, `--membership-rule`, `--allow-same-close`. Delay-0 guard. Additive recipe keys `membership_mask` and `decision_eligibility`, for membership contexts only. `membership_rejected_cells` in readiness. The manifest is written before `.pending` is removed, on both commit paths (I-17). |
| `atx-impl/src/stage_equity_book.cpp` | `--allow-same-close`, delay-0 guard (inherits the baseline delay), manifest before `.pending` (I-17). |
| `atx-impl/src/stage_equity_ic.{hpp,cpp}` | `min_names_per_date` is a parameter, default 50 (E-18). E0a wiring: default delay 1, bootstrap `TwoHorizonV2`, HAC `HansenHodrickV1`, `common_sample_dates = T - label_embargo(maxH, delay)`, alignment label parameterised by delay, block-rule text and ID recorded. The legacy path (`HalfHorizonV1` + delay 0 + `--allow-same-close`) stays reachable. Terminal-return table interface (`EquityTerminalReturnTable`, CSV `security_id,terminal_value,terminal_return,special_dividend,record_date,evidenced,source`, `--terminal-returns`) with the frozen 2013 table as default. The 2013 audit is optional: absent means recorded as absent, present means fully checked (I-15). The as-of membership rule is resolved from the context recipe. A baseline and IC with mismatched rules are refused. Summary and manifest carry `membership`, `alignment`, `execution_delay`, `min_names_per_date`, `block_len_rule`, `terminal_table`. `trial_ledger.hpp` is **not** edited; see Deviations. |
| `atx-impl/src/stage_equity_mine.{hpp,cpp}` | `--membership` is required under the default `as-of-v2`. `--membership-rule year-union-v1` is the explicit legacy fallback and refuses a `--membership` (D-12 / I-16). Delay-0 guard with `--allow-same-close` (B-02). E0b wiring: `pnl_len = train + validation` sessions, `TrialMeta{window, fidelity 0, InSample, family, theme}`, cluster-N DSR via `accounting()` (cluster-mc-floor-v2) with a recorded fallback, `chain_head` in `gate_report` and `manifest` (`trial_registry_chain_head`). `gate_report.config` records `nw_lags`, `ic_horizons`, `periods_per_year`, `allow_same_close`, `membership_rule` (I-23). |
| `atx-impl/tests/w0i0b_support.hpp` (new) | Byte-exact ATXPITU1 `membership.bin` writer (fnv1a64 trailer), dispatch helper. |
| `atx-impl/tests/w0i0b_config_test.cpp` (new) | `ImplConfigBool_{Cli,File,Dispatch}`, `ImplConfigFinite_{Doubles,SiLag}`, `ImplDelayGuard_Config`. |
| `atx-impl/tests/w0i0b_ic_asof_membership_test.cpp` (new) | `ImplIcAsOfMembership_{Views,Stage,TerminalTable}`. |
| `atx-impl/tests/w0i0b_mine_membership_test.cpp` (new) | `ImplMineRequiresMembership_{Cli,Tags}`, `ImplDelayGuard_Mine`. |
| `atx-impl/tests/w0i0b_pending_order_test.cpp` (new) | `ImplPendingOrder_Stages`, `ImplDelayGuard_Stages`. |

### Existing-test expectation changes (each pins a cited defect)

| Test / fixture | Change | Defect |
|---|---|---|
| `config_megabook_flags_test.cpp:93` | `si_publication_lag` default expectation 2 → 7 | D-02 |
| `config_replay_test.cpp` explicit-zero case | adds `--allow-same-close` to the delay-0 invocation | B-02 |
| `replay_policy_stage_test.cpp` fixture | `allow_same_close=true`; borrow (and trade, when planning cost is 0) named in `set_flags`, so frictionless/same-close is an explicit choice | B-02, I-11 |
| `replay_report_test.cpp` `inputs()` fixture + `RejectsIgnoredLegacyRates…` | `set_flags {replay-trade-bps, replay-annual-borrow-bps}`, `allow_same_close=true`; the implicit-fee probe erases `replay-trade-bps` first | I-11, B-02 |
| `stage_equity_baseline_test.cpp` explicit-zero/delay-0 case | adds `allow_same_close` + flag | B-02 |
| `stage_equity_ic_test.cpp` | fixture opts into `min_names_per_date=2` (6-name synthetic) | E-18 |
| `stage_equity_ic_test.cpp` | row counts `T-h` → `T-h-1`; alignment label → `…return-from-entry-close-t-plus-1…`; `common_sample_dates` / `dates_emitted` `T-63` → `T-64` (n 7 → 6 at h=63) | E-09 wiring (E0a note) |
| `stage_equity_ic_test.cpp` `RequiredMarkAuditFailures…` | the "missing audit fails" block removed (missing audit is now allowed and recorded); the present-audit checks are kept unchanged | I-15 |
| `stage_equity_mine_cli_test.cpp` `base_args` | adds `--membership-rule year-union-v1` (fixture has no image; the legacy rule is now an explicit opt-in); its `membership_rule` label expectation is unchanged | D-12 / I-16 |
| `stage_equity_mine_cli_test.cpp` `SmoothWindows…` | `n_raw` 12 → 9 and `counts.degenerate == 3`: the fixture volume is a constant 1e6, and A0's average ties make `rank(volume)` and its two decayed forms all-tie, so they are degenerate | A-01 (A0, surfaced by the pre-review merge) |
| `panel_pipeline_test.cpp` report | `set_flags {replay-trade-bps, replay-annual-borrow-bps}` (frictionless by explicit choice) | I-11 |
| `stage_run_megabook_test.cpp` `base_cfg` | same explicit 0/0 choice; digests unchanged | I-11 |

## Acceptance table

| Accept item | Test(s) | Measured | Status |
|---|---|---|---|
| A mid-year joiner is invisible before its effective session | `ImplIcAsOfMembership_Views.MidYearJoinerIsInvisibleBeforeItsEffectiveSession`; `ImplIcAsOfMembership_Stage.MidWindowJoinersAreOutOfTheIcCrossSectionUntilEffective`; `ImplMineRequiresMembership_Cli.AsOfImageRunsAndTheYearUnionIsOnlyAnExplicitFallback` | views: admitted cells year-union **36** vs as-of **31** (5 joiner cells before the effective session rejected). IC stage: `n_eligible` on row 0 is year-union **16** vs as-of **12**, and as-of reaches **16** at the join row (30); `membership_rejected_cells` = 120. mine: train member cells as-of **20010** vs year-union **23280** (10 joiner ids) | MET |
| With constant membership, output is identical to the year-union path | `ImplIcAsOfMembership_Views.ConstantMembershipIsBitIdenticalToYearUnion`; `ImplIcAsOfMembership_Stage.ConstantMembershipOutputIsIdenticalToTheYearUnionPath` | evaluation payload bit-identical (digest `31f3c71cb1f50bcb` both paths); **5** IC CSV files byte-identical (ic, coverage, ic_decay, signal_autocorr, quantile_spread) | MET |
| Output labels change to `as-of` | `ImplIcAsOfMembership_Stage.ConstantMembership…` (label block); `ImplMineRequiresMembership_Cli.AsOfImageRuns…` | baseline recipe `membership_mask.rule = "as-of-pit-membership-v2"`, `decision_eligibility = "inherited-context-mask-intersect-as-of-membership-intersect-common-readiness"`; IC summary/manifest `membership.rule = "as-of-pit-membership-v2"` (year-union run: `"context-year-union-v1"`); mine `train.membership_rule = "as-of-pit-membership"` vs `"context-year-union-not-as-of"` | MET |

### Build items → evidence

| Build item | Test(s) | Measured / asserted |
|---|---|---|
| Booleans parsed; `--config` everywhere or rejected | `ImplConfigBool_Cli.*`, `ImplConfigBool_File.*`, `ImplConfigBool_Dispatch.*` | `metabook=false` in a file leaves it off, and garbage is rejected. A CLI `false` wins the merge. A non-literal token after a bool is not swallowed. `equity-ic` / `equity-universe` with `--config` exit 2. Merged files meet the same cross-flag rules. |
| `isfinite` on every double flag | `ImplConfigFinite_Doubles.*` | **44** double flags × **5** spellings (`nan`, `NaN`, `inf`, `-inf`, `infinity`) = **220** rejections on CLI and in files; `--holdout-frac nan` no longer disables the holdout |
| Report costs mandatory | `ImplDelayGuard_Stages.ReplaySeamRefusesSameCloseAndSilentCosts` | implicit costs refused. Explicit zero trade cost 0.000000; 5 bps / 365 bps → trade cost 0.010084, borrow 0.005994 |
| As-of mask in IC and baseline views | see Accept rows 1–3; `ImplIcAsOfMembership_Stage.AMembershipContextNeedsItsOwnImageUnderTheAsOfRule` | a membership context without an image is refused under as-of. The image hash must equal the context recipe's `universe_membership_sha256` |
| Cross-sectional ops masked by as-of | admission / IC cross-section / mine book: rows above | **Partial:** DSL-internal CS ops deferred to W4-A6 (see D-12) |
| `--membership` required in equity-mine | `ImplMineRequiresMembership_Cli.WithoutMembershipTheRunIsRefused` | exit 2, "--membership <membership.bin> is required"; `year-union-v1` + `--membership` also refused |
| delay < 1 needs `--allow-same-close` | `ImplDelayGuard_Config.*`, `ImplDelayGuard_Mine.*`, `ImplDelayGuard_Stages.*` | CLI (either flag order), programmatic, `equity-ic`, `equity-mine`, baseline (and the book inherits it), identified replay seam |
| `min_names_per_date` param, default 50 | `ImplIcAsOfMembership_Stage.IcKnobsMinNamesDelayEmbargoAndOptionalAudit` | the default 50 on a 16-name fixture gives `dates_below_min_names_max_over_blocks > 0` and `dates_emitted` 0 for every series. The fixture's explicit 2 gives `common_sample_dates` **6** (T=70, maxH=63, delay 1 → embargo 64), bootstrap block **42** at h=21 (TwoHorizonV2 = 2h), embargo **22**. Legacy delay 0 + HalfHorizonV1 gives the old alignment label, common **7**, block **11** |
| Terminal-return table interface; 2013 audit optional | `…IcKnobs…OptionalAudit`, `ImplIcAsOfMembership_Stage.TerminalReturnTableReplacesTheFrozen2013Table`, `ImplIcAsOfMembership_TerminalTable.ParserEnforcesTheContract` | an IC run with no audit completes and records the audit as absent. A CSV table replaces the frozen one and is hash-bound as a manifest parent. The parser rejects a wrong header, value+return, evidenced-without-value, non-finite, and duplicate ids |
| Manifest before `.pending` release | `ImplPendingOrder_Stages.*` | a failing manifest writer leaves `.pending` in place. Baseline and book publish complete manifests, and `.pending` is gone only afterwards |
| nw_lags / ic_horizons / ppy recorded | `ImplMineRequiresMembership_Cli.RecordingWritesWindowsDsrChainHeadAndScorerKnobs` | `gate_report.config.{nw_lags, ic_horizons, periods_per_year}` present; trials: n_raw=3, **pnl_len=643** (train 388 + val 255), clusters=3, `sr*_mc=0.0421`, chain head in the report and manifest |
| `si_publication_lag` default aligned with D0 | `ImplConfigFinite_SiLag.DefaultIsSevenNyseSessionsAndCalendarDaysStayReachable` | default 7, rule `nyse-sessions-v2`. `calendar-days-v1` stays reachable; a negative lag is refused |

## Defect table

| ID | Status | How / where |
|---|---|---|
| D-12 | CLOSED (decision admission, IC cross-section, mine book) + residual DEFERRED → **W4-A6** | `equity_baseline_views.cpp` as-of eligibility (`EquityMembershipRule::AsOfV2` default, `ContextYearUnionV1` reproducible); IC resolves and records the rule from the context recipe; mine requires an image. **Residual:** CS ops inside DSL expressions still see the context mask (year-union). Fixing that needs separate data and tradable masks in `alpha/vm.hpp` (W4-A6 Build item), because masking the loads would erase joiner time-series history. |
| I-10 | CLOSED | `config.cpp` `kBoolFlags` + `parse_bool_flag_value`; `dispatch.cpp` merges `--config` for all stages or refuses it (equity-ic, equity-universe); nested config refused |
| I-11 | CLOSED | `replay_report.cpp` (identified report) + `config.hpp` comments: both rates must be explicit choices. `stage_run.cpp` (I0a) reaches this check. The legacy `stage_report.cpp` path (B0-owned) is in Integration notes |
| I-12 | CLOSED | `config.cpp` `parse_double` rejects non-finite values (all 44 double flags, CLI and file) |
| I-15 | CLOSED (interface) / data DEFERRED → **W2-D2** | `stage_equity_ic.{hpp,cpp}` `EquityTerminalReturnTable` + `--terminal-returns` CSV; audit optional. W2-D2 supplies the real table |
| I-16 | CLOSED | `stage_equity_mine.cpp`: no silent fallback. As-of needs `--membership`; year-union only via an explicit `--membership-rule year-union-v1`, recorded in the gate report |
| I-17 | CLOSED | `publish_manifest_then_release_pending` used by baseline (both paths) and book |
| I-23 | CLOSED | `stage_equity_mine.cpp` gate report config records `nw_lags`, `ic_horizons`, `periods_per_year` |
| E-18 | CLOSED | `stage_equity_ic.cpp` `IcRules.min_names_per_date` from `--min-names-per-date` (default 50, ≥2) |
| B-02 | CLOSED (impl half) | CLI `--allow-same-close`. delay < 1 refused in config, replay seam, baseline, book, equity-ic, equity-mine. The engine half (`ReplayConfig::allow_same_close`) is B0's and is forwarded when present |
| D-02 | CLOSED (config default) | `config.hpp` `si_publication_lag = 7` sessions, `si_publication_lag_rule = "nyse-sessions-v2"`; `calendar-days-v1` reachable |

## Evidence

Every command was run from `C:\atx-wt\pool-11` with `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'` and
≥ 2 GB free RAM (measured 2.76 to 4.15 GB before each build).

Build after the A0 pre-merge:
```
powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker
[105/106] Linking CXX executable bin\atx-impl-tests.exe
exit=0
```
(The later rebuild for the mine test re-pin also printed `exit=0`.)

Anchored lane suites after the A0 pre-merge:
```
powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^(ImplConfigBool_|ImplConfigFinite_|ImplIcAsOfMembership_|ImplMineRequiresMembership_|ImplPendingOrder_|ImplDelayGuard_)'
100% tests passed, 0 tests failed out of 30
Total Test time (real) = 191.11 sec
exit=0
```
(The pre-merge run printed the same: 30/30, 179.90 sec, exit=0.)

Measured print lines from the lane suites (identical before and after the merge):
```
[ImplConfigFinite] 44 double flags x 5 non-finite spellings rejected: 220
[ImplIcAsOfMembership] joiner admitted cells: year-union 36, as-of 31 (rejected 5)
[ImplIcAsOfMembership] constant membership: 5 IC csv files byte-identical, evaluation payload 31f3c71cb1f50bcb
[ImplIcAsOfMembership] joiners: n_eligible row0 union 16 as-of 12; row 30 as-of 16
[ImplMineRequiresMembership] train member cells: as-of 20010 vs year-union 23280 (10 joiner ids)
[ImplMineRequiresMembership] trials n_raw=3 pnl_len=643 (train 388 + val 255) clusters=3 sr*_mc=0.0421
[ImplDelayGuard] replay seam: explicit-zero trade cost 0.000000, 5bps/365bps trade cost 0.010084 borrow 0.005994
```

Whole owning executable, final (HEAD `a28a6c1e` + report):
```
build-equity\bin\atx-impl-tests.exe --gtest_brief=1
acc_ts produced no finite cell
[  FAILED  ] FundamentalZoo.FixtureParsesTypechecksAndEvaluates (248 ms)
ParseError: trial ledger: CR in line 0 (LF endings only)
[  FAILED  ] TrialLedgerRepository.ExistingCp14Ledger_StillVerifies (7 ms)
[==========] 563 tests from 115 test suites ran. (504381 ms total)
[  PASSED  ] 556 tests.
[  SKIPPED ] 5 tests.
exit=1
```

Why neither failure belongs to this lane:

- **Before the A0 merge** (HEAD `4b4a48c6` code), the whole run printed `563 ran, 557 passed, 5
  skipped`, and the only failure was `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies`
  (`CR in line 0`). The working-tree `atx-engine/reviews/trial-ledger.jsonl` contains CRLF
  (checked: `True`). This is the W0b-notes CRLF hazard, which is pre-existing.
- `FundamentalZoo…` passed before the merge and fails after it. The merge brought in only W0-A0
  (`1a304619`). `acc_ts` is `rank(-1 * ts_zscore(accruals, 252))` over forward-filled synthetic
  accruals, which A-09's flat-window guard turns to NaN.
- The five skips are opt-in or real-data tests (ATX_ALPHA101_PANEL, ATX_L10_FUNDZOO_OUT), which
  existed before this lane.

The first whole run (before the fixture fixes) found three I-11 consequences:
`PanelPipeline.WeeklyBooks…` and `StageRunMegaBook.{MegaBookGraph_InertByteIdentical,
MetabookStage_SkippedAtDefault}` failed with "identified report: report costs are mandatory…".
Their fixtures now make the 0/0 choice explicit, and they pass (10/10 in the filtered rerun).

## Golden-digest old → new

None. No committed digest was re-baselined.

- `StageRunMegaBook` six-digest identity holds, because the costs are the same explicit 0/0.
- IC stage outputs change by design under the new defaults (delay 1, TwoHorizonV2, HAC,
  embargo, min names 50). No committed golden pins them: `stage_equity_ic_test` checks run-to-run
  determinism, and its structural expectations are listed above under E-09 / E-18.
- The constant-membership identity is proved inside one test run (`31f3c71cb1f50bcb` on both
  paths).

## Deviations from brief

1. **`replay_report.cpp` delay guard.** Besides the I-11 cost site, the same `replay_config`
   function refuses delay 0 without `allow_same_close` and forwards the flag to B0's future
   `ReplayConfig::allow_same_close` (compiled out through `requires` until B0 lands). This is
   B-02's impl half at the only seam the identified report goes through. The hunk is about 10
   lines, next to the I-11 hunk.
2. **`trial_ledger.hpp:89-90` grant not used.** The IC stage sets the ledger's alignment,
   block-length and common-sample strings explicitly per run (they now depend on delay and rule).
   Editing the header defaults would have moved `trial_ledger_test`'s pins without need.
3. **`si_publication_lag` has no consumer in atx-impl.** The default and the rule string are
   changed and validated, but no atx-impl stage passes them to the FINRA loader today
   (`stage_augment` does not read them). See Integration notes.
4. **D-12 residual (DSL-internal CS ops)** is deferred, as described in Outcome and in the defect
   table.
5. **An existing mine test was re-pinned for A0's A-01** (12 → 9 trials). The test covers the
   owned stage, and the change was surfaced by the mandatory pre-review merge.

## Integration notes

- **B0:** once `ReplayConfig::allow_same_close` exists, `replay_report.cpp`'s `set_allow_same_close`
  starts forwarding the CLI flag automatically, with no edit needed. `stage_report.cpp` (the
  legacy, unidentified report path, B0-owned) still defaults to frictionless rates. Apply the
  same "explicit choice" rule there to finish I-11 on every report path.
- **stage_augment / D-track owner:** wire `cfg.si_publication_lag` + `cfg.si_publication_lag_rule`
  (`nyse-sessions-v2` = `FinraLagRule::NyseSessionsV2`, `after_close=true`; `calendar-days-v1` =
  the int overload) into the FINRA loader call.
- **W4-A6:** D-12 residual. After data and tradable masks are split in the VM, feed the as-of
  membership as the tradable mask for CS ops in `equity_baseline_views.cpp`. The as-of lookup
  (`EquityAsOfMembership`) and the per-cell flags already exist.
- **W2-D2:** the real terminal-return table goes in via `--terminal-returns <csv>`. The header is
  `security_id,terminal_value,terminal_return,special_dividend,record_date,evidenced,source`, with
  exactly one of value or return per evidenced row.
- **G0 runbook:**
  - `equity-ic` now needs `--min-names-per-date` when a fixture or cell has fewer than 50 names.
  - Membership contexts need `--membership <image>` whose sha256 matches the context recipe (or
    `--membership-rule year-union-v1` to reproduce pre-W0).
  - `equity-mine` needs `--membership` (or the explicit legacy rule).
  - Any delay-0 recipe needs `--allow-same-close`.
  - Identified reports need explicit `--replay-trade-bps` / `--replay-annual-borrow-bps`.
  - Pre-W0 IC numbers are reproducible with `--ic-execution-delay 0 --allow-same-close
    --ic-block-len-rule half-horizon-v1 --min-names-per-date 2`.
- **A0 / W1-A1 / integration gate:** `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` fails
  after A0 (A-09 flat guard), with `acc_ts` all-NaN on the synthetic fixture. Its owner should
  re-seed the fixture's accruals so they are non-flat over 252 dates, or re-pin with A-09
  attribution.
- **W4-I3** (mining v2 also lists I-16): the explicit `--membership-rule` switch and the recorded
  rule label are the contract to keep.

## Ledger candidates

- W0-I0b: as-of PIT membership with a constant image is bit-identical to the year-union path
  (evaluation payload `31f3c71cb1f50bcb`, 5 IC CSVs byte-identical). A mid-window joiner
  removes 5/36 admitted cells in the view fixture.
- W0-I0b: identified reports refuse implicit costs. Three pre-existing atx-impl fixtures
  (PanelPipeline, 2× StageRunMegaBook) relied on silent 0/0 and now state it.
- W0-I0b: IC default (delay 1, maxH 63) gives `common_sample_dates = T - 64`, versus the legacy
  (delay 0) `T - 63`. The equity-mine registry `pnl_len` = train + validation sessions (388 + 255
  = 643 on the lane fixture).
