# IQ2 empty issuer as-of schema repair

Changed files:
- `atx-db/src/atx_db/asof/fundamentals.py`: when the SQL page count is zero, execute the same bounded page query and capture its cursor column names before leaving the loop. The existing nonempty page byte guard, qualification, cutoff, paging, and no-eligible-owner return are unchanged.
- `atx-db/tests/test_issuer_selected_lineage.py`: add `test_no_visible_derived_rows_keep_asof_columns`, which retains the visible owner fixture, deletes derived rows, and asserts an empty as-of frame still exposes `derived_value_id` with zero scanned rows.

Focused commands for the root's guarded runtime (from `atx-db`):
- `pytest -q tests/test_issuer_selected_lineage.py -k "no_visible_derived_rows_keep_asof_columns or tampered_selected_lineage"`
- `ruff check src/atx_db/asof/fundamentals.py tests/test_issuer_selected_lineage.py`

Static edits only. I did not run Python, tests, or a live query.

## Root acceptance

Root ran both affected focused files once under the unchanged 1.5 GiB guard:
all 17 checks passed, exit 0, measured peak 0.630894 GiB. Receipt:
whole-branch-fixes-tests1-memory.json. Scoped Ruff on the four changed Python
files also passed, peak 0.074970 GiB (whole-branch-fixes-ruff1-memory.json).
The regression is accepted on the implementer's report and this focused proof.
No live query, full schema verification, or release claim follows.
