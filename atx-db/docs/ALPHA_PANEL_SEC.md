# Alpha panel: SEC filing metadata, events, earnings calendar, insider transactions (lane SECMETA)

Three stages under the alpha panel build root (`data/alpha_panel/v1`, see `docs/ALPHA_PANEL.md`), each published
last with `common.write_stage_manifest` (SHA-256 of every output file and of the producing code, rules, staleness,
sources, row counts, per-year tables). Data request coverage (`docs/plans/2026-09-28-mega-alpha-data-request-atx-db.md`):
D2 (earnings calendar), D10 (corporate-action events), D11 (insider), D12 (8-K / filing metadata), and inputs to
U3 (filer regime, issuer profile), U4 (delisting events and causes) and U5 (SIC history via former names only).
No statistic in this lane uses returns.

| stage | module | outputs | rows |
| --- | --- | --- | ---: |
| `sec_filings` | `alpha_panel/sec_filings.py` | `filings`, `issuer_profile`, `filer_regime`, `eight_k_items`, `events`, `delisting_causes`, `clock_files` | 18,647,938 filings |
| `earnings_calendar` | `alpha_panel/earnings_calendar.py` | `announcements` | 327,314 |
| `insider` | `alpha_panel/insider.py` | `transactions/year=YYYY/YYYYqN.parquet`, `owners/year=YYYY/YYYYqN.parquet`, `receipts.jsonl` | 4,901,685 transactions, 2,566,677 owner rows |

Tests: `tests/test_alpha_panel_sec_filings.py`, `tests/test_alpha_panel_earnings_calendar.py`,
`tests/test_alpha_panel_insider.py` (29 tests: zip streaming incl. zip64, JSON flattening, item parsing and its SQL
twin, event mapping, regime segments, clock resolution, delisting causes, NYSE calendar, ET timing, expected-date
rule, insider transform on TSV fixtures).

## Build

```powershell
cd C:\atx\atx-db
$env:PYTHONPATH = "C:\atx\atx-db\src"; $env:OPENBLAS_NUM_THREADS = "1"
$G = "..\.superpowers\sdd\tier1-parity\run_memory_guarded.py"
.venv\Scripts\python.exe $G --job-gb 0.8 --wait-minutes 60 -- .venv\Scripts\python.exe -m atx_db.alpha_panel.sec_filings --phase extract   # ~35 min, resumable per 25k-member chunk
.venv\Scripts\python.exe $G --job-gb 0.8 --wait-minutes 60 -- .venv\Scripts\python.exe -m atx_db.alpha_panel.sec_filings --phase assemble  # ~5 min, publishes
.venv\Scripts\python.exe $G --job-gb 0.5 --wait-minutes 60 -- .venv\Scripts\python.exe -m atx_db.alpha_panel.sec_filings --phase verify --scope 2.02   # EDGAR index pages
.venv\Scripts\python.exe $G --job-gb 0.8 --wait-minutes 60 -- .venv\Scripts\python.exe -m atx_db.alpha_panel.earnings_calendar
.venv\Scripts\python.exe $G --job-gb 0.6 --wait-minutes 60 -- .venv\Scripts\python.exe -m atx_db.alpha_panel.insider   # fetch+parse per quarter, resumable, publishes
```

`--phase derived` rebuilds `filer_regime` and `delisting_causes` from the published parquet (no re-extract). The
extract scratch (`_tmp/sec_filings_raw`, 491 MB) and the DuckDB scratch (3.4 GB) were deleted after publication; a
rebuild re-extracts from the zip. `_tmp/insider/acceptance_345.parquet` (42 MB) is kept: it is the insider
stage's resume key (stamped with the SHA-256 of `sec_filings/filings.parquet`), so a new quarter can be added
without reprocessing the others.

## Sources

| source | identity |
| --- | --- |
| SEC bulk submissions | `data/cache/submissions.zip`, 1,564,656,199 bytes, sha256 `702fbcd8b4335bc649e9e4eab3a202f3effc314b43421664bfecb59365767165`, fetched 2026-09-19 (file mtime 2026-09-20 00:07:35 UTC); 991,042 members = 985,667 `CIK##########.json` + 5,374 `-submissions-NNN.json` + 1 other. Read in place: the zip64 central directory is streamed entry by entry and each member inflated with a CRC check (`iter_zip_entries`, `read_zip_member`); nothing is extracted to disk and the ~1M-entry directory never sits in memory as `zipfile.ZipInfo`. |
| SEC Insider Transactions Data Sets | 46 quarterly zips 2015q1..2026q2, URLs taken from `https://www.sec.gov/data-research/sec-markets-data/insider-transactions-data-sets` (page sha256 recorded). Pattern `https://www.sec.gov/files/structureddata/data/insider-transactions-data-sets/YYYYqN_form345.zip`; the newest quarter (2026q2) is under `/files/datastandardsinnovation/data/insider-transactions-data-sets/`. 2026q3 is not yet posted. Each zip deleted after parsing; `insider/receipts.jsonl` (append-only) and the manifest keep url, bytes, sha256, fetched_at. |
| EDGAR index pages | `https://www.sec.gov/Archives/edgar/data/<cik>/<acc>/<acc>-index.htm`, 4 + 71 fetched for clock verification (results in the `sec_filings` manifest). |
| Session calendar | `calendar.parquet` (vendor sessions 2012-03-26..2026-09-18) plus the NYSE rule calendar outside that range. |
| Panel / identity (coverage only) | `panel/` (`member_equity`, `cik`, `link_tier`) and `identity/links_combined.parquet`, bound by manifest SHA in `earnings_calendar`. |

