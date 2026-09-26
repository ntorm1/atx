# FSDS external benchmark (P12)

Trust-parity check of standardized fundamentals against an independent SEC source: the DERA
**Financial Statement Data Sets** (FSDS, `num.txt`/`sub.txt`/`tag.txt`). A desk needs to know that
our revenue / EPS / assets agree with what SEC itself extracted from the same filings.

Code: `src/atx_db/fsds_baseline.py` (loader, canonical mapping, coverage grid, comparison harness),
`scripts/benchmark_fsds.py` (CLI), `tests/test_fsds_baseline.py` (one end-to-end fixture, no network).

**Status (2026-09-25):** mapping coverage measured on real FSDS data; comparison harness proven on a
fixture over the test-template schema. **The warehouse comparison has NOT been run**: it needs the
heavy slot after run5 B1 materializes `fundamental_standardized` (0 rows today). There is no
agreement ratio against the warehouse yet; do not quote one.

## Source and fetch (ruling RX10)

- Eight quarterly zips, the RX10 cap, fetched once with user agent `atx-db/0.1 atx-research@example.com`
  (at most 4 requests/s), streamed to `data/cache/P12-fsds/<quarter>.zip`. SHA-256, bytes, URL,
  HTTP status and `Last-Modified` are in `data/cache/P12-fsds/P12-fsds-manifest.json`. A cached zip
  is hashed again and reused, never downloaded again. A hash mismatch, or a zip that is recorded
  but missing, is an error. The cap counts every quarter ever recorded.
- Quarters: `2021q3 2022q1 2023q3 2023q4 2024q1 2025q3 2025q4 2026q1` (842,240,311 bytes in
  total). A 10-K
  carries three fiscal years of income-statement and cash-flow values and two balance sheets, so
  taking every other filing year covers five fiscal years in each 10-K filing season:
  Q1 filers (Dec/Jan/Nov FYE) use 2022q1/2024q1/2026q1; Q3 filers (May/Jun/Jul FYE) use
  2021q3/2023q3/2025q3; Q4 filers (Aug/Sep FYE) use 2023q4/2025q4. Balance sheets that are missing
  from the Q4 filers' 10-Ks come from 10-Q comparatives. In the measured grid, every cell has a
  filing in the window.

| quarter | sha256 (prefix) | zip bytes | num.txt bytes | panel subs | panel num rows |
|---|---|---:|---:|---:|---:|
| 2021q3 | 0fae2ceb149c | 97,252,395 | 413,619,987 | 50 | 41,034 |
| 2022q1 | 2ebeef77727a | 103,669,919 | 432,574,437 | 51 | 43,646 |
| 2023q3 | f075d721dde4 | 117,776,944 | 494,235,258 | 50 | 39,851 |
| 2023q4 | ad23c5505e59 | 120,272,357 | 514,279,438 | 50 | 40,007 |
| 2024q1 | 94c7894b0090 | 124,336,804 | 486,746,093 | 53 | 45,440 |
| 2025q3 | ebe25fa74c36 | 127,842,298 | 542,201,491 | 55 | 42,789 |
| 2025q4 | 2b36ac3850c0 | 65,830,170 | 567,568,365 | 54 | 42,889 |
| 2026q1 | d18c01c615da | 85,259,424 | 559,383,763 | 52 | 42,480 |

## Method

1. **Loader** (`load_fsds_subset`). Each member is copied out of the zip in 1 MiB chunks to a
   P12 temporary directory, which is deleted after the quarter. DuckDB reads the member at
   `memory_limit=384MB` and `threads=2`, and pushes the panel filter into the scan. `sub.txt` is
   filtered to the 50 panel CIKs, `num.txt` to those submissions, and `tag.txt` to the tags they
   use. No file is loaded into pandas. The subset DB is `data/cache/P12-fsds/P12-fsds-subset.duckdb`
   (9.5 MB, 415 submissions, 338,136 facts). Reloading a quarter is idempotent: the key is the
   quarter + zip hash + panel + loader version. Peak process private memory was about 250 MB.
   Parsing is strict RFC-4180 with every column as VARCHAR. 2025q3 `num.txt` has 48 quoted
   `segments` values that contain tabs, so quoting must stay on.
