# Alpha panel: identity, security master, corporate actions, delisting (request U1-U5, D9, D10)

Answers sections 1, 3.2 D9-D10 and 5.4 of `C:\atx-wt\pool-2\docs\plans\2026-09-28-mega-alpha-data-request-atx-db.md`.
Code: `src/atx_db/alpha_panel/{identity_table,security_master,corporate_actions,delisting}.py`. Build root
`data/alpha_panel/v1`. Every stage publishes a `manifest.json` last. The manifest holds the per-file SHA-256 and the
SHA-256 of the producing code (raw and LF-normalised), plus git HEAD.

## U1: dated CIK link table (`identity/link_table.parquet`)

The table has one row per maximal run of sessions on which a line keeps the same key. The key is (cik, link_tier,
basis, is_issuer_primary, share_class). There are 172,926 rows, 11,435 lines and 8,644 CIKs, covering 2018-01-02 to
2026-09-18.

| column | meaning |
|---|---|
| `security_id` | TickerHistory3 securityID, with the sid0-bracket-v1 repair |
| `cik` | SEC CIK (int64) |
| `valid_from`, `valid_to` | first and last session of the run (inclusive; the line traded on both) |
| `link_tier` | `strict` (r4-links-asof-v1), `name` (finra-name-match-v1) or `backfill` (snapshot-run-backfill-v1) |
| `basis` | the tier's evidence basis (`reconstructed_high`, `reconstructed_medium`, `finra_name_match`, `snapshot_run_backfill`, …) |
| `is_issuer_primary` | the issuer's linked line with the highest prior-63-session dollar volume that session (ties: smallest id); point in time |
| `share_class` | vendor ticker suffix (`BRK.B` → B), else the `Class X` word of the latest FINRA issue name disseminated before the session |
| `available_at` | when the link could first be known (see below) |
| `evidence_at` | clock of the dated evidence the link rests on |
| `ever_member` | the line was a scorecard member (top-3,000) on some session 2018+ |

**Clocks by tier.**
- `strict`: the r4 link's own clock. 13 of the r4 runs are known after their start; consumers gate on `available_at`.
- `name`: the dissemination (22:00 UTC) of the FINRA short-interest row whose issue name matched on the run's first
  session. Each later session is known by its own earlier dissemination.
- `backfill`: `available_at` is the 2026-09-20 snapshot. This tier is not point in time: the choice of CIK comes from
  the snapshot, and only lines alive at the snapshot are covered. `evidence_at` is `valid_from` 22:00 UTC: by then the
  CIK had filed a periodic report and the vendor line had traded.

**The panel's link rule.** A link is used at session d once it is known: `available_at` ≤ d 22:00 UTC for the strict
and name tiers, `evidence_at` for backfill. The panel carries `link_tier` on every row, so issuer fields can be
restricted to `strict` or to the point-in-time tiers (`strict`, `name`).

### Coverage of operating-company members (`member_equity` cells, per year)

This table uses the v1 operating basis: a vendor earnings-reaction day within 400 days. The owner ruled on
2026-09-28 that the vendor earnFlag is unreliable, and the v2 panel defines `member_equity` from SEC periodic filings
and security type instead. The v2 table is section 4 of `docs/ALPHA_PANEL_METRICS.md`, written by the metrics stage
after the v2 panel rebuild. On the v2 basis, 96.6% of member_equity cells are linked (any tier) in the 2021-06 smoke
month.

| year | cells | strict | strict + name | any tier | dead-line cells linked |
|---|---:|---:|---:|---:|---:|
| 2018 | 450,095 | 77.3% | 82.6% | 95.6% | 92.1% |
| 2019 | 574,893 | 77.2% | 82.0% | 96.6% | 92.3% |
| 2020 | 575,906 | 77.9% | 82.0% | 96.8% | 92.0% |
| 2021 | 602,988 | 75.3% | 79.1% | 96.7% | 90.8% |
| 2022 | 580,024 | 76.5% | 79.3% | 97.3% | 91.1% |
| 2023 | 571,754 | 77.8% | 79.8% | 98.0% | 91.0% |
| 2024 | 568,722 | 77.5% | 79.1% | 98.5% | 92.5% |
| 2025 | 548,225 | 77.2% | 78.4% | 98.8% | 93.4% |
| 2026 | 385,629 | 76.0% | 76.7% | 99.1% | 97.3% |

