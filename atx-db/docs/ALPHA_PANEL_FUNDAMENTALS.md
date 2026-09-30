# Alpha panel stages I and F: identity links and fundamentals

Companion to `docs/ALPHA_PANEL.md` (stage contract). Code:
`src/atx_db/alpha_panel/identity_links.py` (stage I) and
`src/atx_db/alpha_panel/fundamentals.py` (stage F).

## Progress

| Step | State |
| --- | --- |
| I: rule r4-links-asof-v1 implemented, validation seal 2025-01-01 reproduced exactly | done |
| I: production seal 2026-09-21 built (`identity/links.parquet`, 7,186 rows) | done |
| F: code (`fundamentals.py`, `fund_items.py`), prepare (FSDS clocks, 433,717 accessions) | done |
| F: scope widened by controller to every Company Facts CIK (not only strict links) | done (code v3) |
| F: spot checks AAPL / MSFT / JPM / NVDA on per-CIK debug runs | done (values match raw facts) |
| F: v3/v4 full runs + validate exposed split-ledger gaps (merger inside the split window, reverse merger, XBRL scale errors, mixed share sources) | fixed in v5 |
| F: full batch run v5, finalize, validate, guard receipts attached to `fundamentals/manifest.json` | done |
| F v10 (tier1-v3 lane FUND, S4.1-S4.3, S4.6, S4.7): code, fixture tests, build into `fundamentals_v10/`, `export/fundamental-events-v2`, `validation/fundamentals.json` | see "Stage F v10" |

## Stage F v10 (`fundamentals_v10/`, rule `fund-events-pit-v3`, code `fundamentals-v10`)

The sections after this one describe the v5 build (published as `fundamentals/`, the v9 panel
input). v10 keeps every v5 rule and adds the rules below. Code: `fundamentals.py`,
`fund_items.py`, `fund_extract.py` (`fund-extract-v3`), `fund_fx.py`, `fund_catalog.py`,
`fund_asof.py`, `fund_export.py`, `fund_validate.py`. Full rule texts are in
`fundamentals_v10/manifest.json` (`rule_text`, `currency_rule`, `fx_rule`, `catalog`).

### Build (publishing gate)

`ATX_FUND_STAGE` names the output stage (default `fundamentals`); the extract work dir follows
it (`<stage>/_work/cf`). v10 is built beside v9 and swapped in by the controller:

```powershell
# every command under run_memory_guarded.py (--job-gb 0.6), PYTHONPATH=atx-db/src
$env:ATX_FUND_STAGE = "fundamentals_v10"; $env:ATX_FUND_DUCKDB_MEM = "350MB"; $env:ATX_FUND_DUCKDB_MEM_BATCH = "250MB"
python -m atx_db.alpha_panel.fund_extract run              # companyfacts.zip -> fundamentals_v10/_work/cf (85 batches)
python -m atx_db.alpha_panel.fundamentals build-all        # prepare -> batches -> finalize (resumable per batch)
python -m atx_db.alpha_panel.fund_validate all             # -> <lake>/validation/fundamentals.json
$env:ATX_FUND_EXPORT = "fundamental-events-v2"
python -m atx_db.alpha_panel.fund_export build; python -m atx_db.alpha_panel.fund_export verify
```

### New rules

* **S4.1 cross-concept quarters and TTM.** A discrete quarter may come from a longer period of
  one concept minus the year-to-date of another concept of the same tier from the same start
  (Visa-style `Revenues` FY with `RevenueFromContract...` 9M), else minus the discrete quarters
  chaining back to that start, else two contiguous stub periods (predecessor/successor). TTM
  keeps the v9 paths first (fiscal-year fact, four single-concept quarters, YTD arithmetic
  within one concept) and only then admits cross-concept quarters and the cross-concept YTD
  path; `sale`, `gp`, `oi` finally chain four item-level quarters of any fallback level
  (`*_src = quarters_mixed`). A derived value exists from its latest component's clock;
  `quarterly_history` recomputes every quarter end a filing's periods can feed (300 days).
