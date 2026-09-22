# Atomic publication — independent source review

Reviewed 2026-09-20, one source-review pass. No tests, DuckDB execution, live access, implementation edits, or external/API requests were performed. Graph tools were unavailable in this reviewer session, so targeted source reads were used. Migration review is limited to 0314 and its registration; schema review is limited to removal of the two bar indexes. Unrelated bootstrap 0280/runner/test changes are excluded. Earlier system-Python/DuckDB 1.5.1 failures are not verification evidence.

## Critical

None in the repaired publication architecture. The prior daily-risk Critical finding is closed at code-design level: the persistent unindexed result stage feeds independently committed, ordered PK-prefix inserts into a physical shadow, with checkpoint/close/reopen between 16-prefix groups; publication swaps physical tables in a short transaction instead of deleting and reinserting the indexed live table. Migration 0314 removes only the approved five non-unique secondary indexes. Required PK, NOT NULL and default semantics remain present. The shared publisher keeps related bar metadata writes and the table swap inside one transaction, and bar staging retains other-source rows.

This closes the architecture finding, not production validation. Root still owns the guarded full-scale gate under the approved memory/process ceilings, with project DuckDB 1.5.5. A production receipt is not a prerequisite for committing the repaired code. The Important defects below must be repaired before executing the production daily-risk stage.

## Important

1. **The daily-risk shadow always fails the exact physical-contract check because its columns are reordered.** `equity_price_metrics.py:441-479` puts the added liquidity, risk and rank columns before `is_latest_revision` and the other metadata columns. The real table's initial DDL (`migrations/bodies_0001_0137.py:2958-2981`) puts metadata immediately after `pct_from_high_252d`; subsequent migrations append the liquidity/risk/rank columns (`:4898-4979`). `_bulk_publication._contract` compares the complete ordered `DESCRIBE` output. Consequently, the first publication reaches `shadow contract differs from live table` after doing the entire build. Reproduce the migrated physical column order in `_BULK_SHADOW_DDL`, retaining the explicit named insert columns, PK, nullability and defaults. Keep the strict contract comparison. Verify a successful daily-risk refresh against the real migrated schema, including an empty replacement.

2. **The SQL publishes the raw input availability clock instead of the propagated clock.** `market_joined` selects `l.*`, which already contains the input `available_at`, and then adds another column named `available_at` (`equity_price_metrics.py:373-377`). This produces duplicate names; the original column retains the unsuffixed name through downstream CTEs. The final selection of `available_at` (`:432`) therefore uses the raw bar clock, losing expanding history, peer-market and rolling-risk availability. For the existing delayed S3 peer on day 80, S1 can be published three days too early. Give the computed clock a unique name (for example `metrics_available_at`) and explicitly project it as the final `available_at`, or remove/rename the raw clock before replacing it. Retain the delayed-peer parity assertion and verify that the delayed dependency remains reflected on subsequent affected rows.

3. **The new failure-preservation test fails during setup and never exercises a failed refresh.** `test_equity_price_metrics.py:243-253` first publishes a row, reads its primary key, then inserts that same primary key under `foreign-source`. The live table correctly rejects this insert before the `pytest.raises` block at `:254`. Change the setup to a valid live state, then inject a failure during shadow construction or swap and assert that the preceding source rows and retained foreign rows remain unchanged. For a real collision fixture, use a distinct new input whose future derived metric ID is owned by a foreign-source row while the previous successful derived row has another ID. Update the obsolete comment referring to a source delete.

## Other reviewed repairs and coverage

- The 252-observation high is now separate from the expanding drawdown peak. The parity fixture exceeds 252 observations and contains an early high that ages out; the prior formula finding is repaired in source.
- The as-of parity expectation now filters both trade date and availability before the pure transform, with a pre-cutoff bar whose availability is after the cutoff. The prior eligibility-test finding is repaired in source.
- The existing symbol-filtered calculation universe remains the intended contract; no finding is raised against it.
- Activation registers the stage immediately before quality and passes explicit cutoff/run ID with useful diagnostics.
- Existing ticker tests cover retained other-source rows and gate failure preservation. Shared publisher tests cover success, metadata callback rollback, contract rejection and view readers. Existing fatal-recovery tests remain applicable to the unchanged recovery path. These are source observations, not claims that tests passed.

## SHA-256 hashes reviewed

Whole-file hashes identify reviewed snapshots; they do not expand the scoped migration/schema review.

| File | SHA-256 |
| --- | --- |
| `atx-db/src/atx_db/_bulk_publication.py` | `07DF36145DF15D05FD7234269285644CBAC6A1D6EDC494DFB994ADB4F6F1A9ED` |
| `atx-db/src/atx_db/ticker_history_bulk.py` | `8F4068DA121E117434F42562F471ABB6D20AE097BECBB267CECA0A24166487F6` |
| `atx-db/src/atx_db/migrations/bodies_0314.py` | `FBE092C9DBD379967CD1B1B5B8EC945D904BEC4417D23B723E8137C367B6B151` |
| `atx-db/src/atx_db/migrations/registry.py` | `099810168A982785BCE4EDFC77A4D8D00358B718C04F14D2932D92EB3177A6CE` |
| `atx-db/src/atx_db/migrations/__init__.py` | `A6D6F1662E380E447E85172A55A4745C271E5D40BAC3AC4FAE2B58FD3DA71A6D` |
| `atx-db/src/atx_db/schema.py` | `23DDC550D0FEBE9E8C8EDCF9452F9C01ADB9E9B36792D0C2F6711A088CCBEBC4` |
| `atx-db/src/atx_db/equity_price_metrics.py` | `49CF0C06AC4F3E6B2A941CAC553265D959878A2F807F775DA50E7393F37A3D62` |
| `atx-db/src/atx_db/activation.py` | `28C9CBA792A20E9DCB0E2FAB6A472FF6AF0BA49CD6C0B2238EDF1A303FC9E864` |
| `atx-db/tests/test_bulk_publication.py` | `EB15CE418F193BEF97B2D4C88C3CD67528740A05A2DAAD58C8423CDEE3D361D0` |
| `atx-db/tests/test_ticker_history_bulk.py` | `F77540BD16F9629B5A2B10DB6C3E330D09D4584E8F284633CFB527053DA408EE` |
| `atx-db/tests/test_price_failure_recovery.py` | `D846A4481649F86A61A80692AD17F0D012D8290439E1F92D35D71021C4B97AD3` |
| `atx-db/tests/test_equity_price_metrics.py` | `B840B46788FB8A2036A5229C4C7E7E8C91887405D11CB9E897F01033E61B21DF` |
| `atx-db/tests/test_activation_ladder.py` | `289C0E0E2CFCEEF2F40FA15017160AECCF62CF9CB0602E24C9C897CC695DD60B` |
