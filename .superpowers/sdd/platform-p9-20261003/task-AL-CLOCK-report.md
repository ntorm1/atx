# Lane AL-CLOCK report

## Outcome

R10 fix round 1 (ALCLOCK-S): DONE; see the section "Fix round 1 (ALCLOCK-S)" at the end (S-3 REQUIRED, S-1, S-2,
S-4 done; Debug `p9-alclock-k` and `-m`, teeth `p9-alclock-l` caught; registration SHA-256 now `7801c198...`).

R9 (pool-30, `feat/p9-alclock2-20261007`; TEETH-POOL, TEST-1): DONE_WITH_CONCERNS. The lane continued from its
committed head `1cbe5233` in the fresh pool-30; the records were copied from frozen pool-8 and committed
(`3cc8a356`); root `685e238b` merged (`adf1c6cb`, docs only, no conflict). One Debug build of the merged head,
`p9-alclock-h` (exit 0, no code warning; its `-Canary` check passed 15 / 15: the build started before ruling TEST-1
reached the lane); named filter 24 / 24 on its exe (EXE-CUR: receipt SHA = run SHA); `test_info_clock_rule.py`
5 / 5 at seeds 0 and 1. Deferred to the gate (TEST-1): the whole target-tests, the Release leg (configure, build,
named, whole, canary), the whole `scripts/tests`. Teeth set 2 skipped (`teeth.py` does not restore the bytes
itself: TEETH-RESTORE). No code changed in R9. The paragraph below is the R8 outcome as it stood in pool-8.

R8 (pool-8, at STOP 8): BLOCKED (PERM-1) after the code, tests and registration were done: the WIP compiled
clean, 13 gtests + 5 pytests
written and green on Debug (`InfoClock*` 13 / 13, named 24 / 24, whole target-tests 403 / 403 at `p9-alclock-e`),
registration and template committed, teeth set 1 caught every mutation; the permission system then refused the
revert of the teeth mutations (`git checkout -- <six files>`), so the six files still carry them in the working tree
and the steps that need the restored tree (teeth set 2, the Debug `-Canary` build of the merged head, the whole
Release leg) are not done. HEAD is correct; the PM / owner restores the six files, then `handoff-AL-CLOCK.md` "Next
step" finishes the lane.

## Branch / SHA

`feat/p9-alclock2-20261007` in `C:/atx-wt/pool-30` (TEETH-POOL; root merges THIS branch), working tree clean.
Commits of R9: `3cc8a356` records copied from frozen pool-8 (DIRTY-1 / TEETH-POOL); `adf1c6cb` merge of root
`685e238b`; then the records commit that carries this revision of the report (HEAD). Lane code head: `1cbe5233`
(no code commit in R9).

