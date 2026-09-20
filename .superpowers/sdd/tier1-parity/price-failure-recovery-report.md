# Price publication failure recovery

Status: implemented; four focused cases passed. No live warehouse access or production retry performed by this task.

The observed production COMMIT exhausted DuckDB's 1 GB query budget and invalidated the connection. The old exception handler then issued DROP against that connection, masking the original exception and preventing dataset and activation failure ledgering. Read-only operator recovery separately proved the original bars and replacement staging survived.

Changes:

- `DuckDBStore.recover_failed_connection()` raw-closes the unusable connection without issuing CHECKPOINT. It requires a persistent warehouse and recorded analytical memory/thread limits, supplies those limits to `duckdb.connect(config=...)` before opening the file, and restores UTC/session settings. It does not initialize, migrate, or retry data work.
- The bulk publisher records its own explicit analytical budget, including standalone use. Failed publication first attempts its dataset ledger update; if unusable, it reconnects once and retries only that update. It preserves the original exception and leaves replacement staging for inspection; the next explicit publication replaces staging normally.
- The activation ladder similarly recovers only when writing the failed-stage ledger requires it. If failure ledgering remains unavailable, the original exception receives an explicit operator-recovery note identifying the run/table; activation also emits `failure_ledger=operator_recovery_required`. No success or durable failure write is claimed in that case.
- Corrected the invalid logging `%,d` placeholders to `%d`, restoring counts in normal publisher logs.

Validation, one focused run:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 2 --receipt ../.superpowers/sdd/tier1-parity/price-failure-recovery-test-memory.json -- .venv/Scripts/python.exe -m pytest tests/test_price_failure_recovery.py -n0 -q
```

All 4 cases passed: standalone bulk invalidation; bulk invalidation through activation; another activation stage invalidation; unavailable reopen with explicit operator recovery. Tiny persistent schemas inject connection invalidation and exercise real bounded reconnects/ledger SQL. They assert original exception identity, one source attempt, raw close before reconnect, limits supplied at connection creation, no initialization during recovery, preserved live/staged markers, ledger outcomes and valid count logging. Native peak job memory: **0.594631 GiB** under a 2 GiB cap; command elapsed 3.7 seconds.

Ruff passed on the three source files and new test file. Mypy with `--follow-imports=silent` passed on the three source files. No full suite or large/OOM benchmark was run. Real fatal WAL recovery and the proposed 2 GB production publication remain operator work under the existing process guard; recovery can still require manual ledger repair if opening the database fails within its budget.

Implementation references: DuckDB's [Python API connection configuration](https://duckdb.org/docs/current/clients/python/reference/) and [fatal error/restart guidance](https://duckdb.org/docs/current/guides/troubleshooting/crashes). Passing the retained limits at connect time avoids an application-level window in which reopening would use DuckDB defaults.

Independent review: controller's single source review; no Critical or Important finding reported before validation. Commit is pathspec-only and leaves other agents' source, registry, snapshot, and user-session files untouched.
