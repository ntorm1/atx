# Handoff AL-CLOCK (lane AL-CLOCK, pool-8, branch `feat/p9-alclock-20261004`)

Stopped a second time by the OWNER STOP relayed by the PM (R7-1 resume), at a clean point. Contract:
`resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`, `resume7-addendum.md`, `r7-dispatch.md` section
"AL-CLOCK", rulings ALCLOCK-1, ALCLOCK-2, W3-9, W3-16, DEC-16, C3-1/3/4/5/9, PRE-7a, PRE-14/19/31, W1-I1, ALCOMB-1.

## State at the stop

- Root head `4dc831ac` merged: merge commit `8a6ebe5b` (clean, no conflict). Root may have moved since: merge it
  again first (step 1 below).
- The code of items 1, 2 (`two_speed_fast_theta(cadence)`), the refusals, the per-book stats and the
  `--info-clock-print` opt-in is WRITTEN BUT NEVER COMPILED, in the commit `WIP(al-clock): ...` on top of `8a6ebe5b`
  (16 C++ files, +352 / -32). Expect slips (missing include, a 100-column line, a warning under /W4 /WX).
- Build trees: `build-equity` was CONFIGURED once (`atx-build.ps1 configure -Preset equity-dev`, exit 0; the
  configure log is in the shared scratchpad, `alclock-configure-dev.log`); NOTHING was built: no build tag was used
  (`p9-alclock-a` is still free), no object tree exists, no receipt. `build-equity-rel` does not exist.
- Tests: NONE written, none run. No Python changed. No permission refusal.

## Items (r7-dispatch "AL-CLOCK" steps 1-7)

| item | status |
|---|---|
| 1 merge root | done (`8a6ebe5b`, root `4dc831ac`) |
| 2 row + parser entry (append-only, W3-9) | written, uncompiled: row `nav_rules.cpp:77-83` (constant `info_clock_refuses` `:55-58`), parser `nav_cli.cpp:203-207` (`--rate info-clock-rate-v1`), help `:65-69`; header comment `nav_rules.hpp:11-12` |
| 2 `two_speed_fast_theta(cadence)` (W3-16) | written: `nav_info_clock_fast_theta` / `nav_info_clock_fast` / `nav_info_clock_rates` declared `strategy_nav_replay.hpp:120-152`, defined `book/nav_decide.cpp:9-36` |
| 2 refusals | written (see list below); NO test, so no teeth yet |
| 2 per-book stats | written: `NavInfoClockStats` `strategy_nav_replay.hpp:402-417`; counted in `nav_decide.cpp:250-266` (`count_info_clock`), called `:333-335` (scored effective rebalances, members, main books) |
| 3 `--info-clock-print` (ALCLOCK-1) | written: CLI `nav_cli.cpp:50-55`, help `:128-130`; NavSpec key `nav_spec.cpp:123-124` (appended at the table's end); `NavEmitOptions::info_clock_print` `strategy_nav_replay.hpp:749-764`; load without `fields_used` `nav_load.cpp:116-127`, `:159-166`, `:205-214`; block `nav_publish.cpp:80-83` + `info_clock_json` `nav_write.cpp:290-311`; `NavRun::info_clock` `nav_replay_internal.hpp:664-670`; wiring `nav_run.cpp:64-68`, `:312-329`, `:402`, `:417`, `:470-486`, `:526-527` |
| 4 tests | NOT STARTED |
| 5 registration + template | NOT STARTED (content decided, below) |
| 6 builds Debug / Release, canary | NOT STARTED |
| 7 report `task-AL-CLOCK-report.md` with "For PRE" | NOT STARTED |

### What the WIP code does (file:line)

- `NavRateRule::InfoClockV1 = 2` and the rule text, constants `nav_info_clock_rate_id`, `nav_info_clock_field`
  (`ea_days_since`), `nav_info_clock_window` 21: `strategy_nav_replay.hpp:120-152`.
- `NavReplayInput::info_clock` (trailing span, date-major `ea_days_since`; empty = the replay without it):
  `strategy_nav_replay.hpp:290-301`. Copied into every book's `Ctx::info_clock` (`nav_replay_internal.hpp:273-274`,
  `nav_lockstep.cpp:269`) and into `replay_books`' input (`nav_lockstep.cpp:220`).
