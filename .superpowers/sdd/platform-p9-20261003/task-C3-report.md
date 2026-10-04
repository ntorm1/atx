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
