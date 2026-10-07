# Lane AL-CLOCK report

## Outcome

BLOCKED (PERM-1) after the code, tests and registration were done: the WIP compiled clean, 13 gtests + 5 pytests
written and green on Debug (`InfoClock*` 13 / 13, named 24 / 24, whole target-tests 403 / 403 at `p9-alclock-e`),
registration and template committed, teeth set 1 caught every mutation; the permission system then refused the
revert of the teeth mutations (`git checkout -- <six files>`), so the six files still carry them in the working tree
and the steps that need the restored tree (teeth set 2, the Debug `-Canary` build of the merged head, the whole
Release leg) are not done. HEAD is correct; the PM / owner restores the six files, then `handoff-AL-CLOCK.md` "Next
step" finishes the lane.

## Branch / SHA

`feat/p9-alclock-20261004` in `C:/atx-wt/pool-8` @ @@HEAD@@ (all work committed; the working tree is NOT clean: the six teeth set 1 files, see "Permission refusals"). Commits of this
resume (R8): `c3f3f1f8` merge of root `1b64ce45`; `76a4f00b` tests, CMake tail, pytest, registration, template;
@@COMMITS@@. Earlier: `ebaf1e3f` (design handoff), `8a6ebe5b` (merge of root `4dc831ac`), `120f6a25`
`WIP(al-clock)` (the 16 C++ files, uncompiled at the stop; they compiled clean at `p9-alclock-b`, no slip to fix).

## Frozen base / lease

Lease untouched by this lane (contract: never touch a lease). Root merged last: `fd084e78` (merge `e40ed78c`, clean).

## Files changed (lane code vs root; `git diff <root-merge-base> HEAD`)

| file | what |
|---|---|
| `atx-impl/src/strategy_nav_replay.hpp` | `NavRateRule::InfoClockV1 = 2` (:123); the rule's statement and constants `nav_info_clock_rate_id` / `nav_info_clock_field` / `nav_info_clock_window` (:120-139); kernel `nav_info_clock_rates`, `nav_info_clock_fast_theta`, `nav_info_clock_fast` (declared :140-154); `NavReplayInput::info_clock` (:299); `NavInfoClockStats` (:410) in `NavConstructionStats`; `NavEmitOptions::info_clock_print` (:762); contract comments |
| `atx-impl/src/book/nav_decide.cpp` | the kernel (:10-36, `two_speed_fast_theta(cadence)`, W3-16); DECIDE takes `shared.clock_rate` under the rule (:291); the census `count_info_clock` (:252) on scored effective rebalances of main books (:335) |
| `atx-impl/src/book/nav_session.cpp` | the rate vector formed once per decision on the calling thread from row t only (:305, :362) |
| `atx-impl/src/book/nav_replay_internal.hpp` | `Construction::clock` / `clock_rate`, `Ctx::info_clock`, `LoadedFields::info_clock(_sha256)`, `NavRun::info_clock`, `admit_and_load(..., info_clock_print)`, `info_clock_json` |
| `atx-impl/src/book/nav_validate.cpp` | config refusal (:84), `rate_ok` (:92), input refusals (geometry :183, rule without clock, domain :193), budget term (:210) |
| `atx-impl/src/book/nav_lockstep.cpp` | reserve term (:70, one f64 per name under the rule only), grid cadence refusal (:203), the clock into every book's ctx |
| `atx-impl/src/book/nav_load.cpp` | `ClockLoad` (:118): the rule loads `ea_days_since` into `fields_used`, the print loads it unrecorded (:159); both need `--fields` (:209) |
| `atx-impl/src/book/nav_run.cpp` | the clock into the input; the block's field record; `emit.info_clock_print` to the main directory only (:422, :532) |
| `atx-impl/src/book/nav_publish.cpp` | `info_clock` block per scenario under the flag (:81); `construction.v5.rate` under the rule |
| `atx-impl/src/book/nav_write.cpp` | recipe keys under the rule (:252), `info_clock_json` (:289) and its declaration |
| `atx-impl/src/book/nav_rules.{hpp,cpp}` | C3's registry: the row `info-clock-rate-v1` appended (nav_rules.cpp:75, refusals :57) (W3-9) |
| `atx-impl/src/book/nav_cli.cpp` | `--rate info-clock-rate-v1` (:206), `--info-clock-print` (:51), help text |
| `atx-impl/src/book/nav_spec.{hpp,cpp}` | NavSpec key `info_clock_print` appended at the table's end (nav_spec.cpp:124) (W3-9) |
| `atx-impl/src/strategy_nav_v7.cpp` | ALCLOCK-3: `--capacity-curve` accepts this rate (:1340-1343); aim-partial-v6 still refuses it |
| `atx-impl/tests/strategy_nav_info_clock_test.cpp` | new: 13 gtests (below) |
| `atx-impl/tests/CMakeLists.txt` | tail append: `target_sources(atx-impl-strategy-target-tests PRIVATE strategy_nav_info_clock_test.cpp)` |
| `scripts/tests/test_info_clock_rule.py` | new: 5 pytests on fakes |
| `scripts/specs/p9/rules/info-clock-rate-v1.json` | new: registration (`atx.p9-rule-registration/v1`) |
| `scripts/specs/p9/templates/info-clock.json` | new: template (`atx.research-cycle-template/v1`) |

