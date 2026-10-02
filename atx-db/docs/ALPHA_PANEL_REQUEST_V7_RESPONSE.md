# atx-db response to the mega-alpha v6.1 → v7 data request

**Request:** `C:\atx-wt\pool-2\docs\plans\2026-09-28-mega-alpha-data-request-atx-db.md` (2026-09-28).
**Build root:** `C:\atx\atx-db\data\alpha_panel\v1`. **Code:** `atx-db/src/atx_db/alpha_panel/`. Nothing is committed yet.
**State on 2026-09-29, 00:45 UTC:** every source stage below is built, and each carries a SHA-bound manifest. The last
steps have not run: the v2 panel rebuild for 2018-2026, the borrow proxy, the section 5 metrics, the aligned export
on your lo1 role, and your loader's acceptance run. The commands are in the last section.

Detail docs:
- [`ALPHA_PANEL_IDENTITY_SECURITY.md`](ALPHA_PANEL_IDENTITY_SECURITY.md): U1-U5, D8-D10;
- [`ALPHA_PANEL_FUNDAMENTALS.md`](ALPHA_PANEL_FUNDAMENTALS.md): section 2, D6, D7;
- [`ALPHA_PANEL_SEC.md`](ALPHA_PANEL_SEC.md): D2, D11, D12;
- [`ALPHA_PANEL_SHORTFLOW.md`](ALPHA_PANEL_SHORTFLOW.md): D1, D3, D4;
- [`ALPHA_PANEL_METRICS.md`](ALPHA_PANEL_METRICS.md): section 5, written by the metrics stage.

## Summary

| id | status | where |
|---|---|---|
| U1 CIK link table | **met**: 95.6-99.1% of member_equity cells linked per year, all tiers | `identity/link_table.parquet`; bridges `export/identity-bridge-v2-{strict,pit,all}` |
| U2 dead-line links | **met**: 91-93% of dead-line cells linked, all by point-in-time tiers | same; `identity/audit.json` |
| U3 security classification | **delivered**, with weak ADR detection | `security_master/`; panel flags |
| U4 delisting returns | **delivered, imputed**: 85.8% of TRAIN member terminations classified | `delisting/events.parquet` |
| U5 sector / industry | SIC, SIC2, FF12, FF49 dated; **GICS and NAICS not available** | `fundamentals/sic_events.parquet`; panel `grp_*` |
| §2 fundamental coverage | **improved; most targets not met** (table below) | `fundamentals/`; `export/fundamental-events-v1` |
| D1 13F | **delivered**: holdings 2013q2-2026q2 plus aggregates; filer type not classified | `thirteenf/` |
| D2 earnings calendar | **delivered**: 8-K 2.02 dates, time of day, reaction session, expected next date | `earnings_calendar/announcements.parquet` |
| D3 short volume, FTD, Reg SHO | **delivered**; NYSE threshold lists still landing | `short_volume_ext/`, `ftd/`, `regsho_threshold/` |
| D4 borrow | **proxy only**: no lending data exists in atx-db; stage written, not built | `borrow_proxy/` |
| D5 consensus | **not available**: needs a license | none |
| D6 statement items | **delivered** (all 15 asked) | fundamentals events and panel |
| D7 history 2010+ | **delivered**: fundamentals from 2010, prices from 2012-03-26 | `fundamentals/quarterly_history.parquet`, `prices_history/` |
| D8 options surface | ATM IV 5-252 d only; **no skew, volume or OI source** | panel `iv_atm_*` |
| D9 daily bars | OHLC, volume, dollar volume, overnight / intraday split; **no VWAP or trade count** | panel |
| D10 corporate actions | splits and distributions (vendor factors), M&A (8-K), buyback authorisation amounts (XBRL); **no buyback-announcement or index add/drop source** | `corporate_actions/`, `sec_filings/` |
| D11 Form 4 | **delivered**: 4.90M transactions, 2015q1-2026q2 | `insider/` |
| D12 8-K metadata | **delivered**: item codes, events, delisting causes | `sec_filings/` |
| §5 metrics | code ready; **runs after the panel rebuild** | `metrics/`, `docs/ALPHA_PANEL_METRICS.md` |

## 0. Ground rules

