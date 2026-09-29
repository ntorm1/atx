# Alpha panel: production daily characteristics for US equities

`atx_db.alpha_panel` builds one point-in-time daily panel for every US-listed
line in the vendor price file, with every field the mega-alpha scorecard
(`docs/plans/2026-09-27-mega-alpha-scorecard.md`, library v5.1) consumes plus
additional characteristics. It exists so that the scorecard's book can be
trained and evaluated on the last six years (2020-09-28 onward) from one
atx-db-owned build instead of per-window research exports.

This document is the contract between the stages. Every stage writes under
the build root and publishes a `manifest.json` last.

## Build root and windows

| Name | Value |
| --- | --- |
| Build root | `atx-db/data/alpha_panel/v1` (override with `ATX_ALPHA_PANEL_ROOT`) |
| Coverage window | 2020-09-28 through the last vendor session (2026-09-18 in the 2026-09-20 snapshot) |
| Warm-up start | 2018-01-02 (252-session windows, 90+400-day share lag, four-quarter fundamental lags) |
| Session calendar | distinct vendor `tradingDate` values with at least 1,000 rows (`calendar.parquet`) |

## Keys and clocks

* `security_id` (BIGINT) is the SpiderRock/ORATS `securityID` of the vendor
  line (`TBLTICKERHISTORY-<id>` in the warehouse). Every stage uses it.
* `session_date` (DATE) is a calendar session.
* Clock: a value stored at `(session_date, security_id)` is known by the
  session's 22:00 UTC end-of-day mark. The book decides at 23:00 UTC and
  trades the next session. Nothing dated after the mark may influence the
  value or whether the cell is NULL.
* Issuer data (fundamentals, SIC) is visible at session `d` when its clock is
  strictly before the 22:00 UTC mark of session `d-1` (one-session lag, as
  fields-v6).
* FINRA short interest is visible on sessions strictly after its official
  dissemination date. FINRA daily short volume for trade date `T` is visible
  from session `T+1`.

## Stages and output schemas

All tables are Parquet (zstd). Years are hive partitions (`year=YYYY/`).

### P: prices (`prices/`)

Source: `TickerHistory3.parquet` (ORATS, 2012-03-26..2026-09-18). One row per
unique `(session_date, security_id)`; duplicate keys are quarantined.

| column | meaning |
| --- | --- |
| `ticker` | vendor `ticker_tk` on that date |
| `open`, `high`, `low`, `close` | raw traded prices |
| `ret` | vendor `totalReturn` (close over split/dividend-adjusted prior close, minus 1); NULL when guarded |
| `ret_guarded` | true when the vendor return is non-finite, `|ln(1+ret)| > 1.5`, or a sentinel price |
| `adj_close` | backward chain of `1 + ret` anchored to the line's last raw close; immune to the vendor factor re-anchoring on 2021-01-04 |
| `volume`, `dollar_volume` | shares and `close * volume` |
| `shares_vendor` | vendor shares (thousands x 1000), same date: NOT point in time (cover-date runs) |
| `earn_flag` | vendor `earnFlag` (`N`, `-1`, `0`, `1`) |
| `iv_atm_{5,10,21,42,63,126,252}d` | vendor clean ATM IV, NULL outside [0.02, 5.0] |
| `gics` | vendor GICS code (static vendor field; not point in time) |

Vendor defects repaired in this stage:

* `securityID` 0 is the vendor's catch-all for unmapped tickers and carries
  real lines (MSFT from 2025-11-24 to 2026-08-04, class shares such as WSO.B).
  Rule `sid0-bracket-v1` reassigns a sid-0 row `(d, ticker)` to line S when it
  is the only sid-0 row with that ticker on d and the nearest non-zero rows
  with that ticker before and after d both belong to S, each within 400 days.
  The original row wins a key collision. 97,745 rows on 5,468 lines.
* `factor-break-v1` on 17 mass sessions (2021-01-04: 2,144 jump cells).
* When the vendor's prior close disagrees with the line's previous observation
  (a gap), `ret` is computed from the observed closes and the day's factor
  (`ret_source = 'observed'`).

### I: identity (`identity/`)

Dated `security_id -> cik` links in three labelled tiers, combined in
`links_combined.parquet` (`security_id, cik, start, end_incl, primary, tier, basis, available_at`):

| tier | module | rule |
| --- | --- | --- |
| strict (`high`/`medium`) | `identity_links.py` | `r4-links-asof-v1` over the identity rehearsal r4 export, seal moved to 2026-09-21 |
| `backfill` | `identity_backfill.py` | `snapshot-run-backfill-v1`: SEC current-ticker links at the 2026-09-20 snapshot extended back over the line's final contiguous trading run, only after the CIK's first periodic filing, never over a strict link |
| `name` | `identity_names.py` | `finra-name-match-v1`: FINRA issue name on the dissemination date matched to a unique SEC filer name printed on a periodic report filed in the prior 450 days |

