# MM1 implementation report

Status: implemented and accepted after one independent root review and focused runtime verification; production migration retry remains pending.

First root focused run: four failures in 2.41 seconds, all during fixture preparation; guarded process peak 0.5962 GiB under the 1 GiB cap. The fixture used an invalid `ColumnSpec.declared_in` value and its schema-only `duckdb_columns()` filter included system/view columns. The Important review repair sets the valid `schema_py` declaration, explicitly registers only the six physical fixture tables, and restricts column seeding to that registry in the current database's main schema. Real schema and checksum verification remain enabled. Root will rerun the repaired cases; no implementer runtime was launched.

Root scoped Ruff found four preexisting production-file issues (UP035, two UP017, SIM105), with no new test issues. They are outside this bounded repair; no unrelated formatting changed. Root's independent review found the production diff clean; only the fixture repair above was requested.

Repaired focused run: `migration-memory-focused2.log`, **4 passed in 2.53 seconds** under a 1 GiB process cap, native peak **0.6025962829589844 GiB**. Root accepted the Important fixture repair on its report and this focused evidence; no Critical repair or second review was required. Root confirmed the four production Ruff findings against committed HEAD and the new test module passed Ruff. Independent review receipt: `migration-memory-review.md`.

`migration_admin.py` now derives the same bounded startup configuration for governed initial apply, post-backup reopen and restore lock cleanup: DuckDB 512 MB, one thread, insertion-order preservation disabled, absolute adjacent spill directory. The limit is active during open/WAL recovery and final checkpoint. Public signatures, lock ownership, backup/hash registration, schema/checksum verification, rollback and retention are unchanged.

The former final-checkpoint sequence is preserved. Closing a dirty connection can itself checkpoint, so an extra close/reopen would not safely avoid that allocation peak and would introduce another governance boundary without evidence of benefit.

The new focused tests use a tiny real persistent schema and one actual registered migration. All actual governance operations run; only the broad warehouse bootstrap/registry is replaced with the tiny fixture. Success proves durable migrated data, backup hash and completed registry versions. Real schema drift, a tampered migration checksum and an injected final-checkpoint allocation error each must restore original data/schema, preserve the backup and clear the target lock. Every connection is inspected immediately after DuckDB opens, including recovery cleanup. No schema verification or checksum verifier is mocked out.

Root ran `python -m pytest tests/test_migration_memory_budget.py -n 0 -o addopts= -q` in `atx-db` under the existing memory guard, then scoped Ruff. No Python, tests or live database workload was run by the implementer. Production memory improvement remains unproven until root's full live governed migration retry succeeds under the unchanged 1.5 GiB process cap and a fresh sustained-headroom window.
