# Alpha panel: ownership, insiders and short side (tier1-v3 lane OWN, S5)

This doc covers the stages built by lane OWN of the tier1-v3 warehouse plan
(`docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md`, section S5):

| task | stage | module(s) | what |
|---|---|---|---|
| S5.1 | `thirteenf_filer_type/` | `adv.py`, `filer_type.py` | dated institution type per 13F filer and quarter |
| S5.2 | `nport/` | `nport.py` | N-PORT fund equity holdings, fund ownership per security-quarter, fund flows |
| S5.3 | `stakes/` | `stakes.py` | Schedule 13D / 13G stakes (EDGAR XML 2024-12+, text 13D 2019-2024) |
| S5.4 | `insider_ext/` | `form144.py`, `insider_measures.py` | Form 144 notices, insider net buying (CMP routine / opportunistic) |
| S5.5 | (check only) | - | Reg SHO threshold-list completeness, 5 listing markets, 2018+ |

Shared helpers for SEC documents live in `sec_docs.py`: the accession list from the published `sec_filings` stage,
fetching primary documents once into a `FetchLedgerStore`, and HTTP range reads of SEC zips (central directory from the
zip tail, one ranged GET per member, CRC-checked inflate), so only the needed members of large data-set zips are
downloaded. Every SEC request goes through `atx_db.sec_http` (approved agent, host-wide 5 req/s limiter).

Every module docstring holds its full rules; each stage writes `manifest.json` last with output and code SHA-256, the
input manifests' SHA-256, the source ledgers, the clock and staleness rules and the build receipt.

## Common rules

- **Visibility.** Every row carries `available_at` (UTC). A consumer uses a value at decision session d only if
  `available_at < 22:00 UTC of session d-1`.
- **Vintages.** Nothing is overwritten: amendments (13D/A, 13G/A, 144/A, NPORT-P/A, Form 4/A) are their own rows.
  Where a clock is a documented floor rather than an observed publication, `vintage_risk` says so.
- **Keys.** Security-level outputs carry `security_id` (TickerHistory3 line) through the stage-`thirteenf` PIT CUSIP
  map (`cusip_map_pit`, FTD evidence public by the 13F deadline), with ISIN and ticker fallbacks where stated;
  issuer-level outputs carry the EDGAR CIK. Delisted issuers are kept.
- **Raw landings.** Under `data/raw/<source>/`: `sec_adv/`, `sec_nport/`, `sec_13dg/`, `sec_144/`. Zips and CSV / TSV
  members are parsed and deleted; the receipts (`receipts.jsonl`, `fetch-ledger.jsonl`) keep url, range, bytes,
  SHA-256, HTTP status and Last-Modified. Primary documents of individual filings stay as gzip objects in the
  fetch-ledger store (small; they are the evidence for re-parsing).

## S5.1 13F filer type (`thirteenf_filer_type/`)

**Output.** `filer_type.parquet`, one row per (13F filer CIK, calendar quarter `period_q`) from 2018q4:

- `filer_type`: `hedge_fund`, `mutual_fund` (RIC / BDC adviser), `bank_trust`, `insurance`, `pension_endowment`
  (public and private pensions, endowments, foundations, sovereign wealth funds, central banks), `broker_dealer`,
  `other` (evidenced: wealth-management and institutional RIAs, private equity, corporates) or `unclassified` (no
  evidence; never a default);
- `filer_subtype` (finer label), `type_basis` (the rule that fired), `match_method` (how the 13F filer was linked to a
  Form ADV adviser), `adviser_crd`, `adviser_name`, `family_n_advisers`, roster date and kind, the evidence
  (`ric_share`, `piv_share`, `retail_share`, `raum_usd`, `n_hedge_funds`, `n_pe_vc_re_funds`,
  `bulk_hedge_gav_share`, `sic`), the filer's 13F SH value in the quarter (`sh_value_usd`, the coverage weight);
- `filer_type_pit` / `filer_subtype_pit` / `type_basis_pit` / `match_method_pit` / `available_at_pit`: the same
  rules on evidence published by the 13F deadline only (no bulk Schedule D backfill, no later roster);
- `available_at`, `vintage_risk` (`roster_after_cutoff`, `adv_bulk_backfill`, `sic_snapshot`).

**Sources.**

| source | url | size | span | cadence | terms / rate |
|---|---|---|---|---|---|
| Form ADV monthly rosters (registered + exempt) | https://www.sec.gov/data-research/sec-markets-data/information-about-registered-investment-advisers-exempt-reporting-advisers | 1.6-14 MB per file | 2018-10 .. 2026-08 (Feb / May / Aug / Nov files kept) | monthly | SEC public data; 5 req/s host-wide |
| Form ADV Part 1 bulk (base A, 1D3 CIK, Schedule D 7.B.1) | https://www.sec.gov/data-research/sec-markets-data/form-adv-data (two zips, 0.70 / 0.43 GB) | members only, by HTTP range | 2011-11-05 .. 2024-12-31 | frozen (no bulk after 2024) | SEC public data |
| 13F data sets: COVERPAGE, OTHERMANAGER2 | https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets | members only, by HTTP range | 2019q1 .. | quarterly | SEC public data |
| sec_filings stage | (lake) | - | - | - | issuer profile (SIC, names), X-17A-5 / N-CSR / N-CEN / NPORT-P filers |

