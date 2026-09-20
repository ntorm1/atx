# Standardized item coverage

The authoritative cohort is `us_common_equity_top3000_market_cap_annual_v1`. This document describes the measurement contract; no live coverage pass has been established by the implementation tests.

For fiscal year Y, the cohort uses the last observed market session in calendar year Y. Securities must have point-in-time US-listed membership, security type `common`, an eligible US exchange, and a positive finite market capitalization known by that session at 22:00. Market capitalization ranks descending, with security ID ascending to break ties; ranks 1 through 3000 are retained. ADRs, REITs, LPs and unclassified bar-only candidates do not enter this common-stock cohort. Fiscal year Y coverage uses the persisted Y cohort, including non-December fiscal year reporters under this explicit annual snapshot convention.

`item_coverage_annual_cohort` records constituent ranks, market caps, ranking dates, input row IDs and availability clocks. `item_coverage_cohort_years` records the source and rule, candidate/eligible/selected counts, listing/cap/rank exclusions and completion status. US-listed intervals include their valid_to date, which is the last included session recorded by the existing writer. Runtime source_loaded_at is deterministically the explicit measurement date at 22:00; available_at retains input availability. Current listing metadata is never backdated. A missing or undersized historical cohort remains unavailable evidence; it cannot pass against a reduced denominator. The older liquidity universe remains separately named and cannot satisfy this gate.

The gate remains **110 annual items at or above 90% coverage in every completed FY2015+ year**, each with exactly 3000 eligible constituents. An explicit measurement date determines the completed years: a September 2026 measurement requires FY2015 through FY2025; FY2026 is reported as incomplete and excluded from the completed-year gate. Missing fact years, missing registered items and members with no facts produce explicit zero-coverage rows. Zero values count; null or absent values do not. Numerators join on both security ID and fiscal year and never exceed the denominator.

Production ranking, revision selection and aggregation run in DuckDB. Only item/basis/year aggregates and year summaries enter Python. Measurement uses facts available by the explicit measurement date at 22:00, selecting an eligible historical revision before considering future revisions. This is retrospective fiscal-year coverage over PIT-selected annual constituents; it does not imply the full year's filings were known at the ranking date. Provider standardized item-count SLOs and the item-coverage quality check consume the same annual cohort gate. Fresh cohort metadata must match each stored measurement date. Empty refreshes remove stale rows within their requested item/basis/year slice.

After a governed migration through 0311 and the required market/listing builds, run from `atx-db` under the controller's process-tree memory guard:

```powershell
.venv\Scripts\python.exe scripts\measure_item_coverage.py --db-path data\warehouse.duckdb --as-of-date 2026-09-20 --rebuild-cohort --memory-limit 1GB --threads 1 --write-docs
```

This command refuses pending migrations. The generated report replaces this contract explanation with actual measured item/year values, the cohort ID and rule, explicit completed-year scope, missing/incomplete years and the unchanged gate result. Failed coverage remains visible; successful command execution alone is not a coverage pass.
