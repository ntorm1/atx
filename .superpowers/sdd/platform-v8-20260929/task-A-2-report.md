# Task A-2 report: `add-alpha` and `run --screen` (lane A, pool-11)

Branch `feat/platform-v8-a-20260929`, on top of A-1 (`ea7cba01`). Python only; nothing built or run on data.

## What was built

- `scripts/research_add_alpha.py` (new, about 280 lines; `research_cycle.py add-alpha ...` dispatches to it), plus four
  small additions to `generate_library.py` (`register_alpha`, `child_library`, `parent_exceptions`, `rename`, and the
  `budget_ids` argument of `validate_plan`) and `Cycle(..., verify=False)` in research_cycle.py (output names without
  pins, so add-alpha can read a parent spec's outputs).
- The `--screen` mode, the `marginal` phase and `cycle_verdict.json` landed with A-3 (`fa61c67f`: `Cycle.screen`,
  `marginal_step`, `exe_capabilities`, `scripts/cycle_verdict.py`); this commit adds their tests.

`research_cycle.py add-alpha --id X --dsl "..." --theme T --tier B --prior-sign 1 --citation "..." --origin prior
--parent v71 [--name v72] [--parent-spec SPEC] [--plan-json PATH] [--prior-sign-source S] [--form F] [--formula F]
[--domain D] [--deviation D] [--root R]`:

1. registers X in `atx-impl/strategies/alphas/registry.json` (`added_in` = the new library name; an identical
   registration of an existing id is reused; another definition of an id, a DSL already registered under another id,
   an unknown theme or tier, an origin outside {prior, grid, mined} are refused);
2. `libraries/NAME.json` = parent members + X (NAME defaults to the parent's trailing number + 1; a second add-alpha
   into the same NAME appends, so one wave is several calls); budget exceptions inherited from the parent library file,
   or from a legacy parent's recipe (v7.0: q5_eg, qmj_safety); the IC library and slim recipe are generated;
3. validates through K1: the parent spec's IC exe runs `--plan-only` on the new library (a temp copy), the parent's
   role and fields (metadata only), or `--plan-json PATH` gives a saved plan; every member needs its row, the new
   members must fit the house budget (inherited members are not re-budgeted: they were admitted under their own rules);
4. writes `libraries/NAME.prereg.md` (a stub with the members' DSL, theme, tier, prior sign, origin, citation and notes,
   and a `Ruling:` line for root; regenerated on every add into NAME);
5. derives `scripts/specs/v8/lib-NAME.json` from the parent's spec (`--parent-spec`, else `specs/v8/lib-PARENT.json`,
   else `specs/PARENT.json`) by the name template `rename(text, PARENT, NAME)` (a token, not inside a longer name or
   number; an output name without it is refused): the parent's cycle outputs (lowest complete u and w attempts, fit,
   NAV cell, its S2 CSV) become `reference_*` inputs; the parent's fields dir is pinned as built; `ic.cache` and
   `fit.work_dir` are omitted (derived from the role, A-3); no static check (K1 replaced it); the standard identity
   compares (parent orientations and daily-IC member rows after u; the S2 CSV after ref, and ref is skipped when the
   fields equal the parent's); gate `p1-NAME` on the new members (require any); `marginal` on `reference_combined`
   with `--themes`; `receipts: every-phase`, `verdict: true`, `summ.dsr_n: "ledger+1"` with `cells_from_ledger`; the
   OD-2 caps written into `runner.phases` when the role has more than 1,200 dates;
6. locks the spec (a missing input leaves it unlocked, exit 3, `lock --write` later).

Nothing is written when any of 1-5 fails (exit 2). A library whose spec already has a u attempt or a fit output is
never extended (exit 2: use a new --name).

`research_cycle.py run SPEC --screen`: fields, check, u (with `--no-composition` and without `--save-combined` when the
exe's `--help` offers the flag; "new members only" comes from the content-keyed cache: the parent's members are cache
and IC-result hits and, under B-1, are not loaded), u-compare, fit, card, marginal (the exe's `marginal` verb when its
`--help` names it, else skipped with a note), gate (prints the admission rows; a failed gate still writes the verdict);
ref, w, nav, monitor and summ are marked skipped. A later `run` resumes from the screen's outputs. Every run writes
`<out_root or build-equity>/cycle-NAME[-suffix]/cycle_verdict.json` (schema `atx.cycle-verdict/v1`): `{schema, cycle,
mode, spec_sha256, admission[], marginal[], (marginal_note), phases[{name, seconds, peak_mib}]}`, plus
`paired{dsr, se, cbb_ci, lw_p}`, `dsr{n, cell_count, effective_n}` and `pbo` after a full run of a spec with
`"verdict": true` (the summ step then passes `--json <cycle dir>/summ.json` and, with `--pbo`, `--pbo-json <cycle
dir>/pbo.json` to nav_summ; the paired row is this cycle's NAV dir's).

## How root verifies

```bash
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle.py atx-impl/strategies/test_generate_library.py
```

Result: 96 passed, 3 skipped (87 + 9; the three skips are the RESEARCH_CYCLE_LIVE_ROOT tests).

- `test_add_alpha_entry_byte_identical_to_committed`: in a tmp root holding the registry without the four wave-2 alphas,
  legacy v7.0 and the v70 spec, `add-alpha` of ftd_fail on parent v70 (arguments = the committed entry's) writes a
  library whose ftd_fail object is byte-identical to the committed v7.1 one, whose 44 v7.0 objects are a byte-identical
  prefix, and a locked spec with the fields above; an identical second call changes nothing.
- `test_screen_stops_before_w`: the screen runs fields, check, U, W (fit), C (card), MIC (marginal) and stops; u carries
  `--no-composition` and no `--save-combined`; the admission and marginal rows are printed; the verdict has no scoring
  blocks; a full `run` then continues with w, nav, monitor, summ. With an exe that offers neither flag nor verb, the u
  pass is unchanged and marginal is skipped with a note (never a stop).
- `test_verdict_schema`: the key set, the paired / DSR / PBO blocks from nav_summ's JSON, per-phase seconds and peak MiB
  (receipt phases) or null peak (direct phases).
- Also `test_add_alpha_refusals_write_nothing` (unknown theme, bad tier, origin `guess`, a registered DSL, a parent
  member, a missing plan row: no file changes; a redefined id and a started library refused) and
  `test_add_alpha_validates_through_the_exe_plan` (a fake exe as a `.cmd`: `--help` capabilities, `--plan-only` rows,
  the OD-2 caps written for a 1,405-date role, a new member over the slot budget refused).

Brief step 3 (root, after E-3's fixture): `research_cycle.py add-alpha ... --parent <tiny library> --parent-spec
scripts/specs/tiny.json --root <tmp>` then `research_cycle.py run scripts/specs/v8/lib-<name>.json --screen --root <tmp>
--no-git`, target under 15 s. The plan flow for real data is the plan's Appendix B command pair (`add-alpha --id
dtc_slow ... --parent v71 --name v80`, then `run scripts/specs/v8/lib-v80.json --screen`), after committing the files
add-alpha wrote (the code pathspec includes atx-impl/ and scripts/).

## Deviations from the brief (with reasons)

1. add-alpha budgets only the new members; the full-library budget check stays in `generate_library.py --plan-json`.
   The v7.1 members were admitted under Python slot estimates; if the exe counts differently, adding an alpha should not
   fail on an inherited member.
2. The derived spec has no `static_check` (the Python checker is one of the P-10 mirrors; K1 replaces it) and no
   compare items copied from the parent (the three standard identity compares are generated instead).
3. The screen skips `ref` (the NAV identity check costs a NAV replay and is not an admission input); the full `run`
   runs it before w.
4. `--plan-json` plans are checked like exe plans (including `library_sha256` when present), so a saved plan must be
   the plan of the new library, or carry no `library_sha256`.

## Cross-lane edits

None. Contracts: K1 read as `plan["candidates"]` rows (a plan without the array stops add-alpha with "contract K1:
atx-equity-strategy-ic --plan-only after lane B-3"); K6 read as `marginal_ic.json` = `{"candidates": [rows]}` or a
bare list; B-1's `--no-composition` and F-2's `marginal` detected from `--help`.

## Open risks

- Capability detection reads the `--help` text: B-1 and F-2 must mention `--no-composition` and `marginal` there.
- The F-2 verb's CLI is taken from its brief (`marginal --candidate-cache DIR --library L --pool combined.json
  [--themes] --output DIR`); if lane F adds pins (e.g. `--library-sha256`), `marginal.flags` in the spec can carry them,
  or the step needs a line of code.
- Derived stores need W0-1 (see A-3): an add-alpha spec cannot run on a branch without it.
