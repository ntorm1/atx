# AR3 annual cohort and bounded coverage implementation

Date: 2026-09-20. Codex implementation; no Claude process/API, production warehouse access, source audit or network request. DB test slot released to controller after focused checks. No full suite executed.

## Result

Migration 0311 persists `item_coverage_annual_cohort` and `item_coverage_cohort_years`, catalog/PIT-exemption metadata, and per-measurement cohort status/ranking date. Only this body's registry/import lines were added. The independent annual universe is `us_common_equity_top3000_market_cap_annual_v1`; it does not rename the legacy liquidity universe.

The cohort SQL chooses the last observed canonical-market session of each explicit calendar year, then positive finite market cap and known common-stock US-listed membership at session+22h, ranking market cap descending/security ID ascending. US-listed endpoints are half-open. Future revisions are excluded before ranking; historical eligible market rows remain usable even if a later revision changed their latest flag. Common-stock classification is required; ADR/REIT/LP/bar-only/unclassified candidates do not qualify. No current directory data is backdated. Input market/listing row IDs, clocks, annual source/rule, selection counts and exclusions are retained. Missing sessions, fewer than 3000 names, and the current incomplete year stay explicit. Any cohort rebuild atomically invalidates measurements for the rebuilt years, including same-date rebuilds.

Coverage now aggregates in DuckDB; only item/basis/year aggregates and cohort year summaries enter Python. The numerator joins `(security_id,fiscal_year)`, distinct-counts non-null values (zero counts), and cross-joins all requested registered items/bases/explicit years, including absent fact years and zero-fact items. Legacy descriptive interval support filters `is_member` and retains that table's inclusive endpoint convention. A requested empty slice deletes stale measurements while preserving unrelated years/items/bases.

The unchanged authoritative gate is 110 annual items at 90% in **every** completed FY2015..as_of.year-1, with exactly 3000 constituents and complete persisted cohort evidence. Current year remains visible and excluded. Provider coverage and DQC call the same SQL gate; missing years, stale-year evidence, NULL status, undersized cohort and foreign source/universe cannot pass. Provider observation date determines required years; DQC uses the persisted latest explicit measurement date without a new library clock read. No public schema condition was changed by this task.

## Controller integration

```python
from atx_db.item_coverage_cohort import AnnualCoverageCohortOptions, refresh_item_coverage_cohort
from atx_db.item_coverage import ItemCoverageOptions, measure_item_coverage, refresh_item_coverage

cohort = refresh_item_coverage_cohort(store, AnnualCoverageCohortOptions(
    as_of_date=as_of_date, run_id=run_id,
))
options = ItemCoverageOptions(as_of_date=as_of_date, run_id=run_id)
frame = measure_item_coverage(store, options)
written = refresh_item_coverage(store, options, frame=frame)
```

Run after real market-daily and US-listed interval construction, before provider coverage/quality. Builders assume a migrated store and do not call initialize/migrate. Explicit as_of_date is now required for measurement. Defaults cover FY2015 through the explicit current year, all 4 bases and all registered items. Result `cohort` includes rows_written and per-year ranking date/selected count/status. Empty historical cohorts produce measured zero coverage and a degraded gate; stage completion does not certify parity.

`scripts/measure_item_coverage.py --as-of-date YYYY-MM-DD --rebuild-cohort --memory-limit 1GB --threads 1 --write-docs` is the operator path, after governed migration. It checks pending migration versions read-only before opening a writer; no implicit live migration is authorized. Keep the controller process-tree guard and one heavy workload rule. `docs/ITEM_COVERAGE.md` documents the convention and limits until live numbers regenerate it. No full source rows are fetched to Python.

## Validation

- One selected covering run: item coverage, quality identities, provider SLOs/provider coverage, module boundaries, schema-contract v2. 95 passed, 12 fixture failures,1 default slow skip. The 12 failures were missing test registry seeds and incorrect `tests.*` helper imports, not production exceptions. All 12 passed on failed-only rerun.
- Two new self-review regressions passed: NULL cohort status/foreign source cannot pass; future market revision cannot replace the eligible ranking input. The failed-only cohort-refresh test also covers atomic invalidation of old measurements.
- Total 109 unique passing checks,1 default slow skip. Actual 3000 boundary/ties, missing historical listing, half-open listed endpoints, inclusive legacy endpoints/is_member, same-year numerator, zero/null, absent item/year, current year, SQL-vs-pure values, eligible historical revisions, narrowed/empty replacement, complete-year gates, consumer alignment and migration/catalog behavior covered.
- All checks used `-n0`, 1 DuckDB test thread and reviewed Windows 3 GiB process-tree guard. Guard receipts/logs: ar3-focused-tests-1/2/3.{json,log,err}. Initial native peak field 0.941 GiB, subsequent 0.679/0.650 GiB; these are native Job Object accounting, not a resident-memory guarantee. Guard headroom remained safe. No production workload ran concurrently.
- Ruff clean across 11 owned source/script/test paths; strict mypy clean across 5 source modules. Module-boundary and schema-contract tests passed in initial selected run. No full non-slow suite or live quality gate/release performed.

## Remaining evidence and limits

Live annual cohorts still require defensible dated US-common listing inputs; source code and fixture passes cannot create them. Ranking follows the explicitly stated last-observed-session convention, not a claim that a partial source archive reaches each exchange's actual final session. Exclusion counts describe canonical-market securities with a market observation known at that session; they are not an all-security-master census. Coverage is retrospective fiscal-year reporting as of the explicit measurement date over PIT-selected annual constituents, not a claim that annual filings were already known at the year-end ranking timestamp.

The pre-existing pure fact+membership helper was replaced in production by `measure_item_coverage`; repository callers are migrated. Tests for prior single-year/100-name and single-name passing gates were corrected to full 2015+ annual cohort evidence. `tests/test_provider_coverage.py` was necessarily adjusted as an additional focused consumer test; no unrelated provider behavior changed. Independent review remains due. DATA_DICTIONARY regeneration belongs to the controller's final truth pass after all migrations settle.
