# Warehouse Parity Handoff

This handoff covers the US-equity fundamentals/public-alt-data warehouse parity goal. The working rule remains: do not touch C++/CMake unless the user explicitly redirects; continue in `atx-impl/db` and `atx-impl/scripts`, using `archive/research` and `.superpowers/sdd/progress.md` as the spec/progress spine.

## Current State

- S4 fundamentals depth is complete in code: canonical statement map coverage, bank/insurance/REIT overlays, four-date period model (`datadate`, `rdq`, `pdate`, `fdate`, `ldate`), and SQL XBRL calculation-linkbase validation.
- S5a-S5c pricing/corporate-action spine is complete in code: PIT shares outstanding, action-type dimensions, event-level adjustment factors, daily cumulative adjustment factors, and adjusted daily-panel return columns.
- S5d-S5e delisting spine is complete in code for public evidence and injectable observed terminal returns: `delist_code_dim`, `delisting_events`, and `delisting_return_observations`. Default returns remain null unless a licensed/manual file is loaded or an explicit imputation option is used.
- S5f-S5k estimates injection, reconciliation, and public guidance extraction surfaces are complete in code for detail, consensus snapshots, recommendation/price-target events, aggregate recommendation/price-target summary snapshots, PIT-safe estimate vendor-security-id links, and SEC 8-K guidance text extraction: `est_detail` loads IBES-like broker/analyst detail rows; `est_consensus` loads IBES `statsumu`-style or normalized summary snapshots; `est_recommendation` loads IBES `recddet`/`ptgdet`-style events; `est_recommendation_summary` loads IBES `recdsum`/`ptgsum`-style or normalized aggregate recommendation distributions and price-target summaries; `est_security_link` reconciles estimate vendor identifiers (`IBES_TICKER`, CUSIP, FactSet fsym, CIQ trading item, etc.) to `security_id` using only PIT-visible evidence; `est_guidance` now accepts local SEC 8-K Item 2.02/7.01 text corpora via `--estimate-guidance-file`, extracts conservative revenue/EPS/operating-income/net-income ranges with confidence and evidence text, and preserves source-file hashes.
- S6a 13F filer entity-resolution is complete in code and populated from public data: `filer_13f_cik_alias` (`db/filer_alias.py`) is a bitemporal CIK alias / subadvisor / M&A-continuity spine. The auto-derived layer is high precision (SELF identity per filer CIK + intra-CIK `NAME_HISTORY`); cross-CIK same-name links are emitted only as low-confidence `NAME_MATCH_CANDIDATE` rows; authoritative rollups arrive via an injectable curated seed CSV (`--seed-file`). `resolve_primary_cik` defaults to authoritative-only (confidence >= 1.0) so unrelated same-named filers never silently merge. Live build over the 10,672-CIK manager universe produced 10,672 SELF + 99 candidate rows (real multi-CIK filers: Dimensional Fund Advisors across 3 CIKs, D.E. Shaw, Citigroup, Northern Trust).
- S6b normalizes the 2023-01-03 SEC 13F `VALUE`-unit cutover (`thirteenf.value_unit_multiplier` / `apply_value_unit_cutover`): VALUE is scaled ×1000 to whole dollars for `period_of_report <= 2022-12-31` at holdings load. No live row change (all loaded periods post-2023); protects future pre-2023 backfills from a 1000× SUM understatement.
- S7a FINRA OTC Transparency off-exchange surfaces are complete in code as an injectable slice: `offexchange_venue` (MPID dim), `offexchange_volume` (per security/venue/period fact with bitemporal availability + FINRA restatement handling via `is_latest`/`restatement_seq`), `offexchange_security_period` (derived ATS-share rollup), and `v_offexchange_security_period` (`db/offexchange.py`). The loader ingests a FINRA `weeklySummary`-shaped or normalized CSV; an end-to-end smoke reproduces the blueprint AAPL fixture exactly (ats_share_pct 31.7470%). Default DB tables are empty until a FINRA file is injected; the live OAuth2 FINRA Query API connector is not yet wired (no network in tests).
- S7b (migration 0031) catalogued the four reference-classification tables and the off-exchange view, closing the standing `missing_table_catalog_entries`/`missing_field_catalog_entries` quality failures.
- S8a makes reference classification populatable without network: `EntityClassificationDataset` accepts an injectable CIK→SIC CSV (`sic_file` / `build_reference_classifications.py --sic-file`) alongside the live SEC fetcher. The SIC/Fama-French/NAICS taxonomy trees are now populated on the live DB (3 taxonomies, 125 nodes, 256 crosswalk rows); `entity_classification` stays empty until a SIC source is supplied for the universe.
- S8b adds a second offline SIC source: a SEC bulk `submissions.zip` fetcher (`EntityClassificationOptions.submissions_zip` / `build_reference_classifications.py --submissions-zip`) with lazy per-CIK lookup, so the full universe can be classified offline once the (one-time) bulk archive is downloaded — no network in tests.
- S9a adds the Compustat/FactSet **calculated-items / financial-ratio** surface: `fundamental_ratios` (`db/fundamental_ratios.py`, migration `0032`) is a long-format PIT fact (one row per `security_id,ratio_code,basis,period_end`) derived purely from `fundamental_ttm_points` (TTM flows) over `fundamental_statement_points` (instant balances) — no network, fully deterministic. v1 ships 15 ratios (margins, ROA/ROE, leverage, FCF/cash-flow, payout, book-value-per-share). Each ratio carries its **own** `available_at` = max availability of its specific inputs (true per-fact PIT), a `numerator_value`/`denominator_value` lineage pair, and an `is_meaningful` flag (false on non-positive denominators). As-of reader `fundamental_ratios_asof` + `query_asof --view fundamental-ratios`; DAG node depends on `sec_company_facts`. Live build materialized real rows over 5 securities (AAPL TTM @2026-03-28 net_profit_margin=0.2715, ROE=1.151, free_cash_flow=129.174B). v1 stores only the latest-revision input vintage (one ratio per period); restatement-vintage history is a planned non-breaking refinement.
- S9b expands the same engine to **23 ratios** (no schema change) — adds an `efficiency`/activity family (`asset_turnover`, `equity_turnover`) plus cash-flow-coverage and reinvestment ratios (`operating_return_on_assets`, `operating_cash_flow_margin`, `operating_cash_flow_to_assets`, `operating_cash_flow_to_liabilities`, `capex_to_operating_cash_flow`, `retention_ratio`), all from the existing 15 canonical metrics. Live rebuild grew `fundamental_ratios` to 5,029 rows / 23 codes.
- S9c adds a `growth` family (5 YoY ratios: revenue/net_income/operating_income/operating_cash_flow/assets `_growth_yoy`) via a pure-pandas prior-year pairing (`_attach_prior_year`, 350–380-day window); `ratio_kind='growth'`, value = (current − prior)/|prior|, not-meaningful off a negative base. Live rebuild grew `fundamental_ratios` to **6,468** rows / **28** ratio codes (1,439 growth rows).
- Full DB test suite passed after S5k–S9c: `python -m pytest db\tests -q` (232 passed; +47 in `test_fundamental_ratios.py`).
- Live default DB is migrated through `0032`; live `fundamental_ratios=6,468`. Estimate counts after S5k smoke are `est_actual=1,240`, `est_surprise=1,222`, and `est_detail`/`est_consensus`/`est_guidance`/`est_recommendation`/`est_recommendation_summary`/`est_security_link=0` by design until licensed/manual files, accepted crosswalk evidence, or local SEC guidance text files are injected.

