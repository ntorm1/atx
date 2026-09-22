# Issuer catalog upgrade review fixes

Revised patch: `integration.patch` (SHA-256 `CAE918966E345766936DC00CF5719A6B26CEE3EEC1E55263308D95B6093E0AE2`). `git apply --check` passes. Static work only; Python, DuckDB, and pytest were not run.

- 0322 now inserts only `ATX.US.ISSUER_CONTENT` and its six schemas, field rows, schema digests, and deterministic unpriced historical price rows. It leaves existing catalog and price rows untouched, including provenance timestamps. Conflicting issuer metadata raises an error in the runner transaction.
- The focused test starts from the fully bootstrapped fixture, removes only the 0322 migration record and issuer publication rows to simulate the persisted 0321 predecessor, sets an existing active price, and applies 0322 through `apply_pending_migrations`. It checks migration tracking, every published metadata column, six baseline prices, unchanged unrelated rows including timestamps, and the schema pin. Direct replay then checks full row idempotence.
- No historical migration or live registry file was changed. Root integration must register 0322 after 0320 and 0321 and run the guarded pytest check.
