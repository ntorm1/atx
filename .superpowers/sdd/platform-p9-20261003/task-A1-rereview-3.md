# Lane A1 re-review 3 (fix round 3)

## Verdict
APPROVE

All findings are closed, including M1 from the original review:

| Finding | Severity | Status |
|---|---|---|
| M1 (original review) | major | addressed |
| N1 | major | addressed in round 2; still holds |
| N2 | minor | addressed in round 2; still holds |
| N3 | major | addressed |
| m3 (adopted, ruling A1-M3) | minor | addressed in round 1 |

The other minors stay deferred, as the PM ruled: m1, m2, m4, m5, m6 and m7.

## Reviewed range
- FIX_BASE `66f77a6e`, head `f8edca96b889df309b4036992d8d091eb5668512`, pool `C:/atx-wt/pool-12`.
- Commits:
  - `b8bf67fa`: `test_field_registry.py`, +3/-1;
  - `f8edca96`: the report.
- The tree was clean before and after, at HEAD `f8edca96`.
- Nothing in the tree was edited. The probes are scratch pytest plugins outside every tree.
- No real data was read. Nothing from 2020-2023 results and nothing dated 2024-01-01 or later was opened.

## Checks
**N3: addressed.**
- `setUpClass` reads `fr.load()` before `path_patch.start()`. That read sees the committed file, or under the probe the
  file the harness reads.
- It records `cls.committed_only = engine_only_names(doc)` before appending `syn_dec5`.
- `test_the_copy_holds_the_engine_only_row` asserts two things:
  - `"syn_dec5" not in committed_only`;
  - `engine_only_names(doc) == committed_only | {"syn_dec5"}`.
- The `names[-1]` / dtype assertion is kept.
- The group subclass inherits `setUpClass`, so the change covers both classes.

**Nothing else moved.**
- `git diff 66f77a6e f8edca96 -- atx-engine/` contains only those 3 lines in `test_field_registry.py`.
- `git diff --quiet e5780efb f8edca96 -- field_registry.py field_registry.json prepare_research_fields.py` is clean.
- `field_registry.json` sha256 is `6c56b739ac232b297d82987b60202d94b647206da0c37b2c51f36eb4e43e3e51`.

**The gia_13f probe, rerun from scratch, is fully green** (the committed file plus an engine-only `gia_13f`, which
stands in for a real DEC-5 append). Neither `test_field_registry.py` nor any loader needs an edit, for f64 or for group.

**N2 still bites.**
- With `beta_dvol_21` flipped to an unrouted engine twin, `test_valid_and_in_v15_order` fails.
- The engine-only `gia_13f` is exempt.

## Findings
path:line | severity | problem | fix

- **M1** (original review) | major | **addressed**. Engine-only rows pass `check` and round-trip through `regenerate`. The
  entry refuses them before any output. A DEC-5 append needs no edit to A1's loader or tests (the probe below shows it).
- **N1** `test_field_registry.py` (the round-1 anchors) | major | **addressed**.
- **N2** `test_field_registry.py:108-110` | minor | **addressed**.
- **N3** `test_field_registry.py:234`, `:249-250` | major | **addressed**.
- No new findings.

## Evidence
Tools pytest at `f8edca96` (pool-12, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`):
```
$ cd C:/atx-wt/pool-12/atx-engine/tools && PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider .
........................................................................ [ 90%]
......................................                             [100%]
398 passed, 6 subtests passed in 161.21s (0:02:41)
exit_code=0
```
DEC-5 probe, rerun from scratch (`dec5_probe.py`, the committed file plus `gia_13f`; the scratch copies were deleted
first):
```
$ DEC5_DTYPE=f64   pytest -q -p no:cacheprovider -p dec5_probe test_field_registry.py test_no_new_python_builder.py test_field_module_imports.py
44 passed in 7.92s
exit=0
fresh interpreter, repository window: {"only": ["gia_13f"], "regen_equal": true, "generate_equal": true, "rows": 92}   exit=0
$ DEC5_DTYPE=group (same command)
44 passed in 8.56s
exit=0
fresh interpreter, repository window: {"only": ["gia_13f"], "regen_equal": true, "generate_equal": true, "rows": 92}   exit=0
$ twin_probe (beta_dvol_21 flipped to an unrouted engine twin): test_valid_and_in_v15_order
FAILED ... 1 failed in 0.28s   (expected: N2 pin)
```