1. **Point in time.** Every source row carries `available_at` in UTC, and every panel join uses
   `available_at < 22:00 UTC of session d-1`.
   - Vintages are never overwritten. Amendments, restatements and 13F amendments are kept as separate rows with their
     own clocks.
   - `vintage_risk` marks sources that were re-posted without revision history: FINRA SI before 2021-06, FTD, Reg
     SHO, CNMS short volume, and vendor factors.
2. **Identity.**
   - Instruments are keyed on TickerHistory3 `securityID`, with the sid0-bracket repair shared by every stage.
   - Issuer items are keyed on CIK.
   - `link_tier` (strict / name / backfill) is on every panel row and in every bridge export.
3. **Coverage** is reported over `member_equity` per year, by the metrics stage (§5).
4. **Staleness** is declared in each stage manifest and in the panel rules:
   - fundamentals: 200 or 400 days (consumer rule);
   - SIC: 550 d; SI: 45 d; FTD: 60 d; Reg SHO: 10 d; 13F: 150 d after the quarter;
   - earnings: 200 d; Form 4: 365 d.
5. **Windows.** Every stage covers 2018-01-01 to the latest available date. Nothing was screened against returns.
   `evaluate.py` (the IC screen) was not run. The metrics stage withholds every distribution of a return field from
   2023 on.
6. **Format.** Parquet, one directory per stage. Each `manifest.json` is written last, with the SHA-256 of every file
   and of the code (raw and LF-normalised) plus git HEAD. The panel manifest also binds the SHA-256 of every input
   stage manifest.

**`member_equity` changed.** The v1 definition used the vendor earnFlag, which the owner ruled unreliable on
2026-09-28, so the vendor flag now drives nothing. v2 `member_equity` is:
- a member;
- not an index line, ETF or SPAC;
- security type common, ADR, REIT or LP;
- when linked, the issuer's latest periodic report (10-K, 10-Q, 20-F or 40-F) within 400 days.

`earn_recent` now comes from 8-K Item 2.02 reaction sessions. The vendor columns stay as `*_vendor`, for audit only.

## 1. Universe and identity

- **U1.** `identity/link_table.parquet` has 172,926 dated runs over 11,435 lines and 8,644 CIKs. The columns are
  `security_id, cik, valid_from, valid_to, link_tier, basis, is_issuer_primary, share_class, available_at,
  evidence_at, ever_member`.
  - Coverage of member_equity cells on the v1 basis: 95.6-99.1% any tier, 77-83% strict + name, 75-78% strict.
  - On the v2 basis, 96.6% of cells are linked (any tier) in the 2021-06 smoke month.
  - 0 ambiguous line-days. 1,320 multi-line issuers are listed in `identity/multi_class_issuers.parquet`.
- **Bridge exports.** Three variants follow your `atx.identity-bridge/v1` contract:
  - `strict`: 16.5k rows;
  - `pit`: strict + name, 146k rows;
  - `all`: 173k rows. The backfill tier here uses `evidence_at`, and `knowledge_at` keeps the true 2026 clock, so a
    survivorship sensitivity is possible.

  Your loader accepted `identity-bridge-v2-pit` with `--fields si_shares,shares_out,me_company` on the lo1 role:
  status `fields-complete`, output in `export/acceptance/bridge-pit-me`.
- **U2.** Dead-line cells are 91-93% linked per year. Every such link is strict or name, because backfill cannot
  reach a line that died before the snapshot.
- **U3.** Flags on every panel row:
  - security type: `security_type`, `is_common`;
  - instrument: `is_adr`, `is_adr_likely`, `is_etf`, `is_index_line`, `is_preferred`, `is_unit_warrant_right`,
    `is_lp`, `is_note`;
  - issuer type: `is_reit`, `is_royalty_trust`, `is_spac`, `is_fpi`;
  - class: `share_class`, `share_class_group_id`, `is_issuer_primary`;
  - listing: `exchange`, `listing_date`, `delisting_date`.

  Names come from FINRA rows, point in time. A caveat: FINRA truncates names at 30 characters, so ADRs are found
  mostly through `is_fpi` / `is_adr_likely`.
