# Standardized NULL contract review follow-up

Patch: `integration.patch` SHA-256 `7AE75E6EE510A5D3B16B87E44111397DA490DCD1B9D2CCE7D7D4062D651C583D`. `git apply --check` passes against the currently applied live contract patch. This draft changes only the focused test and adds a frozen 0319 catalog fixture. No Python, DuckDB, pytest, or network command was run here.

The fixture `atx-db/tests/data/standardized_2_0_0_0319_catalog.json` is a byte-identical copy of the root's read-only production capture at `.superpowers/sdd/tier1-parity/standardized-2.0.0-production-predecessor.json` (both SHA-256 `01FEF4FBA78BE9F0041DCCA18BFEB05C1A14E8FDEF02D4A23FFF56CEAFF450E6`). It contains the observed 0319 `2.0.0` schema row and all 33 field rows, with source and update timestamps and stored schema hash `36a994626594e7250cf558c4f2262e8a34ad7906e088e8a71ee3db5af7e0c985`.

The test seeds those exact rows into the current isolated template, restores physical `NOT NULL`, removes only ledger entry 0321, then applies `apply_pending_migrations` and asserts `[321]`, checksum tracking, and schema pin. It compares the historical row and all 33 fields with the captured fixture after upgrade, allowing only the documented old schema `is_active` and `updated_at` change. It snapshots all API schema/field metadata and prices, plus the target warehouse table/field catalog rows including timestamps, across a direct 0321 body replay; runner replay returns `[]`. The Arrow batch now explicitly asserts `value` field nullability and retains its NULL row assertion.

Root should apply this follow-up and run the focused guarded test. The earlier public contract and migration body are unchanged by this patch.