All SEC requests used the approved agent `atx-db/0.1 atx-research@example.com` through `atx_db.sec_http`
(host-wide 5 req/s token bucket, shared 403/429 pause, bounded backoff on 5xx).

## Clocks

Consumer rule (all three stages): a row is usable at decision session `d` only if `available_at < 22:00 UTC of
session d-1`. `available_at` is UTC on every row. Nothing is overwritten: amendments (`/A` forms, 3/A, 4/A, 8-K/A)
are separate accessions and rows.

### The `acceptanceDateTime` clock is mixed per JSON file (finding)

`acceptanceDateTime` in the submissions JSON always ends in `Z`, but it is true UTC in some member files and the
America/New_York wall clock labelled `Z` in others. Evidence (the counts in bullets 2-4 are diagnostics measured
on the first ~70% of the archive before the rule was written; the post-resolution checks below cover everything):

* EDGAR index pages (which print `Accepted` in ET): `0001140361-26-037020` raw `2026-09-17T22:30:24Z`, index
  18:30:24 ET, so this file holds UTC; `0001498233-23-000080` raw `2023-09-21T19:43:38Z`, index 19:43:38 ET, so
  this file holds ET.
* Of 3.65M accessions that appear in more than one CIK file, 1.45M carry two different values, and every pair differs
  by exactly the ET offset (240 or 300 minutes, 1,657,465 of 1,657,465).
* The encoding is a property of the member file: of CIKs with 20+ comparable rows, 9,666 are all-UTC and 29,981
  all-ET, 174 mixed (mixed ones split by file: e.g. extra files ET, recent file UTC).
* Reading everything as UTC puts 3.1% of filer-submitted filings outside EDGAR's 06:00-22:00 ET hours and makes
  4.5% of 8-K/10-Q/10-K filing dates inconsistent with the 17:30 ET cutoff; reading everything as ET is worse
  (46% cutoff-inconsistent).

Rule `acceptance-per-file-clock-v1`: each `(cik, source)` member file is classified from three kinds of evidence
(`clock_evidence_sql`): a shared accession whose two extreme values differ by exactly the ET offset (the earlier
value is the ET file); a time inside EDGAR's 06:00-22:05 ET window under one reading only (filer-submitted forms);
a filing date consistent with the 17:30 ET cutoff under one reading only (8-K, 10-Q, 10-K, 6-K, 20-F, 40-F, DEF
14A, 10-D, 11-K and their amendments). A file is `et` or `utc` when at least 80% of its evidence agrees, `conflict`
otherwise, `unresolved` without evidence. Per row, `acceptance_utc` is then taken from the first that applies:
1. The file's own resolved reading: `acceptance_clock = file_utc | file_et`.
2. The value from resolved files for the same accession: `accession_consensus`.
3. The conservative, later ET reading: `unresolved_conservative`, with `vintage_risk = 'acceptance_clock_unresolved'`.

The raw string is kept in `acceptance_raw`. Per-file evidence and decisions are in
`sec_filings/clock_files.parquet`.

| result | files | rows (2007+) |
| --- | ---: | ---: |
| utc | 62,976 | 11,050,473 |
| et | 263,269 | 7,880,469 |
| conflict | 801 | 30,957 |
| unresolved | 487,239 | 1,860,146 |

Rows 2009+ by `acceptance_clock`: `file_utc` 10,434,462, `file_et` 6,593,775, `accession_consensus` 518,662,
`unresolved_conservative` 1,101,039 (5.9%). 2,961 accessions have resolved files that disagree
(`vintage_risk = 'acceptance_clock_disagreement'`, the earliest resolved value is used).

After resolution: 99.96-100.00% of filer-submitted filings fall inside EDGAR hours and 98.7-99.7% of cutoff forms
obey the 17:30 ET rule, every year (table below). Index-page verification (random samples, `verify` phase):

| sample | file_utc | file_et | accession_consensus | unresolved_conservative |
| --- | --- | --- | --- | --- |
| 8-K item 2.02, 2012+ | 10/10 exact | 10/10 exact | 1/1 exact | 10/10 exact |
| any form, 2012+ | 10/10 exact | 10/10 exact | 10/10 exact | 8/10 exact, 2/10 four hours late (file was UTC), 0 early |
| hand-picked | 2/2 exact | 2/2 exact | | |

The conservative reading is never earlier than the true acceptance. Other atx-db code that reads
`acceptanceDateTime` from the submissions JSON as UTC inherits this defect.

### Other clock rules

