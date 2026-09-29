# Alpha panel: short-side and ownership data (request D1, D3, D4)

This doc covers the short-side and ownership stages built for the data request
`C:\atx-wt\pool-2\docs\plans\2026-09-28-mega-alpha-data-request-atx-db.md`:

- 13F holdings (D1);
- SEC fails-to-deliver, Reg SHO threshold lists, and the exempt and per-venue short-volume splits (D3);
- a public borrow proxy (D4).

Code lives in `src/atx_db/alpha_panel/`: `ftd.py`, `regsho.py`, `short_volume_ext.py`, `thirteenf.py`,
`borrow_proxy.py`, and the shared helpers in `shortflow_common.py`. Each module docstring holds the full rules. Every
stage publishes its `manifest.json` last. The manifest lists the SHA-256 of every output and of the producing code,
the source files with their SHA-256 and HTTP `Last-Modified`, the clock rule and the staleness rule.

## Common rules

- **Symbol → security_id.** The stage-S as-of rule applies: the SEC or exchange symbol maps to the single
  TickerHistory3 line whose canonical ticker matches. The match is taken on the last session on or before the data
  date, with a 7-day look-back. The sid0-bracket repair is shared, so class shares and MSFT 2025-26 are recovered.
  Two lines carrying the symbol makes it ambiguous, and it stays unmapped.
- **Visibility.** Every row carries `available_at`, in UTC. A consumer uses a value at decision session d only if
  `available_at < 22:00 UTC of session d-1`.
- **Vintages.** Nothing is overwritten. `vintage_risk` is true on every row of these sources, because each publisher
  re-posts files without revision history.
- **Panel columns.** The panel joins the latest visible value as of each session, stale-gated. The columns are:
  - FTD: `ftd_quantity`, `ftd_settlement_date`, `ftd_price`, `ftd_vintage_risk`;
  - Reg SHO: `regsho_last_list_date`, `regsho_run_days`, `regsho_market`;
  - short volume: `sv_short_volume`, `sv_short_exempt`, `sv_total_volume`, `sv_n_facilities`, `sv_market`;
  - 13F: `inst_period`, `inst_shares`, `inst_n_holders`, `inst_top10_share`, `inst_d_shares`,
    `inst_pct_change`, `inst_d_holders`.

## D3a: fails-to-deliver (`ftd/`)

**Source.** The SEC files `cnsfailsYYYYMM{a,b}.zip`, twice a month, 2013-01 onward. The 2013-2017 files serve only
as the look-back for the 13F CUSIP map.

**Output.** `ftd/year=YYYY/ftd.parquet`, one row per file row: `settlement_date, cusip, symbol, quantity, price,
security_id, available_at, map_basis, vintage_risk, source_file`.

**Clock.** `ftd-publication-halfmonth-v1`. `available_at` is the later of two times:
- the nominal SEC date plus 7 days, at 00:00 UTC. The nominal date is month end for the `a` half and the 15th of the
  next month for the `b` half;
- the file's HTTP Last-Modified, when that lies within [-20 d, +60 d] of the nominal date.

**Staleness.** 60 days after the latest published settlement date. A security absent from a published file had no
CNS fail that day.

| year | rows | lines | rows mapped | fail value mapped |
|---|---:|---:|---:|---:|
| 2018 | 1,194,854 | 9,284 | 77.9% | 95.6% |
| 2019 | 1,050,602 | 9,408 | 79.0% | 95.8% |
| 2020 | 1,206,764 | 10,107 | 79.1% | 97.1% |
| 2021 | 1,460,785 | 12,492 | 80.8% | 97.3% |
| 2022 | 1,477,590 | 12,700 | 83.5% | 97.8% |
| 2023 | 1,400,306 | 12,167 | 84.2% | 97.6% |
| 2024 | 1,285,064 | 11,966 | 83.3% | 96.1% |
| 2025 | 1,326,583 | 12,971 | 85.0% | 95.5% |
| 2026 (to 08-31) | 944,739 | 13,712 | 86.8% | 97.5% |

The unmapped rows are mostly OTC and delisted symbols outside the vendor file. By fail value, 96-98% maps.

## D3b: Reg SHO threshold lists (`regsho_threshold/`)

**Sources.** The daily lists of each listing market, landed as served:

| market | source | notes |
|---|---|---|
| Nasdaq | nasdaqtrader | |
| NYSE, NYSE American, NYSE Arca | the NYSE API's combined file, split by market category | see status below |
| Cboe BZX | Cboe CDN | |
| OTC | FINRA Query API | |

**Output.**
- `regsho_threshold/year=YYYY/threshold.parquet`: one row per (list_date, market, symbol), with `on_list`,
  `run_days` (consecutive lists carrying the symbol), `security_id`, `available_at`, `vintage_risk`.
- `lists.parquet`: one row per (market, date) attempted, with `status` = list / empty_list / absent.

**Clock.** `regsho-publication-v1`, set per market:
- Nasdaq: D+1 06:00 UTC, or a later Last-Modified;
- NYSE family: D+1 06:00 UTC;
- Cboe and OTC: the next session at 16:00 UTC.

**Staleness.** 10 days after the latest visible list.

