# Wave review: NAV / book C++ (GATE-1 step 8; RESUME 11, 2026-10-15)

- **Range:** BASE `3c9eeb01` (wave-1 gate head) .. HEAD `d51d011e` (root after M6), branch
  `feat/platform-p9-20261003`, pool-2. Read by SHA only (`git show d51d011e:<path>`, `git diff 3c9eeb01..d51d011e`).
  pool-2's working tree was at `475f5d8f` (docs only above `d51d011e`; `git diff d51d011e 475f5d8f` empty for every
  code path), so the pytest runs below read `d51d011e`'s code.
- **Area:** lanes C3 (part A `327c86b2`, C3b's NAV hunks `b182dfdd`), AL-CLOCK (`858a4fca`, fix 1 `1d003241`), COV-MV
  (`5c811b56`, fix 1 inside), B2's NAV side, D3's rules; plus the wave-2 hunks that sit in these files (C2
  `a25453be`: `--calibrate-gross`; COV `694b4b95`: the container read path of `spo::RiskStore`; AL-COMB `9d518317`:
  two of the four registrations).
- **Reviewer:** fresh, read-only in pool-2 (no git write, no build, no edit of any file in the range).
- **Verdict (whole area, after the ADDENDUM of section 11): CHANGES_REQUIRED** - 2 Required (both M, missing; no I
  finding), 8 Suggested. M-1 is CLOSED BY RULING PRE-MVX (P9-X's parent is P9-F0h; root writes the prereg lines in
  round 2b-ii before any read). M-2 (addendum) is open: a root slip in the gate's step 2 text and report, no code.
  Suggested: S-2 folds into COVMV-S2 S-b (the PM); S-1, S-3 .. S-8 are P10. The code of the area itself, COV / COVR
  included: no incorrect finding.
- **Addendum (ruling NAV-COV-ADD):** lane COV / COVR's risk files and the D-COV store path, same range and rules,
  section 11.

## 1. Files reviewed (50)

Lines = at HEAD; churn = `git diff --numstat 3c9eeb01..d51d011e`. Depth: H = every hunk of the range read; M = C3
part A's move (`strategy_nav_replay.cpp` -> `book/`), read at the hunks later lanes changed, its byte identity taken
from root's M2j real-data runs (section 4.1); R = registration read against its print code.

| file | lines | churn | lane(s) | depth |
|---|---|---|---|---|
| `atx-engine/include/atx/engine/book/mv_target.hpp` | 122 | +122/-0 | COV-MV | H |
| `atx-engine/src/book/mv_target.cpp` | 351 | +351/-0 | COV-MV | H |
| `atx-engine/tests/book/book_mv_target_test.cpp` | 511 | +511/-0 | COV-MV | H (names, placement) |
| `atx-engine/tests/book/book_normal_score_test.cpp` | 127 | +22/-1 | C2 (M1c-RED) | H |
| `atx-impl/src/book/nav_cli.cpp` | 257 | +257/-0 | C3, AL-CLOCK, fix 1 | M + H |
| `atx-impl/src/book/nav_decide.cpp` | 381 | +381/-0 | C3, AL-CLOCK, COV-MV | M + H |
| `atx-impl/src/book/nav_deploy.cpp` | 165 | +165/-0 | C3 | M |
| `atx-impl/src/book/nav_execute.cpp` | 74 | +74/-0 | C3 | M |
| `atx-impl/src/book/nav_liquidity.cpp` | 95 | +95/-0 | C3 | M |
| `atx-impl/src/book/nav_load.cpp` | 253 | +253/-0 | C3, AL-CLOCK | M + H |
| `atx-impl/src/book/nav_lockstep.cpp` | 364 | +364/-0 | C3, AL-CLOCK, COV-MV, fix 1 | M + H |
| `atx-impl/src/book/nav_mark.cpp` | 153 | +153/-0 | C3 | M |
| `atx-impl/src/book/nav_publish.cpp` | 294 | +294/-0 | C3, AL-CLOCK | M + H |
| `atx-impl/src/book/nav_replay_internal.hpp` | 835 | +835/-0 | C3, AL-CLOCK | M + H |
| `atx-impl/src/book/nav_rules.cpp` | 219 | +219/-0 | C3, AL-CLOCK, COV-MV | H |
| `atx-impl/src/book/nav_rules.hpp` | 76 | +76/-0 | C3, AL-CLOCK, COV-MV | H |
| `atx-impl/src/book/nav_run.cpp` | 582 | +582/-0 | C3, C3b, AL-CLOCK | M + H |
| `atx-impl/src/book/nav_session.cpp` | 418 | +418/-0 | C3, AL-CLOCK | M + H (`run_books` whole) |
| `atx-impl/src/book/nav_spec.cpp` | 492 | +492/-0 | C3, C3b, AL-CLOCK | H (table, from_args / to_args) |
| `atx-impl/src/book/nav_spec.hpp` | 106 | +106/-0 | C3, C3b, AL-CLOCK | H |
| `atx-impl/src/book/nav_summarize.cpp` | 200 | +200/-0 | C3 | M |
| `atx-impl/src/book/nav_validate.cpp` | 235 | +235/-0 | C3, AL-CLOCK | M + H |
| `atx-impl/src/book/nav_write.cpp` | 574 | +574/-0 | C3, AL-CLOCK | M + H |
| `atx-impl/src/strategy_mv_aim.cpp` | 336 | +336/-0 | COV-MV | H |
| `atx-impl/src/strategy_mv_aim.hpp` | 158 | +158/-0 | COV-MV | H |
| `atx-impl/src/strategy_nav_calibrate.cpp` | 127 | +127/-0 | C2 | H (flag gating) |
| `atx-impl/src/strategy_nav_calibrate.hpp` | 85 | +85/-0 | C2 | H |
| `atx-impl/src/strategy_nav_replay.cpp` | 0 (deleted) | +0/-3935 | C3 | M |
| `atx-impl/src/strategy_nav_replay.hpp` | 840 | +111/-7 | C3, AL-CLOCK | H |
| `atx-impl/src/strategy_nav_v7.cpp` | 1518 | +153/-21 | C3, AL-CLOCK, COV-MV, fix 1 | H |
| `atx-impl/src/strategy_nav_v7.hpp` | 364 | +30/-0 | COV-MV | H |
| `atx-impl/src/strategy_risk_target.cpp` | 385 | +5/-5 | C2 | H |
| `atx-impl/src/strategy_spo.cpp` | 1891 | +68/-9 | COV | H (`RiskStore::open` / `read`) |
| `atx-impl/src/strategy_spo.hpp` | 490 | +10/-0 | COV | H |
| `atx-impl/src/strategy_target_replay.cpp` | 1963 | +1/-1 | C3 | H |
| `atx-impl/src/strategy_target_replay.hpp` | 267 | +3/-0 | C3 | H |
| `atx-impl/src/strategy_two_speed.hpp` | 16 | +12/-46 | D2 (a shim to the IC area's header) | H |
| `atx-impl/tests/CMakeLists.txt` | 220 | +34/-1 | C2, AL-COMB, C3, AL-CLOCK, COV-MV, SIGN, COV | H |
| `atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json` | 20 | +20/-0 | AL-CLOCK fix 1 | H (= the live row) |
| `atx-impl/tests/strategy_mv_aim_rule_test.cpp` | 943 | +943/-0 | COV-MV | H (identity, reserve, calibrate tests) |
| `atx-impl/tests/strategy_nav_calibrate_test.cpp` | 327 | +327/-0 | C2 | names |
| `atx-impl/tests/strategy_nav_info_clock_test.cpp` | 1143 | +1143/-0 | AL-CLOCK, fix 1 | names, identity tests |
| `atx-impl/tests/strategy_nav_replay_test.cpp` | 3702 | +290/-0 | C2 | H (additions only) |
| `atx-impl/tests/strategy_nav_spec_test.cpp` | 814 | +814/-0 | C3, C3b | names |
| `atx-impl/tests/strategy_spo_v3_test.cpp` | 1664 | +14/-0 | C2 | H |
| `atx-impl/tests/strategy_vol_target_test.cpp` | 775 | +56/-2 | C2, C3 | H |
| `scripts/specs/p9/rules/info-clock-rate-v1.json` | 99 | +99/-0 | AL-CLOCK, fix 1 | R |
| `scripts/specs/p9/rules/mv-aim-v1.json` | 90 | +90/-0 | COV-MV | R |
| `scripts/specs/p9/rules/mom-volman-v1.json` | 108 | +108/-0 | AL-COMB | R |
| `scripts/specs/p9/rules/theme-cov-hedge-v1.json` | 95 | +95/-0 | AL-COMB | R |

Read as inputs, not in the file count: the two templates `scripts/specs/p9/templates/{info-clock,mv-aim}.json`
(claimed by no area), the prereg blocks B.4, B.5, B.7, B.8 and A.4 (`p9-prereg.md`).

**Not reviewed (claimed by no area of `r11-dispatch.md` "WAVE REVIEW"):** lane COV / COVR's risk files
`atx-impl/src/strategy_risk_model.{cpp,hpp}` (+422/-8, +200/-3), `atx-impl/src/strategy_risk_verb.cpp` (+447/-21),
`atx-engine/src/risk/cov_container.cpp` (+988), `atx-engine/include/atx/engine/risk/cov_container.hpp` (+491),
`atx-impl/tests/strategy_cov_{model_test.cpp,stage_diag.cpp,fixture.hpp}`. Only `RiskStore::open` / `read` (the
read path the NAV rules call) was read here. Open question Q1.

B2: no hunk of B2's merges (`c2416d62`, `5dfb0ba1`, `6d47b28d`) is in this area's C++ files (`git diff --stat
<merge>^1 <merge>` empty for every path above); B2's NAV side is `atx-impl/tools/nav_summ.py`, the Python
reviewer's. D3: its merge `0a2ceb2b` edits nothing under `scripts/specs/p9/rules/`; its C3-13 kind derivation is
Python (`scripts/wave_kinds.py`) and is exercised below through `test_info_clock_rule.py` and the composed
derivation (section 6).

## 2. Required findings

| id | file:line | finding | failing input | fix |
|---|---|---|---|---|
| **M-1** (missing) | `atx-impl/src/strategy_nav_v7.cpp:1228-1231` (a second `--risk-model` / `--risk-model-sha256` refused), `:1345-1346` (one `risk_model`), `:1465-1471` (one `spo::RiskStore` opened into `NavV7Options::spo_risk`, shared by the spo rules, the leverage rules and `mv-aim-v1`); against `p9-prereg.md` B.7 (`:515-529`: P9-X = "the Y-1 template's [change] on P9-F0's NAV argv: `--vol-target vol-target-v1 --risk-model build-equity/p9-m2i-dcov-v11 --risk-model-sha256 39736647...`") and B.8 (`:572-580`, P9-V on P9-F0 with the `atx-cov-v1` store), with A.4 "P9-F0: the last accepted P9 cell, leverage excluded" | The NAV exe holds ONE risk store per argv. If P9-MV is accepted, P9-F0 is the `mv-aim-v1` book, whose argv carries `--target-rule mv-aim-v1 --risk-model build-equity/p9-m2i-dcov-covv1 --risk-model-sha256 ea58691f...` (the rule's params, PRE-9). P9-X as registered then cannot run: its argv names a second store. The only argv the exe accepts makes both rules read one store, i.e. either P9-X's vol target reads the `atx-cov-v1` store (P9-X becomes P9-V and the PRE-29 pair "differs by the recipe only" collapses) or `mv-aim-v1` reads the v1.1 store (the accepted P9-MV book is no longer the book P9-X levers). Any of these choices made after P9-MV's read is a change to a registered cell after a read. Nothing in the prereg or the ledger rules it (grep: no ruling names P9-X / P9-V on an mv-aim parent; PRE-10 / PRE-39 give P9-F0h only to G-B8 and P9-W). COMPOSE-1 covered `mv-aim-v1` + `info-clock-rate-v1` only | My probe on the p9-3d NAV exe (`7e101b56...`, receipt `mega-p9-3d-receipt.json`), parse stage only, nothing written: `nav --combined X --combined-sha256 0..0 --role Y --role-sha256 0..0 --output O --rule aim-partial-v5 --trade-fraction .05 --target-rule mv-aim-v1 --risk-model <covv1> --risk-model-sha256 0..0 --vol-target vol-target-v1 --risk-model <v11> --risk-model-sha256 1..1` -> exit 2, `InvalidArgument: nav v7: duplicate/missing value: --risk-model`; the same argv with ONE store passes the v7 parse and stops only at the store open (`IoError: cannot open file for SHA-256 digest`, as the mv-only control) | Before any cell, a PM ruling and one prereg line in B.7 / B.8 (no code change): e.g. (a) if P9-MV is accepted, P9-X and P9-V run on P9-F0h (A.4's "last accepted book that does not read `--risk-model`", already defined by PRE-10) and their prints say so; or (b) if P9-MV is accepted, P9-X and P9-V lapse at 0 in PRE-14's form with the refusal line above as the finding. (c) A second store slot for the target rule is a code lane (P10), not a slip. Root records the choice in the gate report and the P9-X / P9-V hand specs |

## 3. Judgement by item

### 3.1 Flag-absent byte identity (item 1)

Traced per new flag / rule; every branch below is keyed on the flag, and none changes an allocation, an ordering, a
printed field, a reserve value or a summation order when the flag is off.

- **`--rate info-clock-rate-v1` / `--info-clock-print` (AL-CLOCK, fix 1).** `book/nav_load.cpp:206-213`
  (`ClockLoad::None`: the fields count, the budget check and `remaining` are the pre-lane values; the loader's
  `max_working_bytes` copy is unchanged), `book/nav_lockstep.cpp:70` (reserve term 0), `:204` (the variant cadence
  check only under the rule), `book/nav_session.cpp:306` (no `clock_rate` allocation), `:360` (no clock row),
  `book/nav_decide.cpp:296`, `:340` (no rate span, no census), `book/nav_validate.cpp:79-96` (the `rate_ok`
  refactor is the same predicate for Fixed and PerNameV1), `:182-195` (an empty clock skips every check), `:210`
  (`Budget::add(0, 8)` is a no-op: `book/nav_replay_internal.hpp:120-123`), `book/nav_write.cpp:252` (recipe keys
  only under the rule), `book/nav_publish.cpp` (the block only with `run.info_clock`, which `nav_run.cpp` sets only
  under `--info-clock-print`; `v5.rate` only under the rule), `book/nav_spec.cpp:124` (the key appended at the
  table's end; `from_args` / `to_args` (`:378-410`) emit only present keys in table order, so every existing spec
  renders the same argv), `strategy_nav_v7.cpp` `capacity_rule(false)` = `capacity_declaration` (root's
  `m6-logs/alclock-checks.txt`: the same SHA-256 `611d4725...` at `97505c1c` and `1d003241`), `extend_recipe` reads
  the recipe's own `rate` key, which `nav_write.cpp:252` writes only under the rule. `--rate` is not a v7-claimed flag
  (`registry_flag`, `strategy_nav_v7.cpp:155-157`: only `--cost-law`, `--target-rule`), so no plain run moves to the
  v7 path. Changed for every run, by design and ruled: the `--help` text and two refusal messages (ALCLOCK-S1a), the
  NAV listing's new row (W3-9; my `nav --list-rules --json` on p9-3d: 13 rows, `info-clock-rate-v1` last trade-rate
  row, `mv-aim-v1` last row, every other row as root's M4 comparison).
- **`--target-rule mv-aim-v1` (COV-MV, fix 1).** `book/nav_decide.cpp:73-77` (the same `Result<bool>`;
  `v7::apply_target_rule` returns Ok and leaves `desired` untouched without the extension or the rule),
  `book/nav_lockstep.cpp:77` (`extension_workspace_bytes` 0), `State::mv` null, `add_mv_aim` a no-op, the `spo_only`
  predicate and the store-open condition (`strategy_nav_v7.cpp:1465`) unchanged for `mv_aim` false.
- **C3 part A** (typed NavSpec, K-P9-7 registries, the 14-TU split, P13 seal, `--ann-mean-gross`): byte identity
  measured by root at M2j on real data, not by construction: `m2j-logs/item2-cmp.txt` .. `item5-cmp.txt` (X-5 and
  Y-F0 argv runs equal their references outside `/producer`, Release and Debug; the `--spec` runs equal the argv runs
  on all 27 files; `--ann-mean-gross` adds exactly one key per main scenario). C3b's NAV hunks are refusals only
  (`read_grid` duplicate key; `parse_unique_keys` keeps the document `Json::parse` gives).
- **Wave-2 hunks in these files** (C2 `--calibrate-gross`, COV's container read path): flag-gated (`calibrate.on()`;
  the manifest's `container` key) and measured at M2e / M2d (root's records); later lanes do not touch their off path.
- **Evidence, re-derived.** Lane / root gtest logs: `m6-logs/gt-infoclock.log` (14 / 14 incl.
  `InfoClockPrint.FlagAbsentLoadsNoClock`, `.RuleOnWithoutFlagPrintsNoBlock`,
  `InfoClockRate.CapacityCurveAcceptsThisRateOnly`, `.WorkspaceReserveOneRatePerName`), `gt-alclock-named.log`
  (26 / 26) on p9-3d (target-tests `685b8ba2...`, source `1d003241`); `m4-logs/gt-2y-mvaimrule.log` (16 / 16 incl.
  `MvAimRule.FlagAbsentIdentity`: day bits with and without the extension, `.WorkspaceReserveTermOnlyWithTheRule`),
  `gt-2y-mvtarget.log` (12 / 12), `gt-2y-covmv-named.log` (100 / 100) on p9-2y (source `5c811b56`). My own: the
  Debug tiny-world canary, 15 passed, on the p9-3d NAV exe (section 6). **Not yet shown at any head holding AL-CLOCK
  and COV-MV:** the parents' real-data flag-absent identity (X-5, Y-F0) and the Release canary: gate step 3 (TEST-1).

### 3.2 Composition `mv-aim-v1` + `info-clock-rate-v1` (item 2, COMPOSE-1): no conflict found

- **State routing.** `mv-aim-v1` rewrites `shared.desired` inside `decide_construction` (`nav_decide.cpp:76`), so the
  calibrate tape records the post-rule target and a restored pass reads it (`nav_session.cpp:350`); the info-clock
  rates are formed AFTER that branch, from clock row t, into `shared.clock_rate` (`nav_session.cpp:360-367`), so a
  restored pass forms them again. Both on the calling thread between the two `for_each_lane` phases (`:336`, `:375`);
  books only read both. Disjoint storage.
- **Who owns the rate.** `NavReplayConfig::rate` (base parser, `nav_cli.cpp:203-208`); the target rule is
  `NavV7Options::mv_aim` (v7 parser, `strategy_nav_v7.cpp:1171`). Neither reads the other's state.
- **Capacity.** The rate is accepted with `--capacity-curve` (`strategy_nav_v7.cpp:1438-1443`, ALCLOCK-3);
  `mv-aim-v1` does not refuse it. Capacity books read the one shared target and the one rate vector; neither census
  counts a capacity book (`scored = !b.capacity && ...`; `mv_aim` counts a decision index once,
  `strategy_mv_aim.cpp:158`).
- **Refusal sets.** The live rows (p9-3d) list neither rule in the other's `incompatible[]`; both need
  `aim-partial-v5`; the rate needs `--trade-fraction .05`; `mv-aim-v1` refuses `--construction-grid`, the rate needs
  the base cadence in a grid. Compatible.
- **Reserve.** Two independent terms (`nav_lockstep.cpp:70`, `:77`).
- **Probes (mine, parse stage only, nothing written).** Both flags on one argv pass the v7 parse and stop at the store
  open exactly as the mv-only control does; `--rule aim-partial-v6` and `--two-speed two-speed-v1` controls refuse at
  the parse. Python side: `wave_kinds.rule_flags` on the two registrations' `rules` blocks together, against p9-3d's
  listing: no problem; derived `--rate info-clock-rate-v1 --risk-model build-equity/p9-m2i-dcov-covv1
  --risk-model-sha256 ea58691f... --target-rule mv-aim-v1`. The checks after the store open
  (`validate_nav_config`, `validate_nav_input`) remain the gate's COMPOSE-1 tiny-world parse with the fake store.
- The composition that does conflict is `mv-aim-v1` with a leverage rule on another store: M-1.

### 3.3 Performance (item 3)

- No per-decision allocation in the rules' own code after the first decision, except as Suggested S-1 / S-4 /
  S-6: `nav_info_clock_rates` writes the vector `run_books` sized once; the census is index-order arithmetic;
  `mv_aim::Rule` keeps its view, scratch and `FactorEigen` across decisions (COVMV-S S-1 held:
  `MvTarget.FactorEigenOnKeptStorageIsAFreshSolve`); Eigen's per-call Householder vector is disclosed
  (`mv_target.hpp`) and ruled P10 (COVMV-S2 S-a).
- No hot-path exception: every new per-decision path returns `Status` / `Result`; strings are built on error paths
  only (S-6 is the one frequent pass-through that builds one).
- Thread safety in lockstep: the rate vector and the target are written only on the calling thread between the pool
  phases (`nav_session.cpp:336`, `:360`, `:375`); each book writes only its own census; `--book-workers` 1 vs 4 bytes
  are tested with the rate on (`InfoClockRate.BookWorkersOneAndFourSameBytes`) and with the target rule on
  (`MvAimRule.BookWorkersParity`), both green in root's logs.

### 3.4 Registrations promise only printed figures (item 4)

- `info-clock-rate-v1.json`: the `info_clock` keys it lists are exactly `info_clock_json`'s
  (`book/nav_write.cpp:289-309`); the realised-leverage clause (ALCLOCK-4, fix 1) names
  `scenarios[k].exposure.max_gross_leverage` / `mean_gross_leverage`, printed at `book/nav_write.cpp:371-372`; the
  capacity / 4x figures exist under `--capacity-curve`, which the nominal parent carries (the pytest asserts it); the
  constants are the code's (window 21, `two_speed_fast_theta`, slow .05 required, field `ea_days_since`); "the decide
  path refuses every non-fixed rate" holds (`book/nav_deploy.cpp:87`); the row fixture equals the live p9-3d row;
  the ALCLOCK-4 clauses are verbatim and pinned by the pytest (my teeth: section 7). Its `expected_effect` lines are
  `[est]` plan figures, not promised prints.
- `mv-aim-v1.json`: every `printed_only` key is in `Rule::summary_json` (`strategy_mv_aim.cpp:295-333`) except the
  null form of `eigen_00_02_share` (COVMV-S2 S-b, already on the re-pin / PRE list); every gtest it names exists in
  `book_mv_target_test.cpp`; the store pin equals PRE B.5; the template pin equals the file (the lane's
  `validate_specs.py` against p9-3d's listing: every line OK). S-2 below (`ex-ante` in the mechanism text).
- `theme-cov-hedge-v1.json`, `mom-volman-v1.json` (AL-COMB, wave 2): `composition_hedge_trace` /
  `composition_schedule_trace` are printed by the IC exe (`strategy_ic_theme_hedge.cpp:222`,
  `strategy_ic_mom_volman.cpp:187`); the vol and gross-alpha ratios have their producers (PRE-38 `ann_vol`, C3-9
  `ann_mean_gross`); the sleeve vol / Sharpe line reads `atx-research-eval --vol --daily` on the IC exe's sleeve CSVs
  (root M3 "For PRE", D3W). `mom-volman-v1.json:50` is stale by D3W-STALE (PRE-40 form): noted, not a finding.

### 3.5 House style (item 5)

- `/W4 /WX`: 0 warning lines on root's Debug builds p9-2x, p9-2y, p9-3d (root's `warn_count.py`). The AL-CLOCK and
  COV-MV TUs have never compiled in Release; gate step 1 is their first Release compile. The only `assert` in the new
  code (`strategy_mv_aim.cpp:150`) leaves no Release-only unused name.
- 100 columns: no added C++ line over 100 except S-5 (two comment lines at 101, C2's test).
- Test placement: `book_mv_target_test.cpp` in `atx-engine/tests/book/` (`book_` prefix, `atx-engine-book-tests`);
  the `strategy_*` tests through `target_sources` into `atx-impl-strategy-target-tests` and the `atx-impl-tests` glob.
- Duplicated plumbing: S-3 (small helpers) and the PM6-6 test readers (ALCLOCK-S2 S-B, P10). The store plumbing is
  not duplicated: one `RiskStore`, opened once, serves spo, the leverage rules and `mv-aim-v1` (which is M-1's root).

### 3.6 No expected hash / golden / pinned value of record edited (item 6): holds

In-area test edits are additions, except `strategy_vol_target_test.cpp` (C3: an exact `--list-rules` text became a
prefix check because the table grew; the leverage rows stay pinned byte for byte, the rest by `NavRules.*`): not a
pinned value of record. `info-clock-rate-v1.json`'s SHA-256 moved in fix 1 (`51b8b754...` -> `7801c198...`), allowed
by ALCLOCK-S (unpinned): no tracked file outside the sprint dir names either value. No canary golden, gold-run digest
or fixture SHA in this area moved in the range.

## 4. Suggested

| id | file:line | problem | fix |
|---|---|---|---|
| S-1 | `atx-impl/src/strategy_spo.cpp:791`, `:799` | `RiskStore::read` builds a `std::filesystem::path` and a `std::vector<f32> narrow(n x 11)` on every call (pre-existing at BASE); `mv-aim-v1` now calls it once per rebalance decision (`strategy_mv_aim.cpp:110`), as the leverage rules and spo do; `mv_aim::workspace_bytes` counts the f32 buffer as if it were kept (`strategy_mv_aim.cpp:81-82`) | keep the f32 buffer in `RiskSlice` (and the base path in the store): P10 centralization list beside COVMV-S2 S-a |
| S-2 | `scripts/specs/p9/rules/mv-aim-v1.json:14`, `:62` | `predicted_mechanism` says the tilt "lowers ex-ante and realised vol"; no exe prints an ex-ante vol of the MV target against the desired target (the `mv_aim` block prints the specific-vol ratio and the eigen shares; B.5's predictions read `ann_vol` and `ann_mean_gross`). The same word was cut from P9-H under ALCOMB-9 (b) | PRE B.5 (round 2b-ii) states that no ex-ante vol is read; the text stays (PRE-40 form) unless the re-pin edits the unpinned registration as COVMV-S2 S-b allows |
| S-3 | `atx-impl/src/strategy_mv_aim.cpp:27`, `:35`; `strategy_nav_calibrate.cpp:55`; `atx-engine/src/book/mv_target.cpp:42` | `finite_or_null` copied beside `nav_book::finite_or_null` (`book/nav_write.cpp:10`, declared `book/nav_replay_internal.hpp:507`); `reserve_exact` copied in the rule and the kernel | one helper each (an atx-core or engine book detail header): P10 centralization list |
| S-4 | `atx-engine/src/book/mv_target.cpp:181`, `:198` | the scratch's `a` / `u` are reserved at the call's active count, so each new maximum of priced names reallocates (bounded by n, a few times per run) | reserve at n on first use (the Rule knows n), so "no allocation after the first decision" is exact |
| S-5 | `atx-impl/tests/strategy_nav_replay_test.cpp:3528`, `:3612` | two comment lines at 101 columns (C2, wave 2) | rewrap on the next touch |
| S-6 | `atx-impl/src/strategy_spo.cpp:785-789` | the no-forecast `Unavailable` builds a message with `std::to_string` on every such decision; `mv-aim-v1`'s pass-through counts it and drops the text | with S-1: a code-only status on that path, or the message built by the caller that prints it |

## 5. Previous reviews' open items (not re-raised)

AL-CLOCK S-A (rule-on "x1 is S2 bit for bit" untested), S-B (PM6-6 test helpers), S-5: ruled ALCLOCK-S2 / P10.
COV-MV S-a .. S-d: ruled COVMV-S2 (S-b to the re-pin / PRE list, the rest P10). COMPOSE-1, MV-FLAG-P0: rulings
stand; nothing here changes them.

## 6. Tests run

Own runs (interpreter `C:/Program Files/Python312/python.exe`, `PYTHONHASHSEED=0`, `PYTHONDONTWRITEBYTECODE=1`,
`-p no:cacheprovider`, basetemp under the session scratchpad `wave-nav/bt/`; `ATX_EQUITY_ROOT` /
`ATX_EQUITY_BIN` = pool-2's `build-equity` / `bin`, read-only; nothing these runs wrote is under pool-2 (this file is the only write there, uncommitted): `git status` unchanged,
newest `build-equity/bin` entry is root's 11:40 build):

| run | result |
|---|---|
| `scripts/tests/test_info_clock_rule.py` in pool-2 (seed 0) | **5 passed**, exit 0 |
| the same file on a `git archive d51d011e` mirror (control for the mutation) | **5 passed** |
| lane tool `covmv-logs/validate_specs.py` against p9-3d's `nav --list-rules --json` | exit 0: template / registration SHAs as pinned, `rule_cell_problems` [], `registration_problems` [], `rule_flags` problems [] |
| the two NAV rules' `rules` blocks together through `wave_kinds.rule_flags` (p9-3d listing) | exit 0, no problem (section 3.2) |
| `nav --list-rules --json` (p9-3d NAV exe) | exit 0, stderr empty, 13 rows, sha256 `13fcfcd104e1f68c84628d60bd51ee59a7e6b7511308893fb66242c3584882cb`; info-clock row = the committed fixture |
| NAV exe parse probes (six argvs, synthetic paths, nothing written) | both NAV rules: v7 parse passes; `aim-partial-v6` / `--two-speed` controls refuse; two stores refused (M-1); one store passes |
| tiny-world canary `scripts/tests/test_cycle_e2e.py`, Debug goldens, `ATX_CANARY_REQUIRED=1` | **15 passed** (5.84 s) on NAV p9-3d `7e101b56...`, IC p9-3b `ef3974cf...`, eval p9-2z `47910191...` (each = its receipt and root's `m6-logs/exe-sha-end.txt`; EXE-CUR). The NAV exe's source `1d003241` has the head's C++ (`git diff 1d003241 d51d011e` empty for C++ and CMake); the IC exe is stale by NAV help text only, so this is the NAV leg's evidence, not the gate's canary of record |

gtest claims: from the lanes' and root's logs only (no build, no gtest run): section 3.1.

## 7. Mutation (own; TEETH-RESTORE, TEETH-CRLF form)

Done on a `git archive d51d011e` mirror (scripts, `atx-impl/tests/fixtures`, `atx-engine/tools`, `atx-impl/tools`,
`atx-impl/strategies`) under the session scratchpad, not in pool-2: the hard rule forbids editing a file of the range,
and pool-2's tree is read by four other reviewers at the same time. The mirror's blob SHA-256 of the mutated file equals
HEAD's (`7801c198...`) before the run.

- Mutation: in `scripts/specs/p9/rules/info-clock-rate-v1.json`, ALCLOCK-4's clause "realised gross leverage is read
  from the cell's own summary (max and mean over the cell) and printed beside the parent's" replaced by the superseded
  "the realised gross the OD5-7 cap reads is the parent's" (the fix-1 S-2 regression).
- Result: `test_registration_and_template` **1 failed**; bytes restored in `finally` from the tool's own copy;
  SHA-256 after = HEAD's blob; the whole file re-run **5 passed**. **Caught.**

## 8. What root must do before any cell

1. **M-1:** obtain the PM's ruling on P9-X / P9-V when P9-MV is accepted, write the prereg line (B.7, B.8), and
   carry it into the P9-X / P9-V hand specs.
2. COMPOSE-1's tiny-world parse with the fake store (`--rate info-clock-rate-v1 --target-rule mv-aim-v1`): this
   review's probe covers the v7 parse only; the base config and input checks come after the store open.
3. Gate step 3, the first at a head holding AL-CLOCK, COV-MV and both fix rounds: X-5 / Y-F0 flag-absent identity on
   the Release exes (outside `/producer`), the Release canary, the golden at 1 and 4 workers; `mv-aim-v1.json`'s own
   `requires` (rule-absent identity, the listing differing by the row alone, both canaries, S9 / COV-9 delisting
   check, W1-I2 re-pin).
4. Gate steps 1-2: the first Release compile of the AL-CLOCK / COV-MV TUs at `/W4 /WX`; whole target-tests and
   book-tests on both builds, which re-run `MvAimRule.*`, `MvTarget.*` and COV-MV's named filter, last run on p9-2y
   (`5c811b56`), before AL-CLOCK fix 1 changed `strategy_nav_v7.cpp` (`declarations`, `capacity_rule`).
5. PRE round 2b-ii: S-2 beside COVMV-S2 S-b in B.5; S-1, S-3, S-4, S-6 to the freeze report's P10 centralization list.
6. Q1: the unclaimed COV / COVR risk files (section 1) are either assigned to a reviewer or recorded in the gate
   report as not wave-reviewed.

## 9. Permission refusals

None.

## 10. Open questions

- Q1: who reviews lane COV / COVR's risk files (`strategy_risk_model.*`, `strategy_risk_verb.cpp`,
  `atx-engine` `risk/cov_container.*`, `strategy_cov_*` tests)? No area of the wave review claims them.
  **Ruled NAV-COV-ADD: this reviewer, section 11.**
- Q2 (M-1): option (a) P9-X / P9-V on P9-F0h, or (b) lapse at 0, when P9-MV is accepted? **Ruled PRE-MVX: (a) for
  P9-X (P9-V pairs with P9-X); a two-store NAV is P10 (NAV-2STORE).**

## 11. ADDENDUM: lane COV / COVR risk files and the D-COV store path (ruling NAV-COV-ADD, 2026-10-15)

Same range (`3c9eeb01..d51d011e`), same rules: read by SHA, no build, no git write in pool-2, nothing run on real
data (the D-COV stores were only hashed and their `manifest.json` read at the keys named below, which hold no
statistic), blind. Lanes: COV (`694b4b95`, wave 2), COVR (`c8ddf736`), COV-MV's one later test hunk (`5c811b56`);
D-COV = root M2i's runs. Ledger read: COV-1 .. COV-14, COVR-1, CRT-2, OD5-2, M2c-RED-2, COV-12, PRE-MVX,
NAV-COV-ADD; records: `handoff-root-m2i.md`, `root-wave2-merge-report.md` (M2d COV), `task-COVR-report.md`,
`m2i-logs/`, `m2d-logs/cov-diff-legacy.log`.

### 11.1 Files added (12)

| file | lines | churn | lane | depth |
|---|---|---|---|---|
| `atx-engine/include/atx/engine/risk/cov_container.hpp` | 491 | +491/-0 | COV | H (contract, reader API) |
| `atx-engine/include/atx/engine/risk/README.md` | 2 rows | +2/-0 | COV | H |
| `atx-engine/src/risk/cov_container.cpp` | 988 | +988/-0 | COV | reader H (`open`, `view`, `parse_cov_block`, `factor_risk_view`, mapping); writer at its rules |
| `atx-engine/tests/risk/risk_cov_container_test.cpp` | 1053 | +1053/-0 | COV | names, placement, pins |
| `atx-engine/bench/risk_cov_container_bench.cpp` | 339 | +339/-0 | COV | placement (bench, `ATX_BUILD_BENCH`) |
| `atx-impl/src/strategy_risk_model.cpp` | 1652 | +422/-8 | COV | `scatter_cov_block` H; the estimator at COVR's CRT call sites |
| `atx-impl/src/strategy_risk_model.hpp` | 615 | +200/-3 | COV | H (declarations) |
| `atx-impl/src/strategy_risk_verb.cpp` | 1481 | +447/-21 | COV | `cov-diff` / `cov-info` H; the writer's output rule |
| `atx-impl/tools/equity_strategy_risk.cpp` | 20 | +11/-4 | COV | H |
| `atx-impl/tests/strategy_cov_fixture.hpp` | 639 | +639/-0 | COV | pins |
| `atx-impl/tests/strategy_cov_model_test.cpp` | 891 | +891/-0 | COV, COV-MV (+11, the a = 2 control) | pins, the later hunk |
| `atx-impl/tests/strategy_cov_stage_diag.cpp` | 572 | +572/-0 | COVR | placement (DISABLED_, not globbed) |

Cited, not counted: `atx-impl/tests/strategy_cov_store_test.cpp` (427; the store area's by the `*store*` precedence;
read for pins only), `atx-impl/src/strategy_spo.{cpp,hpp}` (`RiskStore::open` / `read`: section 1 above),
`scripts/run_bounded_research.py` (+97/-34: lanes E2 / SQL3, the Python area's; no D-COV hunk). No other D-COV tool or
store writer under `scripts/` or `atx-impl/tools/` is in the range: D-COV's helpers live in the sprint dir
(`m2i-logs/tools/`), not in the code tree.

### 11.2 Required

| id | file:line | finding | failing input | fix |
|---|---|---|---|---|
| **M-2** (missing) | `root-gate-brief.md` step 2 ("`atx-equity-strategy-risk cov-diff --legacy` on the store of record exits 0"); COV-12 (b)'s "the store of record `v8-risk-lo3-v10` as `cov-diff --legacy` checks it"; code `atx-impl/src/strategy_risk_verb.cpp:1294-1297` (a manifest without `container` is refused) and `:1454` (any refusal exits 2) | `cov-diff --legacy DIR` compares the container in DIR with the legacy dense payloads OF THE SAME DIR (one run's internal consistency: every block scattered back equals `factor_covariance.f64`, `specific_variance.f64` and, when written, `style_exposures.f32` / `industry_slot.u8`, by `memcmp`). It does not compare a store with the store of record. The store of record `build-equity/v8-risk-lo3-v10` holds no container, so the gate's check, read as written, exits 2 by construction; identity with `v8-risk-lo3-v10` was shown at M2d another way (a flag-absent rebuild, all 10 payloads byte-identical, `m2d-logs/cov-identity-legacy-cmp.txt`). The Release-built D-COV stores have never been put through `cov-diff --legacy` (M2d ran it on the Debug-built `p9-m2d-cov-risk-container` only: exit 0, 1,405 sessions, four files) | `build-equity/v8-risk-lo3-v10/manifest.json` has no `container` key (read by key only); `compare_container_to_legacy` -> `InvalidArgument: cov-diff --legacy: the manifest names no container` -> exit 2. Not executed here (no real-data run by a reviewer) | Root slip, no code: the gate runs `cov-diff --legacy` on the three container stores, `build-equity/p9-m2i-dcov-v11`, `build-equity/p9-m2i-dcov-covv1` (both list all four legacy payloads in `files`) and `build-equity/p9-m2d-cov-risk-container`, read-only and bounded; it records exit 0 as "each store's container equals its own legacy payloads", and cites M2d's payload comparison for the store-of-record identity. The gate report says this in place of the brief's sentence |

### 11.3 Judgement

- **The two pinned D-COV stores are read-only inputs (CRT-2): holds.** Every file in both directories was last
  written on 2026-10-04 (M2i). `manifest.json` SHA-256 = the pins (`397366473b3a...c7`, `ea58691fc407...f3`);
  `model.atxcov` SHA-256 = the pins (`cd48d574...e3`, `0f90bf7c...2a`); `manifest.container.root_sha256` and
  `file_sha256` and `files["model.atxcov"].sha256` = the pins (keys read only). No tracked file names either directory
  except `mv-aim-v1.json` / `mv-aim.json` as the `--risk-model` input. The writer cannot overwrite them:
  `strategy_risk_verb.cpp:1128-1129` refuses an existing output directory, and the container writer creates
  `<path>.partial` exclusively and renames at `finalize()`. The reader maps read-only
  (`cov_container.cpp:674`, `Mapping::map_file_ro`).
- **The reader path is byte-stable across builds: holds by construction and by evidence.** `CovModelFile::view`
  (`cov_container.cpp:865-884`) returns spans into the mapping after the index check (the hash was checked at open:
  `RiskStore::open` uses `VerifyMode::Full`); `parse_cov_block` checks bounds overflow-safely (offset first, then count
  against the remaining length) and does no arithmetic on values; `scatter_cov_block`
  (`strategy_risk_model.cpp:1623-1650`) copies, and narrows f64 exposures to f32 by `static_cast` (exact IEEE rounding,
  build-invariant); `RiskStore::read` then maps non-finite covariance to 0 and widens f32 to f64 (exact). No CRT
  transcendental on the path: no `exp2` / `exp` / `pow` / `log` / `tanh` in `cov_container.cpp`, `RiskStore::read`,
  `book/risk_target.cpp`, `strategy_risk_target.cpp`, `book/mv_target.cpp`, `strategy_mv_aim.cpp` (my grep; = COVR-1's
  root reading, M2i C5). The Debug / Release difference COVR attributed is on the WRITER only (`std::exp2(-1/H)` at
  `strategy_risk_model.cpp:490` / `:853`, `ucrtbased` vs `ucrtbase`, OD5-2 class): Release is the producer of record
  and the D-COV stores were Release-written and Release = Release byte-identical (M2i C3: 12 / 12 files, equal roots,
  1 and N workers). Evidence on the read side: M2i C5 (the Release NAV on the Debug-built container reproduces the
  Debug-produced Y-1 reference outside `/producer`, 27 / 28 files byte-identical) and
  `MvAimRule.ContainerAndDirectoryStoresGiveTheSameBook` (container and directory stores give the same book). What
  remains build-dependent is consumer arithmetic (FP contraction, the M1c-RED class), covered by the gate's Release
  suites, not by the store.
- **`cov-diff --legacy` semantics** (`strategy_risk_verb.cpp:1283-1373`, `:1448-1460`): exit 0 = the container opens
  under its manifest's root pin with every block hash verified, its session axis equals `diagnostics.csv`, and every
  block scattered to the dense layout equals the legacy payloads of the same directory byte for byte (NaN payloads
  compared bitwise); the output names the files compared (two, or four when the exposure files exist). Exit 1 = the
  first difference (geometry, date count, sessions, or a payload at a session). Exit 2 = a usage error, an unreadable
  manifest, a directory without a container (M-2) or an I/O error. It does not check the legacy payloads against the
  manifest's `files` SHA-256 entries (S-7) and it lets JSON type / key errors escape (S-8).
- **No hot-path allocation in the forecast read:** the container read (`view`, `parse_cov_block`,
  `scatter_cov_block`) allocates nothing on success; `factor_risk_view` reuses the caller's scratch. The one
  per-read allocation is `RiskStore::read`'s, S-1 above (the f32 buffer and, on the container path, an unused
  `std::filesystem::path`): P10 by the PM.
- **`/W4 /WX`: holds.** 0 warning lines at COV's merge (p9-2b Debug), at COVR's (p9-2n Debug, p9-2o Release) and in
  the bench build (p9-2c Release); the risk sources are unchanged since `c8ddf736` except COV-MV's +11 test lines
  (`git diff c8ddf736 d51d011e`). 100 columns: no added C++ line over 100 (two README table rows only). Placement:
  `risk_cov_container_test.cpp` in `atx-engine/tests/risk/` (`atx-engine-risk-tests`); the `strategy_cov_*` tests in
  `atx-impl-strategy-target-tests`; COVR's diagnostic is `DISABLED_` and not a `*_test.cpp` (not globbed), as its
  CMake comment says.
- **No expected hash moved: holds.** The COV / COVR tests carry no SHA-256 literal (no 40+ hex digit literal in any of
  the five files); COVR found no test pinning container bytes under one CRT, so no M2c-RED-2 split was needed; the only
  later edit (COV-MV, COV-11 S10) ADDS the a = 2 over-correction control (a tighter test, not a moved band). The
  store of record `v8-risk-lo3-v10` was reproduced byte for byte at M2d with the flag absent. The deletions in
  `strategy_risk_model.cpp` / `strategy_risk_verb.cpp` are signature refactors (the bias harness and manifest builder
  re-plumbed for the recipe), none a pinned value.
- **Gtests (from logs; no build, no gtest run here):** M2i `gt-exits.txt` Debug p9-2n / Release p9-2o:
  `RiskCovContainer.*` 21 / 21 each; `CovTruncation.* .. CovDiff.*` 29 / 29 each; the risk / spo regression filter
  106 / 106 each; engine `Risk*` 431 + 1 skip (Debug), 430 + 2 skips (Release: the nightly battery and the death
  test `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read`); `CovStageDigest.*` 1 DISABLED; ENV-1 0 hits.
  COV-MV's later control ran in Debug only (p9-2y, `Cov*` in the 100 / 100): its Release run is the gate's.

### 11.4 Suggested (addendum)

| id | file:line | problem | fix |
|---|---|---|---|
| S-7 | `atx-impl/src/strategy_risk_verb.cpp:1283-1373` | `cov-diff --legacy` verifies the container against its root pin but not the legacy payloads (or `model.atxcov`) against the manifest's `files` SHA-256 entries, so its exit 0 says "self-consistent", not "these are the pinned bytes" | the gate pairs it with the payload SHA check (`m2i-logs/store-pins.txt` form); P10: check the `files` entries inside the verb |
| S-8 | `atx-impl/src/strategy_risk_verb.cpp:1298-1302`, `:1424-1480` | after the guarded `Json::parse`, `c.at("root_sha256").get<std::string>()`, `c.at("file")` and `m.at("geometry").at(...)` throw on a malformed manifest, and neither `dispatch_cov_diff` nor `main` (`atx-impl/tools/equity_strategy_risk.cpp`) catches: the process terminates instead of exiting 2. Fails loud (never a false exit 0) | one `try` around the verb (as `dispatch_risk_model` has), mapping to exit 2: P10 |

### 11.5 Tests and checks run (addendum)

No Python test of the range covers these files (the D-COV tools are sprint-dir scripts); none was run. Read-only
checks: SHA-256 of both D-COV stores' `manifest.json` and `model.atxcov` (= the four pins); their manifests'
`container` / `files` keys (= the pins; all four legacy payloads listed); `v8-risk-lo3-v10/manifest.json` has no
`container` key; the risk exe `bin/atx-equity-strategy-risk.exe` SHA-256 `a50bb015...` = its p9-2p receipt and current
by source for the risk verb (`git diff 327c86b2 d51d011e` empty over the risk sources), not run. Grep of the forecast
read path for CRT transcendentals: none. Mutation: none in the addendum (no Python file of these lanes in the range).

### 11.6 What root must do before any cell (addendum)

7. **M-2:** run `atx-equity-strategy-risk cov-diff --legacy` on `build-equity/p9-m2i-dcov-v11`,
   `build-equity/p9-m2i-dcov-covv1` and `build-equity/p9-m2d-cov-risk-container` (bounded, read-only), each expected
   exit 0 with four files compared, and the payload SHA-256 against each manifest's `files` (S-7); record what exit 0
   proves; cite M2d for `v8-risk-lo3-v10`.
8. The Release run of `Cov*` incl. COV-MV's a = 2 control, `RiskCovContainer.*` and `Risk*` on the gate's exes (step
   2); CRT-2's `ucrtbase.dll` record at step 0 (a CRT change voids P9-B0, not the stores).
9. PRE-MVX's prereg lines (B.7 "Parent", B.8, E) in round 2b-ii before any read (M-1, closed by that ruling).