* `filings.available_at` = `acceptance_utc`; if acceptance were missing, `filing_date` + 1 day 00:00
  America/New_York (after EDGAR's 22:00 ET close), `available_basis = 'filing_date_eod_et'`. No 2009+ filing lacks
  an acceptance time.
* `issuer_profile` is the 2026-09-19 snapshot: `available_at` = the zip's mtime (2026-09-20 00:07:35 UTC),
  `vintage_risk = 'snapshot_non_pit'` (name, SIC, category, tickers, exchanges, state, fiscal year end are current
  values). `former_names` carry SEC from/to dates and are the only historical profile field.
* `filer_regime.available_at` = acceptance of the segment's first filing; a `none` lapse: the lapse day + 1 at
  04:00 UTC.
* `delisting_causes.cause_available_at` = the latest acceptance among the Form 25 and the evidence used (the label is
  not known before then; the Form 25 itself is known at `available_at`).
* Earnings: `available_at` = `announcement_utc` = the 8-K's resolved acceptance; `expected_date` known at
  `expected_available_at` (the year-ago announcement's acceptance); `next_expected_date` known at the row's
  `available_at`.
* Insider: `available_at` = the accession's `acceptance_utc` from `sec_filings/filings.parquet` (file-resolved
  preferred over consensus over conservative), else `filing_date` + 1 day 00:00 ET. 2,358,778 of 2,358,852
  submissions (99.997%) join an acceptance; 44 transaction rows use the fallback.

Staleness: all three stages are event data, no staleness (`filer_regime` carries explicit `valid_from`/`valid_to`).

## Stage `sec_filings`

### `filings.parquet` (one row per (cik, accession), filing_date 2009-01-01..2026-09-19)

`cik, accession, form, filing_date, acceptance_utc, acceptance_clock, vintage_risk, acceptance_raw, report_date,
items, is_amendment, primary_document, file_number, act, is_xbrl, is_inline_xbrl, size, available_at,
available_basis`. A filing appears once per CIK it is filed under: Forms 3/4/5 under issuer and reporting owners;
25-NSE under the exchange and the subject company; co-registrant 8-Ks under each registrant. `items` is SEC's item
string for 8-K forms. Duplicates of (cik, accession) across a CIK's main and extra files (51 of 20.8M raw rows):
the main file wins.

### `issuer_profile.parquet` (985,667 rows, one per CIK)

`cik, name, entity_type, sic, sic_description, owner_org, category, state_of_incorporation,
state_of_incorporation_description, fiscal_year_end, tickers[], exchanges[], ein, lei, flags,
insider_tx_for_issuer_exists, insider_tx_for_owner_exists, business_state_or_country, business_is_foreign,
former_names[{name, from_date, to_date}], n_extra_files, n_filings_total, vintage_risk, snapshot_date,
available_at`.

### `filer_regime.parquet` (106,551 segments)

`cik, regime, valid_from, valid_to, first_filing, last_filing, n_filings, available_at`. Regimes from original
forms: `domestic` (10-K, 10-Q, 10-KT, 10-QT, 10-K405, 10-KSB, 10-QSB), `fpi_20f` (20-F, 20-FR12B/G), `canadian_40f`
(40-F, 40FR12B/G), `fund` (N-CSR, N-CSRS, N-1A, N-2, 485BPOS/APOS/BXT, N-Q, NPORT-P, N-CEN, N-MFP*, N-SAR, 24F-2NT,
497, 497K, ...; N-PX is excluded because 13F managers file it since 2024), `none` (lapse). A segment runs from its first
filing to the day before a different regime's filing, or its grace after the last filing (domestic and fund 400
days, 20-F and 40-F 550 days: annual-only, drifting dates). Filings from 2007 seed the state; segments clipped to
2009-01-01..2026-09-19. Only CIKs with at least one regime form have rows. Examples: SHOP fpi_20f
2016-02-17..2017-02-14, canadian_40f ..2025-02-10, domestic from 2025-02-11; TSM fpi_20f throughout; AAPL domestic;
SPY fund. On 2024-06-30: domestic 7,374 CIKs, fund 5,347, fpi_20f 943, canadian_40f 153.

### `eight_k_items.parquet` (2,846,172 rows)

One row per (cik, accession, item) for 8-K forms (8-K, 8-K/A, 8-K12B, ...): `cik, accession, form, filing_date,
acceptance_utc, available_at, report_date, is_amendment, acceptance_clock, vintage_risk, item`. Items are normalised
to `N.NN` (`parse_items` and its SQL twin `items_explode_sql`, tested equal). Co-registrant 8-Ks repeat per CIK, so
the table is unique on (accession, item) within a CIK.

### `events.parquet` (1,082,328 rows)

`cik, accession, form, event_type, item, source, event_date, event_utc, available_at, filing_date, is_amendment,
acceptance_clock, vintage_risk`. `event_utc` = acceptance (public time); `event_date` = SEC reportDate (the date
of the earliest event the filing reports).

* 8-K items: 2.02 `earnings_release`, 2.01 `acquisition_completed`, 1.01 `material_agreement`, 1.03 `bankruptcy`,
  3.01 `delisting_notice`, 5.01 `change_in_control`, 4.02 `nonreliance_restatement`, 4.01 `auditor_change`,
  8.01 `other_events`.
* Forms: 25 `form25_delisting` (issuer-filed), 25-NSE `form25nse_delisting` (exchange-filed),
  15-12B / 15-12G / 15-15D, and 15F-12B / 15F-12G / 15F-15D (deregistration/suspension).
* Late filings: NT 10-K, NT 10-Q, NT 20-F.