No Python module changed (one new test file only). No `atx-db/`, no golden, no expected hash, no pin edited.

## Required items -> change -> test -> evidence

| item (r7 / r8 dispatch) | change | test | evidence log (`alclock-logs/`) |
|---|---|---|---|
| 1 merge root | `c3f3f1f8` (root `1b64ce45`), `e40ed78c` (root `fd084e78`), both clean, no conflict | suites re-run on the merged tree | `builds.txt` |
| 2 row + parser entry, append-only (W3-9) | nav_rules.cpp:57, :75; nav_cli.cpp:206 | `InfoClockRate.RegistryRowAndParser` | `gt-*-new.log` |
| 2 `two_speed_fast_theta(cadence)` (W3-16) | nav_decide.cpp:10 | `InfoClockRate.KernelAtTheWindowBoundary`, `.StepChangeClosedForm` (cadence 1 and 5) | same |
| 2 refusals | nav_validate.cpp:84, :183-195; nav_load.cpp:209; nav_lockstep.cpp:203; nav_decide.cpp:16-30; nav_cli.cpp:51 | `InfoClockRate.ConfigAndInputRefusals`, `.CommandLineRefusalsWriteNothing`, `.KernelAtTheWindowBoundary` | same; teeth `gt-f-teeth.log` |
| 2 per-book stats | nav_decide.cpp:252, :335 | `InfoClockRate.ConfigAndInputRefusals` (census), `InfoClockPrint.PrintOnlyOptInAddsOneBlockPerScenario` (values against the holdings CSV) | same |
| 3 ALCLOCK-1 print-only opt-in | nav_cli.cpp:51; nav_spec.cpp:124; nav_load.cpp:159; nav_publish.cpp:81 | `InfoClockPrint.FlagAbsentLoadsNoClock`, `.PrintOnlyOptInAddsOneBlockPerScenario`, `.RuleOnWithoutFlagPrintsNoBlock`, `.SpecEqualsArgv` | same |
| 4 1-vs-4 `--book-workers`, rule on | nav_session.cpp:362 (rates on the calling thread) | `InfoClockRate.BookWorkersOneAndFourSameBytes` (`--cost-v2 --capacity-curve --info-clock-print`) | same |
| 4 E2 `rule_flags` preflight on fakes | none (E2's code) | `scripts/tests/test_info_clock_rule.py` (5) | `pyt-clock-*.log` |
| ALCLOCK-3 loosened refusal, this rate only | strategy_nav_v7.cpp:1340-1343 | `InfoClockRate.CapacityCurveAcceptsThisRateOnly` | same; teeth `gt-f-teeth.log`, `gt-g-teeth.log` |
| 5 registration + template | the two JSON files | `test_registration_and_template` | `pyt-clock-*.log` |
| 6 builds, named, whole, canary | - | below | `builds.txt`, `gt-exits.txt`, receipts |
| 7 report | this file | - | - |

## The gtests (`atx-impl-strategy-target-tests`, filter `InfoClock*`)

- `InfoClockRate.KernelAtTheWindowBoundary`: [0, 21] closed (nextafter(21) and 22 slow, -0 fast), NaN slow, fast rate
  bit-equal to `two_speed_fast_theta(C)` at C = 1, 2, 5, 21 (.12945 at C = 1, .5 at C = 5); a nonmember slow; every
  refused input (span length, cadence 0, slow rate outside (0, 1], member flag 2, negative / infinite clock) writes
  no rate.
- `.ConfigAndInputRefusals`: in-process replay: census = members of every effective rebalance row (fast / missing
  counts from the clock), the fixed rate with a clock counts the same name-days, none without a clock; all-NaN clock
  valid (every name-day missing); refusals: rule without clock, short clock (rule and print), domain (-1, +inf, -inf;
  the message names row and name), trade fraction .25, baseline rule, any rate parameter, two-speed.
- `.StepChangeClosedForm`: `--emit-holdings csv`, dust 0: every member rebalance row has
  rule_weight = held + theta_i (1.2 desired - held), theta_i by the fixture clock, at cadence 1 and 5; an all-NaN
  clock is the fixed run at .05 byte for byte except `applied_fraction` (equal to .05 within 1e-15).
- `.EventClockIsCausal`: the clock flipped on every row after d0 = 20 leaves every daily row up to d0 identical and
  changes a later one.
- `.CommandLineRefusalsWriteNothing`: 14 command lines (usage 2 / run 1), each message named, no output directory.
- `.CapacityCurveAcceptsThisRateOnly` (ALCLOCK-3): v7 parser matrix (capacity x {none, fixed, this rate, per-name},
  v6 x {fixed, this rate, per-name}, v6 + capacity + this rate) and end to end (capacity directory published, its
  recipe names the rate; per-name-v1 + capacity refused, no output).
- `.BookWorkersOneAndFourSameBytes`: rule on, `--cost-v2 --capacity-curve --info-clock-print`: four workers publish
  every byte of one (capacity directory, v7 files, blocks); the capacity summary has no block.
- `.RegistryRowAndParser`: the 4th trade-rate row, x-flag, no params, `incompatible[]` exactly, `--list-rules` (json
  and text), NavSpec key and option.
- `.WorkspaceReserveOneRatePerName`: the rule adds exactly names x 8 bytes to the reserve (with and without holdings).
- `InfoClockPrint.FlagAbsentLoadsNoClock`: a fields manifest that also lists `ea_days_since` publishes the bytes of
  one that does not, after the substitution list (fields manifest SHA, each file's SHA, each JSON's compact-dump SHA);
  CSVs byte for byte; no `info_clock` / `info-clock` / `ea_days_since` / `rate_theta` / `rate_window` / `rate_field`
  anywhere (v5 fixed and v6 + cost-v2 + capacity shapes).
- `.PrintOnlyOptInAddsOneBlockPerScenario`: on books that do not run the rule (v5 fixed; v6 + cost-v2 + capacity):
  every file but `summary.json` byte for byte, `summary.json` minus the blocks = the absent file; the block's values
  against the primary book's holdings CSV and the fixture clock (name-days, fast, missing exact; planned turnover
  and its fast part = sums of |planned_trade_weight|; shares); every book the same name-days; a per-name-v1 book
  prints `"rate": "per-name-v1"`; a grid variant = its standalone run.