R8 (pool-8, `feat/p9-alclock-20261004` @ `1cbe5233`; frozen, its working tree still holds the six teeth set 1 files
for the owner, see "Permission refusals"): `c3f3f1f8` merge of root `1b64ce45`; `76a4f00b` tests, CMake tail,
pytest, registration, template; `e40ed78c` merge of root `fd084e78`; `1cbe5233` the rate-beside-an-aim-rule test
(C3-13 route is lane D3's). Earlier: `ebaf1e3f` (design handoff), `8a6ebe5b` (merge of root `4dc831ac`), `120f6a25`
`WIP(al-clock)` (the 16 C++ files, uncompiled at the stop; they compiled clean at `p9-alclock-b`, no slip to fix).

## Frozen base / lease

Lease untouched by this lane (contract: never touch a lease; pool-30 was leased by the PM). Root merged last:
`685e238b` (merge `adf1c6cb`, clean; R8: `fd084e78` via `e40ed78c`).

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
| `p9-alclock-h` (R9, pool-30) | `adf1c6cb` (root `685e238b` merged); ConfiguredProvenance `adf1c6cb` (clean); DirtyEntries 2 = the lane's tracked logs `alclock-logs/builds.txt`, `pyt-exits.txt` (no code) | the four canary targets, `-Canary` (started before TEST-1 arrived) | exit 0; 360 TUs, 17 links, 1021 s build (1505 s with the gate wait; jobs 2); only third-party / driver warnings (databento `getenv`, unused `/MP`). Canary check (Debug): exit 0, 15 passed. Exes: target-tests `822de6de...6268`, targets `cb59685e...8d74`, ic `09a1e3b5...379d`, eval `24ee34ea...670c` |

pool-8's trees (`p9-alclock-a` .. `f`) stay with the frozen pool (TEETH-POOL: nothing under its `bin/` is evidence).
Not built: Release (`equity-rel`, tag `p9-alclock-i` unused): deferred to the gate (TEST-1).

## Tests run (counts)

R9, pool-30, merged head `adf1c6cb` (clean code tree). Logs in `alclock-logs/`; one line per run in `gt-exits.txt` /
`pyt-exits.txt`.

| run | exe / head | result | log |
|---|---|---|---|
| Debug named filter `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*` | `p9-alclock-h` target-tests `822de6de...` (= receipt) | 24 / 24 passed (InfoClockRate 9, InfoClockPrint 4, NavSpec 4, NavRules 2, CostLawRegistry 2, TradeRateRegistry 1, TargetRegistry 1, NavAnnMeanGross 1), 0 failed, 0 skipped, ENV-1 hits 0 | `gt-h-named.log` |
| Debug canary (tiny world, Debug goldens) | `p9-alclock-h` `-Canary` | 15 passed, 0 failed (inside the build, before TEST-1) | `mega-p9-alclock-h-canary.log` |
| Debug whole target-tests | - | deferred to the gate (TEST-1); the last whole run is R8's 403 / 403 at `p9-alclock-e` (pool-8; lane code = HEAD's) | `gt-e-whole.log` |
| Release build, named, whole, canary | - | deferred to the gate (TEST-1) | - |
| `scripts/tests/test_info_clock_rule.py` seeds 0 / 1 | `adf1c6cb` (fakes; no exe) | 5 / 5 and 5 / 5 | `pyt-clock-h-s0.log`, `pyt-clock-h-s1.log` |
| `atx-engine/tools/test_stage_chain.py` seeds 0 / 1 | `adf1c6cb` | 10 / 10 and 10 / 10 (ran before TEST-1 arrived) | `pyt-chain-h-s0.log`, `pyt-chain-h-s1.log` |
| whole `scripts/tests` seeds 0 / 1 | - | deferred to the gate (TEST-1). pool-8's seed-0 run (`pyt-scripts-s0.log`) ran on the teeth-mutated tree with 81 ENV-1 hits: void, not evidence | - |

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
to fail `CapacityCurveAcceptsThisRateOnly`): NOT RUN. R8: the revert of set 1 was refused (below). R9: SKIPPED under
TEETH-RESTORE / TEST-1: `alclock-logs/tools/teeth.py` applies exact replacements but neither keeps and restores the
pre-mutation bytes itself nor asserts `git status --porcelain` empty and HEAD's blob SHA afterwards, so it is not run
again as it stands. Set 1's M10 already shows the line has teeth in one direction; the test's assertions for the
other direction (per-name-v1 with the capacity curve refused; aim-partial-v6 with this rate refused) pass on the
clean exe (`p9-alclock-h`, named filter 24 / 24).

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

- Root merges `feat/p9-alclock2-20261007` (pool-30), not pool-8's `feat/p9-alclock-20261004` (TEETH-POOL); the code
  is the same (`1cbe5233`), the new branch adds the records and the merge of root `685e238b`.