The **U1 target (≥ 93% linked per year) is met for every year** when all tiers are counted. With the point-in-time tiers
only (strict + name), coverage is 79-83%. The gap to 93% is the backfill tier: lines alive at the snapshot with no dated
evidence.

**U2 (dead lines).** A line is dead when its last vendor session is before 2026-09-01. Dead lines' member_equity cells
are linked at 91-93% per year in 2018-2025. Every such link is strict or name: backfill cannot reach a dead line, so for
dead lines the "any tier" and point-in-time coverage are the same. The lines linked (not cells) are 90-94% per year
(`identity/audit.json`).

**Other identity checks.**
- Ambiguous line-days: 0. The combined identity assigns one CIK per line-day, with strict winning, then backfill, then
  name.
- 1,320 issuers had two or more linked lines for at least 21 sessions: `identity/multi_class_issuers.parquet`. Examples
  are GOOGL/GOOG, BRK.A/BRK.B, and banks and REITs with preferred lines.
- `share_class` is known for 858 of the 11,435 lines. The rest are single-class or unlabelled.

### Consumer bridge exports (`export/identity-bridge-v2-{strict,pit,all}/`)

These use the consumer's `atx.identity-bridge/v1` contract (`links.parquet`: `sr_id, cik, start, end_incl, available_at,
primary (P|J), tier, basis, class_status`, plus `link_tier, share_class, knowledge_at`). `primary` = P on the issuer's
most liquid line, and intervals are split where that changes.

| variant | tiers | rows | lines | CIKs | available_at |
|---|---|---:|---:|---:|---|
| `strict` | strict | 16,538 | 5,915 | 5,834 | link clock (PIT) |
| `pit` | strict + name | 146,258 | 9,677 | 7,242 | link clock (PIT) |
| `all` | strict + name + backfill | 172,926 | 11,435 | 8,644 | `evidence_at` for backfill rows; `knowledge_at` keeps the true 2026 clock (survivorship sensitivity only) |

## U3: security classification (`security_master/`, panel flags)

The vendor file has no security names. Two name sources are used:

1. **FINRA short-interest rows** (`finra_names.parquet`): 2.12M dated rows, twice monthly. Each row has a 30-character
   `issueName` and the listing market (`marketClassCode` → `exchange` as XNAS / XNYS / XASE / ARCX / BATS / OTC).
   - Point in time: a row is known at its dissemination, 22:00 UTC.
   - Names are classified with the A2 patterns of `universe_us_listed.classify_security_type`.
   - FINRA truncates names at 30 characters. A name cut to a trailing one-letter token is resolved by the Nasdaq
     fifth-letter symbol convention (W warrant, U unit, R right).
2. **Nasdaq Trader directory snapshot** (2026-09-18): full names and the ETF flag. It covers current lines only
   (12,525 of 12,927 lines trading at the snapshot). It is **not point in time**, and those fields are flagged
   non-PIT in every export.

**Per-session panel flags.** Each flag is computed from the latest FINRA row within 45 days, SIC in force (via the
CIK link), filer regime (SEC forms) and vendor data:
- **Security type:** `security_type`, `security_type_basis`.
- **Instrument flags:** `is_common`, `is_adr`, `is_adr_likely`, `is_etf` (ETF, fund, ETN), `is_index_line`,
  `is_preferred`, `is_unit_warrant_right`, `is_lp`, `is_note`.
- **Issuer-type flags:** `is_reit` (SIC 6798 or name), `is_royalty_trust` (SIC 6792 or name), `is_spac` (a
  blank-check name, or SIC 6770 with no revenue), `is_fpi` (20-F or 40-F filer).
- **Operating company:** `is_operating` = not an index line, security type common / common_unverified / ADR / REIT /
  LP, not an ETF or SPAC, and a linked issuer's latest periodic report (10-K, 10-Q, 20-F, 40-F) within 400 days
  (an unlinked line passes on the security type alone). `member_equity` = `member` and `is_operating`. The vendor
  earnFlag drives nothing: its columns are kept as `earn_recent_vendor`, `earn_flag_vendor` and
  `is_operating_vendor` for audit only.
- **Class and identity:** `share_class`, `share_class_group_id` (= cik), `is_issuer_primary`.
- **Listing:** `exchange`, `listing_date` (NULL when left-censored at the vendor start 2012-03-26), `delisting_date`.

Coverage: 99.9% of member cells have a security type in the 2021-03 smoke month. The full per-year table is in
`docs/ALPHA_PANEL_METRICS.md`.

**Limitations.**
- **ADR detection by name is weak.** FINRA names are cut before "American Depositary", and the directory names only
  current lines. `is_adr_likely` adds 20-F filers without ordinary-share name evidence. For example, TSM is
  `common_unverified` by name but `is_fpi`.
- **Canadian 40-F filers list ordinary shares.** They are `is_fpi` and are not flagged as ADRs.

## U4: delisting returns (`delisting/events.parquet`)

The rule is the consumer's T33a rule R (`delisting-feasibility.md`) with the link table in place of the r4 bridge. See
the module docstring. The steps:

1. **Continuation.** A line is continued, not delisted, when:
   - its ticker is reused on another securityID within 30 days, or
   - a same-CIK successor line trades on.
   The exception is an 8-K Item 1.03 in the window, which always counts as a delisting.
2. **Cause**, first match wins:
   - Item 1.03 → performance;
   - Item 2.01 or 5.01, or a target-side merger form → M&A;
   - Item 3.01 or an issuer Form 25 → performance;
   - a non-common line by FINRA name type (ETF, fund, SPAC, preferred, unit, warrant) → `non_common`;
   - otherwise `unknown`, or `unknown_unlinked` when the line has no CIK.

   The vendor earnFlag is not used.
3. **Delisting return (imputed).** There is no post-delisting price source in atx-db, so returns are imputed:
   - M&A and non-common: 0;
   - performance: -0.30 on NYSE / NYSE American / Arca listings (Shumway 1997), -0.55 on Nasdaq listings
     (Shumway-Warther 1999);
   - unknown: NULL, with `dlret_if_performance` given alongside.

The stage has 9,727 rows, and counts per termination year are in its manifest. The `ever_member` column marks lines
that were scorecard members.

For member terminations in TRAIN (2020-2022):

| cause | terminations |
|---|---:|
| M&A | 358 |
| non-common | 69 |
| performance | 51 |
| unknown (no CIK) | 66 |
| unknown (linked) | 13 |

The first three rows are classified: 478 of 557, or 85.8%. The consumer's feasibility gate was 80%, and its r4
bridge result was 59.9%.

## U5: sector and industry

- **SIC:** the dated SIC in force per filing (`fundamentals/sic_events.parquet`, 550-day staleness), with FF12, FF49
  and SIC2 groups on the panel. SIC changes are the event rows themselves; each carries its own clock.
- **GICS: not available.** The vendor file's `GICS` column is empty for every row (checked: 0 of 10,361 lines on
  2024-06-03), and a license would be needed.
- **NAICS: not available.** SEC filings carry SIC only.

## D9: daily bar statistics (panel)

- **Available** (vendor file): `open, high, low, close` (total-return adjusted), `raw_close, volume, dollar_volume`
  (raw close × volume).
- **New:**
  - `ret_intraday` = raw_close / open − 1;
  - `ret_overnight` = (1 + ret) / (1 + ret_intraday) − 1. It inherits the day's split and dividend adjustment from
    `ret`, and is NULL on guarded returns.
- **Not available:** VWAP and trade counts; the vendor file has neither.

## D10: corporate actions (`corporate_actions/vendor_events.parquet`)

262,560 vendor factor events for 2018-2026, one per line and ex-date where `|returnFactor − 1| > 1e-6`.

**Event kinds:**
- `split` / `reverse_split`: 1/f or f within 1% of p/q with q ≤ 4, with `split_ratio`;
- `cash_distribution` (0.8 ≤ f < 1), with `distribution_yield` and `implied_cash`;
- `large_distribution` (spin-off or special dividend);
- `vendor_factor_break`: the 2021-01-04 vendor artefact, excluded as an action.

**Clocks:**
- `available_at` = ex-date 22:00 UTC. The announcement date is unknown, so this is conservative.
- `vintage_risk` = true, because the factors come from one vendor snapshot.

**Examples by year:**
- Cash distributions: 24-38k per year.
- Splits: 29-97 per year.
- Reverse splits: 98-580 per year.

Mergers, material agreements, changes in control and bankruptcies are SEC 8-K events (`sec_filings/`,
docs/ALPHA_PANEL_SEC.md). Buyback authorisations come from XBRL facts (fundamentals lane). **Index additions and
deletions have no source.**

## D8: options surface

The vendor file carries ATM implied volatility at 5-504 day tenors only; the panel has 5, 10, 21, 42, 63, 126 and 252
days, so the term slope can be computed. **25-delta put/call IV (skew), option volume, open interest by call/put and
implied dividend are not in any atx-db source.** They need an options vendor feed.

## v3 security master and symbology (tier1-v3 S2.2-S2.6)

Code: `src/atx_db/alpha_panel/{listing_events,cusip_history,figi_lei,identity_v3}.py`. New files only; the v1
files above are untouched until the controller swaps v3 in. Each output has its own manifest
(`security_master/{listing_events,cusip_history,figi,lei,share_exchange_history}_manifest.json`,
`identity/link_table_v3_manifest.json`) with code SHA-256, output SHA-256 and the SHA-256 of every input manifest.
Measured numbers: `.superpowers/sdd/tier1-v3/task-ID-report.md`.

### S2.2 listing events, listing dates, name history

- `security_master/listing_events.parquet`: one row per (cik, accession) of a listing-related form, all years.
  2009+ rows come from the `sec_filings` stage (resolved acceptance clocks); earlier rows from a streaming pass
  over `data/cache/submissions.zip` (clock: filing date + 1 day 00:00 America/New_York). Classes:
  `registration` (8-A12B/G, 10-12B/G, 20-FR12B/G, 40FR12B/G), `successor` (8-K12B, 8-K12G3, 8-K15D5),
  `certification` (the exchange's CERT* approval), `offering` (S-1, S-11, F-1, SB-2, 424B4/424B1, EFFECT),
  `adr` (F-6 family), `delisting` (25, 25-NSE), `deregistration` (15-12B/G, 15-15D, 15F-*).
- `security_master/line_listing.parquet`, rule `line-listing-v1` (one row per vendor line; see the module
  docstring): `sec_confirmed` (a listing event of the line's first-linked CIK within [first session - 180 d,
  + 10 d], or an S-1/F-1 within 400 d before), `successor_of_line` (a same-CIK or same-ticker line ended within 10
  days before: a vendor re-key; the date is inherited), `sec_registration_before_vendor` / `censored` (the first
  session is a vendor coverage batch: 2012-03-26, 2016-01-04, 2016-08-09), else `vendor_first_session`.
  `listing_type` (ipo / spin_off / successor / adr / exchange_registration), `delisting_date` with the matching
  Form 25 / 15 when one exists. `listing_available_at` is never before the first session's 22:00 UTC.
- `security_master/name_history.parquet`: SEC `formerNames` ranges plus the current name per CIK
  (`available_at` = 22:00 UTC of the day the name took effect on EDGAR; dates from the 2026-09-19 archive,
  `vintage_risk = 'snapshot_dates'`).
- `security_master/filer_addresses.parquet`: snapshot business / mailing address, EIN, LEI field and state of
  incorporation per filer (non-PIT), used by the LEI match.

### S2.3 CUSIP history and ISINs

`security_master/cusip_history.parquet`, rule `cusip-history-v1`: dated (cusip, security_id) runs with
`obs_from/obs_to` (evidence) and `valid_from/valid_to` (extended to the neighbouring runs of the line, at most 730
days beyond the evidence). Evidence `basis`: `ftd_symbol` (SEC fails-to-deliver rows mapped by the ftd stage,
point in time), `nport_ticker` (fund-reported tickers, when the nport stage is published), `openfigi_ticker` /
`openfigi_isin_fragment` (OpenFIGI snapshot: the CUSIP's US composite ticker on the last vendor session, clipped
to the line's trading span; `vintage_risk = 'snapshot_non_pit'`) and `thirteenf_name_alias` (rule
`thirteenf-name-alias-v1`: a 13F CUSIP field that fails the CUSIP check digit, e.g. an ISIN fragment such as
Chubb's `004432874` = CH0044328745, takes the line of the unique mapped CUSIP with the same issuer-name stem that
quarter; a valid CUSIP is never aliased). `isin` = `US`/`CA` + CUSIP + Luhn check digit (`CA` when the linked
issuer is incorporated in a Canadian province); CINS numbers get none. `cusip_check_ok`, `issue_kind`
(equity / debt / unverified).

### S2.4 FIGI and LEI

- `security_master/figi.parquet` (query grain) and `security_master/line_figi.parquet` (per line): OpenFIGI
  mapping API, unauthenticated (25 requests / minute, 10 jobs / request), landed as served under
  `data/raw/openfigi/` with `receipts.jsonl`. Passes: `cusip` (ID_CUSIP, exchCode US), `cusip_any` (no exchange
  filter, for delisted CUSIPs: share-class FIGI), `isin_fragment`, `ticker` (current ticker of alive member lines
  without a CUSIP).
- `security_master/lei.parquet`: CIK -> LEI by `sec_submissions` (the EDGAR profile field; 45 filers carry it),
  `isin_lei_map` (GLEIF ISIN-LEI relationship file on the derived ISINs), `nport_issuer_lei` (when published),
  `name_address` (exact normalised legal name, current or former SEC names, corroborated by ZIP, city + region,
  jurisdiction of incorporation or foreign city + country; unique both ways). GLEIF golden copy (LEI2, RR) and
  ISIN-LEI files are CC0 open data, landed under `data/raw/gleif/` (zips deleted after parse; parsed Parquet and
  receipts kept).

### S2.5 link table v3

`identity/link_table_v3.parquet`, rule `identity-link-table-v3`: tiers `strict` > `dated` > `name` > `backfill`
per line-session (one CIK per line-day). The new `dated` tier (rule `filing-symbol-evidence-v1`, point in time):
Form 3/4/5 `issuerTradingSymbol` (weight 1 per accession) and cover-page `dei:TradingSymbol` rows (weight 2) mapped
to the vendor line as of the filing date; a session links to the CIK with the most weight in the last 120 days
(then 400 days) when its 400-day weight is at least 2. The `name` tier is recomputed on every session without a
strict link (v1 dropped backfill days). New columns: `linktype` (CCM style: `LC` strict, or dated with cover-page
or listing-event corroboration or weight >= 4; `LU` otherwise), `linkprim` (`P` issuer primary, `J` other common /
ADR class, `N` non-common line), `dated_sources`, `listing_event_match`; `share_class` = the vendor ticker's
class suffix, else the class in the FINRA issue name (`security_master/finra_names.parquet`). `dated()` is
resumable per year (`_tmp/identity_v3/dated_parts/`, keyed by the evidence SHA-256, code and panel-core files).
Exports
`export/identity-bridge-v3-{strict,pit,all}/` keep the v2 bridge contract; `pit` = strict + dated + name.
`security_master/share_exchange_history.parquet`: CRSP-style `exchcd` / `shrcd` runs from the v2 panel's
point-in-time security flags.

### S2.6 corporate hierarchy

`security_master/hierarchy.parquet`: GLEIF Level 2 accounting-consolidation parents of every matched LEI
(`parent_level` direct / ultimate), with the parent's name, country, status, the parent's CIK when that LEI is
itself matched, relationship status, periods and validation sources.