- Rates formed ONCE per decision on the calling thread in `run_books` (`nav_session.cpp:302-306`, `:358-367`) into
  `Construction::clock_rate` (`nav_replay_internal.hpp:235-240`); `plan_decision` hands them to aim-partial-v5
  (`nav_decide.cpp:290-291`); so `--book-workers N` stays legal (only per-name-v1 is refused there).
- Refusals written: config `nav_validate.cpp:81-89` (rule needs aim-partial-v5, trade_fraction .05, no rate params);
  `rate_ok` treats the rule like fixed for the params `:91-99`; input `nav_validate.cpp:180-196` (clock geometry,
  rule without clock, a cell outside NaN-or-finite->=0); budget `:209-210`; grid variants must keep the base cadence
  `nav_lockstep.cpp:201-209` (two-speed's message left byte-identical: a test pins it); no `--fields` `nav_load.cpp:
  205-214`; kernel `nav_decide.cpp:13-30`. Existing refusals that already cover the row's `incompatible[]`:
  per-name params `nav_cli.cpp:245-248`, `--rate` aim-partial-v5 only `:241-243`, two-speed `nav_validate.cpp:
  106-110` + v7 `strategy_nav_v7.cpp:1218-1221`, spo `:1292`, aim-partial-v6 `:1340-1345`.
- Loosened: `--capacity-curve` with `--rate info-clock-rate-v1` is now accepted (`strategy_nav_v7.cpp:1340-1345`;
  the rate reads no NAV; Y-F0, P9-C's parent, runs `--capacity-curve`).
- Recipe under the rule: `rate`, `rate_theta_fast`, `rate_theta_slow`, `rate_window_sessions`, `rate_field`,
  `aim_partial` (`nav_write.cpp:250-260`, tail text `:87-96`); `ea_days_since` enters `financing_fields.fields_used`
  only under the rule (`nav_load.cpp:159-166`); summary `construction.v5.rate` (`nav_publish.cpp:95`).
- Reserve: `nav_lockstep.cpp:69-75` (one f64 per name under the rule only; flag absent adds 0).
- Decide path: unchanged; `nav_deploy.cpp:87` already refuses every non-fixed rate.

### Decided, not yet written

- Tests (new file `atx-impl/tests/strategy_nav_info_clock_test.cpp` + CMake tail
  `target_sources(atx-impl-strategy-target-tests PRIVATE strategy_nav_info_clock_test.cpp)` at the END of
  `atx-impl/tests/CMakeLists.txt`; the glob already puts it in atx-impl-tests). Fixture: spo `Role(40, 12, 53)` +
  `write_run_inputs` + an own fields writer (shares_out 1e8, si_shares 1e6, optional ea_days_since; manifest pinned to
  the role's receipts, as `strategy_nav_replay_test.cpp:411-444`). Planned suites: `InfoClockRate.KernelAtTheWindowBoundary`
  (0, 21, nextafter(21), 22, NaN, nonmember; cadence 1 / 5 / 21; .12945 at C = 1; every refusal writes nothing),
  `.StepChangeClosedForm` (holdings sink: rule_weight = held + theta_i (L desired - held), dust 0, exit 1; an
  all-NaN clock equals the fixed run except applied_fraction), `.EventClockIsCausal` (clock rows > d0 changed: days
  <= d0 identical), `.RefusedConfigsAndInputs`, `.BookWorkersOneAndFourSameBytes` (rule on, `--cost-v2
  --capacity-curve`, fields), `.RegistryRowAndParser`; `InfoClockPrint.FlagAbsentLoadsNoClock` (manifest with and
  without the field: NAV files equal, recipe/summary equal after the manifest-SHA substitution, no `info_clock`,
  no `rate`), `.PrintOnlyOptInAddsOneBlockPerScenario` (every file but summary.json equal incl. capacity/ and v7
  files; summary minus the blocks = absent bytes; values checked against `--emit-holdings --holdings-format csv`:
  sum of |planned_trade_weight| over member rows of rebalance sessions, fast by the fixture clock),
  `.RuleOnWithoutFlagPrintsNoBlock`, `.SpecEqualsArgv`. Teeth: one Debug build with every new check disabled
  (commit first, mutate, build, run, `git checkout -- <files>`).
- Pytest `scripts/tests/test_info_clock_rule.py` (never edit D3's `test_wave_kinds.py`): fake NAV table = C1's
  three leverage rows + the row as `--list-rules --json` prints it; `wave_kinds.rule_flags([{"name":
  "info-clock-rate-v1"}])` == `{"nav": {"--rate": "info-clock-rate-v1"}}`; params `{"x": 1}` refused;
  `raw_rule_flags` refuses `nav --rate` and leaves `--info-clock-print` free; a fake-world `wave run --until spec`
  on a kind-rule manifest naming the row gives the cell `--rate info-clock-rate-v1` on the NAV argv. A new Python
  test file means: whole `scripts/tests` under PYTHONHASHSEED 0 and 1 + `test_stage_chain.py`, separate invocations.
- Argv token: the row puts `--rate info-clock-rate-v1` on the NAV argv (`wave_kinds.nav_flags`: x-flag, no
  params). It does not touch `--rule`, so the C3-5 probe should see the parent's `--rule aim-partial-v5` once; a
  parent argv that already carries `--rate` gets its value replaced (derived flag applied), never duplicated.
- Registration `scripts/specs/p9/rules/info-clock-rate-v1.json` (`atx.p9-rule-registration/v1`, AL-COMB's shape,
  `mom-volman-v1.json`): `source_sample_end` 2019, FLAGGED (PRE-19 / PRE-31, ALCLOCK-2; Bowles, Reed, Ringgenberg,
  Thornock, "Anomaly Time", JF 79(5) 2024, p. 14 "Our sample period begins January 1990 and goes through 2019");
  the paper's information-release date mapped to `ea_days_since`'s reaction session stated as a FIXED CHOICE;
  constants theta_f = two_speed_fast_theta(C) (W3-16), theta_s .05, window 21; C3-4 line: the rule changes only the
  per-name rate, never L, so realised gross stays bounded by the parent's L; DEC-16 argument: Y-5 buckets THEMES into
  two netted sleeves, this rule buckets NAME-DAYS by an event clock (one aim, a per-name rate vector, no sleeve,
  no netting), R-3 is a per-signal aim gain; manifest_fragment with `gross_match: "pm6-6-engine"` (a manifest key,
  not a template key) and `rules: [{name: info-clock-rate-v1, params: {}}]`.
- Template `scripts/specs/p9/templates/info-clock.json` (`atx.research-cycle-template/v1`, name `p9-info-clock`;
  parent null, nominal_parent `../../v8/lib-v8ysb-gm.json` (Y-F0); `change.set` only `nav.output`
  `build-equity/mega-nav-p9-info-clock` and `monitor.output` `build-equity/mega-monitor-p9-info-clock`; no `rules`,
  no `change.flags`). `--info-clock-print` is NOT in the template: per ALCLOCK-1 root names it on the Y-F0
  reference at the wave-3 P9-B0 and the cell inherits it.

## Next step for a fresh agent

1. `git merge feat/platform-p9-20261003` in pool-8 (W3-EARLY-2; never rebase).
2. Read this WIP diff (`git show HEAD`); then write the C++ test file + CMake tail, the pytest, the registration
   and the template (above).
3. Build `p9-alclock-a` (equity-dev): `powershell -File scripts\research-build.ps1 -Tag p9-alclock-a -Targets
   "atx-impl-strategy-target-tests,atx-equity-strategy-targets,atx-equity-strategy-ic,atx-research-eval" -Canary`
   (the tree is configured); fix slips; named filter `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:
   NavAnnMeanGross.*`, then whole target-tests; teeth build; Release `equity-rel` (configure first; stale PCH:
   `CCACHE_RECACHE=1`); canary on both goldens.
4. Report `task-AL-CLOCK-report.md` with "For PRE" (P9-C block); merge root again, re-run suites; delete own build
   trees by name.

## Notes

- Anchoring disclosure (unchanged): two public ledger lines with Y-5 / X-10 verdict numbers (`progress.md` :147,
  :156) were shown by a grep in the first session; no constant or prediction is taken from them.
- The R7 dispatch also asks for a `--spec` = argv test with the flag; the NavSpec key exists, the test does not.
