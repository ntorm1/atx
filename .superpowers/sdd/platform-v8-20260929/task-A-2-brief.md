# Brief: task A-2

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task A-2: `add-alpha` and `run --screen`

**Files:** `scripts/research_cycle.py`, `scripts/tests/test_research_cycle.py`

**Interfaces:**
- `research_cycle.py add-alpha --id X --dsl "..." --theme T --tier B --prior-sign 1 --citation "..." --origin prior
  --parent v71 [--name v72]`: validates through K1, writes the registry entry, the library file, a pre-registration stub and a
  spec derived from the parent by name templates, then locks.
- `research_cycle.py run SPEC --screen`: u (new members only) -> fit -> card -> marginal IC -> gate. Prints the admission
  rows. Writes `cycle_verdict.json`: `{admission[], marginal[], paired{dsr, se, cbb_ci, lw_p}, dsr{n, cell_count,
  effective_n}, pbo, phases[{name, seconds, peak_mib}]}` (paired and dsr blocks only after a full `run`).

- [ ] **Step 1:** tests `test_add_alpha_entry_byte_identical_to_committed` (one v7.1 member on parent v70),
  `test_screen_stops_before_w`, `test_verdict_schema`.
- [ ] **Step 2:** implement. **Step 3:** root: on the fixture, `add-alpha` then `run --screen` in under 15 s.