## Important Caveat

Default DB estimate detail/consensus/guidance/recommendation/summary/security-link rows remain empty unless a licensed/manual file, local SEC guidance corpus, or accepted estimate crosswalk is injected. The schema, loaders, as-of APIs, watermarks, lake export, and quality checks are ready, but no real IBES/FactSet/CIQ/Zacks files or SEC exhibit text corpora have been loaded into the shared DB. Estimate as-of APIs resolve vendor-keyed rows through `est_security_link` only when the link's own `available_at`, `as_of_date`, and validity window are visible to the query.

During S5g live smoke, an initial DuckDB migration shape triggered a DuckDB internal error while adding indexes in the same transaction as `ALTER TABLE`. The corrected implementation splits schema migration `0017` and index migration `0018`. The failed WAL was preserved as `db/atx_impl.duckdb.wal.failed-s5g-migration.20260629-074343.bak`, and the DB checkpoint was backed up as `db/atx_impl.duckdb.pre-s5g-wal-recovery.20260629-074343.bak` before moving the WAL aside.

During S5k live smoke, the same DuckDB `ALTER TABLE` + index transaction shape recurred for `est_guidance`. The corrected implementation splits guidance extraction columns/catalog migration `0027` and index migration `0028`. The failed WAL was preserved as `db/atx_impl.duckdb.wal.failed-s5k-migration.20260629-094756.bak`, and the DB checkpoint was backed up as `db/atx_impl.duckdb.pre-s5k-wal-recovery.20260629-094756.bak` before moving the WAL aside.

## Key Files

- Schema/migrations/catalog: `db/schema.py`, `db/migrations.py`, `db/parity.py`, `db/PARITY_GAP.md`
- S4 fundamentals: `db/fundamental_statements.py`, `db/xbrl_validation.py`
- S5 share counts / adjustment factors / delistings / estimates: `db/shares_outstanding.py`, `db/adjustment_factors.py`, `db/delisting.py`, `db/estimates.py`, `db/estimate_security_links.py`, plus `db/asof.py`, `db/jobs.py`, `db/watermarks.py`, `db/quality.py`
- Scripts: `scripts/build_quant_warehouse.py`, `scripts/query_asof.py`
- Tests added/updated across recent tranches: `db/tests/test_shares_outstanding_history.py`, `db/tests/test_adjustment_factors.py`, `db/tests/test_daily_adjustments.py`, `db/tests/test_delisting.py`, `db/tests/test_estimates.py`, `db/tests/test_xbrl_validation.py`, `db/tests/test_fundamental_period_dates.py`, import/migration/job/quality tests

