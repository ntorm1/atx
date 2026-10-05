# Task review: lane C3, part A (P9 wave 3)

Reviewer: independent task review (fresh Opus 5.5), read-only on code. Pool-15, branch `feat/p9-c3-20261004`,
base `e1c16051`, head `4b3f0cd4`. Scope: `4e958185` (NavSpec, K-P9-7 registries, P13 NAV seal, M-1 test, core
split), `7e4c9dc4` (move-only split), `6abd91b2` (INTERFACE guard), and the merge `9dace914` of root `ab189471`
(conflict resolution only). Part B (C2-11, C2-9, C2-10, W3-6) is not in scope (ruling C3-2).

Contract read: `resume4-dispatch.md`, `wave2-review-dispatch.md`, `briefs/review-template.md`, `briefs/brief-C3.md`,
`wave3-carry.md` (brief text superseded, All lanes, C3, PM answers, added after drafting), plan §2.2-§2.4 C3 block,
ledger rulings C3-1, C3-2, COV-3, W3-8, W3-9, E2-11, W1-MIN, P13, P14, OD5-7, C2-11. Lane report / handoff / `c3-logs/`
read as the lane's account and re-derived below. No build was run (no tag used): reading was enough. No pytest: the
lane changed no Python. No data opened; no NAV statistic read.

## Verdict: CHANGES_REQUIRED

Spec: part A meets the brief and the carry except two NavSpec holes (R-1, R-2), both in `book/nav_spec.cpp`, each a
few lines plus a test line. Quality: the move-only split is mechanically proven, the registries match the code, the
P13 half mirrors S2's, the M-1 test has teeth, the CMake split is clean. A short fix round (R-1, R-2) then a scoped
re-review; nothing else blocks the 3b lanes.

## Required

| id | where | problem | fix |
|---|---|---|---|
| R-1 | `atx-impl/src/book/nav_spec.cpp:357` (`read`) | A spec file with a duplicate JSON key is accepted, last value wins. `Json::parse(text, nullptr, false)` uses nlohmann's `json_sax_dom_parser::key`, which does `object->operator[](val)` and overwrites (vendored source `deps/equity-dev/json-src/include/nlohmann/detail/input/json_sax.hpp:258-265`). `{"options": {"aim_leverage": 1.2, "aim_leverage": 1.9}}` runs at 1.9 while a reader of the pinned file sees 1.2 first; the printed `sha256` pins a document the exe reads differently. The flag adapter refuses a repeated flag (`from_args`, nav_spec.cpp `repeated flag`), the spec reader does not, and `NavSpec.RefusesWhatItCannotType` (`strategy_nav_spec_test.cpp:235-266`) has no duplicate case (it builds specs as `Json` objects, which cannot hold one). Review check B names "duplicate keys refused"; C3-1's own rationale (no accepted-and-ignored key in a registered cell) applies. | Parse with the house duplicate-key callback already used by `atx-engine/src/eval/trial_epoch.cpp:76-87` (a stack of key sets: push on `object_start`, refuse on a repeated `key`, pop on `object_end`; catch the throw, or return a flag, and refuse with exit 2 naming the key). Add a test that writes the spec as raw text with a duplicate at each level (top level `options` twice, an `options` key, a `constants` key) and expects `ns::read` to fail and `nav --spec` to exit 2 with no output dir. |
| R-2 | `atx-impl/src/book/nav_spec.cpp:260-277` (`check_leverage_range`) | A narrower `constants.leverage_range` is accepted together with `options.construction_grid`, but the grid file's variants set `--aim-leverage` per variant (`atx.nav-construction-grid/v1` overrides `--aim-leverage`, `nav_cli.cpp` grid loop / `apply_construction_flag`), and nothing checks them against the range. `{"aim_leverage": 1.2, "construction_grid": "g.json"}` with `[1, 1.5]` passes while a variant at 1.8 runs: the declared range is accepted and not honoured, the case C3-1 forbids. The lane already refuses the parallel case for `calibrate_gross` (`:274`). | Refuse a narrower range with `construction_grid` exactly as with `calibrate_gross` (one condition and message), or read the grid and check every variant's `--aim-leverage`; add the refused case to `RefusesWhatItCannotType`. (Vol-target / risk-target do not need this: the range bounds the aim-leverage parameter, which the check does test; see S-7.) |

## Suggested