* **S4.3 FX (ruling D4, `fund-fx-h10-v1`).** Non-USD filers in a currency of
  `reference/fx_daily.parquet` (FRED H.10, 25 currencies) are computed in the reporting
  currency and converted: balances at the last rate on or before the balance date (at most 10
  days older), TTM flows at the mean of the 365 days ending at `period_end`, quarter flows at
  the 91-day mean (rates on at least 50% of the window's weekdays). `currency` stays the
  reporting currency; `fx_converted`, `fx_rate`, `fx_rate_avg_q`, `fx_rate_avg_ttm` describe the
  conversion; `available_at = max(filing clock, available_at of every rate used)`. Uncovered
  currencies keep the v9 USD-facts-only rule.
* **S4.2 catalog (`catalog.parquet`, 81 Compustat-analog items).** `fund_catalog.CATALOG` names
  per item the Compustat mnemonic, the `seeds/fundamental_items.csv` item, the kind (stock,
  TTM flow, per-share, weighted-average shares, derived) and the concept chain (us-gaap, then
  ifrs-full). Zero rule: a line item with no fact within 460 days is 0 (listed in
  `catalog_zero_filled`) when the filing's own FSDS PRE statement exists without an item line
  (`catalog-pre-v2`: tag pattern on any line, label pattern only on custom-tag lines, subtotal /
  comprehensive-income / supplemental tags excluded), or -- items whose absence is common and
  unambiguous (treasury stock, acquisitions, debt issuance, discontinued operations, ...) --
  when no PRE exists and the statement is evidenced by `at` / `ni_ttm` / `cfo_ttm`.
  `cat-nil-zero-v1`: a filing that reports a catalog line item for the comparative period but
  not for its own period showed a blank cell, so 0 enters the knowledge (v9 chain concepts
  excluded). Derived: `intano`, `dlc`, `dltt`, `txditc`, `ceq`, `teq`, `lse`, `wcap`,
  `xopr_ttm`, `txt_ttm`, `lco` (= lct - ap - dlc - txp), `dv_ttm`, and fallbacks for `pi`, `ib`,
  `nicon`, `epspi`, `epsfi`. Industry items (bank, insurer) are structural NaN outside their SIC
  ranges; current-asset/liability components are structural where `act`/`lct` are.
  `catalog.parquet` is keyed by (cik, accession) and kept out of `events.parquet` (the panel
  reads every events column).
* **S4.2(c) FSDS PRE label fallback (`lbl-label-v1`).** Custom-tag income-statement lines
  (FSDS `version` = accession) labelled revenue / cost of revenue / gross profit / operating
  income feed the last tier of those chains (pseudo taxonomy `lbl`).
* **S4.6 vintages.** `is_amendment` (form `/A`), `is_restated` / `restated_items` (the filing
  changed a value an earlier filing reported for the same concept and period by more than
  0.5% on a watched chain), `nonreliance_402_at` (latest 8-K Item 4.02 within 365 days, from
  `sec_filings/eight_k_items.parquet`). Events are new rows (vintages); `fund_asof` builds the
  as-of views: `events_as_of_sql(stage, t)` (rows with `available_at < t`, optionally the latest
  per CIK), `history_as_of_sql`, `vintages_sql` (first-reported vs latest value per CIK, period
  and item) and DuckDB macros `fund_events_asof`, `fund_events_latest_asof`,
  `fund_history_asof`, `fund_vintages`. The cutoff rebuild in the validation uses them.
* **Export `fundamental-events-v2`** follows `atx.fundamental-events/v1`: `accepted_utc` =
  `available_at` (the FX-aware clock), `filing_accepted_utc` = the filing clock, catalog items
  joined, items NaN-filled, the new descriptors carried.

Panel note: the panel as-of join should use `available_at` (>= `clock_utc`; equal for USD
filers).

## Stage I: `identity/links.parquet`

Rule `r4-links-asof-v1` (module docstring has the full text). Numbers measured on
2026-09-27 from `session8-phased-r4/phases/export-001` (31,163 link versions).

| Seal | rows | lines | CIKs | min start | high / medium | ambiguous line-days |
| --- | --- | --- | --- | --- | --- | --- |
| 2025-01-01 (validation) | 6,780 | 6,669 | 6,583 | 2012-03-27 | 2,472 / 4,308 | 2,292 |
| 2026-09-21 (production) | 7,186 | 7,070 | 6,962 | 2012-03-27 | 2,770 / 4,416 | 2,292 |

Validation matches the reference numbers exactly. Ambiguity counts every visible
version (all tiers, bases and primary flags); counting only kept versions gives 346
ambiguous line-days and does not match. No line-company pair ever has two kept
versions visible on the same day (`multi_version_line_days = 0`), and every company-day
has exactly one `P` line (asserted). `perm_security_id` equals the vendor securityID on
all 25,760 permanent ids (asserted). Latest `end_incl` is 2026-09-18 (the last vendor
session): no one-day Sunday intervals appear at the 2026-09-20 snapshot.

Linked line-days per year (calendar days / vendor sessions from `calendar.parquet`):

| year | calendar line-days | session line-days |
| --- | --- | --- |
| 2018 | 1,265,764 | 870,403 |
| 2019 | 1,269,576 | 876,527 |
| 2020 | 1,280,618 | 885,235 |
| 2021 | 1,312,754 | 906,467 |
| 2022 | 1,386,750 | 953,652 |
| 2023 | 1,414,672 | 968,947 |
| 2024 | 1,390,792 | 957,510 |
| 2025 | 1,345,851 | 921,644 |
| 2026 (to 09-18) | 924,939 | 634,065 |

Run: `python -m atx_db.alpha_panel.identity_links build` (guarded, 0.6 GiB cap, peak
0.11 GiB, 2.8 s). The build recomputes the validation seal first and refuses to publish
on a mismatch. `attach-receipt --receipt <guard receipt>` copies the guard's peak memory
into `identity/manifest.json`.

## Stage F: `fundamentals/events.parquet`, `fundamentals/sic_events.parquet`

### Build

```powershell
# every command under run_memory_guarded.py --job-gb 0.8, PYTHONPATH=atx-db/src
python -m atx_db.alpha_panel.fundamentals prepare    # FSDS sub -> _work/sub_clock.parquet (433,717 accessions)
python -m atx_db.alpha_panel.fundamentals batches    # 85 Company Facts batches, resumable per batch receipt
python -m atx_db.alpha_panel.fundamentals finalize   # events.parquet, sic_events.parquet, manifest.json
python -m atx_db.alpha_panel.fundamentals validate   # validation.json (raw-fact cross checks)
python -m atx_db.alpha_panel.fundamentals attach-receipt --key batches --receipt <guard receipt>
```

Each batch writes `fundamentals/_work/parts/{events,sic}-NNNN.parquet` and `batch-NNNN.json`
(source bytes/mtime plus extract sha256, code version, counters). A batch whose receipt
matches the source identity and `CODE_VERSION` (`fundamentals-v5`) is skipped, so a guard
stop (137) resumes where it stopped. Measured 2026-09-28: batches 315 s in total, peak
0.43 GiB; finalize 3 s, peak 0.47 GiB; validate peak 0.52 GiB (0.8 GiB cap, DuckDB
300-450 MB, 2 threads). `_work/` holds 56 MB.

### Scope, forms, clock

* Scope (controller change during the lane): every CIK in the Company Facts staging
  (`ee099c7394a357f1`, snapshot 2026-09-20) with periodic-form facts, not only CIKs in the
  strict links. 6,228 event CIKs are in `identity/links.parquet`; 5,949 of the 6,087
  strict-link CIKs whose interval ends on or after 2017-01-01 have events (the rest are
  IFRS-only 20-F/40-F filers or have no periodic us-gaap facts).
* Forms: 10-K, 10-Q, 10-KT, 10-QT, 20-F, 40-F and their `/A`. Facts: us-gaap in USD
  (shares for share counts) and dei `EntityCommonStockSharesOutstanding`. Dropped: rows
  with `period_end > filed` (0) and instant/duration shape mismatches (13,248 of 26.4M).
* Clock per accession: FSDS `accepted_utc` (tz-aware, stored as naive UTC); accessions
  absent from FSDS get `filed 00:00 UTC + 46h`. Events: 264,401 `fsds_accepted_utc`,
  9,192 `fc1_filed_plus_46h` (5,891 in 2026 after the last FSDS quarter 2026q2, 661 in
  2016). 15,532 events have an acceptance time before the official filing date
  (after-hours acceptance, filing date = next business day); 14 were accepted more than 4
  days after the filing date.
* Emitted events: clock >= 2016-01-01. Knowledge is built from every filing since 2009, so
  lags, TTM and SUE history are complete for the first emitted events.

### Point-in-time semantics

Filings are applied to one issuer's knowledge in `(clock, accession)` order. For every
`(concept, start, end)` the latest-clock value wins, so a restatement or a later
comparative enters on its own clock and never earlier; filings with the same clock are
applied together. An event row carries the items computed from the knowledge right after
that filing. `period_end` is the latest period end of the filing's main statements
(assets, liabilities, equity, net income, revenue, CFO, operating income facts), or the
issuer's previous `period_end` when that is later (3,257 events, mostly amendments of an
older period, keep the latest quarter as current). `fiscal_year` / `fiscal_period` are the Company Facts
`fy`/`fp` of the filing that first reported `period_end`.

