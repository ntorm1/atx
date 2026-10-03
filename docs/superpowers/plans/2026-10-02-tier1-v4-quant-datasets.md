# Tier-1 v4: quant datasets, ratios and golden panels (Compustat / FactSet parity, US equity L/S, 2020+)

Owner goal (2026-10-02): make `atx-db` a Tier-1 competitor to S&P Global Compustat and FactSet for US equity
long/short quant teams on a large cross-sectional universe, primarily data since 2020-01-01: datasets, metrics,
ratios and golden panels.

**Spec (binding):** `.superpowers/sdd/tier1-v4/gap-analysis.md` — §1 product map, §2 ranked build list (items 1-13
carry the deliverables and acceptance gates), §3 in-flight resume commands, §4 do-not-build. A task below cites its
item; the item's deliverable and gate are binding unless the task text amends them. Lake inventory:
`.superpowers/sdd/tier1-v4/lake-inventory.md`. Ledger: `.superpowers/sdd/tier1-v4/progress.md`.

## Global Constraints

- **GC1 PIT.** Every published row carries `available_at` (UTC). Daily: visible at session d only if
  `available_at < 22:00 UTC of session d-1`. Monthly (stamped at month-end session m): uses only rows with
  `available_at < 22:00 UTC of m` and prices ≤ close of m. No backdating; a missing clock gets a documented
  conservative floor. Vintages are new rows, never overwritten. Delisted lines included.
- **GC2 Holdout (D6, strict for this plan).** No return-based statistic (IC, factor correlation, portfolio return,
  replication) on any date ≥ 2023-01-01. Value/coverage comparisons are allowed in any year.
- **GC3 Universe and window.** Score window 2020-01-01 → latest session. Report coverage per year 2020-2025 on panel
  `member_equity` and, once Task 2 lands, on `univ_all_common` and `univ_r3000p`. Lookback history only where a
  measure needs it (fundamentals from 2015; prices from 2018; `prices_history` for factor replication windows).
- **GC4 Host.** Every data job runs under `../.superpowers/sdd/tier1-parity/run_memory_guarded.py`, cap ≤ 1.0 GiB
  (prefer 0.6), DuckDB via `atx_db.alpha_panel.common.connect` (file-backed spill, threads ≤ 2), one heavy job per
  lane at a time. Pytest: offline fixtures, < 60 s per file. C: free ≥ 40 GB before any landing > 1 GB.