**Linking a 13F filer to its adviser** (`LINK_ORDER`, first wins): the adviser's EDGAR CIK on the roster
(`cik_roster`, 2023+ rosters), the CRD on the 13F cover page (`crd_cover`), the SEC 801- / 802- number on the cover
(`sec_number_cover`), the bulk ADV CIK list (`cik_adv_bulk`, backfill), normalised name + state
(`name_state`), unique normalised name (`name_unique`). Holding companies without their own ADV (BlackRock Inc.,
FMR LLC, ...) are typed through the included managers listed on their own 13F (`OTHERMANAGER2`: CRD / SEC number /
CIK), aggregated by regulatory AUM (`included_managers:` bases).

**Rules** (`filer_type.classify`, ordered, first match): own ADV evidence; sovereign / pension / endowment names; bank
SIC or name; insurance SIC or name; included-manager family; X-17A-5 filer; SIC 6211; name root of an X-17A-5
filer; fund registrant or SIC 6722/6726; SIC 6282 (adviser without ADV link); other SICs (corporate). For advisers
(`classify_adviser`): Item 6.A bank / trust company -> `bank_trust`; private-fund adviser (exempt reporting adviser, or
pooled-vehicle RAUM share >= 0.5) -> hedge fund / private equity by the roster fund counts (2023+), else the bulk
Schedule D 7.B.1 gross asset value by fund type (backfill), else (PIT) pooled vehicles with a performance fee ->
`hedge_fund`; RIC + BDC RAUM share >= 0.5 -> `mutual_fund`; broker-dealer -> `broker_dealer`; else `other` with
subtype `ria_wealth` / `ria_institutional` / `ria_no_raum`.

**Clock.** The latest clock of the evidence used: the roster's `available_at` (rule `adv-roster-publication-v1`:
HTTP Last-Modified within [D, D+10 d] of the roster date D, else D + 14 days, `vintage_risk`), the 13F filing's
`available_at`, the bulk zip's Last-Modified for Schedule D / bulk CIK evidence (`adv_bulk_backfill`). The PIT
columns use only evidence published before the 13F deadline + 46 h.

## S5.2 N-PORT (`nport/`)

**Source.** Form N-PORT data sets, https://www.sec.gov/data-research/sec-markets-data/form-n-port-data-sets,
`YYYYqN_nport.zip` (0.24-0.44 GB), 2019q4 onward, quarterly (reports filed in the quarter). Only six members are read
by HTTP range (SUBMISSION, REGISTRANT, FUND_REPORTED_INFO, MONTHLY_TOTAL_RETURN, FUND_REPORTED_HOLDING,
IDENTIFIERS), parsed per quarter and deleted. Until the 2024 N-PORT amendments apply, only the report for the third
month of each fiscal quarter is public, so each series appears about once per calendar quarter at its own fiscal
quarter end.

**Outputs.**

- `parts/quarter=YYYYqN/filings.parquet` (one row per accession: registrant, series, report date, net assets, Item
  B.6 monthly flows, `ret_q` = median over classes of the compounded 3-month total return, `available_at`) and
  `holdings.parquet` (equity rows EC / EP: CUSIP, ISIN, ticker, balance and unit, value, payoff profile, ...);
- `security_map.parquet`: (calendar quarter, report date, CUSIP, ISIN, ticker) -> `security_id`, rule
  `nport-cusip-13fmap-v1`: 13F PIT CUSIP map at the calendar quarter end, then the CUSIP inside a US / CA ISIN
  (check digit verified), then the ticker by the stage-S as-of rule on the report date; `map_basis` says which;
