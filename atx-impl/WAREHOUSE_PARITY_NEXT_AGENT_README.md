# Warehouse Parity Handoff

This handoff covers the US-equity fundamentals/public-alt-data warehouse parity goal. The working rule remains: do not touch C++/CMake unless the user explicitly redirects; continue in `atx-impl/db` and `atx-impl/scripts`, using `archive/research` and `.superpowers/sdd/progress.md` as the spec/progress spine.

## Current State

- S4 fundamentals depth is complete in code: canonical statement map coverage, bank/insurance/REIT overlays, four-date period model (`datadate`, `rdq`, `pdate`, `fdate`, `ldate`), and SQL XBRL calculation-linkbase validation.
- S5a-S5c pricing/corporate-action spine is complete in code: PIT shares outstanding, action-type dimensions, event-level adjustment factors, daily cumulative adjustment factors, and adjusted daily-panel return columns.
- S5d-S5e delisting spine is complete in code for public evidence and injectable observed terminal returns: `delist_code_dim`, `delisting_events`, and `delisting_return_observations`. Default returns remain null unless a licensed/manual file is loaded or an explicit imputation option is used.
- S5f-S5h estimates injection surfaces are complete in code for detail, consensus snapshots, and recommendation/price-target events: `est_detail` loads IBES-like broker/analyst detail rows; `est_consensus` loads IBES `statsumu`-style or normalized summary snapshots; `est_recommendation` loads IBES `recddet`/`ptgdet`-style or normalized recommendation and price-target event rows with stable ids, source-file hashes, active-window fields, broker/analyst dimensions, quality checks, watermarks, lake exports, and as-of APIs.
- Full DB test suite passed after S5h: `python -m pytest db\tests -q`.
- Live default DB is migrated through `0022`; live estimate counts after S5h smoke are `est_actual=1,240`, `est_surprise=1,222`, and `est_detail`/`est_consensus`/`est_recommendation=0` by design until licensed/manual files are injected.

## Important Caveat

Default DB estimate detail/consensus/recommendation rows remain empty unless a licensed/manual file is injected. The schema, loaders, as-of APIs, watermarks, lake export, and quality checks are ready, but no real IBES/FactSet/CIQ/Zacks files have been loaded into the shared DB.

During S5g live smoke, an initial DuckDB migration shape triggered a DuckDB internal error while adding indexes in the same transaction as `ALTER TABLE`. The corrected implementation splits schema migration `0017` and index migration `0018`. The failed WAL was preserved as `db/atx_impl.duckdb.wal.failed-s5g-migration.20260629-074343.bak`, and the DB checkpoint was backed up as `db/atx_impl.duckdb.pre-s5g-wal-recovery.20260629-074343.bak` before moving the WAL aside.

## Key Files

- Schema/migrations/catalog: `db/schema.py`, `db/migrations.py`, `db/parity.py`, `db/PARITY_GAP.md`
- S4 fundamentals: `db/fundamental_statements.py`, `db/xbrl_validation.py`
- S5 share counts / adjustment factors / delistings / estimates: `db/shares_outstanding.py`, `db/adjustment_factors.py`, `db/delisting.py`, `db/estimates.py`, plus `db/asof.py`, `db/jobs.py`, `db/watermarks.py`, `db/quality.py`
- Scripts: `scripts/build_quant_warehouse.py`, `scripts/query_asof.py`
- Tests added/updated across recent tranches: `db/tests/test_shares_outstanding_history.py`, `db/tests/test_adjustment_factors.py`, `db/tests/test_daily_adjustments.py`, `db/tests/test_delisting.py`, `db/tests/test_estimates.py`, `db/tests/test_xbrl_validation.py`, `db/tests/test_fundamental_period_dates.py`, import/migration/job/quality tests

## Recommended Next Slice

1. Load a small approved licensed/manual estimates sample through `--estimate-detail-file`, `--estimate-consensus-file`, and `--estimate-recommendation-file`, then reconcile vendor identifiers (`IBES ticker`, CUSIP, FactSet fsym, CIQ trading item) to `security_id`.
2. Add summary recommendation/price-target normalization from IBES `recdsum`/`ptgsum`-style files and provider-specific rating-scale metadata, then reconcile summary and detail vintages.
3. Start SEC 8-K Item 2.02/7.01 guidance extraction into `est_guidance` with a tiny offline fixture corpus and no public API calls in tests.
4. Separately, clean up existing non-estimate quality failures: duplicate identifier history keys, overlapping identifier intervals, XBRL validation rows, fundamental statement-map gaps, provider-parity rows without open tables, and four table-catalog gaps.

## Verification Commands

Run from `C:\atx`:

```powershell
cd C:\atx\atx-impl
python -m pytest db\tests -q
```

Live smoke examples from `C:\atx\atx-impl`:

```powershell
python scripts\build_estimates.py --db-path db\atx_impl.duckdb
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-29 --view estimate-consensus --limit 5
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-29 --view estimate-recommendations --limit 5
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-29 --view estimate-actuals --measure-codes EPS_DILUTED --limit 5
```

## Tranche Tracking

Use `WAREHOUSE_PARITY_TRANCHES.md` as the append-only implementation ledger. At the end of every coherent tranche, add a row with:

- start commit and end commit SHA
- files or domains touched
- tests and live DB smoke checks run
- known caveats and the next recommended tranche

Do not rewrite older rows except to fix factual mistakes. The point is to make long-running unattended work resumable without archaeology.

## North-Star Goal Prompt

Use `C:\atx\archive\research`, `C:\atx\atx-impl\db`, and `C:\atx\atx-impl\scripts` to build a feature-parity rival to FactSet and S&P Compustat for all US equity fundamental and public alternative data. Design it as a robust schema-based warehouse for quant equity long/short shops like Renaissance Technologies and WorldQuant.

The north star is full end-to-end replication quality for US equity fundamentals, ownership, corporate actions, pricing adjustments, reference data, estimates where public or injectable licensed feeds allow it, off-exchange/short-interest, macro, and other public alt-data surfaces. Prioritize production-quality infrastructure, explicit point-in-time semantics, versioned migrations, bitemporal availability fields, lineage, watermarks, lake exports, quality checks, as-of APIs, tests, and clear DB design over thin demos.

Work autonomously for hours when needed. Avoid stopping to ask questions unless continuing would risk destructive or clearly mis-scoped work. Prefer making conservative, well-documented assumptions, implementing complete vertical slices, validating with tests and live smoke checks, and recording progress in markdown tranche ledgers with commit SHAs. Use subagents when useful to preserve main-agent context, but keep write scopes clear. Do not touch C++ code unless the user explicitly redirects. Ensure testing suite stays light, fast, and avoids hitting public apis.