## Recommended Next Slice

00. **Expand fundamental concept coverage (unblocks the missing ratio families — highest core-parity value).** The cached `sec_company_facts` carries only the **16 fetched concepts** (the 15 already-canonical metrics), so liquidity (current/quick/cash), gross-margin, inventory/receivables turnover, interest coverage, and EV/EBITDA are currently impossible — not a design gap, a data-fetch gap. Widen `DEFAULT_CONCEPTS` in `db/fundamentals.py` (AssetsCurrent, LiabilitiesCurrent, CashAndCashEquivalents, CostOfRevenue/CostOfGoodsAndServicesSold, GrossProfit, InventoryNet, accounts-receivable, InterestExpense, LongTermDebt/DebtCurrent), add the matching `fundamental_statement_map` rows, re-fetch companyfacts for the existing 5 securities (**SEC network — build/smoke only, never in tests**), then re-run `refresh_fundamental_statement_points` → `refresh_fundamental_ttm_points` → `build_fundamental_ratios`. The S9a/S9b engine will automatically compute the new ratios once the metrics exist; add their `RatioDef`s. This produces real rows and is the single biggest core-ratio-parity unlock.
0b. **Price-based valuation multiples (deferred — blocked by a data-coverage gap, NOT design).** P/E, P/B, P/S, EV/EBITDA, dividend/earnings/FCF yield would join market-cap (`equity_daily_bars` adjusted close × PIT `shares_outstanding_history`) to the fundamentals. BLOCKER discovered in S9b: cached `equity_daily_bars` is a **2012–2013** 40-security sample while fundamentals are **2017–2026** — zero temporal overlap, so a PIT price×fundamental join yields ~0 live rows. Load overlapping daily price history first (the price loader is `db/features.py`/`tbltickerhistory*`), then build the multiples (per-fact `available_at = max(price.available_at, fundamental.available_at)`; inject a tiny price+shares fixture in tests, never a price API). Reuse `compute_ratio_rows`-style pure math.
0. **Off-exchange + 13F follow-ups.** (a) Wire the live FINRA OTC Transparency connector for S7a: FINRA Query API `otcMarket` group (OAuth2 client-credentials, token endpoint `https://ews.fip.finra.org/fip/rest/ews/oauth2/access_token`), `POST {base}/data/group/otcMarket/name/weeklySummary`, with the 10 GB/mo cap guard and tiered publication-delay period computation (Tier1 ~14d), feeding `scripts/build_offexchange.py` — keep it out of the test path. (b) Load a curated subadvisor / M&A-continuity seed CSV into `filer_13f_cik_alias` via `python scripts/build_filer_aliases.py --seed-file <csv>` (columns: `parent_cik,child_cik,alias_type,valid_from,confidence,evidence[,valid_to,available_at]`) and verify `resolve_primary_cik` rolls children up PIT-correctly.
1. Load a small approved licensed/manual estimates sample through `--estimate-detail-file`, `--estimate-consensus-file`, `--estimate-recommendation-file`, and `--estimate-recommendation-summary-file`, plus accepted `identifier_resolution_decisions`, then verify `est_security_link` resolves vendor identifiers (`IBES ticker`, CUSIP, FactSet fsym, CIQ trading item) without leaking future links into earlier as-of queries.
2. Add broker-name alias joins where recommendation IDs do not match detail estimate IDs.
3. Feed an approved local SEC 8-K Item 2.02/7.01 exhibit corpus through `--estimate-guidance-file`, inspect false positives/negatives, then extend the parser for open-ended guidance, fiscal calendars, and additional measures without public API calls in tests.
4. Separately, clean up existing non-estimate quality failures: duplicate identifier history keys, overlapping identifier intervals, XBRL validation rows, fundamental statement-map gaps, provider-parity rows without open tables, and four table-catalog gaps.

## Verification Commands

Run from `C:\atx`:

```powershell
cd C:\atx\atx-impl
python -m pytest db\tests -q
```

Live smoke examples from `C:\atx\atx-impl`:

```powershell
python scripts\build_estimates.py --db-path db\atx_impl.duckdb --run-estimate-security-links
python scripts\build_estimates.py --db-path db\atx_impl.duckdb --estimate-guidance-file path\to\local_8k_guidance.csv
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-29 --view estimate-security-links --limit 5
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-29 --view estimate-guidance --limit 5
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-29 --view estimate-consensus --limit 5
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-29 --view estimate-recommendations --limit 5
python scripts\query_asof.py --db-path db\atx_impl.duckdb --as-of-date 2026-06-29 --view estimate-recommendation-summary --limit 5
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
