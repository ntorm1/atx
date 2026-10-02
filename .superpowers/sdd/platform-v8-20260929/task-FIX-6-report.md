# Report: task FIX-6 (v8 spec fixtures, independent of locks; Ruling PM5-20)

Lane FIX-6, pool-10, branch `feat/platform-v8-fix6-20261001`, base `f7c430f2`. Status: DONE. Tests only. No spec,
script, tool or C++ file changed; synthetic data only.

Fix commit: `048a8205` `test(specs): plan the live v8 specs unlocked on stand-ins (Ruling PM5-20)`.

## Causes (20 failed / 162 passed / 4 skipped at `f7c430f2`)

Every failure is in `scripts/tests/test_research_spec.py`. All 20 come from one premise: the fixtures plan the **live**
v8 specs on stand-in input files and assume the pins that `lock --write` fills are null. `f93868bf` locked
`scripts/specs/v8/base-lo1.json` (role, identity_bridge, fund_events, fields.manifest_sha256). Every template inherits
those pins through its nominal-parent chain (b0c -> lo1; r1..r8 -> b0c; r10, r11 -> r1).

| # | cause | tests | symptom |
|---|---|---|---|
| A | `fake_root` copies the resolved spec with its pins and plans it on stand-ins (`"{}"`, a names-only fields manifest), so a real pin can never match | `test_every_v8_spec_loads_and_plans` for base-lo1, base-b0c, r1, r2, r3, r4, r5, r6, r7, r8, r10, r11 (12); `test_r1_runs_the_weighted_pass_under_3072_mib...`; `test_r10_derives_its_rule...`; `test_r11_appends_theme_resid...`; `test_r11_checks_its_re_fit...` (16 in all) | `Cycle()` -> `verify_inputs` -> `PIN MISMATCH role ... spec pins 2ff9d771...` (exit 3) |
| B | `v8_root` writes the live base-lo1 doc into the fake root and runs `lock --write` without `--relock`, so the lock refuses a filled pin that differs from the stand-in | `test_e27b_refuses_every_spelling_at_add_alpha`; `test_add_alpha_on_a_v8_base_spec_screens_and_runs`; `test_add_alpha_on_a_v8_template_removes_replaces_rescreens_and_records_exceptions`; `test_add_alpha_child_of_a_labelled_parent_labels_its_ref_and_nav` (4) | `lock: role pin 2ff9d771... differs from the file (f31a9064...); --relock to replace` (exit 3) |

base-lo3 still passed only because it is not locked yet.

I found three more latent premises of the same kind. They do not fail today, but they fail as soon as root locks a
**template** (b0c's label_role after R15, any template's derived `locked` pins). I found them by planning with every
v8 spec locked (see Verification):

- `test_templates_differ_from_the_parent_only_by_the_registered_change`: `refs == want` with `"sha256": None` for the
  derived `reference_*` inputs.
- `test_v8_base_specs_carry_the_ruled_settings`: B0c's `change.inputs.label_role` compared with `"sha256": None`.
- `test_r11_checks_its_re_fit_against_the_parent_cells_weights`: `reference_resid_parent == {..., "sha256": None}`.

## The helper

One shared helper in `scripts/tests/test_research_spec.py`:

- `unlocked(spec) -> dict` returns a deep copy of a live spec in its unlocked state, with every pin that `lock --write`
  fills set back to null:
  - every `inputs.*.sha256`, except a committed library or recipe (an `atx-impl/` file in this worktree; it is pinned
    with the file and verified against the committed bytes, as before);
  - the as-built `fields.manifest_sha256`.
  - For a template doc it resets `change.inputs.*.sha256` (same exception), a null-able `change.set` fields pin, and
    removes the `locked` block (the derived pins).

  This is exactly the set of pins a lock fills (`research_cycle.lock` / `research_spec.lock_template`). It is also
  exactly the set backed by stand-ins in a fake root.
