# Lane A1 re-review, fix round 1

## Verdict
BLOCK

**M1: open (partly addressed).** The loader half of the fix is correct. Engine-only rows pass `check`, round-trip through
`regenerate`, and the entry refuses them before any output. The test half is not done. A DEC-5 append to the committed
`field_registry.json` still turns 3 registry tests red (4 if the new row has `dtype: group`). So AL-SIG or A3 still
has to edit A1's test file to add a field, which is the cross-lane cost M1 named. The fix is test-only, about 10 lines
(N1 below).

**m3 (adopted, ruling A1-M3): correct.** No new finding.

## Reviewed range
- FIX_BASE `88c9efa2`, fix head `e5780efbf5219b8ce4773c768ea21e59a7a172f6a`. Pool `C:/atx-wt/pool-12`, branch
  `feat/p9-a1-20261003`. HEAD was `e5780efb` and the tree was clean before and after.
- Commits: `d672eb8d` (code), `c43ff27e` (tests), `d049c88c` (report), `4df3f7b7` (m3 and its test), `e5780efb`
  (report line).
- Nothing in the tree was edited. Probes lived in scratch (outside every tree), and a pytest plugin pointed
  `fr.DEFAULT_PATH` at a scratch copy of the registry. No real data was read, and nothing from 2020-2023 results or from
  2024-01-01 onward was opened.

## Checks
**(a) Engine-only rows: loader passes, tests fail.**
- A2's 3-row flip passes. With si_shares, si_dtc and vol_126 flipped, the rows stay producible twins: `check` compares
  them, `regenerate` and `generate` reproduce them, and the `test_lane_a2_flip...` and committed-file assertions still
  hold.
- DEC-5 append: one row `gia_13f` (`kind: engine`, f64, `owner: registry:DEC-5`) added to the committed file. In a fresh
  interpreter under the repository window:
  - `engine_only` = [`gia_13f`];
  - `check` ok;
  - `dump(regenerate(d)) == file bytes`: true;
  - `generate` over the producible rows == those rows: true;
  - 92 producible rows;
  - the root acceptance one-liner prints `ok 93`.
- With the same file seen by the tools harness (`-p dec5_probe`), the run of `test_field_registry.py`,
  `test_no_new_python_builder.py` and `test_field_module_imports.py` gives **3 failed, 25 passed**. With
  `dtype: group`, the dtype test also fails. Details in N1.

**(b) No hole: pass.**
- `extra` is the set of rows that are not in the code and are not `kind: engine`. A python row the code cannot produce
  is therefore still refused (tested: `python_row`, "rows the builder cannot produce").
- The relative order of the producible rows, and their full set (`absent`), are still compared.
- An engine row whose name the code produces is a twin, and it is fully compared on point_in_time, requires, dtype and
  formula. So an engine-only row cannot shadow a code field: names are unique, and "engine-only" means not in
  `code_fields`.
- Can a mis-declared row escape? Only if it is an engine row whose Python module the entry's `bind` does not bind. Then:
  - Python never computes it: the entry refuses it without `--engine-exe`, and with one unless `ENGINE_FIELDS` routes it.
  - All three `ENGINE_FIELDS` are always in `code_fields`: si_shares and si_dtc are inline builder dicts, and vol_126
    is the builder's own price module. So a routed name can never be engine-only.
  - The committed-file round-trip registers every shim. That turns such a row back into a twin, and `regenerate`
    diverges on owner/builder.
- `regenerate` drops a python row the code cannot produce and fills producible slots in code order. Any mis-order, drop
  or extra therefore gives different bytes (tested).

**(c) `generate` and the registry file are unchanged: pass.**
- The `generate` body at FIX_BASE and at head was extracted and compared byte for byte with `cmp`: identical. Only the
  `engine_flips` docstring changed.
- `field_registry.json` sha256 is `6c56b739ac232b297d82987b60202d94b647206da0c37b2c51f36eb4e43e3e51` at FIX_BASE, at
  head and in the worktree. `git diff --quiet` shows no change.

**(d) Entry: pass.**
- `only` = requested engine rows ∩ `engine_only`.
- With `--engine-exe`, an unrouted engine-only row raises before `engine_path`. Without it, `if only: raise` comes
  before `build_main`. Before that point only `load`, `bind` (in memory) and `check` have run.
- The test asserts that no output dir exists in all 3 cases, including `--fields all`.
- For engine twins `only` is empty, so the code path is identical to FIX_BASE: Python fallback with the stderr notice,
  exe routing, and the "no requested row is of kind engine" refusal. The pre-existing
  `test_engine_rows_need_the_executable` is unchanged and passes.