- `.RuleOnWithoutFlagPrintsNoBlock`: the rule alone: no block; recipe `rate`, `rate_theta_fast`, `rate_theta_slow`,
  `rate_window_sessions`, `rate_field`, `aim_partial` text, `fields_used.ea_days_since`; adding the print changes only
  `summary.json`, by the blocks (`"rate": "info-clock-rate-v1"`).
- `.SpecEqualsArgv`: three shapes (print on a fixed book; rule + print; rule + print + cost-v2 + capacity):
  `--print-spec` carries `info_clock_print` (and `rate` only when named); the `--spec` run publishes the argv run's
  bytes; flag absent, no key.

## The pytest (`scripts/tests/test_info_clock_rule.py`)

- `test_rule_flags_put_the_rate_on_the_nav_argv`: `rule_flags([{name: info-clock-rate-v1}])` =
  `{"nav": {"--rate": "info-clock-rate-v1"}}`; a param refused; with per-name-v1 a `--rate` conflict; beside
  `aim-partial-v5` (its own params) both flags derived.
- `test_raw_rate_flag_refused_print_flag_free`: `nav --rate` (named or not, `=value`, case variant, abbreviation)
  refused by `raw_rule_flags`; `--info-clock-print` owned by no row.
- `test_kind_rule_wave_puts_one_rate_and_one_rule`: a fake-world `wave run --until spec` on a v2 kind-rule manifest
  naming the row: change.flags = `{"nav": {"--rate": ...}}`; the cell's NAV argv as `Cycle.nav_step` builds it has
  `--rule aim-partial-v5` once and `--rate info-clock-rate-v1` once; a parent carrying `--rate fixed
  --info-clock-print`: the rate replaced, the print kept once.