Prefix invariance was checked directly: rebuilding AAPL, NVDA, JPM, BK Technologies and
Amcor from facts truncated at 2019-06-30, 2022-03-01 and 2024-08-15 reproduces every
earlier event exactly (329 events, 0 differences).

### Items (all DOUBLE, USD or shares)

Concept chains are the `seeds/statement_map.csv` canonical metrics in `concept_priority`
order, with the three required total-over-component overrides (revenue: `Revenues`
first; cash: `CashAndCashEquivalentsAtCarryingValue` before `Cash`; short-term debt:
`DebtCurrent` before its components). The full chains and item rules are in
`manifest.json` (`item_rules`). Rules marked *ext* extend the seed.

| item | rule |
| --- | --- |
| `at`, `lt`, `seq` | Assets; Liabilities, else at - equity incl. NCI (else at - SEQ - MI); SEQ, else SEQ incl. NCI - MI, else at - Liabilities |
| `che` | CashCashEquivalentsAndShortTermInvestments, else cash chain + short-term investment chain (a missing part counts 0) |
| `debt` | DebtCurrent, else current LTD chain + short-term borrowing chain; plus the long-term debt chain (else *ext* LongTermDebt - current LTD); components are 0 when `at` exists |
| `be` | SEQ + TXDITC (DeferredIncomeTaxLiabilitiesNet, else DeferredTaxLiabilitiesNoncurrent) - PS (redemption, liquidation, carrying value). TXDITC and PS carry from the latest end within 400 days because 10-Qs rarely tag them, else 0. us-gaap has no common-equity concept, so the "common + preferred" fallback is SEQ incl. NCI - MI |
| `noa` | (at - che) - (at - debt - MI - PS - (SEQ - PS)); MI = MinorityInterest, else SEQ incl. NCI - SEQ, else 0 |
| quarterly flows | discrete quarter = 80-120 day fact (13/14-week and 12/16-week retail calendars), else a YTD difference within one concept (Q4 = FY - 9M YTD) |
| TTM flows | fiscal-year fact (350-380 days) at `period_end`, else four chained quarters (each previous end within 10 days of start - 1), else YTD + prior FY - prior YTD; concept priority applies per period |
| `sale_*` | revenue chain; *ext* banks: InterestAndDividendIncomeOperating + NoninterestIncome when the chain is empty (the seed's BK template) |
| `xsga_ttm` | SG&A chain; *ext* else GeneralAndAdministrativeExpense + SellingAndMarketingExpense |
| `gp_ttm` | GrossProfit, else sale_ttm - cogs_ttm |
| `capx`, `dvc`, `prstkc` | keep the reported positive outflow sign (the seed's -1 cash-flow multiplier is not applied) |
| `dvc/prstkc/sstk_ttm` | 0 when `cfo_ttm` exists and the chain has no fact ending within 460 days of `period_end`; NULL when facts exist but no TTM can be formed |
| `shrs_q` | dei cover count with the latest cover date in [end - 15d, end + 120d], else CommonStockSharesOutstanding at end, else weighted-average basic (diluted) shares of the quarter. Multi-class filers such as Alphabet, Meta and HEICO have no non-dimensional dei value |
| `shrs_q_lag4` | same source as `shrs_q` at the year-ago balance-sheet period end (dei: that period's own cover), converted to the current share basis by the split ledger below; NULL if that source is missing or `abs(log10(shrs_q/lag)) >= 2` |
| lags | `at_lag4`, `noa_lag4`: balance sheet at the Assets/SEQ end within 20 days of end - 365; `be_lag1q`: within 25 days of end - 91; `be_lag1q_lag4`: within 20 days of that date - 365; `ni/txt/sale_q_lag4`: quarter ending within 20 days of end - 365; all as known at the event |
| `sue` | (niq_first(t) - niq_first(t-4)) / stdev of the previous up to 8 such differences (quarters ending 60-760 days before t), min 4. niq_first = quarterly net income as first derivable at the filing that first reported the quarter |
| `fscore` | Piotroski's nine signals, NULL unless all nine are computable (house rule of the tier-1 v2 plan): ni_ttm > 0; cfo_ttm > 0; ni_ttm/at_lag4 > ni_ttm(t-4)/at(t-8); cfo_ttm > ni_ttm; long-term debt/at fell vs t-4 (strict, so debt-free firms score 0); act/lct rose vs t-4; sstk_ttm <= 0; gp_ttm/sale_ttm rose vs t-4; sale_ttm/at_lag4 rose vs t-4 |

Split ledger (for `shrs_q_lag4`): a filing that restates share-count keys (weighted-average
or balance-sheet shares) by a common ratio r (|ln r| >= ln 1.09) is evidence of a split
between the last old-basis report and that filing. Windows with the same ratio merge into
one split. A split is accepted when a first-reported share series (dei covers, current
balance-sheet shares, current-quarter weighted-average shares) jumps by r between
consecutive observations inside the window (within 15%) or across it (within 35%). Pure
10^3 and 10^6 ratios need dei or balance-sheet evidence, because XBRL scale errors live in
the weighted-average series. With no series around the window, r must be split-like. The
lag is multiplied by every accepted split reported after the lag observation; 13,698 lags
were adjusted.

### Output and coverage

| output | value |
| --- | --- |
| `events.parquet` | 273,593 rows, 12,524 CIKs, 32 MB |
| `sic_events.parquet` | 270,711 rows, 12,148 CIKs (261,927 FSDS SIC, 8,784 carried) |
| forms | 10-Q 193,828; 10-K 65,187; 10-K/A 5,086; 20-F 4,668; 10-Q/A 3,910; other 914 |
| period_end age at the clock | median 43 days, p95 119 days |

Events (CIKs) per clock year: 2018 24,977 (6,663); 2019 24,210 (6,493); 2020 23,640
(6,435); 2021 27,012 (7,289); 2022 28,246 (7,394); 2023 26,509 (7,080); 2024 24,812
(6,643); 2025 23,897 (6,551); 2026 to 09-19 17,757 (6,469).

Non-null share among events with clock from 2020-01-01: all 171,873 events, and the
113,387 events of CIKs in the strict links.

| item | all | linked | item | all | linked |
| --- | --- | --- | --- | --- | --- |
| at | 0.987 | 0.996 | cfo_ttm | 0.925 | 0.963 |
| lt | 0.985 | 0.992 | capx_ttm | 0.636 | 0.774 |
| che | 0.980 | 0.990 | xrd_ttm | 0.330 | 0.407 |
| debt | 0.989 | 0.996 | dvc_ttm | 0.887 | 0.922 |
| be | 0.988 | 0.993 | prstkc_ttm | 0.817 | 0.835 |
| seq | 0.988 | 0.993 | sstk_ttm | 0.716 | 0.744 |
| sale_q | 0.773 | 0.864 | dp_ttm | 0.759 | 0.872 |
| sale_ttm | 0.768 | 0.854 | txt_q | 0.676 | 0.789 |
| cogs_ttm | 0.458 | 0.515 | shrs_q | 0.935 | 0.986 |
| xsga_ttm | 0.746 | 0.786 | noa | 0.981 | 0.992 |
| gp_ttm | 0.478 | 0.534 | invt | 0.395 | 0.464 |
| oi_ttm | 0.736 | 0.768 | rect | 0.547 | 0.626 |
| ni_q | 0.961 | 0.982 | ppe | 0.738 | 0.856 |
| ni_ttm | 0.939 | 0.971 | at_lag4 | 0.915 | 0.958 |
| be_lag1q | 0.959 | 0.980 | be_lag1q_lag4 | 0.924 | 0.970 |
| ni_q_lag4 | 0.932 | 0.975 | txt_q_lag4 | 0.676 | 0.795 |
| shrs_q_lag4 | 0.839 | 0.919 | noa_lag4 | 0.910 | 0.954 |
| sale_q_lag4 | 0.779 | 0.870 | sue | 0.857 | 0.929 |
| fscore | 0.316 | 0.371 | | | |

Financials drive most of the revenue, gross-profit and F-score gaps: among linked issuers
with assets above $500M in 2024 (v3 build), `sale_ttm` was 0.84 overall but 0.61 for FF12
Money (before the bank-revenue extension), and `fscore` 0.41 overall but 0.04 for Money (no
classified balance sheet or cost of revenue).

### Validation (`fundamentals/validation.json`)

Own-filing cross checks against raw Company Facts for events from 2020, where the event's
own accession reports the value for `period_end` unambiguously:

| check | compared | match |
| --- | --- | --- |
| `at` = Assets | 164,533 | 100% |
| `seq` = StockholdersEquity | 154,043 | 100% |
| `ni_ttm` = FY NetIncomeLoss (10-K) | 41,091 | 100% |
| `ni_q` = 3-month NetIncomeLoss | 118,918 | 99.93% (86 differ) |
| `shrs_q` = own dei cover | 140,324 | 99.85% (205 differ: a later-dated cover of a same-period amendment) |

Spot checks (rows in `validation.json` under `spot`):

* AAPL (52/53-week year ending late September; FY2023 had a 14-week Q1): FY2024 10-K
  sale_ttm 391,035M, ni_ttm 93,736M, at 364,980M, Q4 ni_q 14,736M. Q1 FY2025 sale_q
  124,300M and ni_ttm 96,150M (= 93,736 - 33,916 + 36,330). The 2020 4:1 split is
  adjusted.
* MSFT FY2025 (June): sale_ttm 281,724M, ni_ttm 101,832M, capx 64,551M, xrd 32,488M,
  dvc 24,082M. xsga uses the G&A + S&M extension (7,223 + 25,654).
* JPM FY2024: sale_ttm 177,556M, ni_ttm 58,471M, be = SEQ 344,758M - preferred 20,200M;
  che = CashAndDueFromBanks. Long-term debt is not tagged non-dimensionally, so `debt` holds
  only short-term borrowings; `fscore` is NULL (bank).
* COST (12/12/12/16-week quarters): FY2024 sale_ttm 254,453M, Q4 (16 weeks) sale_q
  79,697M and ni_q 2,354M; Q1 FY2025 ni_ttm 7,576M.
* NVDA 10:1 split (June 2024): Q2 FY2025 shrs_q 24.53B against a split-adjusted lag of
  24.70B (raw lag 2.47B); the ratio stays 0.98-1.01 through FY2026.
* Restatements: 8,531 amendment events repeat the previous event's period; 460 changed `at`,
  1,425 changed `ni_ttm` and 880 changed `be`. Example CIK 1764046: the 10-K/A filed
  2021-05-10 restated FY2020 net income from -106.3M to -311.9M (SPAC warrant
  restatement); the event row changes at the amendment's clock only.
* Reverse splits: BK Technologies (CIK 2186) 1:5 in 2023 is detected from restated
  balance-sheet shares and adjusted. Amcor's 1:5 (January 2026, after the Berry merger) is
  detected from the dei jump inside the window.
* Share pairs from 2020 (144,219): median shrs_q/shrs_q_lag4 1.008, p01 0.34, p99 10.4.
  12,737 pairs move more than 1.8x; among linked issuers with assets above $1B it is 542 of
  53,951 (mergers, spin-offs, bankruptcies, SPAC redemptions). Across all emitted events
  5,396 lags were nulled as implausible (>= 100x) and 13,698 were split-adjusted.

### Known limitations

* Company Facts carries no dimensional facts. Multi-class dei share counts (Alphabet, Meta,
  Berkshire, HEICO) fall back to balance-sheet or weighted-average shares, and some banks'
  long-term debt (JPM) or segment-only revenue is invisible.
* IFRS 20-F/40-F filers (ifrs-full taxonomy) have no events; only US-GAAP foreign filers do.
* Splits that no later filing restates stay unadjusted; the plausibility screen only
  removes moves of 100x or more.
* `fscore` requires all nine signals and is NULL for most financials. `xrd_ttm` is NULL, not
  0, when R&D is not reported.
* FSDS stops at 2026q2, so filings after 2026-06-30 use the modeled fc1 clock
  (filed + 46h).
* After-hours acceptances (15,532 events) carry an acceptance clock before the official
  filing date; the panel's one-session lag applies to the acceptance clock.
* `sic_events` uses the FSDS SUB SIC only when the SUB registrant CIK equals the event CIK.
  873 rows have a SIC with no FF49 industry (French's table has no entry).
* One validate run failed with a transient DuckDB "Invalid unicode ... segment statistics"
  error while building its temp table, then passed unchanged on rerun.
