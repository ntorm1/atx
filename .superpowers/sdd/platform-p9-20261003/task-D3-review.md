# Task D3 review (first review; lane D3: guards and kinds, C3-13; CRITICAL PATH)

| | |
|---|---|
| Verdict | **APPROVE** (0 Required, 7 Suggested; S-1 asks for a PM ruling) |
| Reviewed | HEAD `699440bb` (pool-20, `feat/p9-d2-20261003`), BASE `685e238b` (REV-BASE: `git merge-base 699440bb feat/platform-p9-20261003`); code = `0c9f75d9` + root, only docs above it |
| Scope check | `git diff 685e238b 699440bb` outside the sprint dir touches exactly `scripts/wave_kinds.py`, `scripts/wave_stage_kinds.py`, `scripts/tests/test_wave_kinds.py`, `scripts/specs/p9/kinds/new-cost-model.json`, `scripts/specs/p9/kinds/walk-forward.json` (D3-2's four kind files). Sprint dir: the lane's own records only. The six root merges in range (`f2ff4954`, `899b5811`, `40154ac4`, `d080af09`, `aca78a14`, `3834bc0c`) are plain: `git show --cc` is empty for each |
| C3b | C3b's edits (line 18 `"pm6-6"` -> `"pm6-6-engine"` in eight fixtures; three `test_wave_kinds.py` hunks) are not in this tree, as ruled: not a finding |
| Rulings read | D3-1, D3-2, W3-8a, PRE-35, C3-3, C3-13, C3-13a, E2-8, E2-15, E2-17, W3-7, W3-8, KINDS-TW, D3-Q1, D3-Q2, D3-Q3, REV-BASE, TEST-1, TEETH-RESTORE, PERM-1 |
| Build / data | none; fake worlds only; no 2020-2023 statistic; nothing dated 2024-01-01 or later opened |

## 1. Spec verdict (`r7-dispatch.md` "D3" items 1-8 under D3-2; `r8-dispatch.md` "D3"; `r9-dispatch.md` "D3")

| Item | Verdict | Evidence (file:line at HEAD) |
|---|---|---|
| r7-1 merge root | done | merges `40154ac4`, `d080af09`, `aca78a14`, `3834bc0c`; all plain (above) |
| r7-2 C3-3 alias guard | done | aliases owned: `wave_kinds.py:580` (`table_flags`), `:586-595` (`flag_aliases`); fail closed on a malformed / shared / rule-owned alias or a non-constant x-flag: `:603-640`; exemption of the named row: `wave_stage_kinds.py:228-229`, `:337-341`; bare-switch compare: `:176-183`, `:230`; named row + alias (cell and ref side): `:277`, `:344-367`. Tests `test_wave_kinds.py:1221-1323`. Every route of the brief refused and teethed (section 3): raw alias in constants.flags / template change.flags / horizon.flags; abbreviation (`--cost`); dash / case; set routes adding / dropping / switching the spelling; inherited alias passes in three kinds; template removing it refused unless rules names `cost-sqrt-v2`; named row + alias refused (cell, ref); malformed field refused (9 shapes + 4 rule-owned flags) |
| r7-2 E2-17 `inputs.*` guard | done | `wave_kinds.py:94` (`KIND_INPUTS`); `wave_stage_kinds.py:370-383` (routes), `:386-402` (resolved difference, derived keys vs the parent's outputs), `:405-424` (refusal), called `:276`. Tests `:1328-1389`: constants.set `inputs.<k>` (the O2 probe), whole `inputs`, template change.set / change.unset / change.inputs, the parent's own value still a route, horizon and leverage refused, role wave: `change.inputs.role` allowed (`:1362`), another key refused (`:1344`) |
| r7-2 walk-forward kind out of SCHEMA_ONLY, D3-2 names | done | `SCHEMA_ONLY = ()` `wave_kinds.py:82`; flags `:311-319` derive exactly `--walk-forward expanding --walk-forward-lag 2 --walk-forward-step 21 --walk-forward-min-decisions 21` for the fixture (`specs/p9/kinds/walk-forward.json:28-33` names all four D3-1 constants); owned by the kind `:86-87`, `:581-582`; parent must fit `--engine-fit` without `--theme-tsmom` / `--theme-resid` / `--era` `:341-358`; outputs renamed (fit, card, w, NAV, monitor) `:327-338`. Names match D3W's fitter at `f0726acb` (`composition_engine_fit.py:171-182`) |
| D3-Q3 (no default in the kind; explicit value passed through) | holds | `wave_kinds.py:317-318` passes `min_decisions` only when the block names it; test `test_wave_kinds.py:1145-1148` pins both the pass-through and the absence of a default; teeth D1 (dropped) and D2 (a default 21 added) both caught |
| D3-Q1 (Python-fitted parent: P9-W refused, lapses) | holds | nothing in the code accepts a non-`--engine-fit` parent (`wave_kinds.py:350-353`); the refusal is what the ruling relies on |
| D3-Q2 (`fields.output` unguarded in P9; P10 guard + gate grep) | holds | no `fields.*` guard was added; a role wave's own `fields.output` set (`wave_stage_kinds.py:749-750`) is the register stage's |
| r7-3 W3-8a | done | `SCHEMA_ONLY_FIXTURES: tuple = ()` `test_wave_kinds.py:78`; `:215-216` asserts "10 runnable, 0 schema only" and `WK.SCHEMA_ONLY == ()`; cost model runs to spec on C3's rows `:1119-1130`; all ten fixtures pass preflight `:1108-1116`. The fixture's description only changed (`new-cost-model.json:5`); its template pin unmoved |
| r7-4 live-table test | done | `test_wave_kinds.py:1393-1453`, skipped unless `ATX_EQUITY_BIN` names a dir with both exes (my run: 1 skipped, reason printed) |
| r7-5 KINDS-TW | not done, optional | P10 note by the ruling; `scripts/specs/tiny.json` untouched |
| r7-6 suites | done / deferred by rule | `test_wave_kinds.py` and whole `scripts/tests` seeds 0 / 1 in the lane's logs (`d3-logs/r9-*.log`); stage chain deferred to the gate (TEST-1) |
| r7-7 C3b's room | done | no hunk in C3b's lines; read-only previews merge clean (section 5) |
| r7-8 report / handoff / For PRE | done | `task-D3-report.md` sections 1-8, "What root re-proves", "For PRE" (manifest shape; template path `<parent dir>/<parent stem>-wf2s21m21-<wave>.json`, generated, so no SHA-256 before the parent is fixed: the generator blobs are pinned instead, `d282586a` / `694b619e`, which match HEAD) |
| r8-0 DIRTY-1, r9-1 DIRTY-1 | done | `f8abdfea`, `c525c54c`, `cc10622f` |
| r8-3 C3-13 | done | `wave_kinds.py:96-98`, `:537-551` (`nav_rule_split`); spec stage `wave_stage_kinds.py:121-122`; guard base `:254-257`. Tests `test_wave_kinds.py:1487-1568`: (a) parent's own aim rule with another param (C3-5 probe shape) and (b) another aim rule (a second `--rule` row; a parent on another aim rule): one `--rule <id>` and one `--trade-fraction 0.1` on the cycle's NAV argv for the cell, the `-cal` recipe and the `-gm` record; preflight exits 0 (E2's raw-rule-flag guard accepts); `--rate` / `--target-rule` / `--cost-law` keep the append route; raw `--rule` in a map or set is still refused. `0c9f75d9` edits no fixture. The parent's `nav.rule` is read at `scripts/cycle/phases.py:829` (not edited) |
| r9-3 teeth for C3-13 on the merged head | done | `d3-logs/r9-c3-13-teeth.log` (5 / 5, TEETH-RESTORE checks); re-run by me with my own (a) / (b) mutations (section 3) |

No item is not done. TEST-1's deferrals (stage chain, whole suites) are the gate's.

## 2. Findings

| # | path:line | Severity | Problem | Fix |
|---|---|---|---|---|
| S-1 | `scripts/wave_stage_kinds.py:203-294` (`rule_flag_guard`); `scripts/wave_kinds.py:537-551` | Suggested (PM ruling) | A raw `nav.rule` route is unguarded in every v2 kind: `rule_cell.constants.set nav.rule`, a template's `change.set nav.rule`, or a whole `nav` section switches the aim rule (the `--rule` token the cycle puts on the NAV argv, `phases.py:829`) with no rule named. My probes on fakes (`NAV_C3_13`): a rule wave naming `per-name-v1` + `constants.set nav.rule aim-partial-v6` -> exit 0, cell `nav.rule aim-partial-v6`, argv `--rule aim-partial-v6`; the same in a horizon wave, a role wave (template change.set) and a leverage wave (all preflight exit 0). Pre-existing: on an export of BASE `685e238b` the horizon and nav-rule fixtures with that set also run to spec. C3-13 made `nav.rule` the registered route of an aim rule; E2-8's guard compares only flag lists, so an unnamed aim rule passes, the class of hole E2-8 / E2-10 / C3-3 close. Not in D3's spec text and not introduced by the lane, hence not Required. (A template set of `nav.rule` beside a named aim row is overridden by the derived value, as it should be.) | PM: either (a) a small fix, the twin of `leverage_refusal`: refuse any raw set / unset key `nav.rule` (and a `nav` section set that changes it) in every kind, and a resolved cell whose `nav.rule` differs from the parent's unless `nav_rule_split` set it; tests + teeth (one Python round, D3's files), or (b) P10 for the guard and, for P9, a gate grep over `scripts/specs/p9/` manifests and templates for `nav.rule` in `constants.set` / `change.set` / `change.unset` (as D3-Q2). No registered P9 cell names a `--rule` row (C3-13) |
| S-2 | `scripts/wave_kinds.py:86` vs `:315-318` | Suggested | The four fitter flag names are written twice: the `WALK_FORWARD_FLAGS` tuple (what the kind owns) and literals in `walk_forward_flags` (what it derives). The test pins them equal (my teeth D4 caught a drift), but one definition is the owner's bar | Build `ops` from `WALK_FORWARD_FLAGS` (e.g. `dict(zip(WALK_FORWARD_FLAGS, values))`, dropping the last when `min_decisions` is absent) |
| S-3 | `scripts/wave_kinds.py:88-90`, `:300-308`, `:341-358` | Suggested | The kind's walk-forward preconditions (window set, lag >= 2, step >= 1, min_decisions >= 2, `--engine-fit`, refused `--theme-tsmom` / `--theme-resid` / `--era`) restate D3W's `check_walk_forward` (`composition_engine_fit.py:202-219` at `f0726acb`). Failing early at preflight is right (no register before refusal); not a C++ rule mirror; but two Python copies can drift | Root's D3W-merge check (report "What root re-proves" item 4) compares the refusal set and the bounds too, not only the four names; P10: one shared constant set |
| S-4 | `scripts/wave_kinds.py:183`, `:188`; `scripts/wave_stage_kinds.py:78-79`, `:137`, `:239-240`, `:528`, `:536-537` | Suggested | With `SCHEMA_ONLY = ()` these branches are unreachable and their texts ("schema only until lane D3 wires it") stale | Keep as a mechanism with a neutral text, or remove in P10 |
| S-5 | `scripts/wave_kinds.py:609-616` vs `:562-583` | Suggested | `alias_problems` rebuilds "the flags a rule owns" (rows' x-flags, params' x-flags, composition NAV flags) in parallel to `table_flags`: two derivations of the owned set | One helper (the table's owned flags without aliases) used by both |
| S-6 | `scripts/wave_kinds.py:544-551`; `scripts/tests/test_wave_kinds.py:1436-1437` | Suggested | `nav_rule_split` keys on a derived `--rule` entry, not on the row's x-flag: a NAV row whose param x-flag were `--rule` would be read as `nav.rule`. No such row exists; the live-table test checks every `--rule` row is `--rule <id>` but not that no param x-flag is `--rule` | Fail closed: refuse a table whose param x-flag is `--rule` (beside `alias_problems`), or assert it in the live-table test |
| S-7 | `.superpowers/.../d3-logs/r8-s4-seed0.log`; `d3-logs/teeth.py:120-141` | Suggested | Records: a 1.3 MB VOID (ENV-1) log is committed (`cc10622f`); part A's `teeth.py` asserts porcelain only, not TEETH-RESTORE's SHA / blob check (it predates the ruling) | None needed now (no history rewrite); my re-run of all 22 part-A mutations under the full TEETH-RESTORE checks (section 3) is the evidence of record |

## 3. My own runs (TEST-1: the lane's pytest file only; seed 0; no build)

Interpreter `C:/Program Files/Python312/python.exe`, `-p no:cacheprovider`, own `--basetemp` (parent created first),
`PYTHONDONTWRITEBYTECODE=1`, `ATX_EQUITY_BIN` unset, HEAD `699440bb`, tree clean before and after every run.

| Run | Result |
|---|---|
| `scripts/tests/test_wave_kinds.py`, PYTHONHASHSEED 0 | **192 passed / 0 failed / 1 skipped** (163 s; the skip: `test_wave_kinds.py:1408`, "ATX_EQUITY_BIN does not name a bin dir with both exes"); = the lane's `r9-wave-kinds.log` |
| the same, `-k c3_13`, `-rA` | **10 passed** (nav_rule_split; (a), (b) x 2; rate / target-rule / cost-law; raw `--rule` x 3) |
| live-table test on root's Debug exes | **not run**: skipped per the dispatch rule, because `C:/atx-wt/pool-2/build-equity/mega-p9-2t-receipt.json` does not name the IC / NAV exes (it lists `atx-research-eval` and four test exes). The two exes present hash to `919569fb...` / `637f7069...`, the values `mega-p9-2p-receipt.json` names and the lane's `r9-live-build-equity.log` used |
| merge preview, root `73b4acfd` (B2 + T2) + D3 (read-only: `git archive` / `git show` into my scratch; root's one-line `F.pin_eval_verb(root)` hunk applied) | `test_wave_kinds.py` **192 passed / 1 skipped** |
| merge preview, root `73b4acfd` + D3 + C3b `76b9f256`, three-way per file with `git merge-file` on scratch copies | every file clean, no conflict marker; `test_wave_kinds.py` **191 passed / 1 skipped** (one fewer: C3b drops the `pm6-6` param of the match-stage test, C2-11) |
| nav.rule probes (S-1), fake worlds | 9 probes on HEAD (4 unguarded routes exit 0; two aim rows named -> refused, `rules: nav --rule set to ...`; aim + rate rows together -> one `--rule`; aim row without params -> sets `nav.rule`; whole `nav` set beside a named aim row -> derived wins); 2 probes on a BASE export (unguarded there too) |

Teeth (TEETH-RESTORE): my tool mutates only `scripts/wave_kinds.py` / `scripts/wave_stage_kinds.py` (never a test),
keeps the bytes, restores in `finally`, then asserts SHA-256 = pre-mutation, `git hash-object` = HEAD's blob
(`d282586a` / `694b619e`) and `git status --porcelain` empty. Control (union of all selections, unmutated): 0 failed
/ 83 passed. **52 / 52 caught, every restore verified**; tree clean, both blobs = HEAD after the run.

- The lane's 27 re-run under the full checks (part A's 22 from `teeth.py`, C3-13's N1-N5 from `c3-13-teeth.py`),
  failed counts: M1 10, M2 1, M3 8, M4 4, M5 3, M6 1, M7 2, M8 1, M9 1, M10 1, M11 9, M12 7, M13 1, M14 2, M15 1,
  M16 3, M17 4, M18 5, M19 9, M20 1, M21 3, M22 1; N1 5, N2 4, N3 3, N4 3, N5 4.
- My own 25 (independent of the lane's), per guard route:

| id | route the mutation opens | failed |
|---|---|---|
| A1 | C3-3: raw alias in an option map (every alias exempt in the map check) | 6 |
| A2 | C3-3: abbreviation of an alias (aliases ignored for prefixes) | 1 |
| A3 | C3-3: inherited alias (no bare-switch compare) | 1 |
| A4 | C3-3: set / template routes adding, dropping, switching the alias (resolved compare skips aliases) | 4 |
| A5 | C3-3: removal allowed only when rules names the row (exemption emptied) | 1 |
| A6 | C3-3: named row + its alias (never fires) | 2 |
| A7 | C3-3: malformed alias, another case accepted | 1 |
| A8 | C3-3: malformed alias, duplicates accepted | 1 |
| B1 | E2-17: `rule_cell.constants.set inputs.<k>` route not recorded | 4 |
| B2 | E2-17: a whole `inputs` section | 1 |
| B3 | E2-17: template `change.inputs.<k>` | 2 |
| B4 | E2-17: template `change.unset inputs.<k>` | 1 |
| B5 | E2-17: a role wave also allowed `inputs.library` | 1 |
| B6 | E2-17: an ordinary-key resolved difference | 1 |
| B7 | E2-17: a derived-key resolved difference (reference_cell vs the parent's NAV) | 1 |
| C1 | C3-13 (b) broken: split only when the named id is the parent's own rule | 2 (the two (b) params; (a) passes) |
| C2 | C3-13 (a) broken: split only when the named id differs from the parent's | 1 (the (a) param; (b) passes) |
| C3 | C3-13: `nav.rule` set but `--rule` also left in `nav.flags` (duplicate) | 5 |
| C4 | C3-13: other row kinds lose the append route | 3 |
| D1 | D3-Q3: explicit `min_decisions` not passed through | 1 |
| D2 | D3-Q3: the kind defaults `min_decisions` to 21 | 1 |
| D3 | D3-2: the lag flag carries the step | 1 |
| D4 | D3-2: a flag name drifts in `walk_forward_flags` | 1 |
| D5 | walk-forward: the `m<min>` token dropped from the renames | 1 |
| E1 | W3-8a: a cost-law row refused (cost-model fixture not runnable) | 2 |

C1 / C2 show (a) and (b) are pinned independently. C++ claims: none (no C++ in scope).

## 4. Quality verdict

- One code path: the preflight guard and the spec stage both derive the cell through `kind_constants`
  (`wave_stage_kinds.py:116-129`) and the same `WK` functions; `nav_rule_split` is the single C3-13 split (spec stage
  and the guard's base, `:122`, `:254`); the spec stage re-reads only preflight's receipt (`:733`), the sole consumer
  of the derived flags in `scripts/`. Duplications are small and pinned by tests (S-2, S-3, S-5).
- No Python mirror of an engine rule: the kind code derives argv and refuses routes; it computes no numerical rule.
  The walk-forward preconditions restate the Python fitter's argv checks (S-3), not a C++ rule.
- Fail closed: an unreadable alias field, a resolved difference with no route, an unresolvable cell, an unreadable
  `parent_references` (derived keys then count as changed, `:393-401`), a parent without `fit.flags` (walk-forward
  refused): all refuse. The open route found (S-1) is outside the two guards' remit and pre-existing.
- Flag-absent identity: a v1 manifest never reaches the changed code; for other rows `nav_rule_split` returns the
  derived map unchanged (`:545-546`), so no other kind's cell or receipt moves; new receipt keys only for kind
  walk-forward. 120-column limit kept (the one longer line, `test_wave_kinds.py:993`, is at BASE).
- Tests pin the contract (D3-2 names and D3-1 values as literals; the cycle's own `nav_step` argv; the receipts), not
  the implementation.

## 5. What root must check at merge

1. Ruling C3-13 / C3-5: the probe on a fresh tiny world with the real exes and the MERGED kinds, for (a) the parent's
   own aim rule with other params (`aim-partial-v5`, `trade_fraction` .1) and (b) another aim rule: exactly one
   `--rule` on the NAV argv and the NAV exe accepts; again with `--rate info-clock-rate-v1` at AL-CLOCK's merge.
2. Keep-both (my previews merge clean, no conflict marker): C3b's eight fixture lines (line 18 `"gross_match":
   "pm6-6"` -> `"pm6-6-engine"` in new-combination-rule, new-cost-model, new-field, new-horizon, new-signal,
   new-universe, replaced-signal, walk-forward; leverage-change and new-nav-rule stay `"none"`) and C3b's three
   `test_wave_kinds.py` hunks (`record_spec` in the leverage match test; `parametrize("mode", ["pm6-6-engine"])`;
   the `pm6-6` branch removed), with B2's `F.pin_eval_verb(root)` line in `world()` (root `:171`, merged `:187`).
3. The G-P2 tick for the cost-model kind (W3-8a) on the merged tree: `test_the_ten_fixtures_load_and_cover_the_kinds`
   ("10 runnable, 0 schema only"), `test_the_cost_model_fixture_runs_to_spec_on_c3s_rows`,
   `test_every_fixture_runs_preflight_on_c3s_rows`.
4. `scripts/tests/test_wave_kinds.py` and `scripts/tests/test_wave_eval_verb.py` on the merged tree (my three-way
   preview ran only the first: 191 passed / 1 skipped).
5. The live-table test on both builds (`ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, then
   `.../build-equity-rel/bin`, `-k live_rule_tables`) with exe hashes named by a receipt (EXE-CUR / SPDLOG-RELINK);
   re-run after AL-CLOCK / COV-MV append rows.
6. At D3W's merge: `WK.WALK_FORWARD_FLAGS` against the fitter's four names (they match at `f0726acb`) and S-3's
   refusal set and bounds.
7. At the gate (TEST-1): whole `scripts/tests` seeds 0 and 1, `atx-engine/tools/test_stage_chain.py`; the D3-Q2 grep
   for `fields.output`; and S-1's `nav.rule` grep if the PM rules (b).

Permission refusals: none.