- `test_constants_route_refuses_the_rate_not_the_print`: preflight exit 3 on `rule_cell.constants.flags nav --rate`;
  `nav --info-clock-print` passes.
- `test_registration_and_template`: K-P9-11 keys, 2019 flagged, the template pin = the file's SHA-256 (LF), no rules
  / change.flags, the fragment validates as a v2 manifest, the template resolves on Y-F0 (aim-partial-v5 at .05,
  `--capacity-curve`, no `--rate` / `--two-speed` / `--rule` in its flags, new outputs).

## Builds (`scripts/research-build.ps1` through `alclock-logs/tools/build.sh`: one build on the host, memory gate)

All equity-dev (Debug) in `build-equity`, target-scoped; receipts, build logs and cache stats copied to
`alclock-logs/`; one line per build in `alclock-logs/builds.txt`.

| tag | source (receipt) | targets | result |
|---|---|---|---|
| `p9-alclock-a` | `c3f3f1f8`, clean | the four canary targets + target-tests | exit 1: the stale-PCH ccache trap (`cmake_pch.cxx` mtime changed since the PCH was built; 65 TUs, nothing of the lane reached); not a code red |
| `p9-alclock-b` | `c3f3f1f8`, clean | `atx-impl-strategy-target-tests, atx-equity-strategy-targets, atx-equity-strategy-ic, atx-research-eval` | exit 0 after deleting the six `cmake_pch.cxx.{pch,obj}` by name, `CCACHE_RECACHE=1`; 307 TUs, 1265 s; 0 warning lines: the WIP compiled with no slip. Exes: targets `028303b4...a039`, ic `06f51947...c6c6`, eval `c822cbb8...afc6` |
| `p9-alclock-c` | `c3f3f1f8` + 4 dirty (the new test file, CMake tail, pytest, specs) | target-tests | exit 0, 9 TUs: the test file compiled first time |
| `p9-alclock-d` | same + test fixes | target-tests | exit 0 |
| `p9-alclock-e` | same; content = commit `76a4f00b` | target-tests | exit 0; exe `006d2432...` (the evidence exe below) |
| `p9-alclock-f` | `1cbe5233` (root `fd084e78` merged) + 6 dirty: TEETH set 1 | target-tests | exit 0, 13 TUs; teeth exe `1708ab8d...` (never evidence) |

Not built (blocked, see "Permission refusals"): Debug of the merged head with `-Canary`, the Release configure and
build.

## Tests run (counts)

@@TESTS@@

## Teeth

Set 1 (`alclock-logs/tools/teeth.py 1`, diff `alclock-logs/teeth-set1.diff`; build `p9-alclock-f`, log
`alclock-logs/gt-f-teeth.log`: `InfoClock*` ran 13, 6 passed, 7 FAILED as intended). Each mutation removes one new
check (or breaks the flag-absent identity) and is caught by its test:

