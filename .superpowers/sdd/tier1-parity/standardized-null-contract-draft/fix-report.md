# Standardized NULL contract draft

Integration patch: `integration.patch` (SHA-256 `1838D53E0C1ECC7F97EC379E4DD341AF31C690C691C3B11CB0ED674C53BCCDF2`). `git apply --check` passed against live `1a0e4f7369660a2dcebd3c5e36172e39071c9fbe` plus the uncommitted 0321 source. No Python, DuckDB, pytest, or network command was run in this draft lane.

- The public `ATX.US.FUNDAMENTALS/standardized` record schema advances from `2.0.0` to `3.0.0`. `value` is nullable and explicitly describes the unavailable state. This is a major version because clients may treat its old Arrow/record non-null guarantee as required. The overall dataset/API product version remains unchanged.
- Existing 0319/0320 warehouse upgrades through 0321 insert only the new standardized schema and field metadata with the matching public SHA-256, retain old `2.0.0` fields/hash as historical metadata, and mark old schema inactive. New builds that already seed `3.0.0` validate it without replacing rows. Other datasets, schemas, and all price rows remain untouched; conflicting preexisting `3.0.0` metadata raises within the migration transaction.
- Existing queued `2.0.0` batch jobs remain pinned to the old schema version/hash and fail the worker's explicit contract-change guard. Clients must submit a new job for `3.0.0`; completed artifacts and historical `2.0.0` catalog metadata remain intact.
- The focused fixture constructs a persisted `2.0.0` standardized catalog and physical `NOT NULL` predecessor, applies 0321, checks both catalog versions, hash/nullability, unrelated metadata/prices, schema pin, and reads an early numeric EPS and later NULL EPS through the ordinary public range service and Arrow stream. The two existing API tests expecting a single current catalog row and batch manifest version are updated.

Root must apply the patch and run the guarded focused checks before integrating the separate issuer 0322 draft. No registry, jobs, or activation files were changed here.
