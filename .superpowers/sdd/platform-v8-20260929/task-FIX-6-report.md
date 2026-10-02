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