| mutation | code | caught by (test, line) |
|---|---|---|
| M1 config refusal off | nav_validate.cpp:84 | `ConfigAndInputRefusals` (:381, :384, :391: tf .25, baseline, every rate parameter); `CommandLineRefusalsWriteNothing` case 1 |
| M2 clock geometry off | nav_validate.cpp:183 | `ConfigAndInputRefusals` (:365 rule, :367 print) |
| M3 rule without clock off | nav_validate.cpp:186 | `ConfigAndInputRefusals` (:361) |
| M4 clock domain off | nav_validate.cpp:191 | `ConfigAndInputRefusals` (:374, the kernel's message instead: row / name not named); `CommandLineRefusalsWriteNothing` case 6 |
| M5 grid cadence off | nav_lockstep.cpp:203 | `CommandLineRefusalsWriteNothing` case 10 |
| M6 `--fields` refusal off | nav_load.cpp:209 | `CommandLineRefusalsWriteNothing` cases 3 (rule) and 11 (print) |
| M7 identity: the clock loaded as a run input whenever `--fields` is given | nav_load.cpp:206 | `InfoClockPrint.FlagAbsentLoadsNoClock` (:709) |
| M8 kernel range / domain checks off | nav_decide.cpp:22, :27 | `KernelAtTheWindowBoundary` (:283, :285, :288, :292) |
| M9 duplicate `--info-clock-print` accepted | nav_cli.cpp:52 (the switch, :51-53) | `CommandLineRefusalsWriteNothing` case 13 |
| M10 ALCLOCK-3 reverted (capacity refuses the rule) | strategy_nav_v7.cpp:1342 | `CapacityCurveAcceptsThisRateOnly` (:589, :600), `BookWorkersOneAndFourSameBytes` (:618), `SpecEqualsArgv` (:899) |

Passing under set 1, as expected: `RegistryRowAndParser`, `WorkspaceReserveOneRatePerName`, `StepChangeClosedForm`,
`EventClockIsCausal`, `PrintOnlyOptInAddsOneBlockPerScenario`, `RuleOnWithoutFlagPrintsNoBlock` (none of them tests
a removed check). The kernel's span-length check was not removed (removing it writes out of bounds).

Set 2 (ALCLOCK-3 over-loosened: the capacity curve takes any non-fixed rate, aim-partial-v6 takes this one; expected
to fail `CapacityCurveAcceptsThisRateOnly`): NOT RUN, because the revert of set 1 was refused (below). Set 1's M10
already shows the line has teeth in one direction; the test's assertions for the other direction (per-name-v1 with
the capacity curve refused; aim-partial-v6 with this rate refused) pass on the clean exe.

## New literals, flags, keys, rule ids (for root)

- Rule id `info-clock-rate-v1` (18 chars, W1-I1). No new `atx.<name>/v<n>` schema literal (SQL2-CLS: nothing to add to
  `classes.json`).
- NAV flags: `--rate info-clock-rate-v1` (a value of the existing `--rate`), `--info-clock-print` (valueless switch).
- NavSpec (`atx.nav-spec/v1`) option key `info_clock_print` (Switch), appended at the table's end.
- `summary.json`, each scenario, only under `--info-clock-print`: `info_clock` {`rate`, `field` {`name`, `sha256`},
  `window_sessions`, `theta_fast`, `theta_slow`, `name_days`, `fast_name_days`, `missing_name_days`, `fast_share`,
  `planned_turnover`, `fast_planned_turnover`, `turnover_share`, `definition`}.
- `recipe.json`, only under the rule: `rate` = `info-clock-rate-v1`, `rate_theta_fast`, `rate_theta_slow`,
  `rate_window_sessions`, `rate_field`, `aim_partial` (text), `financing_fields.fields_used.ea_days_since`; summary
  `construction.v5.rate` under the rule when construction keys are published.
- K-P9-7 row `info-clock-rate-v1` (kind `trade-rate`, x-flag `--rate info-clock-rate-v1`, no params, incompatible
  `per-name-v1, two-speed-v1, aim-partial-v6, spo-v1, spo-v2, spo-v3`).

## Cross-lane edits (smallest appends, W3-9)

- C3's registry `atx-impl/src/book/nav_rules.cpp`: one constant array (:57) and one row at the end of
  `trade_rates[]` (:75); `nav_rules.hpp` header comment (one line).
- C3's parser `atx-impl/src/book/nav_cli.cpp`: one `--rate` value (:206), one switch (:51), help text.
- C3's NavSpec `atx-impl/src/book/nav_spec.cpp`: one key at the table's end (:124); `nav_spec.hpp` comment.
- `atx-impl/src/strategy_nav_v7.cpp` (C1 / C3): the ALCLOCK-3 condition (:1340-1343).
- `atx-impl/tests/CMakeLists.txt`: one block at the end (P2 tail).
- COV-MV (pool-24) appends a row and a parser entry in the same files and merges after this lane: keep both.

## Flag-absent identity procedure (for root, C3-6 / PRE-7a form)

1. Build at the merged head (Debug and Release, the canary targets); EXE-CUR: state tag, SHA-256, source commit.
2. Tiny-world canary on both builds: goldens unchanged (this lane edited none).
3. X-5 and Y-F0 NAV through argv and through `--spec`, exactly the P9-B0 reference argv (no `--rate
   info-clock-rate-v1`, no `--info-clock-print`): every file of the NAV directory (daily / events CSVs, `recipe.json`,
   `summary.json`, `capacity/`, the v7 files) byte-identical to its P9-B0 reference outside `summary.json`'s
   `producer` (engine_git_sha, build_type). Substitution list: none (flag absent loads no field: the fields manifest
   and its SHA are the reference's; `InfoClockPrint.FlagAbsentLoadsNoClock` shows even a manifest that lists the
   field moves no byte).