- **U4.** `delisting/events.parquet` has 9,727 rows. The rule is your T33a rule R. Cause comes from 8-K items,
  merger forms and Form 25. Returns are imputed Shumway-style:
  - M&A and non-common: 0;
  - performance: -30% on NYSE, -55% on Nasdaq;
  - unknown: NULL, with `dlret_if_performance` alongside.

  Of TRAIN member terminations, 478 of 557 (85.8%) are classified. The gate was 80%; the r4 bridge reached 59.9%.
  There is no post-delisting price source.
- **U5.** SIC as of each filing, with its own clock, plus SIC2, FF12 and FF49 on the panel. GICS is not available:
  the vendor column is empty, and GICS needs a license. NAICS is not available either, because SEC filings carry
  SIC only.

## 2. Fundamental coverage (the capping fields)

These numbers are from the 2021-06 smoke month of the v2 panel, finite share of cells. The rebuilt panel's per-year
tables will be in `ALPHA_PANEL_METRICS.md`. Bases:
- *linked*: member_equity cells with a CIK;
- *linked USD*: linked cells whose visible filing reports in USD. Money items of other reporting currencies are NaN
  by rule, because atx-db has no point-in-time FX source; that is 5.5% of member_equity cells.

| field | yours (App. A) | member_equity | linked | linked USD | target | verdict |
|---|---:|---:|---:|---:|---:|---|
| `gp_ttm` | 0.606 | 0.704 | 0.729 | 0.753 | 0.90 | short; bank and insurer cells are structurally NaN (0.770 excluding them) |
| `oi_ttm` | 0.767 | 0.815 | 0.844 | 0.875 | 0.92 | short |
| `xrd_ttm` | 0.375 | 0.860 | 0.890 | 0.895 | 0.95 | short; zero-filled with `xrd_reported_zero` |
| `fscore` (all 9) | 0.535 | 0.452 | 0.468 | 0.461 | — | the 9 terms ship individually |
| `fscore_partial` (≥ 6 terms) | — | 0.894 | 0.925 | 0.922 | 0.85 | **met** |
| `capx_ttm` | 0.855 | 0.856 | 0.886 | 0.914 | 0.95 | short |
| `shrs_q` | 0.882 | 0.959 | 0.992 | 0.992 | 0.97 | **met** on linked cells |
| `txt_q` | 0.894 | 0.835 | 0.864 | 0.915 | 0.95 | short |
| `sale_ttm` | 0.907 | 0.875 | 0.906 | 0.939 | 0.96 | short |
| `be` | 0.975 | 0.921 | 0.953 | 0.989 | — | |
| `cfo_ttm` | 0.964 | 0.894 | 0.926 | 0.960 | — | |
| `sue` | 0.940 | 0.814 | 0.843 | 0.891 | — | lower: the wider universe has more issuers with fewer than 4 seasonal differences |

The v2 `member_equity` is wider than your Appendix A basis: about 97% of cells are linked, against your ~60%. It also
includes foreign private issuers. So the member_equity column is not directly comparable to yours; *linked USD* is
the closest to your strict-linked basis.

What was done, per the request:
- `gp_ttm`: sale − cogs when GrossProfit is absent, then sale − operating cost excluding SG&A, R&D and D&A. The
  source is recorded in `gp_src`.
- IFRS `ifrs-full` concepts are mapped for every item, including CostOfSales, Revenue and IncomeTaxExpense.
- `oi_ttm`: pretax + interest fallback, recorded in `oi_src`.
- `xrd_ttm`: zero-filled when the filing's own income statement (FSDS PRE) has no R&D line.
- `capx_ttm`: the capex chain plus a zero-fill when the cash-flow statement has no capex line.
- `shrs_q`: the dei cover count first, then a class-of-stock sum for multi-class issuers (GOOGL, META, BRK).
- `txt_q`: IFRS tax concepts.
- Industry templates: bank, insurer, REIT, utility (`fin_template`), with structural-NaN rules in the manifest.

Why the targets are still missed, measured:
1. **Non-USD reporters: 5.5% of member_equity cells.** The fix is a point-in-time FX source; FRED H.10 daily rates
   are a candidate. That is an owner decision, because it converts filings with market rates that were not in the
   filing.