2. **Canonical mapping** (`canonical_rules` + `canonical_fsds_facts`). This mapping reuses the
   warehouse's own `seeds/standardization_rules.csv` alias priorities and the alias validity
   windows in `seeds/fundamental_items.csv`, so FSDS and the warehouse apply the same rule to the
   same filing:
   - `qtrs` 4 maps to `annual`, 1 to `quarterly` and 0 to `instant`. `ddate` becomes the period.
     Only `version us-gaap/*` facts are used. Facts with `segments` or `coreg` are excluded.
   - `uom` must be `USD`. FSDS publishes `EarningsPerShareDiluted` with uom `USD` in all 8
     quarters, not `USD/shares`.
   - Each value is multiplied by the concept's `statement_map.value_multiplier`, as the
     warehouse's statement points are. For example, `PaymentsToAcquirePropertyPlantAndEquipment`
     has multiplier -1.0, so capex is a signed outflow in `capex__1305`. Without this, every
     PP&E-tagged capex row would read as a `sign_flip` mismatch.
   - **Direct:** within each filing, the alias with the lowest priority number wins. For example,
     `RevenueFromContractWithCustomerExcludingAssessedTax` (10) beats `Revenues` (20).
   - **Derived:** `coalesce_or_difference` and `coalesce_or_sum` rules compose inputs reported in
     the same filing, with the engine's semantics (S1): an input is either a raw item or, when the
     rule declares `{"input": "output"}`, another rule's own output (its direct value, else its
     composition), evaluated in dependency order. Differences are n-ary (input 1 minus every later
     input). `skip` needs every input, `zero_fill` any, `zero_fill_subtrahends` input 1 with absent
     later inputs as 0. Examples: `gross_profit = revenue - cost_of_revenue`,
     `stockholders_equity = equity_incl_NCI - NCI`, `common_equity = stockholders_equity(output) -
     preferred`, `equity_incl_NCI = StockholdersEquity + NCI`, `total_liabilities =
     LiabilitiesAndStockholdersEquity - equity_incl_NCI(output)`, `revenue = utility operating
     revenue(output)` when no revenue tag is reported. An output input is labeled
     `atx-rule-output:<canonical_code>` in `source_tags_json`, as the engine labels it in
     `input_codes_json`. The FSDS side has no industry-template routing, so item 1801 (a UT-template
     concept in the warehouse) applies to every FSDS filer; only utilities report it.
   - Core items: revenue (1001), gross_profit (1004), operating_income (1014), net_income_total
     (1031), eps_diluted (1035), total_assets (1101), total_liabilities (1201), common_equity (1220),
     cash_flow_from_operations (1301), capex (1305).
3. **Grid** (`benchmark_coverage`). The panel is 50 US-GAAP 10-K filers (`BENCHMARK_PANEL`):
   banks (JPM, BAC, WFC), brokers (GS, SCHW), insurers (PGR, TRV, MET), REITs (PLD, O, SPG, AMT),
   utilities (NEE, DUK, SO), energy (XOM, CVX, COP), and 31 others across tech, health, consumer,
   industrial, telecom and media, with every FYE season represented.
   - CIKs are the historical filers. XOM is `0000034088`. The current SEC ticker map points XOM at
     `0002115436`, a 2026 holding company with no FY2021-FY2025 10-K. This is a PIT identity trap
     that P1/C9 must also handle.
   - Each issuer's five fiscal-year ends are the latest annual-report `period` and the four years
     before it (month-end), for 50 x 5 x 10 = **2,500 cells**. A cell is mapped when any loaded
     filing yields a canonical value within 20 days of the fiscal-year end.
   - An unmapped cell gets exactly one reason:

     | reason | meaning |
     |---|---|
     | `issuer_not_in_window` | the issuer has no annual filing in the loaded quarters |
     | `ifrs_filer` | the filings use the IFRS taxonomy |
     | `period_not_in_window` | no loaded filing covers this fiscal-year end |
     | `dimensional_only` | the rule's tags appear only with `segments` or `coreg` |
     | `unit_mismatch` | the rule's tags appear only in another unit |
     | `derivation_incomplete` | only some components of a derived item are present |
     | `alias_gap_us_gaap` | no rule tag, but standard us-gaap tags match the item's keywords (listed) |
     | `custom_extension` | only company-extension tags match (listed) |
     | `not_reported` | nothing matches |

     Keywords only rank candidate tags. They never map a value.