The source has no `15-12B` after 2022; `15-12G` absorbs 12(b) deregistrations (345 in 2022, 649 in 2023). Buyback
announcements (D10) have no item code: they sit in 8.01 / 7.01 text, not parsed here.

### `delisting_causes.parquet` (16,118 rows)

One row per original Form 25 / 25-NSE of a subject CIK: `cik, accession, form, filing_date, available_at,
filer_regime, cause, merger_date, bankruptcy_date, deficiency_notice_date, form15_date, next_periodic_filing,
cause_available_at`. Rows under an exchange's own CIK are dropped. An exchange here is a CIK that self-files 20 or
more 25-NSEs (accession prefix = CIK): NYSE 876661, Nasdaq 1354457, NYSE Arca 1143362, NYSE American 1143313
and Cboe BZX 1417835.

Causes, first match in order:
1. `bankruptcy`: an 8-K item 1.03 filed from 730 days before to 90 days after the Form 25.
2. `merger_or_acquisition`: item 2.01 or 5.01 filed -120..+90 days.
3. `exchange_deficiency`: a 25-NSE with an item 3.01 filed -400..+30 days.
4. `still_reporting`: a 10-K/10-Q filed 30..200 days after, or a 20-F/40-F/6-K 30..400 days after. These are
   class, debt or preferred delistings, or exchange transfers.
5. `fund_or_trust`: the filer regime is `fund` on the filing date.
6. `voluntary_deregistration`: a Form 15 filed -30..+120 days.
7. `unknown`: none of the above.

This is a heuristic label: a Form 25 names a class of securities, and the class is in the document, not the
metadata. Checks:
* Twitter 25-NSE 2022-10-28 08:31 ET is `merger_or_acquisition`, backed by an 8-K with items 2.01/3.01/5.01
  accepted 2022-10-28 20:22 ET and a 15-12G on 2022-11-07.
* SVB 25-NSE 2023-05-02 is `bankruptcy` (item 1.03 on 2023-03-10/17).
* Activision 25-NSE 2023-10-13 is `merger_or_acquisition`.

| year | bankruptcy | merger_or_acquisition | exchange_deficiency | still_reporting | fund_or_trust | voluntary_deregistration | unknown |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2018 | 34 | 266 | 70 | 241 | 168 | 49 | 102 |
| 2019 | 30 | 282 | 65 | 220 | 155 | 53 | 111 |
| 2020 | 63 | 241 | 67 | 295 | 184 | 80 | 103 |
| 2021 | 13 | 433 | 37 | 354 | 124 | 42 | 109 |
| 2022 | 25 | 330 | 157 | 224 | 162 | 124 | 80 |
| 2023 | 75 | 280 | 222 | 255 | 210 | 171 | 56 |
| 2024 | 56 | 235 | 178 | 222 | 156 | 64 | 59 |
| 2025 | 38 | 261 | 144 | 242 | 183 | 65 | 84 |
| 2026 | 16 | 188 | 55 | 125 | 184 | 45 | 116 |