**(e) m3: pass.**
- A present non-dict `seal` (string, null or list) raises `SealError`.
- A dict seal takes the same path as at FIX_BASE: `manifest.get("seal", {}).get("exclusive_end")` equals the old
  expression for every dict. Equal is accepted, different is refused, and a missing or null `exclusive_end` is logged as
  legacy.
- An absent seal is logged and accepted (P13).
- A non-string `exclusive_end` is refused by the `!=` branch, as at base.
- Writers: the builder's manifest writes `seal` as an object (`prepare_research_fields.py:2624`). The only caller is
  `load_prior`, after the schema and status gate. The string `"seal"` in the C++ receipt (`research_fields_cli.cpp:232`)
  is a different schema and never reaches it. A2's `engine.py` (`2b6f8e6f`) writes no seal.
- `MalformedSealBlock` covers all of these.

**(f) Tests: partial.**
- The new Validation and Entry tests test the contract, including negative cases and no-output assertions. The fresh
  committed-file test now also pins flag-absent identity (`generate_equal`).
- But four committed-file tests assume the file has no engine-only rows (N1).
- The relaxed pin loses one guard (N2, minor).

## Findings
path:line | severity | problem | required fix

- **N1** (M1 remains open) `atx-engine/tools/test_field_registry.py:157-158`, `:171`, `:173`, `:150`, `:152`,
  `:183-184` | **major** |
  - Problem: these committed-file tests break on the first DEC-5 append. The probe reproduced every case.
    - `:157-158` (`test_this_harness_generates...`): calls `generate(engine=engine_flips(doc))`, which raises
      "engine rows gia_13f are not producible fields". It then asserts `names(gen) == names(doc)`.
    - `:171` (`test_lane_a2_flip_keeps_the_registry_tests_green`): pins `engine_flips(file)` to exactly the 3 A2 ids.
    - `:173` (same test): `generate`s with every flip.
    - `:150` and `:152` (the new appended-rows test): pins `only` to exactly the 2 synthetic rows, and the refusal text
      to exactly those 2 names.
    - `:183-184` (`test_dtype_is_declared_by_units_not_by_name`): pins the `dtype: group` set over all rows, so a group
      engine-only row turns it red.
  - Required fix: hold these tests to the Python-producible rows.
    - Compare the harness test through `fr.regenerate(vars(tool), doc)`, or over `doc` minus `engine_only`.
    - In the A2-flip test, filter `engine_flips` to the twins (names not in `engine_only`).
    - In the appended-rows test, assert the synthetic names in order within `got["only"]`, and assert that each one
      appears in `refused`.
    - Compute the dtype group set over the producible rows.
    - Add a regression that runs `CommittedRegistry` against the committed file plus one appended engine-only row (for
      example via a patched `fr.DEFAULT_PATH`, as this probe did). That way the "no edit to A1's tests" claim is
      tested.
- **N2** `atx-engine/tools/test_field_registry.py:101-102` | minor |
  - Problem: the relaxed pin no longer requires a committed engine *twin* to be routable (`⊆ ENGINE_FIELDS`). A twin
    flipped outside the shim's route passes the committed-file test. With `--engine-exe` that twin is then computed in
    Python without notice (the hazard of deferred m1). Engine-only rows needed the exemption; twins did not.
  - Fix: assert `{n for n in engine_flips(doc) if n not in engine_only} <= set(engine.ENGINE_FIELDS)`, with engine-only
    rows exempt. This can be deferred together with m1.

## Evidence
Tools pytest at `e5780efb` (pool-12, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`):
```
$ cd C:/atx-wt/pool-12/atx-engine/tools && PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider .
........................................................................ [ 94%]
......................                                             [100%]
382 passed, 6 subtests passed in 171.20s (0:02:51)
exit_code=0
```
The committed file has no engine-only rows today, which is why this run is green. N1 is about the first DEC-5 append.
DEC-5 probe (scratch plugin `dec5_probe.py`: committed file plus `gia_13f`; tree untouched):
```
$ pytest -q -p no:cacheprovider -p dec5_probe test_field_registry.py test_no_new_python_builder.py test_field_module_imports.py --tb=no
FAILED test_field_registry.py::CommittedRegistry::test_engine_only_rows_appended_to_the_committed_file_check_and_round_trip
FAILED test_field_registry.py::CommittedRegistry::test_lane_a2_flip_keeps_the_registry_tests_green
FAILED test_field_registry.py::CommittedRegistry::test_this_harness_generates_the_same_rows_up_to_window_dependent_text
3 failed, 25 passed in 6.17s
$ DEC5_DTYPE=group pytest ... -k dtype_is_declared  ->  1 failed (group set mismatch)
$ fresh interpreter, repository window, same file:
{"only": ["gia_13f"], "regen_equal": true, "generate_equal": true, "rows": 92}
ok 93
```
