# Task C3 report: typed NavSpec, the K-P9-7 registries, the NAV file split, the core split

Lane C3, pool-15, branch `feat/p9-c3-20261004`, base `e1c16051` (C1, C2, COV, AL-COMB, S2, D2 merged), root head
`ab189471` merged in (S2R, B2 identity part). Contract: `resume4-dispatch.md`, `briefs/brief-C3.md`, `wave3-carry.md`
(C3 section, PM answers Q1-Q18, "Added after drafting"), plan section 2.4 C3, rulings C2-9 / C2-10 / C2-11, COV-3,
W3-6 / W3-8 / W3-9, E2-11, W1-MIN, P13, P14.

## Outcome

DONE_WITH_CONCERNS. Part A (all C++) is done, built and tested in Debug; part B (C2-11, C2-9, C2-10, W3-6: the Python
`pm6-6` retirement in files E2 rewrote) is **pending**: root had no E2 merge when part A finished (`git -C
C:/atx-wt/pool-2 log`: last merges S2R `0f7a84b1`, B2 `c2416d62`), so by the dispatch rule it waits for a short
follow-up round.

| commit | what |
|---|---|
| `4e958185` | `wip(c3)`: NavSpec, the K-P9-7 registries, `--cost-law` / `--target-rule`, P13 NAV seal, M-1 test, core split |
| `7e4c9dc4` | `refactor(c3)`: brief task 1, the move-only split of `strategy_nav_replay.cpp` into `atx-impl/src/book/` (alone) |
| `9dace914` | merge of root `ab189471` (conflict: the `atx-impl/CMakeLists.txt` tail, below) |
| `6abd91b2` | `build(c3)`: a deferred configure check refuses any source given to the INTERFACE `atx-impl-core` |
| (report commit) | this report, `handoff-C3.md`, `c3-logs/` |
| `9a7c490f` | `fix(c3)`: fix round 1 (R-1, R-2, C3-3, PRE-7 / PRE-7a, S-1, S-4, S-5, S-6 part, S-7); see "Fix round 1" |
| (fix-round docs commit) | "Fix round 1" below, the handoff, `c3-logs/fix1-*` |

Fix round 1: DONE_WITH_CONCERNS (all required items built and tested in Debug and Release, teeth shown; the explicit
before / after exe comparison was refused by the permission system and is left to root, section "Fix round 1").

## What was built

### NavSpec (`atx.nav-spec/v1`; `atx-impl/src/book/nav_spec.{hpp,cpp}`)

- `nav --spec NAVSPEC.json [--output NEWDIR]` runs one spec file; `nav <flags> --print-spec` prints the NavSpec of a
  command line (the flag adapter; nothing runs). Both are read by `dispatch_nav_replay` right after `--list-rules`.
- Document: `{"schema": "atx.nav-spec/v1", "options": {...}, "constants": {...}}`. `options` has one typed key per run
  flag of BOTH parsers (the replay's and the v7 seam's: 71 keys, snake case of the flag name): switch `true`, finite
  number, non-negative integer count, non-empty text, choice from the parser's own list, `[lo, hi]` pair
  (`--rate-clip`). Unknown keys, wrong types, non-finite or partial numbers, a fractional or negative count, another
  schema and extra top-level keys are refused (exit 2) before anything runs.
- One parser (decision D-1): a spec runs as the canonical command line `to_args` renders (keys in table order,
  numbers in shortest round-trip form, so every value parses to the same bits) through `dispatch_nav_replay`; the
  flags stay the parser, `from_args` maps flags to the same NavSpec. A spec and its flags therefore configure the
  same run; `NavSpec.FlagsAndSpecSameConfig` checks it through the v7 parser too, and
  `NavSpec.SpecRunPublishesTheFlagRunBytes` checks the published bytes.
- Compiled constants as NavSpec fields (brief task 2): `constants` = `initial_nav` 1e9, `liquidity_window` 63,
  `min_vol_pairs` 20, `capacity_multiples` [.5, 1, 2, 4, 8], `leverage_range` [1, 2] (`--print-spec` prints them).
  The first four are accepted at today's value only (D-2); `leverage_range` refuses anything wider than [1, 2]
  (OD-P9-3), accepts a narrower range around the run's `aim_leverage` (default 1), and refuses a narrower range with
  `calibrate_gross` (PM6-6 corrects inside [1, 2]).
- C2's surface: `calibrate_gross`, `calibrate_tolerance` keys; capability `calibrate-gross-pm6-6` kept;
  `strategy_nav_calibrate.*` untouched; `aim_leverage_min/max` = `leverage_range`; the calibration record stays in
  `summary.json` (CalibrateGross.* green; `construction_reused` moved unchanged through the split, C2 review S6).
- COV-3: `risk_model` / `risk_model_sha256` are text keys; a container-bearing store is passed as today (no code).

### K-P9-7 completion (`atx-impl/src/book/nav_rules.{hpp,cpp}`)

One `atx.nav-rules/v1` table (`nav --list-rules [--json]`), kinds in order; C1's leverage rows are printed byte for byte
first. Capabilities gain `nav-spec-v1`, `fields-seal-p13`.

| kind | id | x-flag | params (x-flag) | incompatible |
|---|---|---|---|---|
| trade-rate | `aim-partial-v5` | `--rule aim-partial-v5` | trade_fraction (`--trade-fraction`, (0, 1], .25), dust_multiple (`--dust-multiple`, [0, .5], 0), exit_rate (`--exit-rate`, (0, 1], 1) | aim-partial-v6, spo-v1/v2/v3 |
| trade-rate | `per-name-v1` | `--rate per-name-v1` | rate_rra, rate_lambda ((0, 1e6]), rate_min, rate_max ((0, 1]) at `nav_rate_*` | aim-partial-v6, spo-v1/v2/v3, two-speed-v1 |
| trade-rate | `two-speed-nav-v1` | null | none | per-name-v1, aim-partial-v6, spo-v1/v2/v3 |
| cost-law | `flat-bps-v1` | null (fixed S1) | none | none |
| cost-law | `sqrt-impact-v1` | null (fixed S2 / S3) | none | none |
| cost-law | `ko-invariance-v1` | null (on with cost-sqrt-v2) | none | none |
| cost-law | `fim-live-v1` | null (on with cost-sqrt-v2) | none | none |
| cost-law | `cost-sqrt-v2` | `--cost-law cost-sqrt-v2` | none (params {}) | none |
| target | (none: the slot) | `--target-rule ID` | | |

