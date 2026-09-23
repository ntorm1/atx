# Fatal dataset failure ledger recovery

Root validation: the focused test passed (1 passed in3.05s), under2GiB guard
with0.591GiB peak. First execution failed only a traceback format assertion,
corrected to the observed source-line evidence before the passing rerun.
One independent Codex static review found noCritical/Important findings.
Ruff found three pre-existing UP017 datetime aliases, verified present atHEAD;
the same two files pass with onlyUP017 ignored. No unrelated alias edits.
Evidence:fatal-dataset-ledger-tests2-memory.json andtests2.log. No fullsuite.


`Dataset.run` now captures the original load traceback, attempts its normal `dataset_runs` failure update, and, if that update fails, uses `DuckDBStore.recover_failed_connection()` for one bounded retry of the ledger update. It re-raises the original load exception in all cases. If both ledger attempts fail, the original exception receives an operator recovery note and the failure is logged.

This leaves committed partial rows intact and does not rerun `load`, reinitialize the warehouse, migrate schema, or assign a speculative `rows_loaded` value. The existing store recovery method controls the persistent connection's memory and thread budget, and refuses recovery when those settings are unavailable.

Added `test_dataset_run_records_failure_after_invalidated_connection`, which simulates a fatal COMMIT error followed by an invalidated connection. It checks original exception identity, one load, raw close before bounded reopen, one schema initialization, a terminal failed ledger row, and retention of a committed partial row.

Static review: `git diff --check -- atx-db/src/atx_db/dataset.py atx-db/tests/test_dataset_failure_recovery.py` passed. Root's first focused test run reached the final traceback assertion; that assertion expected a qualified method name absent from Python's traceback format. It now checks the actual raising line. This agent has not run Python or accessed the warehouse per root's runtime ownership; root will rerun the focused test.

Focused test command (from `C:\atx\atx-db`):

```powershell
python -m pytest tests/test_dataset_failure_recovery.py -q
```