4. **Comparison** (`run_fsds_comparison`; heavy slot, post-B1).
   - For each (CIK, item, basis, ddate), the latest filing by `accepted` becomes the
     `vendor_baseline_facts` row. Vendor is `SEC_FSDS`, `source_accession` is the FSDS `adsh`, and
     `available_at` is the EDGAR acceptance time. That time only orders FSDS vintages; it is not a
     warehouse PIT clock.
   - `security_id` comes from `fundamental_standardized` for the CIK: the security whose symbol is
     the panel ticker, otherwise the one with the most rows.
   - The FSDS `ddate` is rounded to month end. It is aligned to the nearest warehouse `period_end`
     within 15 days; Apple's 2025-09-30 becomes 2025-09-27.
   - Scoring goes through `fact_disagreement.refresh_fact_disagreement` in two passes, one per
     tolerance group:
     - Monetary items (`fsds_parity_v1:monetary`): the spec's **0.5 % or $1M**.
     - Per-share items (`fsds_parity_v1:per_share`): **0.5 % or $0.005**. A $1M floor would pass
       any EPS value, so per-share gets the stricter floor. This does not relax the spec.
   - Every row is then classified against the warehouse revision whose `source_accession` equals
     the FSDS `adsh`:

     | class | meaning |
     |---|---|
     | `agrees` | latest warehouse value vs latest FSDS value is within tolerance |
     | `vintage_difference` | the warehouse's latest value is a later filing; its revision from the same filing agrees |
     | `value_mismatch` | the same-filing revision also disagrees |
     | `value_mismatch_no_same_filing` | the latest disagrees and the warehouse has no revision from this filing |
     | `missing_warehouse_period` | the warehouse has this item for other periods only |
     | `missing_warehouse_item` | the warehouse has no rows for this item |

     `value_mismatch` rows carry a hint: `sign_flip` or `scale_x1000`/`scale_div1000`/`scale_x1e6`/`scale_div1e6`.
   - Output per item: the fact_disagreement ratio `latest_agreement_ratio`, the like-for-like
     `same_filing_agreement_ratio`, the class counts, and up to 25 failing rows listed. No
     threshold is changed.
   - The comparison writes only to a scratch DB. The warehouse is attached READ_ONLY and only the
     panel's `fundamental_standardized` rows (every revision) are copied out.
   - A filing's `fy`/`fp` label only its own period. Comparatives, such as FY2021 inside the FY2023
     10-K, carry no fiscal label.
   - Reruns are idempotent. `fact_disagreement` itself upserts by natural key, in one
     transaction: update existing keys, insert new ones, then delete stale keys. The old
     delete-then-insert pattern raised a duplicate-key ConstraintException on DuckDB 1.5.5 when
     rerun; this was fixed in P12F, so P12 needs no workaround.

## Proof of the harness (no warehouse opened)

- `tests/test_fsds_baseline.py` is one end-to-end fixture: a quarterly zip with two Apple 10-Ks and
  a non-panel filer, loaded, mapped, graded on the grid, and compared on the test-template schema.
  - It covers the non-panel filer being dropped, the reload being reused, alias priority,
    derivation by difference, the FSDS EPS uom, and exclusion of segment and co-registrant facts.
  - It checks that `qtrs` 1 maps to quarterly, that a within-window re-report counts as a second
    vintage, and that month-end periods align to the exact period end.
  - It checks the parity classes `agrees`, `value_mismatch` (including an EPS miss that a $1M floor
    would hide), `vintage_difference` (a later warehouse restatement where the same accession
    agrees), `missing_warehouse_item` and `missing_warehouse_period`.