The backfill tier is survivorship-tilted (only lines alive at the snapshot).
The panel's `link_tier` lets a consumer restrict issuer fields to strict links.
Primary per `(cik, day)`: the strict P line, else the backfill line whose
snapshot role is P, else the smallest `security_id`.

### F: fundamentals (`fundamentals/events.parquet`, `fundamentals/sic_events.parquet`)

One row per issuer filing event: the issuer's latest-known state of every item
after that filing (restatements enter on their own filing clock). `clock_utc`
is the FSDS `accepted_utc` of the accession, else `filed + 46h` (labelled
`clock_basis`). Items, all DOUBLE in USD or shares: `at, lt, che, debt, be,
seq, sale_q, sale_ttm, cogs_ttm, xsga_ttm, gp_ttm, oi_ttm, ni_q, ni_ttm,
cfo_ttm, capx_ttm, xrd_ttm, dvc_ttm, prstkc_ttm, sstk_ttm, dp_ttm, txt_q,
shrs_q, noa, invt, rect, ppe` and lags `at_lag4, be_lag1q, be_lag1q_lag4,
ni_q_lag4, txt_q_lag4, shrs_q_lag4, noa_lag4, sale_q_lag4`, plus `sue` and
`fscore`. `sic_events`: `cik, clock_utc, sic, sic2, ff12, ff49`.

Rule `fund-events-pit-v2` (lane FUND2) keeps those columns and adds, all
additive: D6 items `cogs_q, xsga_q, gp_q, oi_q, xint_q, xint_ttm, dp_q,
ebitda_q, ebitda_ttm, dvt_q, dvt_ttm, act, lct, ap, drev, ppegt, gdwl, intan,
mib, pstk, buyback_authorized, buyback_remaining`; the nine Piotroski terms
`f_roa, f_cfo, f_droa, f_accrual, f_dlever, f_dliquid, f_eq_offer, f_dmargin,
f_dturn` with `fscore_n` and `fscore_partial`; descriptors `currency,
fin_template, sic_in_force, staleness_days, xrd_reported_zero, zero_filled,
sale_src, gp_src, oi_src, shrs_src`. Events start at clock 2010-01-01 and
cover IFRS (ifrs-full) filers. `quarterly_history.parquet` holds one row per
`(cik, item, period_end, fiscal_period, accession)` with `value, currency,
available_at`. The consumer export `export/fundamental-events-v1/` follows
`atx.fundamental-events/v1`. Details: `ALPHA_PANEL_FUNDAMENTALS.md`.

### S: short interest (`short_interest/si.parquet`)

`security_id, settlement_date, dissemination_date, si_shares, si_dtc, adv_finra, revision_flag`.

### V: short volume (`short_volume/`)

`security_id, trade_date, short_volume, short_exempt_volume, total_volume`
(FINRA CNMS consolidated TRF/ADF, 2018-08-01 onward).

### X: panel (`panel/`)

One row per `(session_date, security_id)` present in `prices`, with the
scorecard field names (`close`, `raw_close`, `volume`, `mkt_ret`,
`shares_out`, `me_company`, fundamentals, `grp_ff12`, `grp_ff49`, `si_shares`,
`si_dtc`, `iv_atm_*`, `earn_recent`) plus membership flags. `member` is the
scorecard universe `research-prior63-usd-adv-topn-v1` (top 3,000 by prior
63-session dollar volume, ADV > $5M, raw price > $5, one-session lag).
`is_operating` marks lines with a vendor earnings reaction day in the prior
400 days (ETFs never have one), `is_index` marks the vendor index namespace
(`security_id >= 1e12`: NDX, RUT, DJX), and `member_equity = member AND
is_operating AND NOT is_index` is the equity universe used for coverage.
`link_tier` is `strict`, `backfill` or `name`. Short volume columns
`sv_short_volume`, `sv_short_exempt`, `sv_total_volume` hold trade date d-1.

### C: characteristics (`characteristics/`)

Raw (unranked) daily characteristic values: every scorecard base quantity and
the additional characteristics listed in `characteristics.py`.

Scorecard base quantities (library v5.1 names; the DSL applies rank/group
rank and `decay_linear(., 21)` on top): `chtax, droe, ear, sue, asset_growth,
noa_at, issuance_xbrl, issuance_vendor, low_beta, low_ivol, low_max,
lowvol_ind, iv_rv_spread, high_52w, mom_12_1, ind_mom_12_1, within_ind_mom,
accruals, cfoa, fscore, gpa, opbe, opex_at, roa, roe_q, ind_adj_rev_5,
seasonality_same_month, dtc, si_change, si_ratio, bm, cfp, ebit_ev, ep, fcfp,
net_payout, rd_me, sp`.

Additional characteristics (sign = literature prior, higher = long):

