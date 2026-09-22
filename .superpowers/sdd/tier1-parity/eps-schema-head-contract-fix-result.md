# EPS schema HEAD contract fix

## Diagnosis and change

- `atx_db.migrations` imported the registered 0320–0322 bodies through its registry and left those module names visible because its public-namespace cleanup stopped at 0319. `migrations/__init__.py` now removes the three names after import, matching the established boundary.
- The 0320 receipt table has `available_at`, a strong temporal marker in the generated schema manifest. The PIT presence check therefore required all five canonical fact columns and found `as_of_date` and `is_latest_revision` missing. These immutable source-attempt receipts have no observation period or revision chain. `bodies_0320.py` now registers a table-specific exemption for exactly those two columns. It retains the physical `available_at` field and its existing nullable meaning: rejected or failed source attempts need not have a qualified availability clock. The check and manifest logic are unchanged.

## Static verification and runtime handoff

`git diff --check -- src/atx_db/migrations/__init__.py src/atx_db/migrations/bodies_0320.py` is clean. The diff is three namespace cleanup lines and a 14-line exemption registration. I did not run Python, imports, tests, or the warehouse because the root agent owns runtime and DB access.

The root agent should rerun its guarded import and `tests/test_module_boundaries.py tests/test_schema_contract_v2.py`, particularly `test_bootstrapped_warehouse_has_zero_pit_column_presence_after_s2_0`. Check that `pit_column_presence_check` passes with `exempted_pit_columns['sec_earnings_release_receipts'] == ['as_of_date', 'is_latest_revision']`, while `invalid_pit_exemptions` remains empty. The warehouse is still at 0319, so 0320 can apply this exemption normally; no historical 0320 checksum needs rewriting.

No registry, jobs, activation, runtime writer, or unrelated EOL-only files were edited. No commit was made.

## Controller validation

One independent Codex static review is clean (eps-schema-head-contract-review.md).
The guarded import and full two-file focused check completed successfully:
57 passed, 1 expected slow skip in 130.74 seconds. Native process-tree peak was
0.954 GiB under the unchanged 2 GiB guard. Evidence is
eps-head-contract-fixed1-memory.json and eps-head-contract-fixed1.log.
The prior two failures are preserved in head-42883bff-checks2.log. The earlier
checks1 invocation failed only because PowerShell removed inline Python quotes;
the saved check_committed_head.py avoids that invocation issue.

No production migration or source materialization was performed by these checks.