**Status.** The published stage has full Nasdaq, Cboe BZX and OTC history for 2018-2026. For the NYSE family it has
only 51 days of 2018: the earlier per-market NYSE endpoint was throttled.

The combined NYSE file is still being landed. At 00:39 UTC on 2026-09-29, 855 of about 2,180 dates were in
`data/raw/regsho_threshold/nyse_combined/`. The `regsho build` step, then the panel rebuild, adds the NYSE family
once the landing finishes. Until then, NYSE-listed names never show `on_threshold_list`.

Symbols on the lists that were mapped in 2021:
- Nasdaq: 99.4%;
- Cboe: 100%;
- OTC: 0%. OTC symbols are outside the vendor file, which is expected.

## D3c: short volume with exempt volume and venues (`short_volume_ext/`)

**Source.** The FINRA CNMS daily files already landed by stage V, for 2018-08-01 onward. Each file is checked
against its landing SHA-256. Nothing is re-downloaded.

**Output.** One row per (trade_date, symbol) with these columns:
- `short_volume, short_exempt_volume, total_volume, market`;
- per-facility flags `fac_b` (Nasdaq TRF Chicago), `fac_q` (Nasdaq TRF Carteret), `fac_n` (NYSE TRF) and `fac_d`
  (ADF);
- `n_facilities`.

**Clock.** `available_at` = trade date + 1 day, 00:00 UTC. A trade date T is visible from session T+1.

**Staleness.** 5 sessions.

**Coverage.** 99.98-100% of member_equity cells have a row, every year from 2019.

FINRA's per-venue split carries facility flags, not volumes by venue: the CNMS file aggregates across facilities.

## D1: 13F holdings (`thirteenf/`)

**Source.** Every SEC Form 13F data set. Coverage runs from filing quarter 2013q2 to 2026q2, so four-quarter changes
exist well before 2020. The zips are deleted after parsing. A receipt per zip keeps the url, SHA-256, size and
Last-Modified.

**Outputs.**

| file | content |
|---|---|
| `parts/source=<set>/holdings.parquet` | every INFOTABLE row, 124.4M in all: filer CIK, accession, period, filing date, `available_at`, CUSIP (repaired), shares or principal, `value_usd`, put/call, discretion, voting authority |
| `parts/source=<set>/filings.parquet`, `filings.parquet`, `filers.parquet` | one row per filing and per (filer, quarter), with amendment type and a value-unit check |
| `cusip_map_pit.parquet` | CUSIP → security_id at each quarter end (rule `13f-cusip-ftd-window-v1`) |
| `agg_asof45.parquet` | per (quarter, security): `inst_shares, n_holders, top10_share, top1_share, inst_value_usd`, and changes vs the prior quarter (`d_inst_shares, pct_inst_shares, d_n_holders`) |
| `agg_final.parquet` | the same aggregate over all filings, amendments included |

Originals and amendments are separate rows, so both vintages are kept. `agg_asof45.parquet` counts only filings made
by the 45-day deadline, so it is the point-in-time version.

**Clock.** `13f-filed-plus-46h-v1`: `available_at` is the filing date at 00:00 UTC + 46 h. The data sets carry no
acceptance time.

**Staleness.** 150 days after the quarter end. The panel applies this as 105 days after `available_at`.

**Value unit.** Values are in thousands before 2023-01-03 and in dollars after, per the SEC readme. Each filing is
audited against cross-filer implied prices.

| quarter | filers | holdings rows | securities mapped | value mapped (SH, non-option) |
|---|---:|---:|---:|---:|
| 2015-12-31 | 4,102 | 1,545,909 | 7,729 | 99.1% |
| 2018-12-31 | 4,815 | 1,812,983 | 8,540 | 99.6% |
| 2020-12-31 | 5,584 | 2,103,712 | 9,116 | 99.6% |
| 2022-12-31 | 6,606 | 2,343,938 | 11,378 | 99.6% |
| 2024-12-31 | 7,733 | 2,913,984 | 10,980 | 99.9% |
| 2026-06-30 | 8,695 | 3,376,334 | 12,596 | 99.8% |

In the 2021-06 smoke month of the v2 panel, 99.7% of member_equity cells have visible `inst_shares`.

**Not delivered: filer type.** Hedge fund, mutual fund and other are not classified. No free, authoritative,
point-in-time label exists. Form ADV is not landed, and a curated list would not be point in time. `filers.parquet`
has the fields a later classifier needs: CRD number, SEC file number, 13F file number and name.

## D4: borrow proxy (`borrow_proxy/`)

**This is a proxy.** atx-db holds no securities-lending data. There is no borrow fee, utilization, lendable quantity
or rebate rate: those need a licensed feed such as IHS Markit, S3 or Ortex.

The stage lines up the public short-side inputs on the prices grid (every line and session, 2018 onward), each with
its own clock:

| inputs | source |
|---|---|
| short interest | FINRA |
| institutional shares | 13F `agg_asof45` |
| SI / institutional-ownership ratio | computed only when both inputs are fresh |
| latest FTD and last non-zero FTD | FTD |
| threshold-list membership and run length | Reg SHO |

The consumer builds its own hard-to-borrow screen from these.

**Status.** The code is written (`borrow_proxy.py`) but the stage is **not built yet**. It reads the rebuilt panel's
`member_equity` for its coverage table, so it runs after the panel rebuild.
