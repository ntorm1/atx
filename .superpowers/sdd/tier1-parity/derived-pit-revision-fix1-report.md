# Derived PIT review fixes — prepared tests

Date: 2026-09-20. Scope: Important I1 and Minor M1 from `derived-pit-revision-review.md`. The independent review closed the original Critical finding at the implementation/source level and found no new Critical. This follow-up changes only the owned focused test file and reports. Application, migration, registry, and production source remain unchanged.

**Status: requested assertions prepared; execution remains pending with root.** No tests, database connections, probes, lint, package imports, application/process work, network calls, or commits were run. No concrete implementation defect emerged during this test preparation. Companyfacts ingestion was not touched.

## Important I1 — populated0314-to0315 upgrade

Added `test_populated_0314_upgrade_preserves_legacy_contract_and_reentry` in `atx-db/tests/test_derived_pit_revisions.py`.

The test creates a separate temporary warehouse with DuckDB configured to1GB/1thread before initialization. It temporarily limits the real migration registry to0314, runs ordinary `DuckDBStore.initialize`, restores the registry, and inserts exactly two legacy derived rows with the same security/metric and distinct physical IDs/periods. It then uses ordinary `apply_pending_migrations` to apply0315, rather than calling the migration body directly or altering an already-upgraded fixture back into an approximation of0314.

Prepared assertions cover:

- Initial migration head0314, NOTNULL numeric value, and presence of the existing optional lookup index.
- Exactly0315 applies; every original column, including physical IDs, values and source/load lineage, is preserved.
- The canonical table remains physical, its required `derived_value_id` primary key remains, and value becomes nullable. A rolled-back NULL update proves nullability, and a duplicate-ID insert must fail under the retained PK.
- The optional lookup index is removed, without asserting a runtime performance benefit.
- Retained rows receive `history_status='legacy_latest_only'` and valid numeric status, with no fabricated revision group, definition fingerprint, target bucket or interval end.
- Both legacy rows remain separately visible for API `latest` and `first_reported`, exercising the physical-ID fallback for their shared NULL group.
- Legacy rows contribute zero reconstructed coverage counts despite remaining API-visible.
- Field-catalog value nullability agrees with the new schema, and `assert_schema_contract_version` verifies the live manifest pin.
- Ordinary migration re-entry returns no pending versions, ordinary store initialization re-entry succeeds, only one0315 ledger row exists, original rows remain unchanged, and the schema pin still verifies.

This is a real forward-upgrade regression with tiny data. Schema bootstrap cost is unchanged by the two-row size and remains part of root's sole guarded test workload. No existing315 fixture is used as a substitute for the314 starting schema.

## Minor M1 — direct changed-consumer assertions

Extended the existing `test_invalid_recovery_fallback_api_coverage_and_release` fixture rather than introducing a second arithmetic history. Its10/NULL/5 ratio sequence now has direct assertions that:

- API `first_reported`, after recovery is visible, still returns the first valid10 state and its February1 clock.
- A test-only daily metric reads the canonical ratio directly. Its three monthly rows must be10/NULL/5, and March's `fundamental_available_at` must be the March1 invalidation event.
- Quarterly factor input selection must expose a missing numeric value in March, retaining valid10/5 on adjacent months, the March1 metric clock, and `zero_denominator` in the selected canonical state lineage. This tests selection before any factor-value filtering.

Extended `test_chunk_equivalence_ties_and_event_aware_stub_period` with the existing December28-toDecember31 representative-date history:

- Before the rename, the old-date-only API range returns December28/value120.
- After the rename, the same range returns no latest row.
- A range containing both dates returns only December31/value130 for `latest`.
- `first_reported` over the old-date range still returns December28/value120.

The existing tiny projection membership SQL is shared by the two consumer fixtures. No formulas, production code, or consumer behavior changed in this follow-up.

## Root acceptance and files

The main report's pinned-executable command already includes `tests/test_derived_pit_revisions.py`, so its single root-owned focused run includes these assertions automatically. Keep the2.5GiB process guard,1GB/1thread setup, `-n0`, explicit `C:/atx/atx-db/.venv/Scripts/python.exe` for both guard/child, and fresh stdout/stderr/receipt paths. Do not run concurrently with the source writer. There is no additional review pass for these noncritical test-preparation fixes; acceptance is this issue-by-issue report plus root's execution result.

Changed paths in this follow-up:

- `atx-db/tests/test_derived_pit_revisions.py`
- `.superpowers/sdd/tier1-parity/derived-pit-revision-report.md`
- `.superpowers/sdd/tier1-parity/derived-pit-revision-fix1-report.md`