- CLI smoke on real FSDS data: `compare --copy-from-warehouse <template copy seeded with 4 AAPL
  FY2025 rows> --heavy-slot`.
  - 4 rows were copied and 50 AAPL grid rows were scored: 3 `agrees` (revenue, EPS, total assets)
    and 1 `value_mismatch` (net income, seeded +2 %).
  - The other 46 rows are missing (30 item, 16 period), and 49 issuers are unresolved, as
    expected.
  - A second run gave identical results. Both guards refused with exit 2: `--db` pointing at the
    governed warehouse, and `--copy-from-warehouse` without `--heavy-slot`.

## Mapping coverage (measured 2026-09-25 on the eight quarters)

**2,112 / 2,500 cells map cleanly (84.5 %)**: 1,761 direct and 351 derived. All 2,500 cells have
a filing in the window, so no cell is lost to the 8-zip cap. 74 mapped cells carry different FSDS
values across filings; these are comparatives re-reported within the window.

| item | cells | mapped | direct | derived | unmapped (reason: count) |
|---|---:|---:|---:|---:|---|
| revenue | 250 | 240 | 240 | 0 | alias_gap_us_gaap 10 (DUK, NEE: `RegulatedAndUnregulatedOperatingRevenue`, `RegulatedOperatingRevenue*`) |
| gross_profit | 250 | 130 | 55 | 75 | derivation_incomplete 110 (banks, brokers, insurers, REITs, services: no cost-of-revenue line); not_reported 5 (NEE); dimensional_only 5 (DUK) |
| operating_income | 250 | 145 | 145 | 0 | not_reported 105 (BAC, COP, CVX, DIS, GE, GS, HON, IBM, JNJ, JPM, LLY, MET, ...: no `OperatingIncomeLoss` subtotal) |
| net_income | 250 | 250 | 250 | 0 | - |
| eps_diluted | 250 | 245 | 245 | 0 | dimensional_only 5 (V: EPS only per share class) |
| total_assets | 250 | 250 | 250 | 0 | - |
| total_liabilities | 250 | 229 | 175 | 54 | derivation_incomplete 21 (AMZN, FDX, LLY, MCD, NKE: no `Liabilities` and no NCI-inclusive equity tag) |
| common_equity | 250 | 217 | 0 | 217 | derivation_incomplete 16 (JNJ, PG, UNH, VZ); alias_gap_us_gaap 9 (CAT, V); dimensional_only 8 (PG, T, V) |
| cfo | 250 | 250 | 245 | 5 | - |
| capex | 250 | 156 | 156 | 0 | alias_gap_us_gaap 75 (AMZN, COP, CVX, FDX, GE, HD, MET, NEE, NVDA, PEP, PLD, SPG, T, TRV, V, VZ); not_reported 19 (BAC, COP, JPM, WFC) |

Alias-gap candidate tags (keyword-ranked, up to three per cell), counted over cells:
- capex: `PaymentsToAcquireProductiveAssets` 50; `PaymentsToAcquireRealEstateAndRealEstateJointVentures` 10;
  `CapitalExpendituresIncurredButNotYetPaid` 10 (a non-cash disclosure, not capex);
  `PaymentsToAcquireRealEstate` 5; `PaymentsToAcquireRealEstateHeldForInvestment` 5;
  `PaymentsToAcquireOtherProductiveAssets` 5.
- revenue: `RegulatedAndUnregulatedOperatingRevenue` 10; `RegulatedOperatingRevenueElectricNonNuclear` 5;
  `RegulatedOperatingRevenueGas` 5.
- common_equity: `StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest` 9;
  `LiabilitiesAndStockholdersEquity` 9 (keyword noise).