2. **TTM chain breaks.** Among 2020-2022 USD events, 3.8% (sale), 6.6% (oi) and 4.5% (gp) have the quarter value but
   no TTM. Most are a Q4 that cannot be derived because the fiscal-year fact and the 9-month fact sit in different
   concepts (for example Visa FY2020 revenue, which blanks FY2021 Q1-Q3 TTM). The rest are issuers with under four
   quarters of history. Fixing this means cross-concept Q4 derivation, which is not done.
3. **No gross-profit concept.** For the `other` template, 16% of USD cells have no GrossProfit, no cost of revenue
   and no operating-cost split. Examples are V, MA, UNP, BKNG and GM, whose cost lines are custom tags. These are
   real reporting absences, not extraction misses.

**Validation of fundamentals v9.**
- Own-filing values match raw Company Facts: `at` 164,533 of 164,533, and likewise for `seq` and 10-K `ni_ttm`.
- A cutoff rebuild of 118 events reproduced every one (no leakage).
- `export/fundamental-events-v1` loads through your own `load_events`: 421,228 rows, 42,670 dropped by your
  2025 seal, no consumer item missing.

## 3. New raw data

- **D1 13F.** Every INFOTABLE row, 124.4M rows over 2013q2-2026q2 (filer CIK, CUSIP, shares, `value_usd`,
  put/call, voting). The data also includes:
  - the PIT CUSIP map;
  - per-(quarter, security) aggregates `inst_shares, n_holders, top10_share` with quarter-on-quarter changes, in an
    as-of-45-day version (PIT) and a final version;
  - panel columns `inst_*`, with 99.7% of member_equity cells covered in the smoke month.

  The clock is filing date + 46 h, because the data sets carry no acceptance time. Filer type is not classified: no
  free point-in-time label exists.
- **D2 earnings calendar.** 327,314 announcements from 8-K Item 2.02, with:
  - `announcement_utc`, taken from the resolved EDGAR acceptance;
  - `timing` (pre-market, intraday, post-market, closed day) and `reaction_session`;
  - `next_expected_date`, by the same-quarter-last-year rule, known in advance;
  - `fiscal_period_end`.

  Panel columns: `earn_last_*`, `earn_next_expected_date`, `earn_recent`, `earn_day_offset`. Foreign private issuers
  announce on 6-K and are not covered.
- **D3.**
  - Short volume with the exempt volume and per-facility flags. FINRA's file has no volume per venue.
  - FTD, twice monthly, 2013 onward; 96-98% of fail value maps to a line.
  - Reg SHO threshold lists from Nasdaq, Cboe BZX and OTC for 2018-2026. The NYSE family is still landing: 855 of
    about 2,180 dates were landed at 00:39 UTC.
- **D4 borrow.** atx-db has no borrow fee, utilization or lendable data. The `borrow_proxy` stage lines up SI,
  institutional ownership, SI / IO, FTD and threshold flags with their clocks. A real feed needs a license.
- **D5 consensus.** Not available; a license is needed.
- **D6 statement items.** All in events and on the panel: `cogs_q/ttm, xsga_q/ttm, xint_q/ttm, dp_q/ttm, invt,
  rect, ap, drev, ppegt, gdwl, intan, mib, pstk, dvt_q/ttm, ebitda_q/ttm`. Also `buyback_authorized` and
  `buyback_remaining`.
- **D7 history.**
  - Fundamentals events from 2010, from knowledge built since 2009.
  - `quarterly_history.parquet`: 11.7M item-quarter rows, each in its own currency and with its clock.
  - Prices for 2012-03-26 to 2017-12 in `prices_history/`; the vendor file starts 2012-03-26.
- **D8.** ATM IV at 5, 10, 21, 42, 63, 126 and 252 days. Skew, option volume, OI and implied dividend have no
  source.
- **D9.** `open, high, low, close, raw_close, volume, dollar_volume, ret_intraday, ret_overnight`. There is no VWAP
  or trade count.
- **D10.** 262,560 vendor factor events (split, reverse split, cash distribution, large distribution). M&A, material
  agreements and bankruptcies come from 8-K items. Index add/drop has no source.