| id | where | problem | fix |
|---|---|---|---|
| S-1 | `atx-impl/src/book/nav_rules.cpp:56`, `:92`; `strategy_nav_spec_test.cpp:335-338`, `:415`, `:450-466` | The 3b slots are not append-only (W3-9): AL-CLOCK must edit the `std::array<Row, 3>` size, COV-MV must replace `constexpr std::span<const Row> targets{}` with a table, and both must edit C3's tests (`rows.size() == 3`, the full id list in `NavRules.OneTableEveryKindInOrder`, `target_rows().empty()` and the `mv-aim-v1 ... (none registered)` message). Both 3b lanes edit the same id-list line, so the second merge is a guaranteed same-line conflict. | Size-deduced tables (`constexpr Row trade_rates[] = {...}`; a target table COV-MV appends into), and tests that assert C3's rows by lookup / as a prefix and refuse a synthetic id (`zz-no-such-target-v0`) instead of `mv-aim-v1`. |
| S-2 | `cost-sqrt-v2` row (`nav_rules.cpp`, x-flag `--cost-law cost-sqrt-v2`); E2's `wave_kinds.table_flags` (pool-17 `scripts/wave_kinds.py:447-472`) | Under E2-8 the owned NAV flags are each row's x-flag first token and its params' x-flags, so `--cost-law` is owned but its exact alias `--cost-v2` is not: a v2 template's `change.flags` can still set `--cost-v2` and change the cost law outside the rules block. Not a C3 code defect (E2's shape forces a `--flag VALUE` x-flag); a cross-lane gap. | PM / D3 (kind-file owner, W3-8): rule whether `--cost-v2` is refused in v2 option maps, or name it in the guard. |
| S-3 | `atx-impl/src/book/nav_load.cpp:79-95`, `:144`; `nav_run.cpp:316`, `:468` | P13 says absent = accepted **and logged**. The replay and grid paths log; the decide path (shared `admit_and_load`, `nav_deploy.cpp`) accepts a seal-less manifest silently. | Log through the decide path's own record / stream, or record the deviation in the report and ledger. |
| S-4 | `atx-impl/src/book/nav_spec.cpp:425` | The printed spec SHA re-reads the file after parsing (TOCTOU: the hash can describe other bytes than the run used). | Hash the bytes `read` parsed (return them with the document). |
| S-5 | `atx-impl/tests/strategy_nav_spec_test.cpp:356-374` | `flat-bps-v1` / `sqrt-impact-v1` are pinned as literals; the recipe writer emits them from its own literals (`nav_write.cpp:169`). Nothing ties the registry ids to what a recipe records. | One assertion: a tiny NAV run's recipe `cost_rule` values are cost-law row ids. |
| S-6 | `atx-engine/CMakeLists.txt:465`, `:500`; `atx-impl/tests/provenance_test.cpp:12`; `atx-impl/src/strategy_ic_shrink.cpp:3`, `strategy_ic_theme_erc.cpp:3`, `trial_ledger.cpp:3`; `atx-impl/CMakeLists.txt:297-299` | Comments still say "atx-impl-core links it" / "atx-impl-core's source list"; B2's tail comment keeps "so atx-impl-core links the library" above a pipeline link. | Update on the next touch (comments only). |
| S-7 | `atx-impl/src/book/nav_spec.hpp:26-29` | `leverage_range` bounds the aim-leverage parameter, not the realised L_t: vol-target-v1 runs L_t in [1, L], risk-target-v1 in [.8 L, 1.25 L] (`atx-engine/include/atx/engine/book/risk_target.hpp:6`, `:40-41`), i.e. up to 2.5 at L = 2. A reader of `"leverage_range": [1, 2]` may take it for the run's cap. | Say so in the header comment. (The 2.5 itself is pre-existing C1 / R-8 behaviour: open question below.) |
| S-8 | commits / report | `4e958185` is a `wip(c3)` commit holding brief tasks 2 and 3 together (rule 8: commit per task). The report's "p9-c3-a / -b compiled none [pipeline TU]" is loose: builds a and b link `atx-impl-pipeline.lib` for the target-tests (`build-equity/mega-p9-c3-a-build.log`); the real evidence is build c, where the five research exes link before `atx-impl-pipeline.lib` is built (`mega-p9-c3-c-build.log`). | Note only; no history rewrite. |

## Checks A-I (evidence)

**A. Move-only split (`7e4c9dc4`): PASS.** Own scripts (scratchpad `moveck.py`, `orderck.py`) over
`4e958185:atx-impl/src/strategy_nav_replay.cpp` (4231 lines) against the 14 `src/book/nav_*.cpp` TUs plus
`book/nav_replay_internal.hpp` at `7e4c9dc4` (4610 lines):
- Line multiset (rstrip only): the only old lines missing from the new files are 12 blank lines and 3 default-argument
  lines, which moved to the declarations: `ConstructionTape* tape = nullptr` (definition `nav_session.cpp:268`,
  declaration `nav_replay_internal.hpp:498`), `const NavLabelRolePin& label = {}` (`nav_load.cpp:190` / `:559`),
  `ConstructionTape* tape = nullptr, bool observe = true` (`nav_lockstep.cpp:174` / `:578`). Same values, same calls.
- Every added line is one of: the per-TU header comment pair, `#include "book/nav_replay_internal.hpp"`,
  `namespace atx::impl::strategy {`, `using namespace nav_book;`, `namespace nav_book {` / `namespace {` wrappers and
  closers, the header's `#pragma once` + 5-line comment + namespace, and about 70 declarations of functions one TU
  defines and another calls, each with its doc comment copied (which is why those comment lines count twice).
- Order: difflib alignment of each TU against the old file gives contiguous old blocks in file order (validate
  300-487, liquidity 489-604, mark 606-746, execute 747-810, decide 811-1132, session 1133-1544, write 1632-2135, load
  2136-2363, lockstep 2366-2708, run 2816-3838, publish 2892-3287, cli 3840-4068, deploy 4070-4231; the header
  collects types / constants / templates from 3-3392). No line reordered inside a function.
- Linkage: anonymous-namespace items became `nav_book` items (`nav_replay_internal.hpp:49-808`); every namespace-scope
  variable there is `constexpr` (internal linkage, one copy per TU, as before); the `using namespace nav_book;` sits at
  the same scope as the old unnamed namespace's implicit directive, so lookup is unchanged; duplicate definitions
  would not link (they did, builds b, c, e). SSE2 presets, no FMA: cross-TU calls change speed, not values.
- Non-move lines of the commit: `atx-impl/CMakeLists.txt` (`_atx_nav_book_sources` list at `:18-33`, used in the
  library and in both Debug /O2 + SKIP_PCH lists, so the TUs keep the old TU's flags) and one comment line in
  `nav_spec.cpp` (`strategy_nav_replay.cpp` -> `book/nav_run.cpp`). Both fine.
- The merge changed none of it: `git diff 7e4c9dc4 4b3f0cd4 -- atx-impl/src/book atx-impl/src/strategy_nav_* atx-impl/tests/...`
  is empty; `git diff ab189471 4b3f0cd4` outside `atx-impl/` and the sprint dir is empty.
- COV-3: `git diff e1c16051 4b3f0cd4 -- strategy_spo.{hpp,cpp} strategy_risk_model.{hpp,cpp} strategy_risk_verb.cpp
  strategy_nav_calibrate.* strategy_target_replay.hpp strategy_risk_target.cpp strategy_cost_v2.cpp` is empty:
  COV's RiskStore read path moved between CMake targets only (into `atx-impl-strategy`, still in the Debug /O2 list);
  `CovStore.*` 5/5 in the whole runs a, c and Release e.

**B. NavSpec: PASS except R-1, R-2.** Design D-1: one parser; a spec renders a canonical command line (`to_args`,
table order, shortest round-trip numbers via `to_chars`, re-read by the same `stod` / `from_chars` the parsers use),
and `from_args` is the flag adapter. I checked the replay parser (`nav_cli.cpp` loop, `nav_run.cpp:82-109`
`apply_construction_flag`) and the v7 parser (`strategy_nav_v7.cpp:1064-1355`): neither depends on flag order (v7
applies spo values after the scan; `--rule` defaults do not reset parameters), so table-order rendering cannot change
the config. The key table (`nav_spec.cpp`, 71 keys) covers every run flag of both parsers; I compared each key's
type with its parser (switches `--locate-in-aim`, `--liquidity-cache`, `--stage-timers`, `--cost-v2`,
`--capacity-curve` take no value; choices match `parse_neutralize`, `--rule`, `--holdings-format`, `--order-basis`,
`--spo-*`); a mismatch could only refuse, never reconfigure. C2's surface: `calibrate_gross`, `calibrate_tolerance`
keys, capability `calibrate-gross-pm6-6` kept (`NavRules` test), `aim_leverage_min/max` = `constants.leverage_range`;
`--risk-model` and `--book-workers` are typed keys (COV-3: no code). `NavSpec.FlagsAndSpecSameConfig` covers those
lines (incl. `--calibrate-gross 1.25 --calibrate-tolerance .005`, `--risk-model`, `--book-workers 4`) through both
parsers; `SpecRunPublishesTheFlagRunBytes` byte-compares a `--print-spec`-produced spec run with the flag run (v6
shape incl. capacity dir, and v5). Unknown keys, wrong types, non-finite numbers, fractional / negative counts,
another schema and extra top-level keys are refused (`:235-266`). C3-1: `initial_nav 2e9`, `liquidity_window 21`,
`capacity_multiples [1, 2]` refused; `[1, 3]`, `[.5, 2]` refused; `[1, 1.5]` with aim 1.2 accepted, with aim 1.8 or
`calibrate_gross` refused. Schema literal `atx.nav-spec/v1` (`nav_spec.hpp:49`). Constants absent = today's values;
a `false` switch = absent; no default changes which code runs. Holes: duplicate keys (R-1), grid + narrow range (R-2).

**C. K-P9-7 registries: PASS (S-1, S-2).** Rows in C1's shape plus `description`
(`nav_rules.cpp:94-108` `row_json`): `x-flag` (`--flag VALUE` or null), closed `params_schema` whose every property
has an `x-flag`, `incompatible[]`. Ranges and defaults match the code (`strategy_target_replay.cpp:105`, `:130`,
`:140`; `nav_validate.cpp` rate check; `TargetReplayConfig{}`, `nav_rate_*`). Ids collide with no IC id (IC table:
ew-theme-*, ic-shrink-*, theme-*, two-speed-v1, mom-volman-v1, within-theme-v1), so E2's `rule_rows` stays clean;
null-x-flag rows give E2's `nav_flags` a KindError (fail closed) and add nothing to `table_flags`. Pre-existing rows:
`rules_json()` starts with `risk_target::leverage_rules_json()` from the untouched `strategy_risk_target.cpp`, so C1's
leverage rows are byte-identical in the array; the envelope's `capabilities` gains `nav-spec-v1`, `fields-seal-p13`
(a listed contract change). Fixtures / pins: no committed copy of the NAV table exists in root or the lane (only the
`classes.json` literal); none was regenerated or hand-edited. The two C1 tests (`strategy_vol_target_test.cpp:541-605`)
now select kind `leverage` and check the text listing as a prefix; the full table is pinned by
`NavRules.OneTableEveryKindInOrder`. E2's frozen synthetic `NAV_TABLE` (pool-17 `scripts/tests/test_wave_kinds.py:80-89`)
does not see C3's rows. P14: `scripts/specs/p9/kinds/new-cost-model.json` names `cost-sqrt-v2` with `params {}`; E2's
`nav_flags` derives `--cost-law cost-sqrt-v2` (two tokens, lowercase value, not a placeholder); the v7 parser maps it to
the `--cost-v2` run (`strategy_nav_v7.cpp:1100-1121`), byte-identical by `CostLawRegistry.CostSqrtV2IsTheCostV2RunByteForByte`.
The fixture is on the path to runnable; the flip is root's (W3-8). Target slot: `--target-rule` refuses every id
before output, and a registered row without a handler is refused ("has no handler"), never ignored.

**D. P13 NAV half: PASS (S-3).** `fields_seal` (`nav_load.cpp:79-95`) is S2's IC rule
(`strategy_ic_admission.cpp:495-507`) with the NAV prefix: no seal block or no `exclusive_end` = "absent", a
non-object block or an `exclusive_end` other than `kSealBeginDate` refuses; it runs in `load_fields` (`:144`) after the
manifest pin and role binding, before any payload and before any output. `log_fields_seal` prints the legacy line on
the replay (`nav_run.cpp:316`) and grid (`:468`) paths. The gtest (`strategy_nav_replay_test.cpp`,
`NavFieldsSeal.ManifestSealMustBeThisBuildsOrAbsent`) has teeth: absent and keyless runs must log, the matched run
must not, daily / events bytes equal between sealed and legacy runs, and "later", numeric and flat seals must exit 1
with their messages and no `nav` dir.

**E. W1-MIN M-1: PASS.** `NavBookWorkers.V6CostV2CapacityAdvHoldOnFourEqualsOne` runs `--rule aim-partial-v6 ...
--cost-v2 --capacity-curve --adv-hold-q .05` at 1 and 4 workers and compares every file recursively (capacity dir
included). Under v6 (not spo, fixed rate) `nav_lockstep.cpp:202-206` lets 4 workers build a `DetPool` (`:261`), so the
pool really runs; any worker-dependent byte fails the test.

**F. CMake: PASS (S-6).** `atx-impl-strategy` (`atx-impl/CMakeLists.txt:34-88`) / `atx-impl-pipeline` (`:90-125`) share
one setup loop (`:127-140`: include dir, C++20, `atx::core/engine/tsdb` PUBLIC, `atx_warnings` PRIVATE, own PCH);
`atx-impl-core` is INTERFACE over both (`:142-143`); the deferred guard (`:146-155`, CMake 3.20 >= 3.19 for DEFER)
refuses `SOURCES` or `INTERFACE_SOURCES` on it; a `PUBLIC` / `PRIVATE` link on it is a CMake error anyway. Source
lists: root vs head differ only by the 16 `book/` TUs replacing `strategy_nav_replay.cpp`; every tree `.cpp` is listed.
No strategy TU includes a pipeline header (`config.hpp`, `dispatch.hpp`, `stage_equity_*`, `panel_pipeline.hpp`,
`trial_ledger.hpp`); the ledger headers are used only by pipeline TUs, so B2's link line moved to `atx-impl-pipeline`
(`:300`) is correct. Build c links the five research exes before `atx-impl-pipeline.lib`. Tails: every earlier lane's
block kept its place and comment, retargeted; 3b appenders add `target_sources(atx-impl-strategy ...)` at the end; a
lane that writes `atx-impl-core` fails at configure. `/W4 /permissive- /WX` come from `atx_warnings` (root
`CMakeLists.txt:265-274`), untouched; no pragma, NOLINT or `/wd` added; no added line over 100 columns. W1-M-5 comment
corrected as `wave1-whole-review.md:118-123` asks.

**G. Tests and logs: PASS (R-1, R-2 add two cases).** 10 new gtests (8 in `strategy_nav_spec_test.cpp`,
`NavFieldsSeal`, `NavBookWorkers.V6...`) + 8 existing in the named filter = 18. `c3-logs/gtest-a-named.log` 18/18
(build a: dirty `e1c16051` tree before the `4e958185` commit); whole `atx-impl-strategy-target-tests` 387/387 in
`gtest-a`, `-b`, `-c` (Debug; c = clean merged head `9dace914`, code = head) and `gtest-e-rel-whole.log` (Release, source
`6abd91b2`); 387 = base 377 (root M2e) + 10; each whole log contains all 18 plus `CovStore.*` 5 and `CalibrateGross.*` 7.
Canary 15 passed on b, c (Debug) and e (Release) (`mega-p9-c3-{b,c,e}-canary.log`, receipts `ExitCode 0`). Missing
from the brief's list: nothing (`NavSpec.FlagsAndSpecSameConfig`, `CostLawRegistry.*`, `TradeRateRegistry.*`, the NAV
suite green). Not built by the lane: `atx-impl-tests` (globs every `*_test.cpp`, incl. the new file).

**H. Safety / house style: PASS.** Bounds: spec file capped at 1 MiB before allocation (`nav_spec.cpp:348-356`);
counts are u64 with `from_chars`; `to_chars` into 64 bytes. Lifetime: `Row` holds `string_view` / `span` into static
literals and arrays; `dispatch` builds `char*` from a vector that is not resized afterwards. Switches over the enum
classes are exhaustive with a trailing return. Error paths: every refusal (spec shape, constants, registry flags, P13)
precedes the output dir; tests check that no dir appears. No new mutable shared state; registry tables are constexpr.

## What root must check at merge

1. Fix round first (R-1, R-2), scoped re-review; then merge. Builds through `research-build.ps1`, both presets:
   `-Targets "atx-impl-strategy-target-tests,atx-impl-tests,atx-equity-strategy-targets,atx-equity-strategy-ic,atx-equity-strategy,atx-equity-strategy-risk,atx-equity-strategy-mine,atx-research-eval,atx-impl" -Canary`.
   Expected: named filter `NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavFieldsSeal.*:NavBookWorkers.*:NavBookRule.*`
   18/18 (+ the fix-round cases), `CovStore.*` 5, `CalibrateGross.*` 7; whole target-tests 387 (+ fix-round cases),
   Debug and Release; `atx-impl-tests` whole once (base count + 10 + fix-round cases, the lane never built it); canary
   15 on Debug and Release goldens. The split renames the PCH owners: on "modified since the precompiled header",
   delete `cmake_pch.cxx.{pch,obj}` by name and rebuild with `CCACHE_RECACHE=1`.
2. `atx-impl-core` is INTERFACE: Ninja has no build target of that name any more. Any root list or script that builds
   it (B2 did: `b2-evidence/mega-p9-b2-b-receipt.json` Targets) must name `atx-impl-strategy` / `atx-impl-pipeline`.
   Any root-side append since `ab189471` that names `atx-impl-core` must be retargeted at the merge, as B2's was.
3. X-5 and Y-F0 `--spec` vs argv (same exe, argv taken from the cells' receipts, no NAV statistic read):
   `$EXE nav <argv> --print-spec > x.navspec.json`, then `$EXE nav <argv>` into NEW/argv and
   `$EXE nav --spec x.navspec.json --output NEW/spec`; compare the two NAV dirs recursively: every file byte-identical,
   no substitution. Write the printed spec with a byte-preserving redirect (Git Bash `>` or `cmd /c ... >`), never
   PowerShell 5.1 `>` (UTF-16LE: the spec is then "not JSON", exit 2). The spec run's console gains one
   `nav spec: atx.nav-spec/v1 <path> sha256 <sha>` line (stdout only).
4. Flag-absent identity against the pre-C3 reference: the NAV dir equals it except `summary.json`
   `producer.engine_git_sha`. If a cell's `--fields` manifest has no `seal` block, the NAV stdout gains the P13 legacy
   line, so the wrapper's `stdout.log` and its digest in `receipt.json` (`scripts/run_bounded_research.py:495`) move:
   check the X-5 / Y-F0 fields manifests for a seal block first, and keep that digest on the volatile list if not.
5. Release = Debug NAV on X-5 (G-P3 NAV half): equal except `summary.json` `producer` (`engine_git_sha`, `build_type`).
6. Wall time: the five research exes' build time on a fresh tag against the pre-C3 receipt (F-10); X-5 NAV wall time
   before / after the 14-TU split in Debug and Release (cross-TU calls are no longer inlined, C-3).
7. W1-I2: the NAV exe changed; re-pin the lineage parents to the wave-3 P9-B0 references before the first P9 cell.
8. W3-8 flip (root post-merge, D3 owns the kind files): drop `new-cost-model.json` from `SCHEMA_ONLY_FIXTURES` and its
   "SCHEMA ONLY" description. E2's tests use a frozen synthetic NAV table, so they will not see C3's rows: run the
   tiny-world kinds (all 10, the flipped one included) against the real exe's `nav --list-rules --json`. Under E2-8 the
   real table now owns `--rule`, `--trade-fraction`, `--dust-multiple`, `--exit-rate`, `--rate`, `--rate-rra`,
   `--rate-lambda`, `--rate-min`, `--rate-max`, `--cost-law`: check no P9 template's `change.flags`,
   `rule_cell.constants.flags` or `horizon.flags` names one (none in pool-17 `scripts/specs/p9/` today).
9. SQL2-CLS: add `atx.nav-spec/v1` to `atx-engine/schemas/research_store/classes.json`.
10. Part B (C2-11, C2-9, C2-10, W3-6) in the C3b round after E2 is in root (C3-2).

## Open questions (PM)

- S-2: is `--cost-v2` refused in v2 option maps (E2-8 owns `--cost-law`, not its alias)? Owner: PM / D3.
- risk-target-v1 runs L_t up to 1.25 L, i.e. 2.5 at L = 2 (`risk_target.hpp:6`, `:41`); OD5-7 keeps "L <= 2.0" and
  E2-13 calls fixed-v1's [1, 2] the P9 cap. Pre-existing (C1 / R-8), not C3's; confirm no P9 cell runs risk-target-v1
  at L above 1.6, or rule the cap applies to L_t.