Unmapped reasons across all items: derivation_incomplete 147, not_reported 129, alias_gap_us_gaap 94,
dimensional_only 18. None are `period_not_in_window`, `issuer_not_in_window`, `ifrs_filer` or
`unit_mismatch`.

### What the unmapped cells say about the warehouse rules (findings, no rule changed here)

Because the mapping uses the warehouse's own rules, these gaps are expected in
`fundamental_standardized` too. The post-B1 comparison will show them as `missing_warehouse_*`.

1. **capex (item 1305) misses `PaymentsToAcquireProductiveAssets`.** This is the top alias-gap tag.
   AMZN, CVX, FDX, GE, PEP, T, V and others use it for "purchases of property and equipment". The
   registry maps it to item 1306 (`capex_broader_incl_intangibles`). REITs use
   `PaymentsToAcquireRealEstate*`. Candidate fix: a lower-priority alias of 1305, or a
   `coalesce_or_*` fallback to 1306.
2. **common_equity cannot chain to a derived stockholders' equity.** JNJ, UNH, VZ, PG, CAT and V
   tag total equity only as `StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest`.
   Rule 1221 derives stockholders' equity (1222 - 1213). Rule 1220 composes only the raw 1221, so
   common_equity is missing for them. `PreferredStockValue` = 0 is present, which is why the reason
   is `derivation_incomplete`.
3. **total_liabilities needs the NCI-inclusive equity tag.** AMZN, FDX, LLY, MCD and NKE report no
   `Liabilities` total. They also report only `StockholdersEquity`, not the NCI-inclusive tag, so
   `1223 - 1222` cannot be computed.
4. **Utilities' revenue lives in item 1801.** DUK and NEE report only
   `RegulatedAndUnregulatedOperatingRevenue`, which is mapped to `utility_operating_revenue` and
   not to `revenue`.
5. **Structural, not errors.** Gross profit for financials, REITs and services; operating income
   for filers without that subtotal (banks, integrated energy, pharma, several industrials);
   per-class EPS (V); preferred stock only by series (T). These are honest NULLs.

## Heavy command (post-B1, HEAVY SLOT ONLY)

Run this after run5 B1 has populated `fundamental_standardized`. The warehouse is attached
READ_ONLY, and all writes go to the scratch DB. From PowerShell:

```powershell
Set-Location C:\atx\atx-db
$env:OPENBLAS_NUM_THREADS = '1'
$py  = 'C:\atx\atx-db\.venv\Scripts\python.exe'
$ctl = 'C:\atx\.superpowers\sdd\tier1-parity'
& $py "$ctl\run_memory_guarded.py" --job-gb 1.5 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
  --receipt "$ctl\claude-ctl\P12-compare-memory.json" --stdout "$ctl\claude-ctl\P12-compare.log" --stderr "$ctl\claude-ctl\P12-compare.err" `
  -- $py scripts\benchmark_fsds.py compare `
  --db C:\atx\atx-db\data\cache\P12-fsds\P12-compare-scratch.duckdb `
  --copy-from-warehouse C:\atx\atx-db\data\warehouse.duckdb --heavy-slot `
  --memory-limit 1GB --threads 2 --run-id P12-post-B1 --out "$ctl\claude-ctl\P12-out"
```

The outputs are `P12-out/P12-fsds-comparison.json` (coverage + comparison summary, per-item
`latest_agreement_ratio` / `same_filing_agreement_ratio`, and failing samples) and
`P12-out/P12-fsds-comparison-rows.csv` (one classified row per compared fact).

- `compare` refuses `--db` equal to the governed warehouse.
- `compare` refuses `--copy-from-warehouse` without `--heavy-slot`.
- The first run initializes the scratch DB schema.
- No network is used: the zips and the subset DB are already cached.

To rebuild the subset from the cached zips (light): `scripts\benchmark_fsds.py load`, then
`scripts\benchmark_fsds.py coverage --out <dir>`.
