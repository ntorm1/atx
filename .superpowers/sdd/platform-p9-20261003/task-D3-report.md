# Task D3 report

## Part A: guards and kinds (lane D3 after ruling D3-2; dispatch R7-1)

| | |
|---|---|
| Outcome | DONE: green at `d080af09` (owner-stop run) and again at resume 8; the C3-13 fix of resume 8 is the section "C3-13" below |
| Pool / branch | `C:/atx-wt/pool-20`, `feat/p9-d2-20261003` (D2's branch; no switch, no history rewrite; WIP `0389e868` kept) |
| Root merged | `a15ad887` (merge `40154ac4`, no conflict); `1b64ce45` (merge `d080af09`, no conflict) |
| Commits | `40154ac4` merge; `e531257b` code + tests (finishes the WIP); `d080af09` merge; resume 8: see "C3-13" |
| Rulings | D3-1, D3-2, W3-8a, PRE-35, C3-3, E2-17, E2-8, E2-15, P14, KINDS-TW, W3-7, W3-8 |
| Files | `scripts/wave_kinds.py`, `scripts/wave_stage_kinds.py`, `scripts/tests/test_wave_kinds.py`, `scripts/specs/p9/kinds/walk-forward.json`, `scripts/specs/p9/kinds/new-cost-model.json` (the four kind files of D3-2 only) |
| Build | none (Python only); no configure, no compile |
| Data | synthetic fake worlds only; the live-table test ran root's exes' `--list-rules --json` (no data read) |

Line numbers below are at `e531257b` (unchanged by the later commits, which touch no source line).

### 1. C3-3: the alias guard (`x-flag-aliases`)

An alias (C3's `cost-sqrt-v2` lists `--cost-v2`) is the owned flag it stands for: in a v2 wave it appears only as
inherited unchanged from the lineage parent; adding, removing or changing it needs a `rules:` entry naming the row.

| Check | file:line | Test (scripts/tests/test_wave_kinds.py) | Teeth |
|---|---|---|---|
| aliases are owned NAV flags (E2-8 reads them: raw maps, resolved lists, ref side) | `wave_kinds.py:557` (`table_flags`), `:563` (`flag_aliases`) | `test_c3_3_an_alias_is_owned_as_its_rows_flag` (12 probes, `:1217-1255`) | M1 |
| malformed field (not a non-empty list of distinct lowercase long options) | `wave_kinds.py:598-602` | `test_c3_3_a_malformed_alias_field_stops_the_wave` (`:1276-1301`) | M3 |
| alias on a row whose x-flag is no constant `--flag VALUE` pair (new, fail closed) | `wave_kinds.py:603-608` | `test_c3_3_an_alias_stands_for_a_constant_pair` (`:1311`) | M5 |
| two rows sharing an alias | `wave_kinds.py:610` | `test_c3_3_two_rows_may_not_share_an_alias` (`:1304`) | M6 |
| an alias that is a rule's own flag (new, fail closed: naming the row would exempt another rule's flag) | `wave_kinds.py:613` | malformed-field test, 4 params | M4 |
| the guard stops the wave on any of these | `wave_stage_kinds.py:223` | malformed-field test, end-to-end param | M2 |
| the named row may add / drop its own alias (exempt) | `wave_stage_kinds.py:226-227`, `:334` (`alias_exempt`); `wave_kinds.py:655` (`raw_rule_flags(..., exempt)`) | `test_c3_3_an_inherited_alias_passes_and_the_named_row_drops_it` (`:1259`) | M10 |
| an alias is compared as a bare switch, not by argparse values (fixes a WIP false positive: an inherited `--cost-v2` followed by a flag the wave appends was refused) | `wave_stage_kinds.py:174` (`reads`), `:228`, `:259`, `:265` | same test (horizon wave appends `--cadence 5` after the inherited alias) | M9 |
| named row + its alias left on the list ("one of the two, never both"), cell side and ref side (ref side new) | `wave_stage_kinds.py:274`, `:341-364` | C3-3 probes 11 and 12 | M7, M8 |

Routes closed: `rule_cell.constants.flags`, the template's `change.flags`, `horizon.flags` naming the alias in any
spelling (exact, `=value`, another case, a single dash, an abbreviation such as `--cost`, which abbreviates
`--cost-law` and `--cost-v2`); `constants.set` / the template's `change.set` / `change.unset` of `nav.flags` or the whole
`nav` section adding, dropping or switching the spelling (`--cost-law cost-sqrt-v2` -> `--cost-v2`); the template's
own `ref` block carrying the alias beside the derived x-flag; a table whose alias field the guard cannot read.

### 2. E2-17: the `inputs.*` guard

| Check | file:line | Test | Teeth |
|---|---|---|---|
| the kinds and keys that may change inputs: `field` (), `role` (`role`), `horizon` () | `wave_kinds.py:91` (`KIND_INPUTS`) | `test_e2_17_a_role_wave_changes_inputs_role` (`:1358`) | M14 |
| every raw route that sets / unsets / replaces an `inputs.*` key, named with key and route | `wave_stage_kinds.py:367` (`inputs_routes`) | `test_e2_17_inputs_only_by_the_kinds_that_name_them` (9 probes, `:1324-1356`) | M12 |
| a resolved difference no raw route names ("the resolved cell"; derived keys against the parent's own outputs) | `wave_stage_kinds.py:383` (`inputs_changed`) | `test_e2_17_a_resolved_difference_without_a_route_is_refused` (`:1367`) | M13 |
| the refusal, called by the E2-8 guard for every kind with a rule cell | `wave_stage_kinds.py:273`, `:402` (`inputs_refusal`) | the 9 probes | M11 |

Routes closed: `rule_cell.constants.set inputs.<k>` (E2 re-review 4's O2 probe, `inputs.reference_combined`),
`constants.set inputs` (whole section), the template's `change.set inputs.<k>[.<x>]`, `change.unset inputs.<k>`,
`change.inputs.<k>`; setting a key to the parent's own value is still a route (refused); in kinds rule, horizon,
leverage (with a rule cell), and role for any key but `role`. Library kinds (add, replace, field) have no rule-cell
route (add-alpha copies the parent); kind walk-forward has none either (no rule_cell, the template is generated).

### 3. Kind walk-forward out of SCHEMA_ONLY (P14, PRE-35; D3-1, D3-2)

| Check / wiring | file:line | Test | Teeth |
|---|---|---|---|
| `SCHEMA_ONLY = ()` | `wave_kinds.py:79` | `test_walk_forward_runs_to_spec` (`:1134`), `test_the_ten_fixtures_load_and_cover_the_kinds` | M21 |
| the fitter flags, D3-2's names exactly: `--walk-forward expanding --walk-forward-lag N --walk-forward-step N [--walk-forward-min-decisions N]` | `wave_kinds.py:83` (`WALK_FORWARD_FLAGS`), `:305` (`walk_forward_flags`) | `test_walk_forward_runs_to_spec` asserts the tuple, the dict and the resolved `fit.flags` | - |
| the flags owned by the kind (E2-8): no other kind reaches them by any route or spelling | `wave_kinds.py:84` (`KIND_FLAGS`), `:558` | `test_walk_forward_flags_are_the_kinds_own` (5 probes, `:1193-1213`) | M18 |
| the block: lag >= 2, step > 0, min_decisions >= 2, integers, window expanding, no other key | `wave_kinds.py:294` | `test_walk_forward_block_refusals` (9 params, `:1184`) | M19, M20 |
| the parent must fit under `--engine-fit` (ALCOMB-7) | `wave_kinds.py:344`; called `wave_stage_kinds.py:91` | `test_walk_forward_refuses_a_parent_it_cannot_refit` (`:1171`) | M15, M17 |
| a parent fitted with `--theme-tsmom`, `--theme-resid` or `--era` refused | `wave_kinds.py:348` | same test, 3 params | M16 |
| every output downstream of the fit renamed `-wf<lag>s<step>[m<min>]`: fit, card, w, NAV, monitor | `wave_kinds.py:321` (`WALK_FORWARD_OUTPUTS`), `:325` | `test_walk_forward_runs_to_spec` | M22 |
| the template next to the parent spec, the spec stage's constants, the plan line | `wave_kinds.py:355`; `wave_stage_kinds.py:89-93`, `:125`, `:543`, `:712` | `test_walk_forward_runs_to_spec`, `test_every_fixture_plans_in_a_fake_world[walk-forward.json]` | - |

Fixture `scripts/specs/p9/kinds/walk-forward.json` now names every registered constant (D3-1: window expanding, lag 2,
step 21, min_decisions 21), so the cell's argv does not rest on a fitter default.

### 4. W3-8a: `new-cost-model.json` flipped

`SCHEMA_ONLY_FIXTURES = ()` (`test_wave_kinds.py:74`); `test_the_ten_fixtures_load_and_cover_the_kinds` states
"10 runnable, 0 schema only". `NAV_C3` (`:1035-1062`) = E2's synthetic NAV table plus lane C3's trade-rate and cost-law
rows in `nav_rules.cpp`'s shape (x-flag null rows, `--rule`, `--rate`, their param x-flags, `cost-sqrt-v2` with
`--cost-v2`); the live-table test checks those rows against the exe. `test_the_cost_model_fixture_runs_to_spec_on_c3s_rows`
(the derived `--cost-law cost-sqrt-v2` reaches the cell) and `test_every_fixture_runs_preflight_on_c3s_rows` (all ten
fixtures pass preflight; walk-forward on an `--engine-fit` parent). On E2's table without the row the name is still
refused (`test_rule_wave_refuses_a_name_no_table_lists`, kept). The fixture's "SCHEMA ONLY" description is replaced;
its template `rule-cost.json` is pinned by the fixture's `template_sha256` and was not edited (no pin moves).

### 5. Handoff row 3c: the live-table check as a test

`test_the_live_rule_tables` (`:1405`), skipped unless `ATX_EQUITY_BIN` names a bin dir holding
`atx-equity-strategy-ic.exe` and `atx-equity-strategy-targets.exe`. Ports root M2h's `e2-live-table-check.py` (IC
listing == the committed `composition_rules_list.json`, bytes and JSON; every IC rule mapped; MR-4 ids; fixed-v1
[1, 2]; new-nav-rule's flags) and adds: `alias_problems(live) == []`, `flag_aliases(live) == {"--cost-v2":
"cost-sqrt-v2"}`, new-cost-model derives `--cost-law cost-sqrt-v2`, `NAV_C3`'s rows equal the live rows (kind, x-flag,
aliases, param x-flags), and all ten fixtures in the fake world served the LIVE tables (cost-model, combination-rule
and walk-forward to spec). Ran at `d080af09` on root's bins (Debug `build-equity/bin`, Release `build-equity-rel/bin`,
exe SHA-256 in the logs): 1 passed each (`d3-logs/live-build-equity.log`, `d3-logs/live-build-equity-rel.log`); again
at resume 8 with the C3-13 check (`d3-logs/r8-live-build-equity*.log`).

### 6. Teeth

`d3-logs/teeth.py` (in this dir): each of 22 mutations removes one check in the working tree, runs the tests that pin
it, restores the bytes in `finally` and asserts `git status --porcelain` is empty; M0 is the unmutated control of every
selection. Log `d3-logs/teeth.log`. At `e531257b`: 22 / 22 caught, tree clean after every revert.

### 7. Suites

@@SUITES@@

### Optional, not done

- KINDS-TW (real-exe kinds canary as a new fixture): not done (optional; add no scope). Per the ruling it is a P10 note;
  root's M2h probe stays the P9 evidence. `scripts/specs/tiny.json` untouched.
- E2 re-review 4 S1 (route message for a match flag in a NAV param value): optional, not done.

### Deviations from the WIP design (handoff rows 2, 3a, 3b)

1. Walk-forward renames card and monitor too (the WIP renamed fit, w, NAV): they read the fit's dir (`card_step`
   reads `<fit>/admission.json`, `monitor_step` the fit's admission and the card's sleeve); research_spec's convention
   renames all five on a composition change, so nothing made from the parent's fit is silently reused.
2. `alias_problems` gains two fail-closed checks (an alias that is a rule's own flag; an alias on a row whose x-flag is
   no constant pair). Neither fires on the live table.
3. `reads`: the WIP compared an alias by argparse values and refused an inherited `--cost-v2` whenever the wave
   appended a flag after it (found by the new test); an alias is now compared as a bare switch.
4. `alias_beside_rule` also reads the template's own ref block (E2-15 (a)).
5. The E2-17 message names the keys the kind may change ("a kind role wave may set or change inputs.role").
6. Slip: the first merge commit `40154ac4` carries git's default message without the trailer; not amended (no history
   rewrite).

### New literals, flags, keys, rule ids

- No new `atx.<name>/v<n>` literal; no rule id; no CMake edit; no cross-lane edit.
- Fitter flags derived by the kind (defined by D3W, names fixed by D3-2): `--walk-forward`, `--walk-forward-lag`,
  `--walk-forward-step`, `--walk-forward-min-decisions`.
- Preflight receipt (`01-preflight.json` `outputs.kind`) of a walk-forward wave: `walk_forward` (the block) and `flags`
  (`{"fit": {...}}`). Manifest block `walk_forward {window?, lag, step, min_decisions?}` is E2's, unchanged.
- Python names: `WK.KIND_FLAGS`, `WK.KIND_INPUTS`, `WK.ALIASES_KEY`, `WK.WALK_FORWARD_*`, `SK.reads`,
  `SK.alias_exempt`, `SK.alias_beside_rule`, `SK.inputs_routes`, `SK.inputs_changed`, `SK.inputs_refusal`.

### Flag-absent identity (for root)

No flag is involved. A v1 manifest never reaches the changed code (`kind` is None): pinned by
`test_v1_manifests_keep_their_validation` and `test_verify_plan_line_states_the_rule_cell_refusal` (v1 plan bytes).
For v2 kinds the change only adds refusals (owned aliases, `KIND_FLAGS`, `inputs.*`) and the walk-forward wiring; no
receipt key of another kind moves. No exe, golden or pin changes; nothing to substitute.

### What root re-proves at merge

1. `scripts/tests` under PYTHONHASHSEED 0 and 1, and `atx-engine/tools/test_stage_chain.py`, separate invocations.
2. The live-table test on real exes, both builds:
   `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin` and `.../build-equity-rel/bin`, then
   `python -m pytest -q -p no:cacheprovider --basetemp <own> scripts/tests/test_wave_kinds.py -k live_rule_tables`
   (1 passed each; it ran here on p9-2p / p9-2q, see section 5). After AL-CLOCK / COV-MV append rows, re-run it.
3. Lane C3b edits the same files (one `gross_match` line per kind fixture, line 18; three hunks in
   `test_wave_kinds.py` around `test_the_match_stage_still_writes_its_own_leverage` and the leverage test's `-gm`
   read). This lane left those lines untouched; my fixture hunks are the description (line 5) and the
   `walk_forward` block; a textual conflict is not expected.
4. At D3W's merge: `WK.WALK_FORWARD_FLAGS` against D3W's `composition_engine_fit.add_argument` (the four names); the
   walk-forward fixture's derived argv parses under the fitter (`allow_abbrev=False`). D3W branched at `899b5811`
   (before the WIP); it does not edit the kind files.

### For PRE (prereg B.6, P9-W; Appendix "Open for the PM" item 18)

- **Manifest shape** (`atx.research-wave/v2`, kind `walk-forward`; no `rule_cell`, no `rules`, no `candidates`):
  `{"schema": "atx.research-wave/v2", "wave": "p9w-walk-forward", "kind": "walk-forward", "parent": {"spec": <P9-F0h's
  cell of record>, "library": <its library>}, "fields", "acceptance", "gross_match": "pm6-6-engine", "budget",
  "ledger", "expect", "out_dir", "exes" (B2-10), "walk_forward": {"window": "expanding", "lag": 2, "step": 21,
  "min_decisions": 21}}`; name all four constants (D3-1).
- **Fit mode**: the parent must fit under `--engine-fit` without `--theme-tsmom`, `--theme-resid`, `--era`
  (preflight refuses otherwise); the cell's fit = the parent's fit flags plus `--walk-forward expanding
  --walk-forward-lag 2 --walk-forward-step 21 --walk-forward-min-decisions 21`.
- **Per-date weights producer**: the C++ composition exe's walk-forward fit through the fitter's flags and post-step
  (lane D3W), weights schema `atx.dsl-composition-weights/v3`.
- **Template**: none is committed, by design (handoff "Design decided"): the spec stage writes the cell
  `<parent spec dir>/<parent stem>-wf2s21m21-p9w-walk-forward.json` = parent + `change.set` (fit, card, w, NAV,
  monitor outputs renamed `-wf2s21m21`) + `change.flags.fit` (the four flags), generated by
  `wave_kinds.walk_forward_template` / `walk_forward_set` / `walk_forward_flags`. Its SHA-256 exists only once the
  parent is fixed; the spec stage records the cell's `spec_digest` (`04-spec.json`) and verify re-checks it. Pin
  instead: the manifest's SHA-256 and the generator @@SHAS@@.

### Open questions

1. P9-W needs an `--engine-fit` parent (ALCOMB-7); if P9-F0h is fitted in Python at the wave-3 P9-B0, P9-W is refused
   at preflight: PRE / root must fix P9-F0h's fit mode or P9-W lapses.
2. E2-17 covers `inputs.*` only: a rule cell's `constants.set fields.output` (another fields dir) is not in its text;
   guard it too (P10?) or leave it.
3. min_decisions omitted in a manifest leaves the fitter's default in force (D3W's); the kind does not default it to 21.
