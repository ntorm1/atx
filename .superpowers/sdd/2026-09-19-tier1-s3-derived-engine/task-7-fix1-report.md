# S3 T7 fix round 1 (2026-09-20)

Disposition: **I1 and M1 addressed; no open findings from the one-pass review.** Codex only. Files are released for the subsequent S3 T8 compatibility work after this task's commit.

## Changes

- **I1 (Important):** `compute_projection_rows` now sets output `available_at` to the maximum input decision time in each `(factor_id, as_of_date)` cohort after missing/nonfinite values and insufficient-breadth dates are excluded. Winsorization and z-score outputs therefore cannot be published before their latest contributing peer. The original per-row input decision time is retained as `decision.input_available_at`; `decision.available_at` agrees with the published cohort timestamp, and `metric.available_at` retains the selected metric's own timestamp.
- **M1 (Minor):** the retirement expectation is now an explicit independent set of 18 module names. The seed guard requires one mapping per ordinary retirement module, three annual-margin mappings, four enterprise-yield mappings, and one `KEPT` mapping, totaling 24 distinct factor IDs. Removing a skipped module's seed row now fails coverage rather than shrinking the expected set with it.

## Focused verification

Interpreter: `C:/atx/atx-db/.venv/Scripts/python.exe`; working directory: `C:/atx/atx-db`.

- `python -m pytest tests/test_derived_parity.py -n 0 -q -k 'standardized_values_wait_for_eligible_cohort or projection_covers_every_retired_module or every_skipped_module_has_a_documented_reason'`: **3 passed**, exit 0 (47 deselected).
- The new pure regression verifies actual nonzero z-scores for inputs `(1, 2, 9)`, heterogeneous decision times, independent date cohorts, exclusion of a later nonfinite peer, and both individual timestamps in lineage. The later peer changes the earlier security's published score; the earlier security waits for that peer's availability.
- `python -m ruff check src/atx_db/derived_factor_projection.py tests/test_derived_parity.py`: clean.
- `python -m mypy --strict src/atx_db/derived_factor_projection.py`: clean, one source file.
- Scoped diff whitespace check: clean.

All selected tests are pure/seed checks and request no database fixture; no schema template or warehouse was built, and the serial database pytest slot was not used. No full suite or previously passing database tests were rerun.

## Scope and remaining program limits

Only `derived_factor_projection.py`, `test_derived_parity.py`, and this report changed. No seed, migration, registry, jobs, activation, legacy formula, or shared progress file was changed. The existing 14 skipped module cases remain retirement blockers as documented in `task-7-report.md`; this timestamp correction adds no retirement-parity evidence and does not authorize deleting any module. Acceptance is on this fix report per the controller's Important-finding ruling; no re-review is required.
