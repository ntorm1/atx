# Standardized NULL contract — static review

Reviewed `standardized-null-contract-draft/integration.patch` at SHA-256 `1838D53E0C1ECC7F97EC379E4DD341AF31C690C691C3B11CB0ED674C53BCCDF2` and its `fix-report.md`. **Disposition: no Critical finding; accept after root's guarded runtime checks.** This was static inspection only: no Python, imports, tests, database, network, or live source edits.

## Critical

None found.

## Important

1. **The upgrade test does not prove compatibility with the actual persisted 0319 `2.0.0` hash.** `atx-db/tests/test_standardized_null_contract.py:17–25` reconstructs a predecessor from the current `3.0.0` Python schema, and lines 35–65 insert its freshly computed hash as the supposed old catalog row. The subsequent `old_hash` assertion therefore compares the test's own constructed value with itself. A historical description or field difference would pass unnoticed. Anchor the predecessor to a known 0319 catalog fixture or recorded `2.0.0` hash and field metadata, then assert that 0321 leaves those exact bytes and the old fields unchanged while marking only that schema row inactive.

2. **The test does not exercise migration replay or the real pending-migration transaction.** `atx-db/tests/test_standardized_null_contract.py:94` calls `_reported_eps_nullable_standardized_value(conn)` once against a database whose template already records 0321. The code has plausible idempotence guards, and `_runner.py` wraps pending migrations in a transaction, but the test does not check a second call, row counts and hashes after replay, or application from a warehouse whose schema ledger stops at 0320. Add a focused replay assertion, and have root's guarded run include a real 0320-to-0321 migration against an isolated persisted predecessor. This also exercises the new schema pin and checksum in the same path production uses.

## Minor

- `test_standardized_null_contract.py:165–173` checks that Arrow's value is `None` but does not inspect the Arrow field's `nullable` flag. The public schema, `api_field_catalog`, JSON result, and Arrow payload are checked; asserting the Arrow field's nullability would directly cover the old wire-contract guarantee that prompted the major version.

## Contract checks

- `api/catalog.py:159–200` moves only `ATX.US.FUNDAMENTALS/standardized` to `3.0.0` and makes `value` nullable. `get_schema` and `public_schema` derive the public version, field description, nullability, and hash from that definition.
- On an existing warehouse, 0321 inserts the `3.0.0` schema and fields, validates their exact core metadata, then deactivates the `2.0.0` schema row without changing its hash or fields (`bodies_0321.py:15–89`). It does not call `_seed_public_contract` or update price rows. On a fresh build, earlier seeds use the current `DATASETS`, and 0298 assigns its current hash; 0321 validates the already present row.
- The ordinary range SQL ranks visible standardized revisions before projecting `value` and has no non-NULL filter (`api/service.py:778–865`). The proposed early numeric/later NULL fixture matches that ordering. The batch worker rejects queued jobs whose pinned `2.0.0` version/hash no longer matches the current schema (`api/batch.py:629–640`).
- The migration runner's checksum scope excludes imported `DATASETS` from prior migration bodies (`migrations/_runner.py:90–137`), so changing the public catalog definition does not by itself rewrite historical migration checksums. Root's guarded run must confirm DuckDB binding, current schema pin, and fresh/upgrade execution.