- **GC5 Files.** Never edit or stage any path in `.superpowers/sdd/tier1-v4/foreign-dirty-files.txt` (another
  session's uncommitted work). New behaviour goes into new modules under `atx-db/src/atx_db/alpha_panel/`. Stage
  conventions per `.superpowers/sdd/tier1-v3/LANE-RULES.md` (`stage_dir`, `connect`, atomic `copy_to_parquet`,
  `write_stage_manifest` with `input_manifests_sha256`). Do NOT edit shared registries (`atx_db/stagelake/*.py`,
  `DATA_DICTIONARY.md`, `PARITY_SCORECARD.md`): lanes run concurrently in one tree and a shared file would be swept
  into another lane's commit. Instead write a stage-registration stub `atx-db/docs/datasets/<stage>.registry.json`
  (stage name, path, keys, schema id, as-of macro SQL); Task 17 folds every stub into `stagelake`.
- **GC6 Commits.** `git add <paths> && git commit -m "<msg>" -- <paths>` (explicit pathspec), message ends with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never `git add -A`, never commit data. Report file
  committed with `git add -f`.
- **GC7 Sources.** SEC via `atx_db.sec_http` (≤ 5 req/s, existing user agent); other hosts ≤ 1 req/s. Raw landings in
  `data/raw/<source>/` with `receipts.jsonl`. Free sources only (D3).
- **GC8 Naming and docs.** Compustat mnemonics for standardized items, WRDS WFRS names for ratios, JKP names for
  characteristics. Each new stage gets `atx-db/docs/datasets/<stage>.md`: columns (name, type, unit, definition,
  source, clock, staleness), keys, PIT rule, measured coverage table, known limits.
- **GC9 Evidence.** Every gate is reported PASS/FAIL with the measured number and the exact command. A FAIL that
  cannot be closed inside the task is reported, not hidden; the stage still publishes with `gate_status` in its
  manifest.

## Lanes and order

Lane A (market): Task 1 → 2 → 6. Lane B (fundamentals): Task 3 → 4 → 5. Then Task 7 → 8 (characteristics,
needs A+B). Lane C (events/ownership): Tasks 9-15 (independent of each other except 10 after 9, 12 after 13 not
required). Lane D (product): Task 16 → 17 last.

### Task 1: Market infrastructure runs (liquidity, market index, index proxies, identity codes)

Spec: gap-analysis §3b and §3c (resume commands). Run the committed MKT/ID lane code to completion under the guard:
`liquidity all`, `market_index build`, `indexes build`, `returns_validation`, `identity_v3 codes`. Gates: the done
criteria for S3.2, S3.6, S3.7 in `docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md` and the gates in
`.superpowers/sdd/tier1-v3/task-MKT-report.md` / `task-ID-report.md`; additionally the value-weighted all-common
market return vs French Mkt (RF added back) monthly ρ ≥ 0.99 over 2013-01..2022-12 (GC2). Fix defects only in the
modules these commands run (clean files). Files: `alpha_panel/{liquidity,market_index,indexes,returns_validation,
identity_v3}.py` and their tests. Report: `.superpowers/sdd/tier1-v4/task-1-report.md`.

### Task 2: Universe stage `universe/`

Spec: gap-analysis §2 item 1 (deliverable and gate binding). Amendments: also publish per-year counts and the
30-core-field coverage table (fields: ret, prc, shrout/me, adv, sale_ttm/saleq, atq, ceqq, ibq, oancf, sic, ff49,
inst ownership, short interest, short volume, ftd, earnings date, iv_atm, borrow proxy, beta, idio vol, and the
remaining existing panel fields of most use) to `docs/datasets/universe.md`. Depends on Task 1 outputs. Files: new
`alpha_panel/universe.py`, `tests/test_universe_*.py`. Report: `task-2-report.md`.

### Task 3: Compustat-shaped PIT `compustat_std/` (fundq + funda + rdq)

Spec: gap-analysis §2 item 2 (binding). Build from `fundamentals_v10` (stage name configurable so Task 5 can rebuild
on v11) and `earnings_calendar`. Include the as-of macros (`fundq_asof`, `funda_asof`) as catalog views. Clean the
`period_end` outliers the inventory found (max 6016-06-30): exclude and count rows with datadate outside
[1990-01-01, today + 1 year], report the count. Files: new `alpha_panel/compustat_std.py`, tests. Report:
`task-3-report.md`.

### Task 4: Financial ratio suite `ratios/` (WRDS WFRS analog)

Spec: gap-analysis §2 item 3 (binding). Inputs: `compustat_std/` (Task 3) and `market/shares_daily` + prices. Output
monthly (month-end, GC1 monthly rule) and a daily as-of view (catalog macro). Add Piotroski F-score, Altman Z,
Ohlson O, Beneish M as composite scores (documented formulas). Files: new `alpha_panel/ratios.py`, tests. Report:
`task-4-report.md`.

### Task 5: Fundamentals depth `fundamentals_v11` + rebuild of Tasks 3-4

Spec: gap-analysis §2 item 4 (binding). After v11 passes, rebuild `compustat_std/` and `ratios/` on v11 and report
the coverage delta. Files: `alpha_panel/{fund_items,fund_catalog,fundamentals,fund_validate}.py` only if clean
(check GC5 list first), tests. Report: `task-5-report.md`.

### Task 6: Factor returns + risk input pack `factors/`, `risk_inputs/`

Spec: gap-analysis §2 item 5 (binding). Universe from Task 2 (`univ_all_common`, NYSE breakpoints), book equity /
op. profitability / investment from Task 3 (or v10 directly if Task 3 is not landed). Daily and monthly factor
returns; per line-day exposures. Files: new `alpha_panel/factors.py`, `alpha_panel/risk_inputs.py`, tests. Report:
`task-6-report.md`.

### Task 7: JKP-layout monthly characteristic library `char_monthly/`

Spec: gap-analysis §2 item 6, deliverable and coverage gate (binding). ≥ 150 JKP characteristics where inputs exist,
plus the gold T5 extras listed in the item. A machine-readable registry (name, JKP name, family, formula, inputs,
clock, staleness, sign convention). Do not edit `characteristics.py` (foreign). Files: new
`alpha_panel/char_monthly.py` (+ a registry module), tests. Report: `task-7-report.md`.

### Task 8: Characteristic replication harness (≤ 2022)

Spec: gap-analysis §2 item 6, replication gate (binding; GC2). Capped-VW tercile long-short per characteristic on
`univ_all_common` with NYSE breakpoints, vs `bench_jkp_us_factors` (usa, vw_cap) and OSAP `PredictorLSretWide`.
Output `validation/char_replication.json` + `docs/datasets/char_replication.md` (per-characteristic ρ, mean sign,
t-stats ≤ 2022). Files: new `alpha_panel/char_replication.py`, tests. Report: `task-8-report.md`.

### Task 9: Announcement-clock earnings (calendar v2, EX-99 actuals, SUE family)

Spec: gap-analysis §2 item 7 (binding). Files: `alpha_panel/{earnings_calendar_v2,events_sources}.py` (if clean),
new `alpha_panel/ea_actuals.py`, tests. Report: `task-9-report.md`.

### Task 10: Guidance stage (estimates substitute)

Spec: gap-analysis §2 item 8 (binding). Hand checks are done by the implementer reading the source release text and
recorded row by row in the report. Files: `alpha_panel/guidance.py`, tests. Report: `task-10-report.md`.

### Task 11: Ownership completion (13F filer type, N-PORT, 13D/G, insider price sanity)

Spec: gap-analysis §2 item 9 (binding). Files: `alpha_panel/{filer_type,nport,stakes,insider_measures}.py`, tests.
Report: `task-11-report.md`.

### Task 12: M&A deals + exact DLRET v2

Spec: gap-analysis §2 item 10 (binding). Publish `delisting/events` v2 as a new stage version; do not edit
`labels.py` (foreign). Files: `alpha_panel/{events_mna,delisting}.py`, tests. Report: `task-12-report.md`.

### Task 13: Distributions + buybacks

Spec: gap-analysis §2 item 11 (binding). Files: `alpha_panel/{corporate_actions,events_buyback}.py`, new
`alpha_panel/distributions.py`, tests. Report: `task-13-report.md`.

### Task 14: Segments + notes

Spec: gap-analysis §2 item 12 (binding), resume command §3d. Files: `alpha_panel/fund_notes.py`, tests. Report:
`task-14-report.md`.

### Task 15: Text features + TNIC

Spec: gap-analysis §2 item 13 (binding), resume command §3h (verify phase first; one fetch loop). Files:
`alpha_panel/{filing_text,tnic,text_features}.py`, tests. Report: `task-15-report.md`.

### Task 16: Golden research panels `golden_panel/{monthly,daily}`

Deliverable: research-ready panels keyed (session_date or month_end, security_id) with cik, permco-like issuer id,
universe flags (Task 2), returns (ret, ret_excess, dlret-adjusted next-period return label kept in a separate
`labels` table), me, Compustat-std as-of items (Task 3), ratios (Task 4), factor exposures (Task 6), JKP
characteristics (Task 7, monthly), earnings/guidance/ownership/short/events/text fields that exist (Tasks 9-15),
each column with source stage and clock in the manifest. Monthly: 2020-01..latest (warmup history allowed in
inputs, not output); daily: 2020-01-02..latest, year files. Gates: unique keys; complete `univ_all_common`
membership alignment; per-column coverage table per year; PIT audit — for 200 random (date, security, column)
cells recompute the value from the source stage with the GC1 rule and match exactly; a reader helper
`load_golden(start, end, columns, universe)` with memory-bounded scans. Independent of the foreign `gold.py`
alpha-admission panel (different stage name; do not edit it). Files: new `alpha_panel/golden_panel.py`, tests,
`docs/datasets/golden_panel.md`. Report: `task-16-report.md`.

### Task 17: Product catalog + parity scorecard refresh

Deliverable: `atx-db/docs/TIER1_V4_PRODUCT_CATALOG.md` — one section per dataset family (§1 map rows): stage,
keys, cadence, history span, measured coverage 2020-2025 on `univ_all_common`, PIT rule, Compustat/FactSet
equivalent, known limits; plus a rebuilt `catalog.duckdb` (stagelake) exposing every new stage and as-of macro, and
`lake verify` PASS. Update `PARITY_SCORECARD.md` measured rows for families this plan touched (if clean). Report:
`task-17-report.md`.