| name | definition | source |
| --- | --- | --- |
| `resid_mom_12_1` | sum of market-model residual returns t-251..t-21 over their stdev | Blitz, Huij and Martens (2011, JEF) |
| `rev_21`, `ind_adj_rev_21` | minus 21-session return (raw / FF49-demeaned) | Jegadeesh (1990); Da, Liu and Schaumburg (2014) |
| `iv_term_slope` | `iv_atm_252d - iv_atm_21d` | Vasquez (2017, JFQA) |
| `iv_change_21` | minus the 21-session change in 63-day ATM IV | Ang, Bali and Cakici (2010); An, Ang, Bali and Cakici (2014) |
| `iv_rv_ratio` | 21-day ATM IV over 21-day realized volatility | Bali and Hovakimian (2009) |
| `short_vol_ratio_5`, `_21` | minus FINRA off-exchange short volume share, 5/21 sessions | Diether, Lee and Werner (2009, RFS) |
| `si_to_adv` | minus short interest over 21-session average volume (unfloored days to cover) | Hong, Li, Ni, Scheinkman and Yan (2015) |
| `abn_turnover` | 21-session over 252-session average volume, minus 1 | Gervais, Kaniel and Mingelgrin (2001, JF) |
| `amihud_21` | mean abs return per $1M traded | Amihud (2002, JFM) |
| `hl_spread_21` | Corwin-Schultz high-low spread estimate (cost input, unsigned) | Corwin and Schultz (2012, JF) |
| `sale_growth_q`, `gm_change` | year-over-year quarterly sales growth; TTM gross margin change | Lakonishok, Shleifer and Vishny (1994); Abarbanell and Bushee (1998) |
| `capx_at`, `leverage`, `cash_at`, `dvc_yield` | investment, book leverage, cash, dividend yield | Titman, Wei and Xie (2004) and standard controls |
| `beta_252`, `vol_21`, `vol_252`, `ivol_21`, `turnover_21`, `log_me`, `log_me_line`, `earn_days_since` | risk, size, liquidity and event-time controls | |

### E: export

`export_impl.py` writes a role and fields directory in the atx-impl binary
layout (`atx.recent-research-role/v1`, `atx.research-role-fields/v1`) for any
window of the panel.

## Build and refresh

One command runs every stage in order, each in its own guarded process
(a stage that the guard stops for low memory is retried; every stage resumes):

```powershell
cd C:\atx\atx-db
$env:PYTHONPATH = "C:\atx\atx-db\src"
.venv\Scripts\python.exe ..\.superpowers\sdd\tier1-parity\run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 -- `
    .venv\Scripts\python.exe -m atx_db.alpha_panel.build            # or --from panel / --only coverage
```

To refresh with new data: replace the vendor file (`ATX_TICKERHISTORY`), the
Company Facts / FSDS staging, and run `build`; the FINRA stages fetch new
settlements and short-volume days themselves. The projection cache is keyed
on the vendor file's size and modification time.

Downstream consumers:

```powershell
# atx-impl role + fields (<= 2024-12-31: the atx-engine loader seals roles at 2025-01-01)
.venv\Scripts\python.exe -m atx_db.alpha_panel.export_impl --out data\alpha_panel\v1\export\impl-2020-2024 --score-start 2020-09-28 --end 2024-12-31
# TRAIN-only IC screen of the characteristics (2020-09-28 .. 2022-12-31)
.venv\Scripts\python.exe -m atx_db.alpha_panel.evaluate
```

Measured coverage: `docs/ALPHA_PANEL_COVERAGE.md`. IC screen:
`docs/ALPHA_PANEL_IC.md`. Stage reports: `docs/ALPHA_PANEL_FUNDAMENTALS.md`,
`docs/ALPHA_PANEL_FINRA.md`.

## Known limitations

* Vendor prices and IV carry no vintage proof (`historical_vintage_verified`
  false); FINRA short interest before June 2021 is FINRA's later
  republication.
* Identity: the backfill tier covers only lines alive at the 2026-09-20
  snapshot (survivorship tilt); the name tier relies on unique name matches
  against FINRA's 30-character issue names. `link_tier` identifies both.
* Fundamentals come from Company Facts (us-gaap and, from rule v2, ifrs-full).
  There is no point-in-time FX source, so money items of filings that report
  in another currency (TSM in TWD, ASML in EUR) are NaN; their
  currency-invariant items and the quarterly history (with `currency`) are
  filled. Company Facts drops dimensional facts; multi-class share counts use
  FSDS class-of-stock sums as the last fallback.
* `member` replicates the scorecard universe and therefore includes ETFs and
  vendor index lines; use `member_equity` for an operating-company universe.
* The atx-engine role loader refuses sessions on or after 2025-01-01 (its
  holdout seal); lifting that seal is an owner decision outside atx-db.

## Resource rules

Run every stage under the memory guard
(`.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb <= 1.0`).
DuckDB connections set `memory_limit` well below the cap and at most two
threads. Stages stream by year or by batch and are resumable: a completed
partition with a matching receipt is skipped.
