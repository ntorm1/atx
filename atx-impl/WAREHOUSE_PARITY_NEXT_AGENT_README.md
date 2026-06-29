# Warehouse Parity Handoff

This handoff covers the US-equity fundamentals/public-alt-data warehouse parity goal. The working rule remains: do not touch C++/CMake unless the user explicitly redirects; continue in `atx-impl/db` and `atx-impl/scripts`, using `archive/research` and `.superpowers/sdd/progress.md` as the spec/progress spine.

## Current State

- S4 fundamentals depth is complete in code: canonical statement map coverage, bank/insurance/REIT overlays, four-date period model (`datadate`, `rdq`, `pdate`, `fdate`, `ldate`), and SQL XBRL calculation-linkbase validation.
- S5a is complete in code: `shares_outstanding_history` derives PIT share counts from normalized SEC XBRL statement points and is exposed through jobs, lake export, watermarks, quality checks, scripts, and `shares_outstanding_asof`.
- S5b is partially complete in code: `corp_action_type_dim` and `adjustment_factor_history` create an event-level factor spine over existing `corporate_actions`. The default live DB has only inferred cash-dividend rows from local ticker history right now.
- Full DB test suite passed after S5b: `python -m pytest atx-impl\db\tests -q`.
- Live default DB was migrated through `0011`; live counts at handoff were: `shares_outstanding_history=342`, `corporate_actions=142`, `corp_action_type_dim=5`, `adjustment_factor_history=142`.

## Important Caveat

Right before the stop request, a live CLI smoke exposed a data issue in S5b: one KO row with `factor_price=0.5` and a huge inferred `cash_div_amount` is currently classified as `CASH_DIV` because the source `corporate_actions` row was inferred as `cash_dividend_inferred`. Next agent should add a conservative split-artifact heuristic before building on adjustment factors:

- If `adjustment_factor` is close to a common split price factor (`0.5`, `0.333333`, `0.25`, `2.0`, etc.) and inferred `cash_amount` is implausibly large relative to price/evidence, classify as `SPLIT` or quarantine as `OTHER`.
- Ideally preserve both the raw source action and normalized event classification with a reason field.
- Add a regression test using the KO-like `0.5` factor case.

## Key Files

- Schema/migrations/catalog: `db/schema.py`, `db/migrations.py`, `db/parity.py`, `db/PARITY_GAP.md`
- S4 fundamentals: `db/fundamental_statements.py`, `db/xbrl_validation.py`
- S5 share counts: `db/shares_outstanding.py`, `db/asof.py`, `db/jobs.py`, `db/watermarks.py`, `db/quality.py`
- S5 adjustment factors: `db/adjustment_factors.py`, plus the same as-of/jobs/watermark/quality/lake/script surfaces
- Scripts: `scripts/build_quant_warehouse.py`, `scripts/query_asof.py`
- Tests added/updated: `db/tests/test_shares_outstanding_history.py`, `db/tests/test_adjustment_factors.py`, `db/tests/test_xbrl_validation.py`, `db/tests/test_fundamental_period_dates.py`, import/migration/job/quality tests

## Recommended Next Slice

1. Fix S5b classification for split-like artifacts in `adjustment_factors.py`.
2. Add `adjustment_factor_history_asof` smoke coverage for split/quarantine cases.
3. Add daily cumulative factor materialization only after event classification is trustworthy. Target fields: split-only price factor, share factor, and total-return dividend factor; keep raw close, split-adjusted close, and total-return adjusted close separate.
4. Then move to delisting proxy tables from `listing_status_intervals`, but do not claim CRSP-quality `DLRET` until Form 25/15, OTC, merger consideration, or a defensible imputation policy exists.

## Verification Commands

Run from `C:\atx`:

```powershell
python -m pytest atx-impl\db\tests -q
```

Live smoke examples from `C:\atx\atx-impl`:

```powershell
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-28 --view shares-outstanding --symbols AAPL --share-count-types shares_outstanding --limit 5
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2013-03-15 --view adjustment-factors --symbols KO --event-types CASH_DIV --limit 3
```

The second smoke currently demonstrates the split-artifact caveat above.

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

Work autonomously for hours when needed. Avoid stopping to ask questions unless continuing would risk destructive or clearly mis-scoped work. Prefer making conservative, well-documented assumptions, implementing complete vertical slices, validating with tests and live smoke checks, and recording progress in markdown tranche ledgers with commit SHAs. Use subagents when useful to preserve main-agent context, but keep write scopes clear. Do not touch C++ code unless the user explicitly redirects.
