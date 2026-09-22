# S4 Task 8 fix round 1

Review: `task-8-review.md`, covering implementation commit `9a5c75bd`.
Outcome: both findings addressed (one Important, one Minor); no Critical findings and
no re-review required under the program's speed ruling.

## I1: governed migration preflight

The publication CLI now calls the existing read-only `pending_migrations` helper before
constructing a writable `DuckDBStore`. Any pending migration causes a clear refusal
naming the pending versions and the governed `scripts/warehouse_migrate.py --db-path`
command. This also refuses missing and legacy databases without bootstrapping them.
The already-current path continues directly to publication.

The command does not migrate, create backups, prune backups, or enter the activation
ladder. The recommended standalone migration script uses `run_governed_migrations`
without the activation command's automatic backup pruning.

`publish_release` no longer calls `store.initialize()`. Its docstring explicitly requires
an already-open, writable, migrated store; migration responsibility stays with the caller.

## M1: export ordering independent of session defaults

Every release key is now ordered explicitly with `ASC NULLS LAST` in the hashed SQL.
Publication also sorts exported metadata by its physical column ordinal: the shared
lake catalog helper has an unqualified `ORDER BY column_index`, so a DESC session would
otherwise reverse projection columns even with the release-key fix. The shared helper
was not changed.

The existing large deterministic export test now includes a NULL key, sets the second
session to `default_order=DESC` and `default_null_order=NULLS_FIRST`, and verifies identical
Parquet, query and schema hashes, explicit ascending/NULL-last rows, and preserved caller
settings across 260,001 rows.

## Validation

One focused invocation, no failures or reruns:

```
.venv/Scripts/python.exe -m pytest \
  tests/test_publication.py::test_cli_refuses_pending_migrations_before_opening_writable_store \
  tests/test_publication.py::test_cli_read_only_preflight_preserves_missing_or_legacy_database \
  tests/test_publication.py::test_release_query_pins_sort_direction_and_null_order \
  tests/test_publication.py::test_the_cli_publishes_a_release \
  tests/test_publication.py::test_parquet_bytes_stable_across_thread_settings_and_row_groups \
  -n 0 -q
```

Result: 6 passed, exit 0. The CLI preflight regression injects pending migration 0308 and
fails if any writable store is constructed. The missing/legacy cases use the real
read-only helper and verify original database bytes (or nonexistence) are preserved.
The already-current CLI path still exports successfully.

Touched-file ruff and strict mypy (`publication.py`, `cli.py`) passed. No full suite, live
warehouse access, migration, publication, or backup deletion occurred. Pytest slot was
released immediately after the focused run. Registry files were not modified.

Existing publication limitations from `task-8-report.md` remain unchanged, including
the process-crash window between filesystem rename and database commit.
