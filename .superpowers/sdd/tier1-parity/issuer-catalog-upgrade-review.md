# Issuer catalog upgrade draft review

Patch reviewed: `issuer-catalog-upgrade-draft/integration.patch` (SHA-256 `27FBEE83FB770AE154582541EDFFD2B4B5F811F91530064AB99BED6F9434A620`). Static review only; no Python, tests, or database execution.

## Critical

None found in the migration body. The 0322 registry entry is intentionally owned by the root integration, outside this patch.

## Important

1. **The test does not cover an actual existing-warehouse upgrade.** In the patch's `test_issuer_catalog_upgrade.py`, `tmp_store` is a fully bootstrapped warehouse; the test deletes issuer rows and invokes `_issuer_content_public_catalog(con)` directly. That bypasses the migration registry, checksum tracking, transaction path, and the real persisted state at 0319 (followed by reserved 0320/0321). It can pass even if 0322 is unregistered or fails when applied by the runner. **Remedy:** add a focused upgrade test with a database stopped at the predecessor migration, containing its preexisting API catalog and a configured active price; run `apply_pending_migrations` through 0322 and assert the six issuer schemas, their field rows and hashes, one baseline issuer price per schema, preserved prior prices, the migration record, and the schema-contract pin. Keep the direct helper replay check as a separate idempotence check if useful.

2. **The migration rewrites unrelated public catalog rows on an existing warehouse.** `_issuer_content_public_catalog` calls `_seed_public_contract(conn)` with no dataset filter. The 0267 helper `INSERT OR REPLACE`s every dataset and schema row, deletes and reinserts every existing field row, and resets `source_loaded_at`/`updated_at` for the entire public catalog. This is broader than publishing `ATX.US.ISSUER_CONTENT`; a repeated invocation also changes persisted timestamps even though the test's snapshot excludes them. The price table is spared, but the stated idempotence and narrow upgrade are not true for catalog provenance. **Remedy:** seed only the issuer dataset (for example, add a dataset selector to the 0267 helper or use an issuer-scoped equivalent), update hashes only for issuer schemas, and check both issuer and unrelated catalog rows including provenance timestamps before and after a replay.

## Minor

1. **The current assertions cover only part of the persisted schema contract.** The test checks issuer dataset attributes, selected schema columns, selected field columns, and hashes, but omits schema title/description, PIT policy, supported encodings, max sync rows, and field semantic type. A bad persisted value in those columns would pass. **Remedy:** compare the complete persisted issuer catalog rows with the 0267 seed specification, or explicitly assert the omitted columns in the upgrade test.

## Checked and found sound

- The migration iterates `DATASETS`, so the issuer dataset's six schemas in `api/catalog.py` are included; the schema hash update follows the established 0298/0296 pattern and repairs the digest after reseeding.
- The historical unpriced price ID, billing unit, and 1900 start match 0270. `INSERT OR IGNORE` leaves an existing row with that ID untouched, and the code does not update or delete other price rows.
- `_refresh_schema_contract_v2_pin` follows the existing migration pattern and asserts the refreshed structural manifest. No schema-pin defect was evident by inspection.