Every row: `{id, kind, x-flag, params_schema (closed object, every property's x-flag), incompatible, description}`;
defaults come from the exe (`TargetReplayConfig{}`, `nav_rate_*`). Parser entries (`strategy_nav_v7.cpp`,
`registry_flag`): `--cost-law cost-sqrt-v2` is byte for byte the `--cost-v2` run (giving both, a fixed law, a stress law
alone or an unknown id is a usage error); `--target-rule ID` refuses every id ("none registered"), and a registered
row without a handler is refused, never ignored (COV-MV adds `mv-aim-v1`'s row and its handler; AL-CLOCK adds its
trade-rate row and a `--rate` value: Ruling W3-9, append-only). The ids recipes already print (`flat-bps-v1`,
`sqrt-impact-v1`, `ko-invariance-v1`, `fim-live-v1`) name the cost laws. `cost-sqrt-v2` is the id E2's kinds fixture
`scripts/specs/p9/kinds/new-cost-model.json` already names with `params {}`: E2's `nav_flags` derives `--cost-law
cost-sqrt-v2` from the row, so P14 / G-P2's fixture becomes runnable by the root flip alone (W3-8).

### P13 NAV half (ruling P13; `book/nav_load.cpp` after the split)

`fields_seal` (S2's IC rule, copied): the `--fields` manifest's `seal.exclusive_end` must be this build's research seal
(2024-01-01); no seal block or no `exclusive_end` = legacy, accepted and logged (`nav replay: fields-seal seal=absent
accepted-as-legacy-manifest`, console only, never published); another seal or a malformed block refuses before any
payload or output. The decide path's shared loader refuses the same way (it has no progress stream, so no log line).

### W1-MIN M-1

`NavBookWorkers.V6CostV2CapacityAdvHoldOnFourEqualsOne`: `--rule aim-partial-v6 --cost-v2 --capacity-curve
--adv-hold-q .05` on `--book-workers 4` publishes every byte (recursively, the capacity directory included) of the
one-worker run.

### Brief task 1: the move-only NAV split (`7e4c9dc4`, alone)

`strategy_nav_replay.cpp` (4232 lines) is now 14 TUs under `atx-impl/src/book/`: `nav_validate` (config / input
validation, cost model), `nav_liquidity` (execution liquidity, events), `nav_mark` (MARK), `nav_execute` (EXECUTE),
`nav_decide` (DECIDE: shared construction, plans, close), `nav_session` (the books' session loop), `nav_summarize`,
`nav_write` (recipe / summary JSON, CSV writers), `nav_load` (pinned loaders, admission), `nav_lockstep` (scenarios,
workspace, lockstep books), `nav_run` (run_nav_replay, calibration, grid run), `nav_publish`, `nav_cli` (the nav
verb), `nav_deploy` (the decide-path seams). Every unit moved unchanged (generated, not hand-edited: a unit parser
cut the file at the seams; a line-multiset check shows the only differences are namespace wrappers, the include line,
the header's declarations and three default-argument lines). What crosses a seam lives in
`book/nav_replay_internal.hpp` (namespace `nav_book`): the anonymous-namespace types, constants and templates two TUs
use, unchanged, and declarations of the functions one TU defines and another calls (defined in `namespace nav_book` of
their TU; default arguments on the declaration only). The TUs keep the old TU's Debug /O2 flag set.

### Brief task 3: the core split (F-10; `atx-impl/CMakeLists.txt`)

`atx-impl-strategy` (the research path: every `strategy_*` TU, `src/book/**`, and the shared files they read:
`artifacts`, `serialize_panel`, `panel_artifact`, `stage_data_provenance`, the generated build provenance) and
`atx-impl-pipeline` (config, dispatch, every other `stage_*`, trial ledger, ...; links `atx-impl-strategy`).
`atx-impl-core` is an INTERFACE library over both, so the atx-impl exe and every test target link one name unchanged.
The five research exes (`atx-equity-strategy`, `-ic`, `-targets`, `-risk`, `-mine`) link `atx-impl-strategy` alone and
build without compiling a pipeline TU (p9-c3-a / -b compiled none). `strategy_spo.*` and COV's RiskStore read path
moved unchanged into `atx-impl-strategy` (COV-3; `CovStore.*` green). W1-MIN M-5: the D1 block's /O2 comment
corrected (the ISA-macro rule, not /O2, keeps `strategy_ic_detail.hpp`'s inline variables one definition).

## Carry items (wave3-carry C3, All lanes, Added after drafting)

| item | tag | status |
|---|---|---|
| W1-MIN M-1 | required | done (above) |
| P13 NAV half | required | done (above) |
| COV-3 | required | done: moved unchanged; CovStore.* green; `risk_model` key |
| C2's surface in NavSpec | required | done (above) |
| K-P9-7 completion + target slot | required | done (above) |
| P14 / G-P2 | required | done on C3's side (row `cost-sqrt-v2`); the fixture flip is root's (W3-8) |
| Identity X-5 / Y-F0 `--spec` vs argv; Release = Debug NAV | required (root) | lane proof on fixtures + tiny_world (below); root commands below |
| delete `wave_rules.py:110-119` (C2-11) | required | PENDING (part B: E2 not in root) |
| C2-9 plan-time refusal | required | PENDING (part B) |
| C2-10 digest re-check before `wave_result.build` | required | PENDING (part B) |
| W3-6 PM7-35 copy `wave_rules.py:50-51` | required (C3's) | PENDING (part B; deleted only after root's identity vs B1's predicate) |
| E2-11 SPO rows in the NAV table | optional | not done (no P9 SPO cell; `incompatible` lists name spo-v1/v2/v3) |
| S2-FIX1 (5) | optional | moved to D3 (W3-1), not C3's |
| C1-SPO leftover (`capacity_spo_v3.rule` gtest pin) | optional | not done |
| C2 review S6 (`construction_reused` through the split) | optional | kept (moved unchanged; CalibrateGross.* green) |
| W1-MIN M-5 comment | optional | done |
| SQL2-CLS literals | required | `atx.nav-spec/v1` (new) for root's `classes.json` |
| CMake tails (P2) | required | listed below |

## Tests (Debug and Release)

| build | what | result |
|---|---|---|
| p9-c3-a (Debug) | named: `NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavFieldsSeal.*:NavBookWorkers.*:NavBookRule.*` | 18 / 18 (`c3-logs/gtest-a-named.log`) |
| p9-c3-a (Debug) | whole `atx-impl-strategy-target-tests` | 387 / 387 (`c3-logs/gtest-a-whole.log`) |
| p9-c3-b (Debug, split) | whole `atx-impl-strategy-target-tests` | 387 / 387 (`c3-logs/gtest-b-whole.log`) |
| p9-c3-b (Debug, split) | `-Canary` tiny_world (Debug goldens: u, fit, w, NAV primary daily CSV digests) | 15 passed |
| p9-c3-c (Debug, merged head) | built the five research exes (atx-impl-strategy alone), atx-research-eval and the `atx-impl` exe (pipeline + strategy + B2's ledger library); whole target-tests | 387 / 387 (`c3-logs/gtest-c-whole.log`); canary 15 passed |
| p9-c3-d (Release) | refused: ccache-restored PCH older than this tree's `deps/equity-rel` fetch (C2's known stale-PCH trap) | exit 1, nothing tested |
| p9-c3-e (Release, merged head + guard; the six `cmake_pch.cxx.{pch,obj}` deleted by name, `CCACHE_RECACHE=1`) | whole target-tests; `-Canary` (Release goldens) | 387 / 387 (`c3-logs/gtest-e-rel-whole.log`); canary 15 passed |

Release = Debug on the lane's fixtures: both build types pass the same byte-identity tests (spec vs flags, cost-law vs
--cost-v2, 1 vs 4 workers, the NAV pins) and their own recorded tiny_world NAV goldens.

New gtests: `NavSpec.FlagsAndSpecSameConfig`, `NavSpec.RefusesWhatItCannotType`, `NavSpec.SpecRunPublishesTheFlagRunBytes`,
`NavRules.OneTableEveryKindInOrder`, `CostLawRegistry.RowsAreTheReplaysLawsAndOneNameableSet`,
`CostLawRegistry.CostSqrtV2IsTheCostV2RunByteForByte`, `TradeRateRegistry.RowsCarryTheParsersFlagsAndDefaults`,
`TargetRegistry.SlotIsEmptyAndRefusesEveryId` (`strategy_nav_spec_test.cpp`), `NavFieldsSeal.ManifestSealMustBeThisBuildsOrAbsent`,
`NavBookWorkers.V6CostV2CapacityAdvHoldOnFourEqualsOne` (`strategy_nav_replay_test.cpp`). Changed:
`NavBookRule.ListRulesJsonCarriesTheLeverageRows` and `.AimLeverageIsOptionalWithItsDefault` now select the rows of kind
leverage (the table carries the other kinds by contract; the leverage rows' assertions and their bytes are unchanged;
the full table is pinned by `NavRules.OneTableEveryKindInOrder`). No expected hash, golden or pin was edited.
No pytest suite was touched (no Python changed in part A).

## Cross-lane edits

- `atx-impl/tests/CMakeLists.txt` (T1's): one block appended at the tail (`strategy_nav_spec_test.cpp` into
  `atx-impl-strategy-target-tests`).
- `atx-impl/CMakeLists.txt`: C3 restructures it (the core split); every earlier lane's tail block keeps its place
  and comment, retargeted to `atx-impl-strategy` (S1, B1, D1, AL-COMB, D2, C2) or `atx-impl-pipeline` (Lane 9's
  `stage_equity_mine`). Merge `9dace914`: B2's appended `target_link_libraries(atx-impl-core PUBLIC
  atx::engine-research-ledger)` became `atx-impl-pipeline` (an INTERFACE library takes no PUBLIC link; trial_ledger and
  its users are pipeline TUs). **Wave-3b appenders**: a research source goes into `atx-impl-strategy`;
  `target_sources(atx-impl-core ...)` would compile nothing.

## Root at merge (exact items and commands)

1. Build (both presets): `-Targets "atx-impl-strategy-target-tests,atx-equity-strategy-targets,atx-equity-strategy-ic,
   atx-research-eval,atx-impl"` (`-Canary`); gtests: the named filter above plus `CalibrateGross.*:CovStore.*`, then the
   whole `atx-impl-strategy-target-tests`; the whole `atx-impl-tests` once (pipeline TUs link through
   `atx-impl-core`; the lane built the `atx-impl` exe, not that test target). The core split renames the PCH
   owners (`atx-impl-strategy.dir`, `atx-impl-pipeline.dir`): if a tree refuses with "has been modified since the
   precompiled header", delete its `cmake_pch.cxx.{pch,obj}` by name and rebuild with `CCACHE_RECACHE=1`.
2. X-5 and Y-F0 via `--spec` vs argv (same exe; expected: every file byte-identical, no substitution):
   `EXE=<bin>/atx-equity-strategy-targets.exe`;
   `$EXE nav <the cell's NAV argv, verbatim> --print-spec > <scratch>/x5.navspec.json` (prints, runs nothing);
   `$EXE nav <the cell's NAV argv with --output <NEW>/x5-argv>`;
   `$EXE nav --spec <scratch>/x5.navspec.json --output <NEW>/x5-spec`; compare the two trees recursively. Same for
   Y-F0. Flag-absent identity against the pre-C3 reference: the argv run equals the reference except `summary.json`
   `producer.engine_git_sha` (P9-B0 volatile list); the console gains the P13 line only if a fields manifest has no
   seal.
3. Release = Debug NAV on X-5 (G-P3 NAV half, M1c): the Release exe's argv run equals the Debug exe's except
   `summary.json` `producer` (`engine_git_sha`, `build_type`).
4. Research-exe build time before / after the core split (F-10): a fresh tag of the five research exes against the
   pre-C3 tree's receipt.
5. W1-I2: the NAV exe changed; re-pin the lineage parents before the first P9 cell.
6. W3-8 fixture flip (root post-merge, D3 owns the kind files): drop `new-cost-model` from `SCHEMA_ONLY_FIXTURES`
   (`scripts/tests/test_wave_kinds.py` :57 / :193 / :362 at E2's head) and its "SCHEMA ONLY" description; the rule name
   `cost-sqrt-v2` already matches the row. Any frozen copy of the NAV table in E2's tests is a listed contract change
   (new rows; C1's rows unchanged).
7. SQL2-CLS: add `atx.nav-spec/v1` to `classes.json` (a NavSpec file a cell pins).
8. Part B follow-up round (C2-11, C2-9, C2-10, W3-6) after E2's merge.

## Decisions

- D-1 NavSpec through the one parser: two parsers already exist (the v7 seam rewrites `--rule aim-partial-v6` and
  passes the rest on); a second typed parse path could drift from them, a canonical rendering cannot. Typing is at
  the spec boundary; numbers round-trip to the same bits.
- D-2 Constants pinned at today's values: `initial_nav` is the "1bn" of the scenario ids and the capacity books'
  base, `capacity_multiples` name the capacity files, and moving the liquidity window would need a new unowned
  research flag outside the rule registry (F-8). A moved constant is a new registered rule (NavSpec v2), not an edit.
- D-3 `cost-sqrt-v2` is the v2 stress pair (KO + FIM, both square-root laws), the id E2's fixture already uses; the
  two laws are listed with null x-flag because the exe runs them only together (one-law books would be a new rule).
- D-4 `two-speed-nav-v1` (null x-flag): `two-speed-v1` is the IC table's id (an id in both tables breaks E2's
  `rule_rows`), and naming it already sets `--two-speed` on the NAV.
- D-5 `--target-rule` sits in the v7 parser (COV-MV's rule reads the risk store the v7 seam opens) and fails closed.
- D-6 The split is generated from a unit parser (scratch script, not committed) so the move is mechanical; the
  internals cross seams in `namespace nav_book`, not the public header.

## Concerns

- C-1 Part B pending (by rule): the Python `pm6-6` mode, C2-9, C2-10 and W3-6 remain until a follow-up round on a
  root head that contains E2.
- C-2 Merged root `ab189471`, not a wave-2 gate head (none exists yet); root re-merges at its gate.
- C-3 The split makes some per-name calls cross TUs (e.g. EXECUTE -> `liquidity_row`), so the compiler cannot inline
  them without LTO; values are unchanged (IEEE, no FMA at the x64 baseline; tests and canary hold), wall time may
  move: root's X-5 run shows it.
- C-4 `nav --list-rules` (text and JSON) gains rows: any consumer that assumed leverage rows only (C1's two tests did)
  must select by kind.

## Ledger candidates

- C3-1: NavSpec = typed spec over the one nav parser (`nav --spec`, `--print-spec`); constants pinned in v1.
- C3-2: `atx-impl-core` is INTERFACE since C3; research sources go to `atx-impl-strategy`.

## Fix round 1

Review `99b676f7` (CHANGES_REQUIRED: R-1, R-2; S-1..S-8) and rulings C3-3, C3-4, PRE-7, PRE-7a. Code and tests in
`9a7c490f` (one commit on `99b676f7`; nothing squashed); this section, the handoff and the logs in the docs commit
after it. Part B (C2-11, C2-9, C2-10, W3-6) is still pending (C3-2) and was not started.

The round was stopped after build j and resumed by a fresh agent (same pool, same branch, no history rewritten, no
code changed on resume): it re-checked every item below against the code at `9a7c490f`, ran the Release j gtests,
the teeth build k, the clean Debug build l with the canary, and the tiny-world opt-in tree check. Logs: `c3-logs/`.

### Summary table

| finding | change (file:line at `9a7c490f`) | test (`atx-impl/tests/strategy_nav_spec_test.cpp`) | evidence |
|---|---|---|---|
| R-1 duplicate JSON key | `book/nav_spec.cpp:317-340` `parse_spec` (trial_epoch callback), `:343-354` `load`, `:462-474` `dispatch`; `nav_spec.hpp:42-43`, `:87-88` | `NavSpec.RefusesADuplicateKey` (:339, raw text, 5 cases + 2 controls) | `fix1-gtest-l-named.log`, `fix1-gtest-j-named.log`; teeth M1 -> fails at :358 (`fix1-teeth-k-named.log`) |
| R-2 narrower range + grid | `book/nav_spec.cpp:281-283`; `nav_spec.hpp:26-34` | `NavSpec.RefusesWhatItCannotType` (:291; grid lines :312-313, :318-323) | same logs; teeth M2 -> fails at :315, :323 |
| C3-3 `x-flag-aliases` | `book/nav_rules.hpp:26-29`, `:55`; `nav_rules.cpp:74`, `:93`, `:114-119` | `NavRules.LegacyAliasesOnTheirRowsOnly` (:475) | same logs; teeth M3 -> fails at :508 |
| PRE-7 / PRE-7a opt-in | `strategy_nav_replay.hpp:518-521`, `:689-700`, `:746-747`; `book/nav_summarize.cpp:159`; `nav_cli.cpp:39-49`, `:116-118`; `nav_spec.cpp:91-92`; `nav_replay_internal.hpp:644-647`; `nav_run.cpp:414`, `:521`; `nav_publish.cpp:78-79` | `NavAnnMeanGross.PrintOnlyOptInAddsOneSummaryKey` (:673); `NavSpec.FlagsAndSpecSameConfig` line 3 | same logs; teeth M4 (key always on) -> fails at :706, :715; M5 (value from `net`) -> :724, :728, :729; exe-level tree check `fix1-nav-optin-l.log`; canary `mega-p9-c3-{l,j}-canary.log` |
| S-1 append-only | `book/nav_rules.cpp:57`, `:76`, `:98` | `NavRules.OneTableEveryKindInOrder`, `CostLawRegistry.*`, `TradeRateRegistry.*`, `TargetRegistry.UnregisteredIdIsRefusedBeforeOutput` (:639) | named logs |
| S-4 spec SHA of the bytes parsed | `book/nav_spec.cpp:343-354`, `:474` | (no own test: `SpecRunPublishesTheFlagRunBytes` runs the path) | named logs |
| S-5 recipe ids = registry ids | (test only) | `CostLawRegistry.CostSqrtV2IsTheCostV2RunByteForByte` (:556, :575-583) | named logs; teeth M6 (recipe label `sqrt-impact-v0`) -> fails at :579, :583 |
| S-6 (part) | `atx-impl/CMakeLists.txt:297-300` | none (comment) | n/a |
| S-7 | `book/nav_spec.hpp:26-34` | none (comment) | n/a |

### Per finding

**R-1 (duplicate JSON key): fixed.** `atx-impl/src/book/nav_spec.cpp:317-340` `parse_spec` parses with the
duplicate-key callback of `atx-engine/src/eval/trial_epoch.cpp:76-87` (one key set per open object: push on
`object_start`, refuse a repeated `key`, pop on `object_end`). `ns::read` and `nav --spec` refuse with
`nav spec: duplicate key "K" in PATH ...`, exit 2, before the `nav spec:` line and before any output directory
(`:347-354` `load`, `:462-474` in `dispatch`). Header: `nav_spec.hpp:42-43` and `read`'s contract.
Test `NavSpec.RefusesADuplicateKey` (raw text): `options` twice at the top level, `options.aim_leverage` twice
(1.2 / 1.9), `constants.leverage_range` twice, `schema` twice, `options.cadence` twice with equal values. Each:
`ns::read` fails naming the key; `nav --spec F --output D` exits 2, prints nothing on stdout, creates no `D`. Control:
the same file without the repetition reads; `schema` inside `options` is no repetition of the top-level key (refused
as an unknown option, not as a duplicate).

**R-2 (narrower range with a grid): refused** (honouring would mean reading the grid file and checking every
variant's `--aim-leverage`; not trivial). `nav_spec.cpp:281-283`, beside the `calibrate_gross` refusal; header
`nav_spec.hpp:26-34`. Test `NavSpec.RefusesWhatItCannotType`: `{aim_leverage 1.2, construction_grid}` with
`[1, 1.5]` refused; `{construction_grid}` with `[1, 1.5]` refused naming `construction_grid`;
`{aim_leverage 1.2, construction_grid}` with `[1, 2]` accepted.

**C3-3 (`x-flag-aliases`): done.** `atx-impl/src/book/nav_rules.hpp:55` `Row::x_flag_aliases` (default empty),
contract `:26-29`; `nav_rules.cpp:74` `cost_sqrt_v2_aliases {"--cost-v2"}` on the `cost-sqrt-v2` row (`:93`);
`row_json` (`:114-119`) prints `"x-flag-aliases": ["--cost-v2"]` only on a row with an alias, so every other row keeps
its part-A shape. No other new row has a legacy spelling: `aim-partial-v5` and `per-name-v1` have one route each
(`--rule` / `--rate`); `two-speed-nav-v1`, `flat-bps-v1`, `sqrt-impact-v1`, `ko-invariance-v1`, `fim-live-v1` have
no x-flag (nothing to alias). C1's rows: `rules_json()` still prints `risk_target::leverage_rules_json()` first, and
`strategy_risk_target.{hpp,cpp}` have an empty diff since base `e1c16051`.
Test `NavRules.LegacyAliasesOnTheirRowsOnly`: the printed C1 rows equal `leverage_rules_json()` row by row (compact
dump bytes) and carry no alias key; every C3 row has exactly the part-A key set (`description, id, incompatible,
kind, params_schema, x-flag`) plus the alias key where it has one; every alias is a NavSpec run flag and differs
from its row's x-flag; `cost-sqrt-v2` lists `["--cost-v2"]` (a Switch flag); C3's seven other rows list none.

**PRE-7 / PRE-7a (`ann_mean_gross`): done, byte-neutral when absent.** Opt-in: the valueless flag
`--ann-mean-gross`, NavSpec key `ann_mean_gross` (a Switch; `nav_spec.cpp:91-92`). Not a rule flag: no weight,
trade, cost or NAV value reads it and the recipe never records it.
- `atx-impl/src/strategy_nav_replay.hpp:518-521` `NavSummary::ann_mean_gross`; `:689-700`
  `NavEmitOptions::ann_mean_gross` (contract); `:746-747` CLI contract.
- `book/nav_summarize.cpp:159` computes it next to `ann_mean`; `book/nav_cli.cpp:39-49` parses the switch like
  `--stage-timers` (repeated = usage error), help `:113-117`.
- `book/nav_replay_internal.hpp:644-647` `NavRun::ann_mean_gross`; `book/nav_run.cpp:414` (single and calibrated
  run), `:521` (every grid variant). The capacity directory's run keeps it false (`publish_capacity` builds its NavRun
  without it), so `capacity/summary.json` and the main summary's binding of it do not move.
- `book/nav_publish.cpp:78-79` is the only output change: `entry["ann_mean_gross"]`, under the flag alone.
- The v7 seam forwards the token to the replay parser (its token scan passes every non-v7 flag through).

Definition, from the code that computes `ann_mean` (`book/nav_summarize.cpp:140-159`): over the book's return rows
(each day of the result with `return_observation`, the result's first row excluded: the rows of `ann_mean`,
`net_sharpe` and `observations`), `ann_mean = 252 x mean(net_return)` and `ann_mean_gross = 252 x mean(gross_return)`,
both by the same running mean (`Moments::add`; `gross` is the accumulator `gross_sharpe` already used). Per row
(`book/nav_mark.cpp:141-149`): `gross_return_t = (mark P&L_t + write-off P&L_t) / NAVpre_{t-1}` and
`net_return_t = NAVpre_t / NAVpre_{t-1} - 1 = gross_return_t - trade_cost_return_t - borrow_return_t -
long_financing_return_t` (checked every session within `identity_tolerance`). So `ann_mean_gross - ann_mean` is 252 x
the mean of the trade-cost (fills of t-1), short-leg and long-leg financing returns, up to that tolerance and
rounding. It is gross of costs and financing, not of write-offs (a written-off loss stays in it). One value per
scenario (book), beside `ann_mean`, in each scenario object of the main `summary.json`.

Test `NavAnnMeanGross.PrintOnlyOptInAddsOneSummaryKey` (lane fixture `Role(40, 12, 53)`; two shapes: the v7 seam's
`v6_shape` with `--cost-v2 --capacity-curve`, and plain aim-partial-v5):
- absent vs present: every file but `summary.json` byte-identical (recipe, daily and events CSVs incl. the KO / FIM
  books, `capacity/`, `v7_extras.json`); the absent `summary.json` holds no `ann_mean_gross` byte; the present one,
  the key erased from every scenario, re-dumps to the absent file byte for byte;
- the value, per scenario: 252 x the running mean recomputed in the test from that scenario's published daily CSV
  (`gross_return` over `return_observation` 1 rows, first row excluded; 17-digit CSV) equals the printed value bit for
  bit; the same recomputation of `net_return` gives `ann_mean` bit for bit (same rows); the plain mean agrees within
  1e-12; the value exceeds `ann_mean`;
- `--print-spec` carries `"ann_mean_gross": true` only when the flag is named, and `nav --spec` of it publishes the
  opt-in run's bytes; a one-variant grid with the flag publishes its standalone opt-in run; the flag twice exits 2.
`NavSpec.FlagsAndSpecSameConfig` line 3 (spo-v3 under the v7 seam) now carries the flag through both parsers.

Byte neutrality with the flag absent: `summarize_nav` stores one more number in a struct field nothing else reads
(every `NavSummary` field is serialised by name: `book/nav_write.cpp:377`, `strategy_nav_v7.cpp:368`; the only
`NavSummary` construction outside `summarize_nav` is a value-initialisation, `strategy_cost_v2_test.cpp:543`, so the
mid-struct field shifts no positional initialiser); the publish line is guarded. Evidence:
- the unchanged tiny-world canary goldens pass on Debug (f, g, l) and Release (j): u / fit / w and the NAV primary
  daily CSV bytes as recorded before C3 (the canary pins no `summary.json`);
- the 387 pre-existing target tests pass unchanged (whole 390 / 390 on l and j), incl. the pinned `recipe.json`
  digests of `strategy_nav_replay_test.cpp:1646`, `:2000`, `:2151`, `:2862`;
- the gtest above: absent = present minus the key, on both shapes, byte for byte;
- exe level, tiny world (`c3-logs/fix1-nav-identity.py`, `fix1-nav-optin-l.log`; Debug l exe `9e910c34`): the
  cycle's own NAV argv (`--rule aim-partial-v5 ... --locate-in-aim --liquidity-cache`) re-run flag-absent twice is
  byte-identical (12 files, console identical), and with `--ann-mean-gross` only `summary.json` differs, by exactly 5
  `"ann_mean_gross"` lines for 5 scenarios: deleting those lines gives the flag-absent file byte for byte. The same
  check on the g exe gave the same result (console only, scratch dir `%TEMP%\c3-navid-dry`).
- NOT DONE: the explicit before / after exe comparison (the pre-round exe of `99b676f7` vs the post-round exe, flag
  absent). Its "before" build needed the worktree at `99b676f7`; the permission system refused that command (see
  "Permission refusals" below) and it was not retried. The driver does the check as written once root has both bins:
  `python fix1-nav-identity.py old --bin <pre-merge root bin> --work W` then `new --bin <merged bin> --work W`
  (pass = every file equal, `summary.json` after substituting the `producer.engine_git_sha` value alone; console
  equal; the old exe refuses `--ann-mean-gross`). Root at merge item 5 (flag-absent identity against the pre-C3
  reference on X-5) is the same check on real data.
No golden, pin or expected hash was edited (`git diff e1c16051 HEAD -- scripts/tests/fixtures/tiny_world_goldens.json`
is empty).

### Review suggestions

| id | status |
|---|---|
| S-1 append-only slots | done: `nav_rules.cpp:57`, `:76` size-deduced `constexpr Row trade_rates[]` / `cost_laws[]` (AL-CLOCK appends one line). The empty target slot stays `std::span<const Row> targets{}` (`:98`): a zero-length array is ill-formed, so COV-MV replaces that one line with its table. Tests check C3's rows by lookup / index and as a prefix of their kind (`NavRules.OneTableEveryKindInOrder`; `CostLawRegistry.RowsAreTheReplaysLawsAndOneNameableSet` over the first five rows; `TradeRateRegistry` with `ASSERT_GE` and `flag_ids` prefixes). `TargetRegistry.SlotIsEmptyAndRefusesEveryId` became `TargetRegistry.UnregisteredIdIsRefusedBeforeOutput`: synthetic id `zz-no-such-target-v0`, expected message built from `flag_ids(Target)` ("none registered" while empty), so COV-MV's row needs no edit of C3's tests |
| S-2 `--cost-v2` alias | C3's half done by C3-3 (the row lists it); the guard is D3's carry (ruling C3-3) |
| S-3 decide-path log | done by the review's second option (record the deviation; ledger candidate C3-6 below), not by a log line: `load_nav_deploy` (`book/nav_deploy.cpp:40`) and its caller `verified_load` (`strategy_live.cpp:1237`) have no stream; a log line needs one threaded through the decide verb, another lane's file. Deviation for the ledger: on the decide path a seal-less fields manifest is accepted without the log line (refusals are the same); the decide path publishes no NAV directory |
| S-4 spec SHA TOCTOU | done: `nav_spec.cpp:341-354` `load` hashes the very bytes it parsed; dispatch prints that digest (`:474`); the file is read once |
| S-5 recipe ids = registry ids | done: `CostLawRegistry.CostSqrtV2IsTheCostV2RunByteForByte` asserts every `recipe.json` `scenarios[].cost_rule` of the v2 run is a cost-law row id, and the set is `{flat-bps-v1, sqrt-impact-v1, ko-invariance-v1, fim-live-v1}` |
| S-6 stale comments | partly: `atx-impl/CMakeLists.txt:297-300` (B2's tail comment, in the file C3 restructured) fixed. Not done: `atx-engine/CMakeLists.txt:465`, `:500`, `atx-impl/tests/provenance_test.cpp:12`, `strategy_ic_shrink.cpp:3`, `strategy_ic_theme_erc.cpp:3`, `trial_ledger.cpp:3` (comments only, other lanes' files: "on the next touch" per the review; touching them now only widens the merge surface) |
| S-7 range vs realised leverage | done: `nav_spec.hpp:26-34` (the range bounds the parameter L; vol-target-v1 runs L_t in [1, L], risk-target-v1 in [.8 L, 1.25 L]; ruling C3-4) |
| S-8 commit / report wording | note only, no history rewrite: the evidence that the research exes build without a pipeline TU is build c (`mega-p9-c3-c-build.log`), not a / b |

### Builds and tests (fix round 1)

| tag | preset | source | result |
|---|---|---|---|
| p9-c3-f | equity-dev (Debug), fresh tree | `99b676f7` + the fix (12 dirty entries) | 359 TUs, 443 s; canary 15 passed. Named run 32 / 33: the one red was my own new S-5 assertion (a range-for over a member of a temporary `Json`, dangling before C++23); fixed in the test, no code change |
| p9-c3-g | equity-dev (Debug) | the content of `9a7c490f` (dirty tree, before the commit) | 8 TUs, 17 s; canary 15 passed; named 33 / 33 (`c3-logs/fix1-gtest-g-named.log`); whole `atx-impl-strategy-target-tests` 390 / 390 (`c3-logs/fix1-gtest-g-whole.log`) |
| p9-c3-h | equity-rel (Release), fresh tree, configured at `9a7c490f` | `9a7c490f` | refused by the stale-PCH trap (`atx-impl-strategy.dir/cmake_pch.cxx.pch` older than the fresh `deps/equity-rel` Eigen); nothing tested. Remedy: the six `cmake_pch.cxx.{pch,obj}` (atx-engine, atx-impl-strategy, atx-impl-pipeline) deleted by name, `CCACHE_RECACHE=1` |
| p9-c3-i | equity-rel | `9a7c490f` | refused at memory admission (commit 2483 MiB < 2500): nothing built, no receipt; waited, then j |
| p9-c3-j | equity-rel (Release) | `9a7c490f` clean (0 dirty), `CCACHE_RECACHE=1` | 274 TUs, 1082 s, 18 links; canary 15 passed on the Release goldens (`c3-logs/mega-p9-c3-j-canary.log`). Run on resume (exe SHA-256 checked against the receipt: target-tests `f6c0a240`, targets `3fbe85a2`): named 33 / 33 (`fix1-gtest-j-named.log`), whole target-tests 390 / 390 (`fix1-gtest-j-whole.log`) |
| p9-c3-k | equity-dev (Debug) | `9a7c490f` + six working-tree mutations (teeth, below; 7 dirty entries) | 12 TUs, 18 s, target-tests only; named: 28 pass, 5 FAIL as intended (`fix1-teeth-k-named.log`). Mutations reverted by hand, `git diff` of the code empty after |
| p9-c3-l | equity-dev (Debug) | `9a7c490f`, code clean (2 dirty entries: this report and the handoff) | 12 TUs, 12 s, 7 links; canary 15 passed on the Debug goldens (`mega-p9-c3-l-canary.log`); named 33 / 33 (`fix1-gtest-l-named.log`); whole target-tests 390 / 390 (`fix1-gtest-l-whole.log`); targets exe `9e910c34` used for `fix1-nav-optin-l.log`. ConfiguredProvenance `99b676f7-dirty` (the Debug tree was configured at f; provenance is configure-time only and enters `summary.json` `producer` alone) |

The Debug and Release builds of record of this round are l and j (code `9a7c490f`); g's logs stay as the first
Debug pass. No ENV-1 exit (3221225794) in any run.

### Teeth (build k)

Each mutation removes or breaks one check of this round; one Debug build carried all six (they hit disjoint tests),
the named filter ran once, and each failing assertion is the mutated check's own:

| mutation | file (at `9a7c490f`) | test that fails, at (test file line) |
|---|---|---|
| M1 duplicate-key refusal off (`if (false && ...)`) | `book/nav_spec.cpp:336` | `NavSpec.RefusesADuplicateKey` :358 (`ns::read` accepts the repeated `options`) |
| M2 grid refusal off | `book/nav_spec.cpp:281` | `NavSpec.RefusesWhatItCannotType` :315 (the grid spec), :323 (`grid_narrow`) |
| M3 `cost-sqrt-v2` row without its alias | `book/nav_rules.cpp:93` | `NavRules.LegacyAliasesOnTheirRowsOnly` :508 (alias count 0) |
| M4 publish guard always on (`if (true || ...)`) | `book/nav_publish.cpp:79` | `NavAnnMeanGross.PrintOnlyOptInAddsOneSummaryKey` :706 (the flag-absent summary holds the key), :715 (minus the key != absent) |
| M5 value from the net mean | `book/nav_summarize.cpp:159` | same test :724 (bits vs the CSV gross mean), :728, :729 (not above `ann_mean`), every scenario of both shapes |
| M6 recipe label `sqrt-impact-v0` | `book/nav_write.cpp:169` | `CostLawRegistry.CostSqrtV2IsTheCostV2RunByteForByte` :579 (not a cost-law id), :583 (the set) |

The other 28 named tests passed under the mutations. Reverted with the editor (no git command); after the revert
`git status` shows only the two sprint-dir docs, and build l rebuilt the five TUs from the committed source.

### Permission refusals (not retried, not routed around)

1. Bash: `rm -rf C:/Users/natha/AppData/Local/Temp/c3-navid-dry && cd C:/atx-wt/pool-15 && rtk proxy git switch
   --detach 99b676f7 && rtk proxy git status --short && rtk proxy git log --oneline -1` (refused as "Irreversible
   Local Destruction"). Purpose: delete my driver's dry-run dir, then put the worktree on the pre-round commit to
   build the "before" exe (tag k was meant for it) and return to the branch after. Consequence: no before / after
   exe comparison (above); the dry-run dir (12 MB, synthetic tiny world) is left in place.
2. Edit of `c3-logs/fix1-nav-identity.py` adding an `optin` phase (the opt-in check on an existing world with another
   bin, e.g. the Release exe). Consequence: the exe-level opt-in tree check ran on Debug only; Release has the gtest
   `NavAnnMeanGross` (33 / 33 on j). The half-applied second edit of that script was undone, so the committed script
   has the two phases `old` / `new` only.

### New literals, flags and keys (for root)

- Flag `--ann-mean-gross` (valueless, nav verb; print-only); NavSpec key `options.ann_mean_gross` (`true` or absent;
  `false` = absent).
- `summary.json`: optional key `ann_mean_gross` in each scenario object of the main directory (inside
  `atx.dsl-nav-replay-summary/v1`), only under the flag.
- `--list-rules --json` (`atx.nav-rules/v1`): optional row key `x-flag-aliases` (array of flags), on `cost-sqrt-v2`
  only: `["--cost-v2"]`.
- No new schema literal in this round; part A's `atx.nav-spec/v1` is still the one for `classes.json` (SQL2-CLS).

Named filter `NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavFieldsSeal.*:NavBookWorkers.*:NavBookRule.*:NavAnnMeanGross.*:CalibrateGross.*:CovStore.*`
= 21 C3 tests (part A's 18 + `NavSpec.RefusesADuplicateKey`, `NavRules.LegacyAliasesOnTheirRowsOnly`,
`NavAnnMeanGross.PrintOnlyOptInAddsOneSummaryKey`; the renamed `TargetRegistry` test counts once) + `CalibrateGross.*`
7 + `CovStore.*` 5 = 33. Whole target-tests: 387 + 3 = 390. `atx-impl-tests` not built (as in part A: root item 1).

### Root at merge (replaces the list above)

1. Scoped re-review of this round, then merge. Builds through `research-build.ps1`, both presets:
   `-Targets "atx-impl-strategy-target-tests,atx-impl-tests,atx-equity-strategy-targets,atx-equity-strategy-ic,atx-equity-strategy,atx-equity-strategy-risk,atx-equity-strategy-mine,atx-research-eval,atx-impl" -Canary`.
   Expected: the named filter above 33 / 33; whole target-tests 390; `atx-impl-tests` whole once (its base count + the
   C3 tests it globs: part A's 10 + this round's 3); canary 15 on the unchanged Debug and Release goldens. On "modified
   since the precompiled header": delete that tree's `cmake_pch.cxx.{pch,obj}` by name, rebuild with
   `CCACHE_RECACHE=1`.
2. SQL2-CLS, literals for `atx-engine/schemas/research_store/classes.json`: `atx.nav-spec/v1` (part A). This round adds
   no schema literal: `x-flag-aliases` is an optional row key inside `atx.nav-rules/v1`, `ann_mean_gross` an optional
   scenario key inside `atx.dsl-nav-replay-summary/v1`, each present only when used.
3. `atx-impl-core` is INTERFACE: a root list or script naming it as a build target names `atx-impl-strategy` /
   `atx-impl-pipeline`; a root-side append since `ab189471` that writes `atx-impl-core` is retargeted at the merge.
4. X-5 and Y-F0 `--spec` vs argv (same exe; argv verbatim from the cells' receipts; no statistic read), per cell:
   `EXE=<bin>/atx-equity-strategy-targets.exe`;
   `$EXE nav <argv> --print-spec > <scratch>/x5.navspec.json` (Git Bash `>` or `cmd /c ... >`; never PowerShell 5.1
   `>`, whose UTF-16LE file is "not JSON", exit 2);
   `$EXE nav <argv with --output NEW/x5-argv>`; `$EXE nav --spec <scratch>/x5.navspec.json --output NEW/x5-spec`;
   compare the two trees recursively: every file byte-identical. The spec run's stdout gains
   `nav spec: atx.nav-spec/v1 <path> sha256 <SHA-256 of the bytes it read>`. Same for Y-F0.
5. Flag-absent identity against the pre-C3 reference (argv without `--ann-mean-gross`): the NAV dir equals it except
   `summary.json` `producer.engine_git_sha`. A `--fields` manifest without a `seal` block adds the P13 legacy line to
   stdout (the wrapper's `stdout.log` digest moves): check the X-5 / Y-F0 manifests first. The lane's own
   before / after exe check was refused (permission refusal 1); on the tiny world root can run it with both bins at
   hand, no real data: `python .superpowers/sdd/platform-p9-20261003/c3-logs/fix1-nav-identity.py old --bin
   <pre-merge bin> --work <new dir outside the repo>`, then `... new --bin <merged bin> --work <same dir>`; pass =
   `RESULT PASS` (flag-absent trees equal but `producer.engine_git_sha`, console equal, the old exe refuses
   `--ann-mean-gross`, the opt-in adds one `ann_mean_gross` line per scenario and nothing else).
6. Release = Debug NAV on X-5 (G-P3 NAV half): the Release exe's argv run equals the Debug exe's except
   `summary.json` `producer` (`engine_git_sha`, `build_type`).
7. PRE-7a at the wave-3 P9-B0: every P9 cell and its reference name `--ann-mean-gross` alike (NavSpec
   `"ann_mean_gross": true`). With it a NAV dir differs from its flag-absent run only by the `ann_mean_gross` key in each
   scenario of `summary.json` (and, for a grid, the variants' `summary_file_sha256` in `grid_manifest.json`). The
   gross-alpha lines (P9-H, P9-MV, G-B3) read that key (a C++ producer, G-P5), not a Python sum.
8. Wall time: the five research exes' build time on a fresh tag against the pre-C3 receipt (F-10); X-5 NAV wall time
   before / after the 14-TU split, Debug and Release (cross-TU calls are no longer inlined, C-3).
9. W1-I2: the NAV exe changed; re-pin the lineage parents to the wave-3 P9-B0 references before the first P9 cell.
10. W3-8 kind-fixture flip (root post-merge; D3 owns the kind files): drop `new-cost-model.json` from
    `SCHEMA_ONLY_FIXTURES` and its "SCHEMA ONLY" description; run the tiny-world kinds (all 10) against the real exe's
    `nav --list-rules --json` (E2's tests use a frozen synthetic NAV table). Under E2-8 the real table owns `--rule`,
    `--trade-fraction`, `--dust-multiple`, `--exit-rate`, `--rate`, `--rate-rra`, `--rate-lambda`, `--rate-min`,
    `--rate-max`, `--cost-law`; under C3-3 D3's guard also reads `x-flag-aliases` (`--cost-v2` stands for
    `--cost-law cost-sqrt-v2`). Check no P9 template's `change.flags`, `rule_cell.constants.flags` or `horizon.flags`
    names one. `--ann-mean-gross` is no rule flag and a prefix of none.
11. Part B (C2-11, C2-9, C2-10, W3-6) in the C3b round after E2 is in root (C3-2).

### Ledger candidates (fix round 1)

- C3-5: `--ann-mean-gross` (NavSpec `ann_mean_gross`) is the PRE-7a producer: print-only, one key per scenario of the
  main `summary.json`, 252 x the running mean of `gross_return` over `ann_mean`'s rows.
- C3-6 (S-3 deviation): the decide path accepts a seal-less fields manifest without the P13 log line.

### Open questions

- The explicit before / after exe check (pre-round vs post-round NAV, flag absent) was not run: its build needed a
  worktree switch the permission system refused. Root runs it at merge (item 5), or the owner allows the switch and a
  resumed lane runs it (one Debug build of `atx-equity-strategy-targets` at `99b676f7`, one back at the head).
- Scratch left by the refusal: `C:/Users/natha/AppData/Local/Temp/c3-navid-dry` (12 MB, synthetic tiny world and the
  driver's dry-run outputs); delete by name.
