# Lane A1 re-review 2 (fix round 2)

## Verdict
BLOCK

- **N1 (major, M1 still open in round 1): addressed** at every anchor round 1 listed.
- **N2 (minor): addressed.**
- **N3 (new, major): open.** Round 2 adds a self-check in the regression class that pins the copy's engine-only set to
  exactly `{"syn_dec5"}`. The first real DEC-5 append to `field_registry.json` makes that assertion fail in both
  regression classes. So the report's claim "a DEC-5 append therefore needs no edit to A1's tests" is still false: it is
  the M1 failure mode again, in one line. The fix is one assertion.

## Reviewed range
- FIX_BASE `e5780efb`, head `66f77a6e710bd2ebcaea5c39af2d22b10e7ad8c1`, pool `C:/atx-wt/pool-12`.
- Commits:
  - `8fd511d0`: `test_field_registry.py` only;
  - `66f77a6e`: the report.
- The tree was clean, HEAD `66f77a6e`.
- Nothing in the tree was edited. The probes are scratch pytest plugins (`dec5_probe.py`, `twin_probe.py`) outside
  every tree. They point `fr.DEFAULT_PATH` at a scratch copy of the committed file. No real data, nothing from
  2020-2023 results and nothing dated 2024-01-01 or later was opened.

## Checks
**No loader, builder or registry byte changed: pass.**
- `git diff --quiet e5780efb 66f77a6e -- field_registry.py field_registry.json prepare_research_fields.py` is clean.
- `field_registry.json` sha256 is `6c56b739ac232b297d82987b60202d94b647206da0c37b2c51f36eb4e43e3e51`.
- The diff touches `test_field_registry.py` and the report only.

**Only the Python-producible rows are held to the code: pass.**
- `engine_only_names` = `fr.engine_only` with every shim bound.
- `test_this_harness...` goes through `regenerate`. With no engine-only row, that equals the old
  `generate(engine_flips(doc))` (all flips are twins), so it is still as strong on today's file.
- `test_lane_a2_flip...` generates with the twins only and adds a names equality over the producible rows.
- The dtype test works over the producible rows. `check` still holds those rows' dtype to `dtype_of(spec)`.
- The appended-rows test finds the synthetic names in order inside `only`, and each name in the refusal text.
- The fresh-interpreter test gets `fr.DEFAULT_PATH` as argv, so the patched copy reaches the subprocess.

**The regression reruns `CommittedRegistry` unedited (f64 and group): pass, apart from N3.**
- Both subclasses inherit every test. `fr.DEFAULT_PATH` is patched for the class (`load()` reads the global at call
  time), and `syn_dec5` is appended with dtype f64 or group. The group row carries "categorical code" units.

**Twins ⊆ `ENGINE_FIELDS`, engine-only rows exempt: pass.**
- `test_field_registry.py:108-110` performs that check.
- Probe 1, an unrouted twin: the committed file with `beta_dvol_21` flipped to engine fails
  `test_valid_and_in_v15_order` with `{'beta_dvol_21': 'beta_dvol_21'}`.
- Probe 2, an engine-only row: `gia_13f` (builder `gia_13f`) passes the same test.
- A2's flip of the three `ENGINE_FIELDS` passes.

**gia_13f probe rerun from scratch: N1's failures are gone; N3 found.**
- The committed file plus `gia_13f`, under the tools harness:
  - the three round-1 failures pass;
  - every inherited test passes in all three classes;
  - the group-dtype test passes.
- Only `test_the_copy_holds_the_engine_only_row` fails, ×2.
- In a fresh interpreter under the repository window, the same file gives `only` `[gia_13f]`, `regen_equal` true,
  `generate_equal` true and 92 producible rows.

## Findings
path:line | severity | problem | fix

- **N1** `test_field_registry.py:157-158`, `:171`, `:173`, `:150`, `:152`, `:183-184` (round-1 anchors) | major |
  **addressed**. See Checks.
- **N2** `test_field_registry.py:101-102` (round-1 anchor; now `:108-110`) | minor | **addressed**. The probe shows the
  pin bites on an unrouted twin and exempts an engine-only row.
- **N3** `atx-engine/tools/test_field_registry.py:248` (inherited by `CommittedRegistryWithADec5GroupRow`, `:252`) |
  major | **open**.
  - Problem: `test_the_copy_holds_the_engine_only_row` asserts `engine_only_names(doc) == {"syn_dec5"}`. The copy is
    the committed file plus `syn_dec5`. Once AL-SIG or A3 appends a real engine-only row (for example `gia_13f`), the
    set is `{gia_13f, syn_dec5}` and the test fails in both classes. The probe reproduced it:
    `AssertionError: Items in the first set but not the second: 'gia_13f'`, 2 failed. A DEC-5 append still needs an
    edit to A1's tests. That is M1's cross-lane cost, and it contradicts the round-2 report's claim.
  - Fix: in `setUpClass`, capture the committed file's engine-only set before patching, for example
    `cls.committed_only = engine_only_names(fr.load())`. Then assert
    `engine_only_names(doc) == cls.committed_only | {"syn_dec5"}`, or at least `assertIn("syn_dec5", ...)`. Keep the
    `names[-1]` / dtype assertion.

## Evidence
Tools pytest at `66f77a6e` (pool-12, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`):
```
$ cd C:/atx-wt/pool-12/atx-engine/tools && PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider .
........................................................................ [ 90%]
......................................                             [100%]
398 passed, 6 subtests passed in 166.43s (0:02:46)
exit_code=0
```
The committed file has no engine-only rows today, which is why this run is green. N3 is about the first DEC-5 append.

DEC-5 probe, rerun from scratch at `66f77a6e` (`dec5_probe.py`: the committed file plus `gia_13f`; the tree is
untouched):
```
$ DEC5_DTYPE=f64   pytest -q -p no:cacheprovider -p dec5_probe test_field_registry.py test_no_new_python_builder.py test_field_module_imports.py
FAILED test_field_registry.py::CommittedRegistryWithADec5Row::test_the_copy_holds_the_engine_only_row
FAILED test_field_registry.py::CommittedRegistryWithADec5GroupRow::test_the_copy_holds_the_engine_only_row
2 failed, 42 passed in 6.47s          (AssertionError: Items in the first set but not the second: 'gia_13f')
$ DEC5_DTYPE=group (same command)
2 failed, 42 passed in 6.56s          (same two tests, same cause)
$ fresh interpreter, repository window, the group file:
{"only": ["gia_13f"], "regen_equal": true, "generate_equal": true, "rows": 92}
$ twin_probe (beta_dvol_21 flipped to an unrouted engine twin): test_valid_and_in_v15_order
AssertionError: False is not true : {'beta_dvol_21': 'beta_dvol_21'}   1 failed
```