- **D11.** Form 4 transactions (derivative and non-derivative) and owners, 2015q1-2026q2, each at its acceptance
  clock. Panel columns: `insider_last_*` (open-market P/S).
- **D12.** Every filing's metadata (18.65M rows), 8-K item codes, and 1.08M events. The event classes are:
  - earnings 2.02;
  - acquisition 2.01, change in control 5.01, material agreement 1.01;
  - bankruptcy 1.03;
  - delisting notice 3.01, and Forms 25, 25-NSE and 15;
  - non-reliance restatement 4.02 and auditor change 4.01;
  - late filings (NT 10-K, NT 10-Q, NT 20-F).

  Filer regime and delisting causes are included too. There is no going-concern flag, and no text sentiment.
  Buyback announcements sit in 8.01 / 7.01 text and are not parsed.

## 5-6. Metrics and out-of-sample

The metrics stage (`metrics.py`) writes the five §5 outputs:
- coverage per field per year, on bases `member_equity`, `linked`, `linked_strict`, `linked_pit`, `linked_usd` and
  `excl_structural`;
- distributions p0.1 / p1 / p50 / p99 / p99.9 with zero and negative shares;
- the vintage audit;
- the identity audit;
- the change log against the v1 panel schema.

It reads no return-conditioned statistic, and return fields get no distribution from 2023 on. 2023+ data is built
the same way as TRAIN and is not summarised against returns anywhere.

## 8. Acceptance checklist

| criterion | state |
|---|---|
| (i) manifest with file and code SHA | every stage; the panel manifest also binds input manifests (from the rebuild) |
| (ii) `available_at` on every row | every source stage; the panel applies the gate per source |
| (iii) member_equity coverage per year, shortfall explained | metrics stage, pending the panel rebuild; section 2 above explains the shortfall |
| (iv) staleness stated | stage manifests and panel rules; the aligned export declares it per field |
| (v) `link_tier` on issuer joins | panel and bridges |
| (vi) one atx-impl load on the lo1 role binding our manifest | identity bridge: passed. Fundamental events: passed the consumer `load_events` check. Full `prepare_research_fields.py` run with the new fields: **pending** |
| (vii) no return-conditioned statistic for 2023+ | holds |

## To finish (from `C:\atx\atx-db`)

```bash
export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
G=../.superpowers/sdd/tier1-parity/run_memory_guarded.py; PY=.venv/Scripts/python.exe
# 0. when the NYSE combined Reg SHO landing has finished (resumable: regsho download --market nyse_combined)
$PY $G --job-gb 0.8 --wait-minutes 60 -- $PY -m atx_db.alpha_panel.regsho build
# 1. v2 panel 2018-2026 (two ranges can run side by side), then the manifest pass
$PY $G --job-gb 0.8 --wait-minutes 60 -- $PY -m atx_db.alpha_panel.panel --stages assemble --years 2018-2021
$PY $G --job-gb 0.8 --wait-minutes 60 -- $PY -m atx_db.alpha_panel.panel --stages assemble --years 2022-2026
$PY $G --job-gb 0.3 --wait-minutes 60 -- $PY -m atx_db.alpha_panel.panel --stages "" --years 2018
# 2. borrow proxy, section 5 metrics, fields on the lo1 role's axes
$PY $G --job-gb 0.8 --wait-minutes 60 -- $PY -m atx_db.alpha_panel.borrow_proxy
$PY $G --job-gb 1.0 --wait-minutes 60 -- $PY -m atx_db.alpha_panel.metrics
$PY $G --job-gb 1.0 --wait-minutes 60 -- $PY -m atx_db.alpha_panel.export_impl align \
    --role C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-lo1 --out data/alpha_panel/v1/export/lo1-fields-v2
```

After step 2, run the consumer's `prepare_research_fields.py` on the lo1 role:
- with `--identity-bridge export/identity-bridge-v2-pit` and `--fund-events export/fundamental-events-v1`, each with
  its manifest SHA;
- with `--fields` set to the section 2 items plus `si_shares,shares_out,me_company`.

Record the output in `export/acceptance/`. The build orchestrator runs the same chain, with the panel in a single
process: `python -m atx_db.alpha_panel.build --only regsho,panel,borrow_proxy,metrics,export_lo1`.