- `committed(rel)` returns the committed bytes, else None. It is the one predicate shared by `unlocked` and
  `fake_root`, which previously had it inline.
- `fake_root` now plans `unlocked(spec)` (this replaces the old JSON round-trip copy) and still re-points absolute
  paths. This fixes cause A everywhere `fake_root` is used: test_every, r1, r10, r11 and v8_root.
- `v8_root` writes `fake_root`'s returned copy, and its `lock --write` re-pins it to the stand-ins' own digests. This
  matches the fixture's stated intent ("locked on stand-ins of its inputs") and fixes cause B.
- `plan_on_stand_ins(tmp_path, path)` and `unlocked_pins(c, spec)` factor out the fixture path of
  `test_every_v8_spec_loads_and_plans`, so the new test uses the same path.
- The three latent assertions now compare `unlocked(...)` of the live spec. The paths, dirs, keys, templates, parents,
  flags, criteria texts and refusals asserted are unchanged. Only the pin value premise changed. `NULL_PINS` stays the
  registry of which pins a lock fills per spec, and the UNLOCKED set is still asserted equal to it.

New test, `test_the_fixtures_plan_a_locked_spec_as_unlocked`:

1. Copy every v8 spec to tmp.
2. Lock each copy with the real `research_cycle.lock(..., relock=True)`, with `Resolver.sha` monkeypatched to return
   an arbitrary digest per path (committed `atx-impl/` files: their real digest), and write the result. The copies of
   base-lo1 and base-lo3 then have their inputs and fields pinned; the templates have change.inputs pinned plus a
   `locked` block. No file under any root is read.
3. Assert every resolved copy has every pin filled.
4. Plan each copy through `plan_on_stand_ins`, and assert the same `NULL_PINS[name]` UNLOCKED set and the fields line
   UNLOCKED.
5. Control: putting the lock's role pin back onto the planned copy is refused (`PIN MISMATCH role`, exit 3). This shows
   the reset is load-bearing.

## Files touched

- `scripts/tests/test_research_spec.py` (only file; +98 / -20).
- This report.

Cross-lane edits: none. Nothing under `scripts/specs/`, and no change to `scripts/research_cycle.py`,
`scripts/research_spec.py` or any tool.