- At the gate (TEST-1): Debug whole target-tests, the Release leg (build, named, whole, canary) and the whole
  `scripts/tests` seeds 0 / 1, all deferred by the lane.
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
  `7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152` (LF bytes; fix round 1, review S-2: only the
  `realised_leverage` line changed, to ALCLOCK-4's wording; before it `51b8b754b7e16f49d2185a5e27bf5fa1260b8890f34490cea82d0bc1ffa4e33c`,
  the SHA at the APPROVE head `e9b3a323`, the one root merged in M4). `source_sample_end` 2019 (Bowles,
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
  Fix round 1 (Ruling ALCLOCK-4, review S-2): the registration's `realised_leverage` now carries the bound verbatim
  ("planned gross <= (theta_f / theta_s) x gross(aim) by contraction outside the dust band, 2.589x at cadence 1;
  P9-C's mean gross matched to its parent by pm6-6-engine") and the realised clause verbatim ("realised gross
  leverage is read from the cell's own summary (max and mean over the cell) and printed beside the parent's":
  `summary.json` `scenarios[k].exposure.max_gross_leverage` / `mean_gross_leverage`); the OD5-7 cap test applies to
  that realised figure, not to the planned bound. The prereg's P9-C block (round 2b-ii) carries the same two clauses.
  P9-C's own NAV shape (the rule under `--calibrate-gross`) is now covered in-process by
  `InfoClockRate.CalibrateGrossKeepsTheRatesAndCountsOnce` (fix round 1, S-3).
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

- R9: the whole target-tests, Release and the whole `scripts/tests` are not run on this head (deferred to the gate,
  TEST-1); the C++ evidence is Debug `p9-alclock-h` (named 24 / 24, canary 15 / 15) plus R8's whole run at
  `p9-alclock-e` (lane code = HEAD's). Teeth set 2 not run (no restore-capable tool).
- R9 build shape: `p9-alclock-h` built the four canary targets with `-Canary` (the dispatch before TEST-1); TEST-1,
  which arrived during the build, asks for target-tests only and no canary. Nothing ran after TEST-1 but the named
  filter (the canary result is reported because it exists).

R8 concerns (pool-8, as they stood):

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

R9 (pool-30): none.

R8 (pool-8):

One refusal; not retried, no same-effect substitute used (PERM-1). The exact command (one Bash call):

`cd C:/atx-wt/pool-8 && git checkout -- atx-impl/src/book/nav_validate.cpp atx-impl/src/book/nav_lockstep.cpp atx-impl/src/book/nav_load.cpp atx-impl/src/book/nav_decide.cpp atx-impl/src/book/nav_cli.cpp atx-impl/src/strategy_nav_v7.cpp && rtk proxy git status --short && "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/alclock-logs/tools/teeth.py 2 && rtk proxy git diff`

Its purpose: restore the six files of teeth set 1 to HEAD (the step the earlier handoff planned:
"commit first, mutate, build, run, `git checkout -- <files>`"), then apply set 2. Consequence: the six files still
carry the set 1 mutations in the working tree (uncommitted; HEAD is correct); the lane stopped every step that needs
the restored tree (teeth set 2, the Debug `-Canary` build of the merged head, the Release configure / build / tests /
canary). The PM / owner restores the six files to HEAD in an accepted form; `alclock-logs/teeth-set1.diff` is the
exact diff to drop.

## Disk

R9 (pool-30): after this report's commit the lane deletes by name the `build-equity` object tree of `p9-alclock-h`
(about 2.7 GB: `CMakeFiles/`, `atx-core/`, `atx-engine/`, `atx-impl/`, `atx-tsdb/`, `bin/`, `lib/`, `tests/` and the
top-level ninja / cmake files), keeping the tracked `audits/` and `v8-interim3-pitch-render-run2/` and the
`mega-p9-alclock-h-*` receipt / logs (copied to `alclock-logs/`); and its own pytest basetemps (`alclock-bt`). The
one-time `build/` left by the lease's `dev` configure is not the lane's (r6-wave3-common) and stays.

R8 (pool-8, frozen; kept for the owner):

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

R9:
- Teeth set 2 (ALCLOCK-3 over-loosened) is owed only if the PM wants it: it needs a restore-capable `teeth.py`
  (TEETH-RESTORE) and one more target-tests build; the reviewer or the gate could take it.
- C3-4 (below) is still open.

R8 (as they stood; the first is answered by TEETH-POOL, the third by the R9 deletion):

- Who restores the six teeth-mutated files to HEAD (the lane's `git checkout -- <files>` was refused): the PM / owner,
  or a form the PM names for the resumed lane?
- C3-4: is the registration's cap (planned gross <= (theta_f / theta_s) x gross(aim), with P9-C's mean gross matched
  to its parent by pm6-6-engine) the realised-leverage statement the prereg wants for P9-C?
- `build-equity` kept warm for the resume (2.7 GB): keep until the lane ends, or delete now?

## Fix round 1 (ALCLOCK-S)

R10, pool-30, `feat/p9-alclock2-20261007`. Contract: `resume10-addendum.md` (TEST-1, HOST-4), `r10-dispatch.md`
"ALCLOCK-FIX1", the handoff's "Fix round 1 (ALCLOCK-S)" design table; rulings ALCLOCK-S, ALCLOCK-S1a, ALCLOCK-S3a,
ALCLOCK-4, TEETH-RESTORE, HOST-4 and HOST-5 (the PM's message during the round: only builds under `C:\atx-wt\` count).
Start: handoff head `a662d1c2` (APPROVE head `e9b3a323`, code `1cbe5233`). The fix head is the commit that carries
this section; re-review 1 has BASE `e9b3a323` (the dispatch).

**Outcome: DONE.** S-3 (REQUIRED), S-1, S-2 and S-4 written and tested; one Debug build of the round
(`p9-alclock-k`), the teeth build (`p9-alclock-l`, caught), and one more Debug build (`p9-alclock-m`) because root
moved in NAV code (COV-MV) after `-k` and item 4 asks for the named filter on the merged tree. S-5 is P10 (nothing
in the lane). No permission refusal.

### Commits of the round

| commit | what |
|---|---|
| `65ec16af` | merge of root `b182dfdd` (C3b; root changed `nav_run.cpp`, `nav_spec.{hpp,cpp}`, `strategy_nav_spec_test.cpp`): no conflict |
| `170ac2b2` | S-3 gtest; S-4 row fixture + its gtest check; `alclock-logs/tools/mk_row_fixture.py` |
| `80b7ae69` | S-2 registration sentence; S-4 pytest reads the fixture; S-2 pytest asserts |
| `bcade871` | S-1: help, refusals, the conditional capacity sentence; its test checks and the refusal needles |
| `1d603d23` | merge of root `858a4fca` (D3W, ABS, D3, OD3P and this lane's `e9b3a323`): no conflict; no NAV file differs from the lane's |
| `9d3916b8` | build wrapper for HOST-4 / HOST-5 (`tools/build.sh`, new `tools/pool-builds.ps1`) |
| `b96fe073` | records of `p9-alclock-k`; the S-3 teeth tool `tools/teeth_s3.py` (committed before its run) |
| `ee70a69f` | records of the teeth build `p9-alclock-l` |
| `38967882` | merge of root `5c811b56` (COV-MV `25e11b32`; root changed `nav_cli.cpp`, `nav_decide.cpp`, `nav_lockstep.cpp`, `nav_rules.{hpp,cpp}`, `strategy_nav_v7.{hpp,cpp}`, the CMake tail): auto-merged, no conflict (W3-9 keep-both held: this round's hunks and COV-MV's sit in different places) |
| this commit | records of `p9-alclock-m`, this section, the handoff line |

### Items -> change -> test -> evidence

| item | change (file:line at the fix head) | test | evidence (`alclock-logs/`) |
|---|---|---|---|
| S-3 (REQUIRED; ALCLOCK-S3a shape) | `atx-impl/tests/strategy_nav_info_clock_test.cpp:765` `InfoClockRate.CalibrateGrossKeepsTheRatesAndCountsOnce`; helpers `all_rows_gross` `:252`, `four_decimals`, `text_of`, `rebalance_census` `:280`, `closed_form_rows` `:308` | the rule (`aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --exit-rate .05 --rate info-clock-rate-v1`) + `--order-basis delta --liquidity-cache --capacity-curve --info-clock-print`: `first` at 1.2 gives g (numpy_mean of the primary daily gross); target 1.1 g, tolerance g / 1000; L' = `leverage_4_decimals(1.2 x target / g)`; `second` at L' (4 decimals) and `calibrated` at 1.2 with `--calibrate-gross --calibrate-tolerance`, both with `--emit-holdings csv`. Asserted: every published file but `summary.json` (capacity directory and v7 files included) and the whole holdings directory byte-equal to `second`; `summary.json` minus `calibration` = `second`'s; `corrected` and `construction_reused` true; leverage finite in [1, 2] and bit-equal L'; `gross_requested` = g, `gross` = `second`'s G (bits), `matched` = (abs(G - target) <= tolerance); recipe `rate` = the id in `recipe.json` and `capacity/recipe.json`; the closed form rule = held + theta_i (L' desired - held) on every moved member row of the calibrated holdings, theta_i by row d of the clock, fast and slow rows > 0; every main block's `name_days` / `fast_name_days` / `missing_name_days` = the members of the primary daily CSV's decision-and-rebalance rows (5 main books; pass 1's book is dropped); no block in `capacity/summary.json`; a 4-worker calibrated run (no holdings) = the 1-worker one byte for byte. Left out, stated in the test's comment (ALCLOCK-S3a): `--neutralize price-risk-v1`, `--locate-in-aim` (40 sessions < `min_return_pairs` 126) and `--warm-start-sessions` (score_begin 0) | `gt-k-clock.log`, `gt-k-named.log`, `gt-m-named.log`; teeth `gt-l-teeth.log` |
| S-1 (a) help (ALCLOCK-S1a) | `atx-impl/src/strategy_nav_v7.cpp:1070-1071` (`--capacity-curve`: "the fixed rate or --rate info-clock-rate-v1 (Ruling ALCLOCK-3: it reads no NAV), not per-name-v1"); `atx-impl/src/book/nav_cli.cpp:95-96` (`--book-workers`: "the fixed rate or info-clock-rate-v1, not per-name-v1 or an spo rule") | help only (no test reads the help; grep) | `check-fix1-s1.log` |
| S-1 (b) refusals (ALCLOCK-S1a) | `strategy_nav_v7.cpp:1441-1442` "aim-partial-v6 needs the fixed rate, --capacity-curve the fixed rate or info-clock-rate-v1 (per-name-v1 reads the NAV)" (predicate untouched); `atx-impl/src/book/nav_lockstep.cpp:218-220` "--book-workers above 1 needs the fixed rate or info-clock-rate-v1 (per-name-v1's rates share one buffer) and no spo rule (...)" (predicate untouched; no test needles it) | `CommandLineRefusalsWriteNothing` case 8 and `CapacityCurveAcceptsThisRateOnly`'s parse matrix needle the new message | `gt-k-clock.log` |
| S-1 (c) capacity sentence, conditional | `strategy_nav_v7.cpp:215-226` `capacity_info_clock_declaration` + `capacity_rule(info_clock)` beside `capacity_declaration` (unchanged; precedent `capacity_spo_v3_rule`); `State::info_clock` `:92`, set in `configure()` `:875` from `cfg.rate`; `declarations(s, adv_hold, info_clock)` `:306` for the summary `:613` and the holdings manifest `:1049` (state), the recipe by its own `rate` key `:1019-1021` (nav_write sets it only under the rule, before `extend_recipe`); `v7_extras.json` `capacity_rule` `:497` (state). Not pinned anywhere (grep of tests, specs, goldens, fixtures, ledger; the tiny canary runs no `--capacity-curve`) | `CapacityCurveAcceptsThisRateOnly`: under the rule the sentence in `recipe.json` `v7.capacity`, `summary.json` `v7.declarations.capacity` and `v7_extras.json` `capacity_rule` names the rate and lacks "fixed rate only"; a fixed-rate capacity run keeps "; fixed rate only; x1 is S2 bit for bit" and no "info-clock" | `gt-k-clock.log` |
| S-2 (ALCLOCK-4) | `scripts/specs/p9/rules/info-clock-rate-v1.json:51` `realised_leverage` only: "Rulings C3-4 and ALCLOCK-4" (C3-4 kept), the bound verbatim, the realised clause verbatim, "the OD5-7 cap test applies to that realised figure"; the superseded clause ("the realised gross the OD5-7 cap reads is the parent's") removed. SHA-256 `51b8b754...` -> `7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152` (LF; "For PRE" re-quoted) | `test_registration_and_template` asserts both verbatim clauses, ALCLOCK-4, the OD5-7 sentence and the old clause's absence | `pyt-clock-k-s{0,1}.log`, `pyt-clock-m-s{0,1}.log` |
| S-4 | new `atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json` (the row alone, as `row_json` prints it; generated from `nav_rules.cpp`'s own literals by `tools/mk_row_fixture.py`, `--check` = True); `RegistryRowAndParser` `:885` asserts the listed row == the fixture (`ATX_IMPL_TESTS_DIR`); `scripts/tests/test_info_clock_rule.py:48`, `:80`: `NAV_TABLE`'s last row is the fixture | `RegistryRowAndParser`; the five pytests | `gt-k-clock.log`, `pyt-clock-*` |
| S-5 | P10 (record only) | - | - |

### Builds (equity-dev, `build-equity`, through `scripts/research-build.ps1`; one build on the host at a time)

| tag | source (receipt) | result |
|---|---|---|
| `p9-alclock-k` | `9d3916b8` (root `858a4fca` merged); ConfiguredProvenance `9d3916b8` (clean); DirtyEntries 2 = the tracked logs `builds.txt`, `pyt-exits.txt` | exit 0; 139 TUs, 6 links, 90 s (jobs 4, free 3615 MiB); 0 warning lines; target-tests `44a2dd76e8f3267f3c4c3288fb792291ed01e02d9b5b7cacdb0297349bfab039` (= the run's exe) |
| `p9-alclock-l` (teeth) | `b96fe073` + 1 dirty (the mutated `nav_session.cpp`) | exit 0, 1 TU; teeth exe `202aad461e912dbb...` (never evidence of the code) |
| `p9-alclock-m` | `38967882` (root `5c811b56`, COV-MV, merged); ConfiguredProvenance `38967882` (clean); DirtyEntries 2 = `builds.txt` and this report (records) | exit 0; 34 TUs; 0 warning lines; target-tests `78311aa94f0ab61de61ed79ea3b4ceb99b2e6cb2d376234882049621395ddd9e` (= the run's exe) |

The single-TU checks (`atx-build.ps1 -Preset equity-dev check`) of the test file and of `strategy_nav_v7.cpp`,
`nav_cli.cpp`, `nav_lockstep.cpp` compiled clean under /W4 /WX (`check-fix1-s3-test.log`, `check-fix1-s1.log`; the
first one built the 220 prerequisite objects of a fresh tree from the cache). Every build waited on the HOST-5 probe
(`tools/pool-builds.ps1`: no pool build, 60 s quiet, research-build's own memory thresholds); none had to wait.

### Tests (TEST-1: the lane's filters and pytest file only)

| run | exe / head | result | log |
|---|---|---|---|
| `InfoClock*` | `p9-alclock-k` `44a2dd76...` | 14 / 14 (InfoClockRate 10, InfoClockPrint 4; the new test passes), ENV-1 0 | `gt-k-clock.log` |
| named `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*` | `p9-alclock-k` | 26 / 26 (NavSpec 5 with root's C3-7 test) | `gt-k-named.log` |
| named filter (root moved in NAV code: COV-MV) | `p9-alclock-m` `78311aa9...` at `38967882` | 26 / 26, ENV-1 0 | `gt-m-named.log` |
| `scripts/tests/test_info_clock_rule.py` seeds 0 / 1 | `9d3916b8` | 5 / 5, 5 / 5 | `pyt-clock-k-s0.log`, `-s1.log` |
| the same, seeds 0 / 1 | `38967882` | 5 / 5, 5 / 5 | `pyt-clock-m-s0.log`, `-s1.log` |
| whole target-tests, Release, canaries, whole `scripts/tests`, stage chain | - | deferred to the gate (TEST-1) | - |

### Teeth (S-3; TEETH-RESTORE; build `p9-alclock-l`)

- Tool `alclock-logs/tools/teeth_s3.py` (committed at `b96fe073` before its run): asserts porcelain empty and
  hash-object = HEAD blob, keeps the bytes (memory and a git-ignored copy), one CRLF-aware replacement asserted
  unique in `atx-impl/src/book/nav_session.cpp`: `if (clock_rule)` (before `nav_info_clock_rates`) -> `if (clock_rule
  && !(tape && tape->replay))` (the restored pass of `--calibrate-gross` stops forming the rates; the construction's
  initial vector, every name at .05, stays). One build; in `finally`, before any test: raw SHA-256 `c86532ab...` =
  before, hash-object = HEAD blob `0aa59660...`, porcelain empty: OK (`teeth-s3.log`). No checkout / restore / stash.
- `InfoClock*` on the teeth exe: 14 ran, 13 passed, 1 FAILED, exactly the expected one:
  `CalibrateGrossKeepsTheRatesAndCountsOnce` (14 published files differ from the plain run at L' `:81`, the summary
  minus calibration `:805`, the holdings directory `:72`, the closed form on 194 moved rows `:326`, the published gross
  `:815`). The census assertions pass under the mutation (the census reads the clock, not the rates): as designed,
  the rates' survival is shown by the bytes and the closed form (`gt-l-teeth.log`).

### Flag-absent identity (for root)

No output of a run without `--rate info-clock-rate-v1` moves: the capacity sentence is selected by the rule (the
old constant is unchanged, and the test shows a fixed-rate capacity run still prints it); the rule's arithmetic is
untouched. What changes by design on every run: the `--help` text (two clauses) and the two refusal messages
(ALCLOCK-S: help / refusal text). The procedure of "Flag-absent identity procedure" above stands as written; no new
substitution. New files: `atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json`; no new `atx.<name>/v<n>`
literal, flag, JSON key or rule id; the CMake tail is unchanged.

### What root re-proves at merge

- Merge the fix head of `feat/p9-alclock2-20261007` (pool-30); root already holds `e9b3a323` (M4), so the merge brings
  only this round. Then the named filter above on root's Debug target-tests and `scripts/tests/test_info_clock_rule.py`.
- The registration's SHA-256 is now `7801c198...`; it is pinned nowhere yet (W1-I2's re-pin at the gate reads it);
  the template SHA (`95d4104a...`) is unchanged.
- COV-MV is already in this branch (`38967882`): the root merge of this round is not a keep-both step.

### Disk, refusals, open questions

- Disk: after this commit the round deletes by name the `build-equity` object tree (`CMakeFiles/`, `atx-core/`,
  `atx-engine/`, `atx-impl/`, `atx-tsdb/`, `bin/`, `lib/`, `tests/` and the top-level ninja / cmake / cpack files,
  `compile_commands.json`, `spdlog.pc`, `vcpkg-manifest-install.log`), keeping the tracked `audits/`,
  `v8-interim3-pitch-render-run2/` and the `mega-p9-alclock-*` receipts and logs (copied to `alclock-logs/`); and its
  own scratch (the teeth copy `alclock-logs/teeth-s3-p9-alclock-l.orig`, the pytest basetemps). About 2.7 GB.
- Permission refusals: none.
- Open (one line): `p9-alclock-m` is a second Debug build of the round (TEST-1 says one), run because r10 item 4 asks
  for the named filter after a merge that moved NAV code; the PM may count it as the merge's check.