(2009-2017 in the manifest's `delisting_causes_per_year`.) Delisting *returns* (U4) are not computed here; this
table supplies the dated cause for the controller's delisting-return work.

### Filing coverage by year (post-resolution clock checks)

| year | filings | CIKs | 8-K* | 10-K/10-Q | in EDGAR window | 17:30 cutoff consistent | clock unresolved |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2009 | 938,363 | 126,162 | 84,989 | 37,656 | 0.9999 | 0.9926 | 0.055 |
| 2010 | 979,559 | 128,435 | 83,262 | 35,715 | 1.0000 | 0.9932 | 0.053 |
| 2011 | 979,579 | 129,333 | 82,729 | 34,516 | 1.0000 | 0.9905 | 0.048 |
| 2012 | 975,057 | 128,436 | 80,149 | 32,579 | 1.0000 | 0.9938 | 0.047 |
| 2013 | 990,548 | 129,895 | 79,205 | 31,355 | 1.0000 | 0.9912 | 0.046 |
| 2014 | 996,851 | 134,706 | 79,574 | 30,960 | 1.0000 | 0.9942 | 0.053 |
| 2015 | 995,452 | 135,569 | 78,725 | 30,151 | 0.9998 | 0.9944 | 0.053 |
| 2016 | 953,019 | 132,589 | 74,548 | 28,198 | 1.0000 | 0.9923 | 0.056 |
| 2017 | 971,318 | 135,020 | 73,243 | 27,058 | 1.0000 | 0.9942 | 0.056 |
| 2018 | 949,939 | 132,837 | 70,043 | 26,308 | 0.9998 | 0.9945 | 0.061 |
| 2019 | 951,714 | 133,524 | 68,198 | 25,408 | 0.9996 | 0.9938 | 0.061 |
| 2020 | 1,045,523 | 135,693 | 74,139 | 24,748 | 1.0000 | 0.9872 | 0.058 |
| 2021 | 1,171,999 | 160,439 | 76,467 | 27,574 | 1.0000 | 0.9963 | 0.067 |
| 2022 | 1,116,615 | 158,755 | 71,540 | 28,550 | 1.0000 | 0.9947 | 0.074 |
| 2023 | 1,160,211 | 148,166 | 73,850 | 26,469 | 1.0000 | 0.9965 | 0.064 |
| 2024 | 1,241,456 | 147,627 | 70,058 | 24,648 | 1.0000 | 0.9967 | 0.063 |
| 2025 | 1,236,309 | 151,916 | 69,151 | 23,979 | 1.0000 | 0.9968 | 0.068 |
| 2026 | 994,426 | 137,925 | 49,114 | 18,106 | 0.9997 | 0.9935 | 0.073 |

### Events by year (all filers, originals and amendments)

| year | 2.02 | 1.01 | 2.01 | 5.01 | 1.03 | 3.01 | 4.02 | 4.01 | 8.01 | 25-NSE | 25 | 15-12B | 15-12G | 15-15D | NT 10-K | NT 10-Q |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2018 | 17,911 | 11,027 | 1,761 | 600 | 89 | 904 | 137 | 868 | 17,261 | 1,658 | 105 | 274 | 293 | 436 | 980 | 1,678 |
| 2019 | 17,478 | 10,645 | 1,551 | 496 | 116 | 1,136 | 103 | 743 | 16,589 | 1,630 | 111 | 275 | 235 | 421 | 883 | 1,561 |
| 2020 | 18,046 | 11,611 | 1,275 | 495 | 182 | 1,022 | 104 | 631 | 20,600 | 1,804 | 141 | 254 | 146 | 230 | 739 | 1,085 |
| 2021 | 18,757 | 13,085 | 1,775 | 658 | 79 | 883 | 866 | 799 | 20,281 | 2,050 | 100 | 314 | 145 | 329 | 717 | 1,902 |
| 2022 | 19,223 | 10,495 | 1,369 | 490 | 44 | 1,530 | 307 | 822 | 16,259 | 1,988 | 118 | 126 | 355 | 777 | 1,004 | 1,646 |
| 2023 | 18,676 | 11,070 | 1,157 | 441 | 159 | 2,448 | 269 | 856 | 18,185 | 2,308 | 129 | 0 | 653 | 183 | 993 | 1,958 |
| 2024 | 17,919 | 11,197 | 1,118 | 426 | 160 | 2,402 | 250 | 1,029 | 17,166 | 1,704 | 123 | 0 | 477 | 132 | 978 | 1,689 |
| 2025 | 17,292 | 11,389 | 1,195 | 442 | 88 | 1,635 | 151 | 812 | 17,945 | 1,796 | 132 | 0 | 452 | 149 | 742 | 1,174 |
| 2026 | 12,595 | 7,621 | 967 | 332 | 51 | 1,000 | 81 | 455 | 12,591 | 1,315 | 82 | 0 | 333 | 140 | 531 | 657 |

(2009-2017 in the manifest's `coverage.events_per_year`. 25-NSE counts include the exchange-CIK copy of each
filing.)

## Stage `earnings_calendar` (D2)

`announcements.parquet`: one row per (cik, accession) of an original 8-K with item 2.02. Columns: `cik,
accession, form, items, filing_date, event_date, announcement_utc, announcement_et, available_at,
acceptance_clock, vintage_risk, timing, session_date, reaction_session, session_basis, fiscal_period_end,
fiscal_period_form, period_lag_days, same_session_dup, is_primary, next_expected_date, expected_date,
expected_rule, expected_available_at, expected_source_accession, expected_error_sessions, expected_error_days`.

* `timing` from the acceptance time in America/New_York against the NYSE session. `pre_market` is before 09:30.
  `intraday` runs from 09:30 to the close (16:00, or 13:00 on the day after Thanksgiving and on July 3 / Dec 24 when
  Monday-Thursday). `post_market` is at or after the close. `closed_day` means the ET date has no session (EDGAR is
  open on Good Friday).
* `session_date` = the ET date if it is a session, else the next session. `reaction_session` = the first session
  whose close reflects the announcement: `session_date` for pre-market, intraday and closed_day, the next session for
  post-market.
* Sessions: `calendar.parquet` inside 2012-03-26..2026-09-18, the NYSE rule calendar (`nyse_holidays`: weekend
  observance, Juneteenth from 2022, Good Friday, closures 2012-10-29/30, 2018-12-05, 2025-01-09) outside. The rule
  reproduces all 3,642 vendor sessions exactly (0 mismatches either way).
* `fiscal_period_end` = the latest original 10-Q/10-K/10-QT/10-KT `report_date` at or before the announcement's ET
  date. Filings of any date count, so this is a label, not a signal. `is_primary` = the first announcement per
  (cik, fiscal_period_end) with lag <= 120 days that is not a second 2.02 on the same reaction session
  (`same_session_dup`).
* Expected dates, rule `yoy_364`, point in time by construction:
  - `next_expected_date` (primary rows) = the first session on or after `session_date` + 364 days, i.e. the same
    fiscal quarter next year. It is known at the row's own `available_at`. To get the next announcement date as of
    `d`, take the minimum `next_expected_date >= d` over rows visible at `d`.
  - `expected_date` = the year-ago primary announcement's `next_expected_date`. The year-ago announcement is the one
    whose fiscal period end is 345-385 days earlier, closest to 365. `expected_available_at` = its acceptance.
  - Fallback `prev_primary_plus_91` = the previous primary announcement's session + 91 days, rolled forward.
  - `expected_error_sessions` / `_days` = actual `session_date` minus `expected_date`.

| year | ann. | CIKs | primary | pre | intraday | post | closed | 10-Q CIKs | share >=3 ann. | share >=1 | yoy expected | exact | +-1 sess. | +-5 sess. | median abs err |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2009 | 20,491 | 5,153 | 18,046 | 0.276 | 0.271 | 0.453 | 0.0006 | 9,546 | 0.470 | 0.532 | 15 | | | | |
| 2010 | 19,596 | 4,996 | 17,588 | 0.292 | 0.244 | 0.464 | 0.0004 | 9,115 | 0.477 | 0.540 | 15,770 | 0.327 | 0.518 | 0.825 | 1 |
| 2011 | 19,068 | 4,859 | 17,199 | 0.305 | 0.228 | 0.466 | 0.0013 | 8,830 | 0.478 | 0.544 | 15,367 | 0.358 | 0.551 | 0.854 | 1 |
| 2012 | 18,812 | 4,735 | 16,889 | 0.314 | 0.207 | 0.463 | 0.0163 | 8,380 | 0.498 | 0.559 | 15,208 | 0.329 | 0.513 | 0.851 | 1 |
| 2013 | 18,483 | 4,679 | 16,651 | 0.320 | 0.189 | 0.490 | 0.0012 | 8,030 | 0.511 | 0.573 | 14,984 | 0.335 | 0.517 | 0.852 | 1 |
| 2014 | 18,900 | 4,749 | 17,009 | 0.325 | 0.165 | 0.509 | 0.0005 | 7,872 | 0.533 | 0.595 | 14,988 | 0.358 | 0.553 | 0.854 | 1 |
| 2015 | 19,021 | 4,777 | 17,195 | 0.335 | 0.147 | 0.518 | 0.0005 | 7,676 | 0.552 | 0.614 | 15,369 | 0.362 | 0.545 | 0.856 | 1 |
| 2016 | 18,440 | 4,571 | 16,703 | 0.336 | 0.133 | 0.530 | 0.0008 | 7,147 | 0.578 | 0.632 | 15,304 | 0.333 | 0.506 | 0.861 | 1 |
| 2017 | 17,983 | 4,450 | 16,275 | 0.341 | 0.117 | 0.541 | 0.0004 | 6,816 | 0.593 | 0.644 | 14,980 | 0.362 | 0.544 | 0.865 | 1 |
| 2018 | 17,726 | 4,409 | 16,086 | 0.347 | 0.106 | 0.546 | 0.0006 | 6,549 | 0.608 | 0.666 | 14,717 | 0.384 | 0.570 | 0.860 | 1 |
| 2019 | 17,321 | 4,334 | 15,876 | 0.349 | 0.093 | 0.557 | 0.0007 | 6,327 | 0.623 | 0.678 | 14,573 | 0.378 | 0.568 | 0.866 | 1 |
| 2020 | 17,874 | 4,329 | 15,806 | 0.357 | 0.083 | 0.560 | 0.0003 | 6,235 | 0.626 | 0.686 | 14,533 | 0.276 | 0.429 | 0.798 | 2 |
| 2021 | 18,585 | 4,698 | 16,819 | 0.358 | 0.074 | 0.567 | 0.0002 | 7,073 | 0.581 | 0.657 | 14,645 | 0.345 | 0.511 | 0.825 | 1 |
| 2022 | 19,045 | 4,686 | 17,354 | 0.359 | 0.064 | 0.577 | 0.0004 | 7,104 | 0.609 | 0.654 | 15,454 | 0.410 | 0.583 | 0.859 | 1 |
| 2023 | 18,519 | 4,563 | 16,820 | 0.365 | 0.061 | 0.574 | 0.0003 | 6,608 | 0.634 | 0.684 | 15,816 | 0.410 | 0.581 | 0.864 | 1 |
| 2024 | 17,770 | 4,393 | 16,172 | 0.377 | 0.052 | 0.570 | 0.0012 | 6,146 | 0.655 | 0.707 | 15,144 | 0.346 | 0.505 | 0.860 | 1 |
| 2025 | 17,164 | 4,275 | 15,661 | 0.383 | 0.050 | 0.566 | 0.0005 | 5,974 | 0.649 | 0.706 | 14,651 | 0.373 | 0.543 | 0.864 | 1 |
| 2026 | 12,516 | 4,104 | 11,394 | 0.388 | 0.048 | 0.564 | 0.0006 | 5,874 | 0.613 | 0.692 | 10,530 | 0.431 | 0.595 | 0.868 | 1 |

The "10-Q CIKs" denominator (CIKs filing at least one 10-Q that year) includes shells, SPACs and small filers
that never furnish an earnings 8-K. 2009 has no year-ago rows (2008 8-Ks are outside the window); the 2020
expected-date error widens with COVID-era delays; 2026 is partial (through 2026-09-19).

### Coverage over `member_equity` cells (basis of the data request's Appendix A)

The share of panel `member_equity` (session, line) cells whose linked CIK has a primary 2.02 announcement visible at
the cell (`available_at` < 22:00 UTC of d-1). The 364-day window is equivalent to a `next_expected_date >= d` being
known.

| year | member_equity cells | linked | ann. <= 120 d (all cells) | ann. <= 364 d (all) | ann. <= 364 d (linked) | strict | backfill | name |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2018 | 450,095 | 0.956 | 0.851 | 0.862 | 0.901 | 0.978 | 0.547 | 0.642 |
| 2019 | 574,893 | 0.966 | 0.854 | 0.862 | 0.893 | 0.978 | 0.537 | 0.606 |
| 2020 | 575,906 | 0.968 | 0.848 | 0.858 | 0.886 | 0.969 | 0.538 | 0.589 |
| 2021 | 602,988 | 0.967 | 0.842 | 0.851 | 0.880 | 0.963 | 0.569 | 0.684 |
| 2022 | 580,024 | 0.973 | 0.858 | 0.866 | 0.889 | 0.964 | 0.606 | 0.679 |
| 2023 | 184,719 | 0.979 | 0.861 | 0.870 | 0.889 | 0.963 | 0.592 | 0.701 |
| 2024 | 568,722 | 0.970 | 0.852 | 0.859 | 0.886 | 0.959 | 0.593 | n/a |
| 2025 | 548,225 | 0.976 | 0.852 | 0.859 | 0.880 | 0.956 | 0.592 | n/a |
| 2026 | 385,629 | 0.984 | 0.860 | 0.867 | 0.882 | 0.966 | 0.597 | n/a |

Strict-linked members are covered at 96-98%. The shortfall is structural and sits in the backfill and name tiers:
foreign private issuers (6-K earnings releases carry no item code), REITs and trusts that do not furnish 2.02, and
name-tier link noise. **The current panel's 2023 partition is incomplete:** only months 01-04 are present, and
`panel/year=2023/panel-05.parquet.partial` was left by an interrupted panel build on 2026-09-27. The 2023 row
covers Jan-Apr only; this is the panel stage's issue, not this lane's.

### Validation (SEC evidence only)

* **Spot checks**, primary rows over 2009-2026, ET acceptance of the item 2.02 8-K:
  - AAPL: 72 of 72 post-market, median 16:30 (16:27-18:55).
  - MSFT: 68 post-market (median 16:05), 2 pre-market, 1 intraday at 15:57.
  - JPM: 70 pre-market (median 06:46), 1 post-market.
  - GS: 70 pre-market (07:39).
  - WMT: 71 pre-market (07:00).
  - NVDA: 69 post-market (16:23).

  Recent JPM, AAPL and MSFT expected dates hit exactly or within 2 sessions (in the manifest's `spot_checks`).
* **Event date vs acceptance**, 2012+: the 8-K's own event date equals the ET date of the resolved acceptance for
  86-89% of pre- and post-market releases. The rate is the same under `file_utc` (0.891 / 0.893) and `file_et`
  (0.871 / 0.856), which is independent confirmation of the per-file clock. A wrong 4-5 h clock would move evening
  filings onto the next day. Intraday and closed-day 8-Ks report a prior-day event 19-21% of the time: a release
  after yesterday's close, filed today. The 8-K acceptance is then later than the wire release, and
  `reaction_session` is conservative (never early).
* **Fiscal period**: the lag from period end to announcement is p10 / p50 / p90 = 20 / 34 / 54 days, and 99.9% are
  within 90 days, every year. The matched 10-Q/10-K is filed on or after the release 93% of the time in 2009 and
  98% in 2025, the same day for 20% in 2009 and 48% in 2025, with a median gap of 6 days in 2009 and 0 in 2025.
* **Vendor flag comparison: vendor flag, known unreliable, not a validation.** Per the owner ruling of 2026-09-28,
  the TickerHistory3 earnFlag has bad logic. It is not a target or a reference, no rule was tuned or accepted on it,
  and no output column derives from it. The manifest keeps a labelled `vendor_flag_comparison` for information
  only: 150,587 (announcement, linked line) pairs, 58% on the same session, 81% within one.

## Stage `insider` (D11)

`transactions/year=YYYY/YYYYqN.parquet` holds one row per non-derivative (NONDERIV_TRANS) and derivative
(DERIV_TRANS) transaction. Columns:
* **Filing:** `table_type, issuer_cik, issuer_name, issuer_symbol, accession, form, is_amendment, filing_date,
  period_of_report, date_of_orig_sub, aff10b5one`.
* **Primary reporting owner:** `owner_cik, owner_name, is_director, is_officer, officer_title,
  is_ten_percent_owner, is_other, other_text`.
* **All owners:** `n_reporting_owners, reporting_owner_ciks[], any_director, any_officer, any_ten_percent_owner`.
* **Transaction:** `trans_sk, security_title, transaction_date, deemed_execution_date, trans_form_type,
  transaction_code, equity_swap, timeliness, shares, price, acquired_disposed, shares_owned_after,
  direct_indirect, nature_of_ownership`.
* **Derivative only:** `conv_exercise_price, exercise_date, expiration_date, underlying_title,
  underlying_shares, total_value`.
* **Flags and clock:** `price_footnoted, shares_footnoted, acceptance_utc, acceptance_clock, vintage_risk,
  available_at, available_basis, dataset_quarter`.

The primary owner (`owner_*`) is the reporting owner with the smallest CIK on the filing, which is deterministic;
the `any_*` flags cover every owner. `owners/` holds one row per (accession, reporting owner) with the relationship
flags, title, issuer, form, filing date and clock. Joint filings share one transaction across
owners (4.3% of transaction rows are on joint filings), so sum shares per transaction, not per owner. Holdings tables (NONDERIV_HOLDING, DERIV_HOLDING) and
footnote texts are not loaded.

| year | transactions | non-deriv | deriv | filings | issuers | issuers with P/S | P (buys) | S (sales) | amendments | acceptance basis | EOD fallback | clock unresolved |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2015 | 443,218 | 310,999 | 132,219 | 198,222 | 5,948 | 5,035 | 43,263 | 81,513 | 9,659 | 443,217 | 1 | 30 |
| 2016 | 407,662 | 287,587 | 120,075 | 187,862 | 5,653 | 4,710 | 37,605 | 71,014 | 8,863 | 407,662 | 0 | 17 |
| 2017 | 413,639 | 292,115 | 121,524 | 185,812 | 5,526 | 4,608 | 30,179 | 78,927 | 7,649 | 413,639 | 0 | 4 |
| 2018 | 427,387 | 305,934 | 121,453 | 185,530 | 5,472 | 4,540 | 47,823 | 76,274 | 7,575 | 427,387 | 0 | 14 |
| 2019 | 410,655 | 293,866 | 116,789 | 180,246 | 5,283 | 4,438 | 41,106 | 74,477 | 7,758 | 410,655 | 0 | 30 |
| 2020 | 430,379 | 309,873 | 120,506 | 185,340 | 5,388 | 4,550 | 33,467 | 93,361 | 7,588 | 430,379 | 0 | 8 |
| 2021 | 508,320 | 368,826 | 139,494 | 198,982 | 5,918 | 4,893 | 28,095 | 136,009 | 7,850 | 508,320 | 0 | 1 |
| 2022 | 407,747 | 292,273 | 115,474 | 186,744 | 5,681 | 4,690 | 35,292 | 73,379 | 6,816 | 407,732 | 15 | 0 |
| 2023 | 400,064 | 285,611 | 114,453 | 185,510 | 5,651 | 4,534 | 31,101 | 71,112 | 6,424 | 400,064 | 0 | 1 |
| 2024 | 411,461 | 300,344 | 111,117 | 180,821 | 5,333 | 4,377 | 23,921 | 93,250 | 5,201 | 411,461 | 0 | 0 |
| 2025 | 390,812 | 284,747 | 106,065 | 172,180 | 5,209 | 4,288 | 23,452 | 83,924 | 4,857 | 390,792 | 20 | 1 |
| 2026 | 250,341 | 182,061 | 68,280 | 108,576 | 5,137 | 3,821 | 11,771 | 51,403 | 3,023 | 250,333 | 8 | 2 |

(Year = filing year; 2026 = 2026q1-q2.) Checks:
* Tim Cook's (CEO) AAPL sales of 2023-04-03 appear on the Form 4 filed 2023-04-04, accepted 18:31 ET.
* No row has `available_at` before its filing date.
* 343 rows have an acceptance two or more days after the filing date (re-accepted filings; the later clock is
  used).
* 678 rows report a `transaction_date` after the filing date (filer data errors, kept as filed).
* 7 submissions have a data-set filing date that differs from EDGAR's.

## Gaps and risks

1. **Acceptance clock.** 5.9% of 2009+ filing rows (1.10M) come from member files with no clock evidence. They use
   the conservative ET reading, flagged `vintage_risk = 'acceptance_clock_unresolved'`. On index pages that reading
   was exact in 18 of 20 samples and 4 h late in 2, never early. The share is small where it matters: 80 of
   268,159 2.02 announcements (2012+) and 108 insider rows.
2. **Other atx-db code.** The same `acceptanceDateTime` defect affects any other atx-db module that reads the
   submissions JSON clock as UTC (for example `sec_submissions.py`). It is not changed here (other lanes' files).
3. **8-K vs wire release.** The 8-K acceptance can follow the press release (19-21% of intraday 2.02 8-Ks report
   a prior-day event). The calendar is then conservative by up to one session. Wire times are not available.
4. **Earnings coverage outside domestic filers.** Foreign private issuers announce on 6-K without item codes, so
   they are not in the calendar. The backfill tier shows about 55-60% coverage.
5. **Issuer profile is a snapshot.** Name, SIC, category, tickers and exchanges are as of 2026-09-19
   (`snapshot_non_pit`). Historical SIC (U5) is not in the submissions data. Former names are dated.
6. **Delisting causes are heuristic.** The 25-NSE security class (common, preferred, notes) is not in the metadata.
   `unknown` is 56-116 rows a year. Delisting returns are not computed.
7. **Buybacks, spin-offs, index changes (D10)** have no 8-K item code; only 1.01 / 2.01 / 5.01 / 8.01 events are
   dated here.
8. **Insider scope.** No holdings tables and no footnote text are loaded. The primary owner is chosen by smallest
   CIK, not file order (DuckDB does not preserve TSV order). 2026q3 is not yet published by the SEC.
9. **The 2023 panel partition is incomplete** (see member coverage). Rerun `earnings_calendar` after the panel is
   rebuilt to refresh the member-basis table.
10. The earnings calendar starts in 2009, so the 2009 rows have no year-ago expected date.