- `fund_ownership.parquet`: per (security_id, calendar quarter `period_q`): over the fund series whose latest report
  with report date in the quarter was filed by its deadline (report date + 60 days, next SEC business day; the latest
  filing per (series, report date) wins, amendments included): `fund_shares` (common equity, unit NS, long),
  `fund_shares_short`, `fund_value_usd`, `n_fund_series`, `n_fund_registrants`, `top5_share`,
  `flow_induced_shares` (sum of held shares x the series' reported quarterly net flow / prior net assets, Lou 2012),
  quarter-on-quarter changes, `available_at` = the latest clock of the included filings;
- `fund_flows.parquet`: per (series, report date): net assets, reported net flow (sales + reinvestments -
  redemptions over the 3 months), and `flow_proxy` = NA_t - NA_(t-1) x (1 + ret_q) with its share of NA_(t-1), when
  the prior report is 75-105 days earlier.

**Clock.** `nport-acceptance-v1`: the accession's EDGAR acceptance from `sec_filings`, else filing date + 1 day 00:00
America/New_York. **Staleness:** 180 days after the calendar quarter end.

## S5.3 Schedules 13D / 13G (`stakes/`)

**Sources.** The primary document of each 13D / 13G filing (EDGAR XML `primary_doc.xml` since 2024-12-18; the HTML /
text document before), fetched once into `data/raw/sec_13dg/`; accession list and clocks from `sec_filings`.

**Outputs.** `filings.parquet` (one row per landed accession: schedule, amendment, issuer CIK / CUSIP / name, event
date, 13G rule, stake = max over reporting persons of shares and percent, Item 4 purpose text and `activism_flag`
(13D), `security_id` via the 13F PIT CUSIP map, `parse_basis`, `parse_ok`, `available_at`), `persons.parquet` (one
row per reporting person), `notices.parquet` (every 13D / 13G accession 2019+ in `sec_filings`, landed or not, with
subject / filer CIK split and `doc_status`).

**Text 13D rule `13d-text-v1`** (`stakes.parse_text_13d`): issuer = the line(s) before "(Name of Issuer)", CUSIP =
the token before "(CUSIP Number)", event date = the date before "(Date of Event ...)"; per reporting-person cover
block, row 11 (aggregate amount) and row 13 (percent of class); Item 4 text up to Item 5. `activism_flag` = any
activism term in Item 4 after removing the standard "may ... actions described in (a) through (j)" reservation.
13G documents before 2024-12 are not parsed (metadata only, via `notices.parquet`).

**Clock.** EDGAR acceptance from `sec_filings` (earliest over the CIK rows of the accession).

## S5.4 Form 144 and insider measures (`insider_ext/`)

- `form144_notices.parquet`: one row per 144 / 144/A accession 2019+ (every notice in `sec_filings`), parsed XML
  fields where landed (EDGAR structured Form 144 since 2023-04): seller, relationship, units and value to be sold,
  units outstanding, approximate sale date, broker, exchange, nature of acquisition, 10b5-1 adoption date;
- `insider_trades.parquet`: open-market P / S trades from the `insider` stage (non-derivative, original filings,
  shares > 0) from 2019 with the rule `cmp-routine-v1` label (Cohen, Malloy and Pomorski 2012): routine if the insider
  traded in the same calendar month in each of the three prior years, opportunistic if he traded in each of the three
  years otherwise, unclassified with less history; only trades public by the end of their year count as history;
- `net_buying_monthly.parquet`: per (issuer CIK, month of `available_at`): buys / sells, shares, value, net shares and
  value, distinct buyers / sellers, the same for opportunistic, routine and officer + director trades, share of
  10b5-1 trades, `available_at` = latest trade clock of the month;
- `form144_monthly.parquet`: per (issuer CIK, month): 144 notices, distinct filers, units and value proposed.

## S5.5 Reg SHO threshold-list completeness (check of `regsho_threshold/`, read-only)

See the measured table below.

## Measured (at the 2026-09-30 owner stop)

- **S5.1.** Inputs are landed: 82 ADV rosters 2018-10 .. 2026-08, 7 bulk tables, 31 13F cover sets and
  `filer_values.parquet` (213,248 filer-quarters). `filer_type build` is not run yet.
- **S5.2.** 16 of 27 quarters parsed (2019q4-2023q3): 9.1-13.2 k filings and 1.4-1.9 M equity holdings per
  quarter. On the 2020q2 check, the acceptance clock covers ~99.9% of filings; CUSIP is present on 66% of equity
  rows and ISIN on 98%, hence the ISIN -> CUSIP step. The build is not run yet.
- **S5.3.**
  - XML 13D (2024-12 .. 2026-09): 9,734 of 9,743 documents parsed; 2025 SCHEDULE 13D/A parse_ok 99.87%.
  - XML 13G: 1,456 of 24,740 landed.
  - Pre-2024 text 13D: a 100-filing hand check has stake fields correct in 97/100 (shares 100, percent 98, CUSIP
    99). Reporting-person names are correct in 65/100 and the activism flag in 72/98. The known fixes are listed in
    the sprint report `task-OWN-report.md`.
- **S5.4.**
  - `insider_trades` has 873,513 P/S trades 2019+ with CMP labels; `net_buying_monthly` has 136,164 issuer-months.
  - Value sums still need a price sanity rule, because the monthly net value is dominated by misreported prices.
  - Form 144 documents are not landed yet.
- **S5.5 Reg SHO completeness 2018-01-02 .. 2026-09-25.**
  - Nasdaq, NYSE, NYSE American, NYSE Arca, Cboe BZX (and FINRA OTC) have a list for every settlement session.
  - The only absent sessions are Columbus Day and Veterans Day each year: bank holidays with no settlement, so no
    list is published.
  - Empty lists (NYSE 2-77 a year, NYSE American 5-129 a year) never break symbol continuity.
  - On-list rows map to `security_id` at 95.5-100% (NYSE 2018: 91.6%). FINRA OTC maps at 0-4%, by design: OTC names
    are outside the vendor universe.
  - Every non-Nasdaq list carries `vintage_risk` (derived clock).