4. Opt-in check (ALCLOCK-1): the Y-F0 reference argv plus `--info-clock-print`: every file but `summary.json`
   byte-identical; `summary.json` minus each scenario's `info_clock` = the absent run's file.

## Root at merge

- Merge AFTER this lane's review, BEFORE COV-MV (W3-9); COV-MV's root merge is keep-both in `nav_rules.cpp`,
  `nav_cli.cpp`, `nav_spec.cpp`, the CMake tail.
- C3-5 probe (C3-13) with `--rate info-clock-rate-v1` on real exes: expected one `--rule aim-partial-v5` (nav.rule)
  and one `--rate info-clock-rate-v1` on the NAV argv (the pytest shows it on fakes).
- W1-I2: the NAV exe changed: re-pin the lineage parents to the wave-3 P9-B0 references before the first P9 cell.
- OD3P (ALCLOCK-2): `info-clock-rate-v1` moves from `rules_pending` to `rules` in `flagged_members.json` (OD3P's item).
- Re-run: `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*`,
  whole target-tests, `scripts/tests/test_info_clock_rule.py`, canary both goldens.

## For PRE: the P9-C block

- **Rule and row.** `info-clock-rate-v1`, K-P9-7 kind `trade-rate`, x-flag `--rate info-clock-rate-v1`, no
  parameter (params_schema with no property), incompatible `per-name-v1, two-speed-v1, aim-partial-v6, spo-v1,
  spo-v2, spo-v3` (`atx-impl/src/book/nav_rules.cpp:75`). Rule: at each rebalance decision d, aim-partial-v5's
  theta per member i is theta_f = `two_speed_fast_theta(C)` = 1 - 2^(-C/5) at the rebalance cadence C (W3-16;
  .12945 at C = 1) when `ea_days_since[d, i]` lies in [0, 21], else theta_s = .05 (`--trade-fraction`, required);
  NaN is slow; row d only; one rate vector per decision for every book.
- **Registration.** `scripts/specs/p9/rules/info-clock-rate-v1.json`, SHA-256
  `51b8b754b7e16f49d2185a5e27bf5fa1260b8890f34490cea82d0bc1ffa4e33c` (LF bytes). `source_sample_end` 2019 (Bowles,
  Reed, Ringgenberg and Thornock 2024, JF 79(5), sample 1990-2019), FLAGGED under PRE-19 / PRE-31 (ALCLOCK-2). The
  `ea_days_since` mapping is stated as a FIXED CHOICE (fixed_choices[0]): the paper's information-release date = the
  reaction session of the name's latest visible primary earnings announcement as the role field records it; no other
  release enters the clock. ALCLOCK-3 stated (fixed_choices[5]). `data_class` H.
- **Template.** `scripts/specs/p9/templates/info-clock.json`, SHA-256
  `95d4104a421c32f91877649f1ec9dfbdb409c9a121d6f6e2377e5e25e19b42d5` (pinned in the registration's
  `manifest_fragment.rule_cell.template_sha256`); name `p9-info-clock`; parent null, nominal parent
  `../../v8/lib-v8ysb-gm.json` (Y-F0); `change.set` only `nav.output` (`build-equity/mega-nav-p9-info-clock`) and
  `monitor.output`; no `rules`, no `change.flags` (ALCOMB-1). The wave manifest (root's): kind `rule`, rules
  `[{name: info-clock-rate-v1, params: {}}]`, `gross_match: pm6-6-engine`, wave id `p9c-info-clock` (prereg B.4).
- **NAV argv token and the kind-rule route (C3-5 / C3-13).** The row puts `--rate info-clock-rate-v1` on the NAV argv
  (`wave_kinds.rule_flags` -> `nav_flags`: the x-flag, no params) on the append route (not a `--rule` row): the
  cell's argv carries the parent's `--rule aim-partial-v5` once (from `nav.rule`) and `--rate info-clock-rate-v1`
  once; a parent `--rate` value is replaced, never duplicated; `--info-clock-print` on the parent is kept once
  (`test_kind_rule_wave_puts_one_rate_and_one_rule`, fake world, `Cycle.nav_step`). `--rate` is rule-owned:
  `rule_cell.constants.flags` / a template's `change.flags` setting it is refused at preflight (exit 3). Root repeats
  the probe on real exes at this merge.
- **Parent conditions.** The parent NAV runs `--rule aim-partial-v5 --trade-fraction .05` at the fixed rate, without
  `--two-speed`, aim-partial-v6 or an spo rule (each refused with the rule); its `--fields` manifest lists
  `ea_days_since` point in time (refused otherwise, before output); a construction grid keeps the base cadence;
  `--capacity-curve` is accepted (ALCLOCK-3; Y-F0 runs it). Y-F0 (`lib-v8ysb-gm.json`) meets these at registration
  (cadence 1, .05, `--capacity-curve`, `--warm-start-sessions 60`, `--locate-in-aim`, delta orders). Fit mode: the
  parent's (NAV-only cell; root, at the gate's P9-B0).
- **The flag pair, cell / reference.** `--info-clock-print` on BOTH (ALCLOCK-1: root names it on the Y-F0 reference
  at the wave-3 P9-B0; the cell inherits it from its parent's NAV flags; it is no rule's flag, so the E2-8 guard
  passes it), and `--ann-mean-gross` on both (PRE-7a). The rule flag `--rate info-clock-rate-v1` on the cell only.
- **Producers of each printed figure** (all the NAV exe, `atx-equity-strategy-targets`):
  - fast-rate share of name-days: `summary.json` `scenarios[k].info_clock.fast_share` (and `fast_name_days`,
    `missing_name_days`, `name_days`), under `--info-clock-print` (`atx-impl/src/book/nav_write.cpp:289`
    `info_clock_json`; census `nav_decide.cpp:252`);
  - turnover share up (OD-3): `info_clock.turnover_share` = fast planned turnover / planned turnover, on the cell and
    on its parent (same producer, same flag);
  - gross up (OD-3): realised exposure `summary.json` `scenarios[k].exposure.mean_gross_leverage` (existing) and daily
    `gross_leverage`; gross return `scenarios[k].ann_mean_gross` under `--ann-mean-gross` (C3-9, existing). On TRAIN
    the cell is gross-matched (pm6-6-engine), so its mean gross exposure is the parent's by construction there;
  - cost per traded dollar and 4x: the capacity curve under `--capacity-curve` (`strategy_nav_v7.cpp` `capacity_row`:
    `cost_bps_per_traded_dollar`, `net_sharpe` at multiple 4) (existing; ALCLOCK-3 makes it run with the rule).
- **C3-4 (realised leverage).** The rule changes only the per-name rate, never L or the aim; per name the step is
  convex, but unequal rates can carry the planned gross above gross(aim) in a rotation (fast names fill while slow
  names unwind). Cap stated in the registration: planned gross <= (theta_f / theta_s) x gross(aim) by contraction,
  outside the dust band (2.589 x at cadence 1). P9-C is gross-matched (pm6-6-engine): the match stage solves L' so the
  cell's realised mean gross equals its parent's within C2's tolerance; the daily realised gross is printed.
- **DEC-16 restatement argument.** Y-5 (two-speed-v1) buckets THEMES into a fast and a slow sleeve (own aims and
  holdings, netted into one trade); this rule keeps one aim and one book and sets a rate per NAME-DAY from an event
  clock (the name's earnings announcement), whatever theme its alpha comes from: another decision unit and another
  object (a rate vector, no sleeve, no netting); R-3 is a per-signal aim gain at one rate.
- **Constants (no new one).** theta_f = Y-5's function, theta_s = Y-5's slow rate .05, window 21 = [29]'s month,
  field `ea_days_since` (`sec-ea-days-since-reaction-v1`).
- **Lapse.** P9-C lapses at 0 if this lane is not merged in root and APPROVE when the P9 cells start (PRE-14).

## Decisions

- No code slip to fix: the 16 WIP files compiled clean under /W4 /WX at the first build that got past the stale-PCH
  trap (`p9-alclock-a`: ccache's PCH mtime refusal, not a code red; `p9-alclock-b` after deleting the six
  `cmake_pch.cxx.{pch,obj}` by name, `CCACHE_RECACHE=1`).
- C3-4 line corrected against the handoff's draft ("realised gross stays bounded by the parent's L"): unequal per-name
  rates can lift the planned gross above gross(aim) in a rotation; the registration states the contraction cap
  (theta_f / theta_s) x gross(aim) and that P9-C's realised mean gross is the parent's by the pm6-6-engine match.
- The template's printed line follows the plan's P9-C row (fast-rate share of name-days, cost per traded $, 4x) and
  the OD-3 prediction (turnover share up, gross up); `--info-clock-print` is not in the template (ALCLOCK-1: named
  on the reference, inherited).
- The CLI two-speed case is shown on the v7 seam (`--cost-v2` claims the line); the replay's own refusal of the pair
  is shown in-process (on the fixture the replay's saved-sleeves check comes first).

## Concerns

- The working tree is NOT clean: the six files of teeth set 1 (`alclock-logs/teeth-set1.diff`) are mutated and
  uncommitted (the revert was refused). Nothing mutated is committed; HEAD is the correct code. Do not build or merge
  this tree's working copy before the six files are restored.
- Release is not built or tested (blocked); the Debug evidence of the C++ is `p9-alclock-e` (lane code = HEAD's;
  the tree then was root `1b64ce45` + the lane; root `fd084e78`'s merge touches no NAV file: seal_guard,
  classes.json, stage_chain, book_monitor, the store hook). The canary ran on `p9-alclock-b`'s exes (source
  `c3f3f1f8`; the lane code there equals HEAD's).
- C3-4: the handoff's draft line ("realised gross stays bounded by the parent's L") was not true for unequal
  per-name rates; the registration states the contraction cap and the gross match instead (PM to accept).
- `source_sample_end` 2019: the Bowles et al. sample quote is the first AL-CLOCK session's read (handoff), not
  re-read in this session (no web read; ALCLOCK-2 already ruled the value).
- The pytest's NAV row is a literal of what the exe prints; its C++ side is pinned by
  `InfoClockRate.RegistryRowAndParser`, but a drift between the two would show only at root's C3-5 probe on real exes.
- `build-equity` is KEPT (2.7 GB, configured and warm) for the resume, against the end-of-lane deletion rule, because
  the lane did not end (open question below).

## Permission refusals

One refusal; not retried, no same-effect substitute used (PERM-1). The exact command (one Bash call):

`cd C:/atx-wt/pool-8 && git checkout -- atx-impl/src/book/nav_validate.cpp atx-impl/src/book/nav_lockstep.cpp atx-impl/src/book/nav_load.cpp atx-impl/src/book/nav_decide.cpp atx-impl/src/book/nav_cli.cpp atx-impl/src/strategy_nav_v7.cpp && rtk proxy git status --short && "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/alclock-logs/tools/teeth.py 2 && rtk proxy git diff`

Its purpose: restore the six files of teeth set 1 to HEAD (the step the earlier handoff planned:
"commit first, mutate, build, run, `git checkout -- <files>`"), then apply set 2. Consequence: the six files still
carry the set 1 mutations in the working tree (uncommitted; HEAD is correct); the lane stopped every step that needs
the restored tree (teeth set 2, the Debug `-Canary` build of the merged head, the Release configure / build / tests /
canary). The PM / owner restores the six files to HEAD in an accepted form; `alclock-logs/teeth-set1.diff` is the
exact diff to drop.

## Disk

Nothing deleted yet (the lane did not end). Kept: `build-equity` 2.7 GB (configured; its `bin/` holds the teeth
target-tests exe of `p9-alclock-f` and `p9-alclock-b`'s canary exes); the one-time `build/` (592 KB, the failed
`dev` lease configure, ignored per r6-wave3-common). Own pytest basetemps under this session's scratchpad
(`alclock-bt-*`). C: 34 GB free at the stop.

## Ledger candidates

- AL-CLOCK: unequal per-name trade rates do not keep the planned gross within gross(aim) (fixed theta does); cap by
  contraction (theta_f / theta_s) x gross(aim); a gross-matched cell (pm6-6-engine) reads its parent's mean gross.
- `summary.json`'s `recipe_sha256` is the SHA-256 of the recipe's compact dump, not of `recipe.json`'s bytes: a
  substitution list for a recipe change needs both.

## Open questions

- Who restores the six teeth-mutated files to HEAD (the lane's `git checkout -- <files>` was refused): the PM / owner,
  or a form the PM names for the resumed lane?
- C3-4: is the registration's cap (planned gross <= (theta_f / theta_s) x gross(aim), with P9-C's mean gross matched
  to its parent by pm6-6-engine) the realised-leverage statement the prereg wants for P9-C?
- `build-equity` kept warm for the resume (2.7 GB): keep until the lane ends, or delete now?