## Verification (how root reproduces)

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests
-> 183 passed, 4 skipped (the 182 + test_the_fixtures_plan_a_locked_spec_as_unlocked; 0 failed)
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_spec.py
-> 44 passed
```

Lock independence was checked in three states of the spec dir. The scratch runners imported the test module, pointed
its `V8` at a copy, then ran pytest on the module. Nothing in the worktree was written. The runners are not committed.

| state of the v8 specs `V8` points at | result |
|---|---|
| live (`f7c430f2`: base-lo1 locked) | 44 passed |
| pre-lock (`git show da805a2a:scripts/specs/v8/*.json`: every pin null) | 44 passed |
| every spec locked (base-lo1, base-lo3 and all 11 templates; arbitrary digests via `research_cycle.lock --relock`) | 44 passed |

A control run with `V8` pointed at a missing dir failed 37 tests, which confirms the runners did repoint `V8`.

Other suites consuming these fixtures: none. `fake_root`, `v8_root` and the live `scripts/specs/v8` files are used
only by `test_research_spec.py`. `test_research_cycle.py` and `test_cycle_scoring.py` build their own synthetic
`scripts/specs/v8/lib-*.json` roots. Both pass in the full run.

## Open risks

- Setting a template's `parent` (root's step before `run`) still fails `test_every_v8_spec_loads_and_plans` (`doc["parent"] is
  None`) and `test_templates_differ...` (it compares with the nominal parent). That is spec content, not a pin premise,
  and is out of scope here.
- If root ever locks r2-lib-v80 / r7-lib-v81 themselves after their v8.0 / v8.1 library is committed to `atx-impl/`,
  the library and recipe pins would be verified rather than reset. Their `NULL_PINS` entry would then drop
  `inputs.library` and `inputs.recipe`. Their `requires` says the cell runs as add-alpha's `lib-v80.json`, never as
  the template, so this is not expected.

## Round 1: fixtures independent of the parent root sets

Coordinator follow-up: close open risk 1. Root sets a template's `"parent"` for every cell from R-1 on (cells brief:
"Set parent in the template, nothing else"). Tests only, same branch, from `0f00013d`.

Commit: `ffc93e6e` `test(specs): hold v8 templates to their nominal parent whatever parent root sets`.

### What depended on a null parent

| test | premise |
|---|---|
| `test_every_v8_spec_loads_and_plans` (11 templates) | `doc["parent"] is None`, `# template X: parent null`, run refused "template parent is null"; `NULL_PINS` / `FILLS` hold only for the nominal chain |
| `test_templates_differ_from_the_parent_only_by_the_registered_change` (11) | resolves the live template (on its live parent) and compares it with `nominal_parent` |
| `test_r1_runs...`, `test_r3_maps...`, `test_r10_derives...`, `test_r11_appends...`, `test_r11_checks...` | read the live templates, or write a temp template whose parent is a live file (`scripts/specs/v8/...`), so the chain under it follows root's parents; the R-10 nominal case plans the live `r10.json` and expects `ic-shrink-v1` |
| `test_add_alpha_on_a_v8_template...` | copies live `r4-hold-band.json` / `base-b0c.json` into its root and expects "template parent is null" |

Control: the test file of `0f00013d` failed **23 tests** on a copy of the specs where every cell up to R-11 ran with its
parent set (B0a won, all accepted). The round 1 file passes all 45 on the same copy.

### The helper (one place)

- `unlocked` is renamed `as_authored(spec)`: the spec as its author committed it, before root's runbook steps on it.
  The pin reset is unchanged. In addition, a template's `parent` is set back to null, so it is planned on its nominal
  parent.
- `authored_dir(src, dest)` writes every spec of a directory as authored. The function-scoped fixture `authored_v8`
  is that copy of the live `scripts/specs/v8`.
- `fake_root` still resets the pins of the resolved spec it plans, via `as_authored`.

### The tests

- `test_every_v8_spec_loads_and_plans(name)` runs two checks:
  - `check_nominal_plan`: the authored copy, with every earlier assertion unchanged. `NULL_PINS` equal,
    `FILLS`, `parent null`, the run refused with exit 3 and nothing called, a base spec without refusal, no skipped
    step but marginal.
  - `check_live_plan`: the spec as it stands, with its live parent and live pins.
    - A parent root set names another spec of the same directory.
    - The template plans on it, the header names it, and `reference_cell` is that parent's NAV cell.
    - Every pin a lock fills is UNLOCKED. On a chain of registered specs, at least `NULL_PINS[name]` is (a base-lo3
      chain adds `sic_events`).
    - The run refusal mentions a null parent only when the parent is null, and lists the template's own `requires`.
- `test_templates_differ...` is `check_registered_change(authored_v8, name)`, the same body over the authored copy.
- The other tests listed above read their templates and their nominal or explicit parents from `authored_v8`.
  Their explicit parents are absolute paths into that copy.
- Not weakened: every assertion on flags (`nav_delta`, the composition maps, `W_3072`, `FIT_APPENDED`), the criteria
  texts (PM5-11, E-44, E-45), the refusals and the E-38 / E-45 conditions is unchanged.
- `test_the_fixtures_plan_a_locked_spec_as_unlocked` now runs both `check_nominal_plan` and `check_live_plan` on the
  locked copies.

New test, `test_the_fixtures_plan_every_template_on_every_plausible_parent`:

- **Histories** (`plausible_histories`, from the cells brief):
  - Every template on every plausible accepted predecessor P, in run order B0c, R-1, R-2, R-3, R-4, R-5, R-6, R-7,
    R-8, R-10, R-11.
  - B0c sits on the winner, base-lo1 or base-lo3.
  - R-10 and R-11 run only if R-1 and R-6 were accepted (E-38).
  - R-2 and R-7 run as add-alpha's `lib-v80.json` / `lib-v81.json`; their templates stand in for them as parents.
  - Each P is planned after two histories: a dense one (every cell up to P accepted, B0a won) and a sparse one (only
    P and the cells P and the template need accepted, B0b won).
  - Every cell that ran has the last accepted cell as its parent. A cell whose condition failed never ran.
  - That gives 88 histories: b0c 2, r1 2, r2 4, r3 6, r4 8, r5 10, r6 12, r7 14, r8 16, r10 6, r11 8.
  - The test asserts the predecessor sets themselves: b0c {lo1, lo3}; R-k all earlier cells; r10 {r6, r7, r8};
    r11 {r6, r7, r8, r10}.
- **Per history**, in a temp copy of `scripts/specs/v8` with those parents set:
  1. The authored copy is byte for byte the live specs' authored copy. So every assertion held against a nominal
     parent reads the same files, whatever root has set.
  2. `check_live_plan` passes for the template.
  3. `check_registered_change` passes for it.
  4. The chain-decided rules hold:
     - R-3 is `ew-theme-std-aim-v1` if R-1 is in its chain, else `ew-theme-aim-v2` (E-27).
     - R-10 is `ic-shrink-aim-v1` if R-3 is in its chain, else `ic-shrink-v1`, and it always loads, because R-1 is
       accepted on every plausible R-10 chain (E-44, E-45).
     - R-11 keeps its parent's composition and adds `--theme-resid theme-resid-v1`.

### Files touched

`scripts/tests/test_research_spec.py` only (+211 / -70) and this section. No spec, script or tool changed.

### Verification

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests
-> 184 passed, 4 skipped (182 + the two FIX-6 tests; 0 failed)
```

`test_research_spec.py` gave 45/45 in every state of the spec dir below. The scratch runners pointed the module's
`V8` at a copy; nothing in the worktree was written.

| state | result |
|---|---|
| live (`ffc93e6e`) | 45 passed |
| pre-lock (`da805a2a`) | 45 passed |
| every spec locked, no parent set | 45 passed |
| B0c on base-lo1, every spec locked | 45 passed |
| up to R-4 run on base-lo3, unlocked | 45 passed |
| up to R-11 run (all accepted) on base-lo1, unlocked | 45 passed |
| up to R-11 run (all accepted) on base-lo3, every spec locked | 45 passed |

### Open risks (round 1)

- **R-2 / R-7 add a spec file.** add-alpha writes `scripts/specs/v8/lib-v80.json` (and later `lib-v81.json`).
  `test_every` asserts `set(V8_SPECS) == set(NULL_PINS)` and is parametrized over the directory, so it fails once that
  file is committed, until the spec is registered or the add-alpha specs are excluded by a rule. This is a spec-set
  premise, not a parent premise.
  - `check_live_plan` already accepts a `lib-*.json` parent: it requires the `NULL_PINS` superset only on a chain of
    registered specs, because an add-alpha child drops `identity_bridge` and `fund_events`.
  - That path is untested, because no such file exists yet.
- **R-6 / R-8 fill values.** Root fills the `<fill:nav.flags --risk-model>` / `--risk-model-sha256` values in
  `r6-spo-v3.json` (and `r8.json`) before their run. `check_nominal_plan`'s `FILLS` and `check_registered_change`'s
  `nav_delta` expect the placeholders, so both fail from R-6 on.
  - The same fix applies: `as_authored` would reset the flag values the `FILLS` registry names to null, and the
    history test would fill them.
  - This was not asked for in round 1, so it is not done.
- The history test writes parents as bare file names. `check_live_plan` also accepts the repo-relative spelling
  (`scripts/specs/v8/X.json`, which resolves into the same directory), but only in the live tree.
