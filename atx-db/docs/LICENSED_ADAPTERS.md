# Licensed-data adapters (tier1-v3 S8)

Ruling D3 (`.superpowers/sdd/tier1-v3/CARRY.md`): **buy nothing now**. For every domain that only a license can
supply (plan §1.2), atx-db ships a tested adapter contract, a mock delivery in the vendor's layout, and a pointer to
the best free substitute stage. When a product is bought (S8.3), its files are landed as served and loaded by the
same code; the contract tests become the acceptance gate on real data.

Code: `atx_db/licensed/` (`contract.py`, `identity.py`, `mock.py`, one module per adapter). Tests:
`tests/test_licensed_*.py` (offline, fixture-only, a few seconds each). CLI: `python -m atx_db.licensed`.

## 1. Contract (every adapter)

**Input.** A delivery directory `raw_dir` holding the vendor files as served and a `receipts.jsonl` (one JSON line per
file: `file, url, bytes, sha256, http_status, fetched_at, history_mode`; `contract.write_receipt` appends one). A
receipt whose SHA-256 does not match the file stops the load.

**Output.** `load(raw_dir) -> Stage` returns validated pyarrow tables; with `out_dir` (the CLI default is
`<lake>/licensed_<adapter>/`) it writes one Parquet per table (zstd, row groups ≤ 32,768) and, last, `manifest.json`:
schema id `atx.licensed.<adapter>/v1`, product, PIT rule, per-table key / series / staleness, SHA-256 of every
`licensed/*.py`, of every output and of every raw input (with its `history_mode` and delivery time), the SHA-256 of
the identity input manifests (`input_manifests_sha256`), reject counts and the validation report.

**Columns shared by every table.**

| group | columns | meaning |
|---|---|---|
| identity | `security_id`, `cik`, `link_tier`, `cik_link_tier`, `vendor_security_id`, `cusip`, `ticker`, `id_date` | TickerHistory3 securityID and CIK resolved on `id_date` (rule below); the vendor identifiers as delivered |
| clocks (naive UTC) | `vendor_snapshot_at`, `delivered_at`, `available_at`, `clock_basis`, `history_mode`, `vintage_risk` | see below |
| lineage | `source_file`, `source_file_sha256` | the raw file each row came from |

**Clocks.** Each adapter computes a product *rule clock* from the vendor's own dates (e.g. STATPERS + publication lag,
activation time, documented delivery time). Then, by the file's `history_mode`:

| `history_mode` | when | `available_at` |
|---|---|---|
| `pit_archive` | the vendor attests the history is as first published (I/B/E/S detail and summary history, SpiderRock EOD history) | the rule clock |
| `daily` | our own daily pull | `greatest(rule clock, delivered_at)` |
| `backfill` | an as-of history re-cut by the vendor (restated consensus, revised lending history, GICS restated to today's structure), **and any file without a receipt** | `greatest(rule clock, delivered_at or build time)`; `vintage_risk = true` |

So a vendor as-of backfill can never reach the past: its rows are visible only from their delivery. `clock_basis`
names the clock that won: `vendor_pit` (a vendor timestamp), `publication_lag` (a documented schedule), `floor` (a
documented conservative floor for a missing time), `delivery`, `structure_guard` (GICS, §2.3). A consumer sees a row
at decision session d iff `available_at < 22:00 UTC of d-1` (plan §3), plus, for dated spells (GICS, index
membership), `effective_from <= d <= effective_to` (inclusive, NULL = open; the MKT index stages use the same
convention).

**Vintages.** Revisions, restatements and new versions are new rows; nothing is overwritten. Each table declares a
unique `key` and a vintage `series` (the key without the snapshot).

**Validation** (`Adapter.validate`, fatal unless noted; `load(strict=True)` raises `ContractViolation`):

| check | rule |
|---|---|
| `schema` | the table's pyarrow schema equals the contract schema |
| `keys_unique` | no duplicate key |
| `clocks_present` | `available_at`, `vendor_snapshot_at`, `clock_basis` non-null |
| `snapshot_before_available` | `vendor_snapshot_at <= available_at` |
| `no_future_clocks` | `available_at`, `vendor_snapshot_at`, `delivered_at` <= build time |
| `clocks_monotone_per_vintage` | along each series in snapshot order, `available_at` never decreases (an older vintage cannot become visible after a newer one) |
| `link_tier_consistent` | `link_tier` in the domain; `security_id` set iff the tier is a mapped one |
| `clock_domains` | `history_mode`, `clock_basis` in their domains |
| adapter checks | e.g. rec codes 1-5, utilization in [0, 100], index weights sum to 1; data-quality checks (consensus low <= mean <= high, GICS structure, overlapping spells) are reported, not fatal |

Rows missing a mandatory vendor field are rejected before validation and counted per reason in `Stage.rejects`
(e.g. an I/B/E/S detail row without ACTDATS).

**Identifier mapping** (rule `licensed-id-v1`, `identity.py`). Histories: CUSIP intervals from
`security_master/cusip_history.parquet` (lane ID, S2.3) when present, else the FTD CUSIP map plus the 13F PIT map;
ticker intervals from `security_master/ticker_history.parquet` when present, else the vendor ticker runs of the prices
stage; CIK intervals from `identity/link_table.parquet`. Per row on its `id_date`, the first tier with a candidate
wins, and two or more distinct lines in that tier make the row `ambiguous`:

| tier | evidence |
|---|---|
| `vendor_native` | the vendor carries the TickerHistory3 securityID (SpiderRock products) |
| `cusip_dated` | a CUSIP interval (first 8 characters) contains `id_date` |
| `cusip_undated` | a CUSIP interval lies within 400 days of `id_date` (observed intervals, snapshot-dependent: labelled apart) |
| `ticker_dated` | a ticker interval (canonical form: `.`, `/`, `-`, whitespace stripped, case-sensitive) contains `id_date`; a vendor ticker with a separator (`NE.WT`, `T.PC`) never matches a history ticker without one (`NEWT`, `TPC`), while `BRKB` still matches `BRK.B` |
| `unmapped` / `ambiguous` | kept, `security_id` NULL (no survivorship by identity) |

`cik` comes from the link-table interval containing `id_date` (a gap of up to 10 days is bridged; ties go to the latest
interval, then strict < name < backfill); `cik_link_tier` repeats its tier.

## 2. Adapters

### 2.1 Analyst estimates (`estimates.py`, stage `licensed_estimates/`)

Product: I/B/E/S History (LSEG) via WRDS; FactSet Estimates and Zacks through the column aliases. Raw families:
`statsum*` (summary), `det*` (detail), `act*` (actuals), `recd*` (recommendations), `ptg*` (price targets); I/B/E/S
times are US/Eastern.

| table | key (series = key without the snapshot) | PIT rule (`estimates-pit-v1`) |
|---|---|---|
| `consensus` | vendor id, measure, period type, period end, STATPERS | the summary as of STATPERS is visible from **the next weekday after STATPERS at 12:00 ET** (publication lag); snapshot = STATPERS 23:59:59 ET. Monthly STATPERS is the Thursday before the third Friday, so the first usable session is the following Monday. Never a vendor "as-of" re-cut: restated summaries are `backfill` files, clocked at delivery |
| `detail` | vendor id, broker, analyst, measure, period type, period end, activation | visible from **activation** (ACTDATS + ACTTIMS ET, the entry into the vendor database), never from ANNDATS (the analyst's date, usually earlier) or REVDATS (a later confirmation). Missing ACTTIMS: 23:59:59 ET of ACTDATS (`floor`); missing ACTDATS: rejected |
| `actuals` | vendor id, measure, period type, period end, activation | `greatest(activation, announcement)`; a restated actual is a second vintage |
| `recommendations` | vendor id, broker, analyst, activation | `greatest(activation, announcement)`; `prior_rec_code` and `action` (INITIATE / UPGRADE / DOWNGRADE / REITERATE) from earlier rows of the series only |
| `price_targets` | vendor id, broker, analyst, horizon, activation | `greatest(activation, announcement)`; `prior_target` from earlier rows |

Land the **unadjusted** I/B/E/S files (per-share values as published). The split-adjusted files rescale past
estimates by later splits: harmless in availability, but mixing them with raw prices is a look-ahead in units.

Measures: `EPS` with PDF `P` -> `EPS_BASIC`, `D` -> `EPS_DILUTED`, no PDF -> `EPS` (the summary's majority basis);
`SAL` -> `REVENUE`, `NET` -> `NET_INCOME`, `OPR` -> `OPERATING_INCOME`, others as delivered. Staleness declared:
consensus 45 d, detail 105 d, recommendations and targets 365 d.

Substitute: `events/guidance.parquet` (lane EVT, S6.4; `measure`, `period_type`, `period_end`, `low` / `high` / `mid`):
management guidance ranges from 8-K EX-99 releases for consensus; the panel's time-series `sue` for the consensus surprise. No free substitute for detail, recommendations
or targets.

Reused from v2 `atx_db/estimates/` (ported, so this package does not import the retired warehouse; a test pins them
equal while v2 exists): `IBES_MEASURE_MAP`, the EPS PDF rule, the FPI -> period-type map, `RECOMMENDATION_TEXT_MAP`,
`RECOMMENDATION_LABELS` and the upgrade/downgrade rule, and the column aliases. Superseded as unsound: the
hash-of-ticker `security_id` (now TickerHistory3 ids with `link_tier`), the STATPERS end-of-day consensus clock (now
plus the publication lag), `now()` as the clock of rows without one (now the delivery clock), and EPS without PDF
defaulting to diluted. The v2 guidance text extractor and SUE are superseded by lane EVT (S6.4) and the panel's `sue`.

### 2.2 Securities lending (`lending.py`, stage `licensed_lending/`)

Product: S&P Global Securities Finance (ex IHS Markit) equity daily file; DataLend / EquiLend / Hazeltree spellings
through the aliases (field names to confirm against the vendor dictionary at purchase). Table `lending_daily` (key:
CUSIP, ISIN, ticker, business date): fee and rebate (bps p.a.), daily cost-of-borrow score (1-10), utilization (%),
lendable / on-loan quantity and value, short-loan quantity, borrower concentration.

PIT rule `lending-pit-v1`: business date D (end-of-day positions) is visible from **the next weekday after D at 12:00
UTC** (vendor publishes the London morning after); snapshot = D 23:59:59 ET. A daily pull delivered later than that
is clocked at delivery; a vendor history re-cut is a `backfill`. Checks: utilization in [0, 100], score in 1-10, fee
>= 0; on-loan > lendable is reported.

Substitute: `borrow_proxy/` (controller, S0.1): utilization ≈ `si_to_io` (FINRA short interest / 13F shares),
lendable ≈ `inst_shares`, on loan ≈ `si_shares`, hard-to-borrow ≈ `on_threshold_list` (Reg SHO) and FTDs. No fee
proxy.

### 2.3 GICS (`gics.py`, stage `licensed_gics/`)

Product: S&P/MSCI GICS Direct history (or a Compustat `co_hgic`-style extract re-keyed to CUSIP). Table
`gics_history`: sector / group / industry / sub-industry (2/4/6/8-digit prefixes), `effective_from`, `effective_to`,
`effective_from_vendor`, `structure_ok`.

PIT rule `gics-pit-v1`: a spell "code X from F" is known at its publication (SNAPSHOTDATE 23:59:59 ET; changes are
announced before F) and applies to sessions `F <= d <= effective_to` (INDTHRU, inclusive); without a publication date it is known at F
00:00 ET (`floor`). **Structure guard** against the classic GICS backfill (today's code applied to years before its
structure existed): a code used before its structure start has `effective_from` clipped to that start
(`structure_guard`, `vintage_risk`); a code used after its discontinuation fails `structure_ok` (reported). Seed
breaks (`GICS_STRUCTURE`, extend from the vendor's structure file): Real Estate sector 60 from 2016-09-01; Media &
Entertainment group 5020 from 2018-10-01; Transaction & Payment Processing 40201060 from 2023-03-20; Data Processing
& Outsourced Services 45102020 discontinued 2023-03-17.

The vendor price file (TickerHistory3) has a `GICS` column, but it is empty in every lake prices row 2018-2026
(0 non-null of 21.9M rows, measured 2026-09-29), so there is no free GICS history to load.

Substitute: `classification/issuer_industry.parquet` (lane MKT, S7.1): SIC -> NAICS 2022 (approximate) and Fama-French
5-49 industries per CIK at the SIC event clock; text industries (lane TXT, S7.2) for peer sets.

### 2.4 Index constituents and weights (`indexes.py`, stage `licensed_indexes/`)

Product: S&P DJI (500/400/600/1500) and FTSE Russell (1000/2000/3000) constituent history and end-of-day files.

| table | key | PIT rule (`index-pit-v1`) |
|---|---|---|
| `constituents` | index, CUSIP, ticker, effective_from, snapshot | a membership spell is known at its **announcement** (date + time ET; 23:59:59 ET without a time) and applies to `effective_from <= d <= effective_to`; without an announcement it is known at the effective date's 09:30 ET open (`floor`). Spells of delisted names are kept with their end date |
| `weights` | index, CUSIP, ticker, date | the close file of D (snapshot D 16:00 ET) is visible at **D 23:59:59 ET** (evening delivery); weights normalized to fractions per (index, date) and checked to sum to 1 |

A provider history that drops dead names or re-keys them to today's identifiers surfaces as `unmapped` /
`cusip_undated` rows in the validation stats.

Substitute: `indexes/constituents.parquet` and the daily `indexes/membership/` (lane MKT, S3.6): rule-based Russell
1000/2000/3000 proxies, S&P 500 change list from public
releases where terms allow, CRSP-style VW/EW market returns.

### 2.5 Earnings-call transcripts (`transcripts.py`, stage `licensed_transcripts/`)

Product: S&P Capital IQ Transcripts (FactSet CallStreet, LSEG StreetEvents differ in spelling only). Tables `calls`
(one row per transcript **version**: preliminary, edited, proofed, audited; event id, call time, fiscal period,
component and word counts, text SHA-256) and `components` (speaker turns with speaker type and text).

PIT rule `transcripts-pit-v1`: a version is visible from `greatest(version creation time, call start)` (UTC): the
audio was public at the call, the text record not before its creation. A version without a creation time is visible
from the file's delivery (`delivery`, `vintage_risk`); a fixed lag after the call could backdate an audited copy made
weeks later. Components inherit their version's clocks and identifiers.

Substitute: none (plan §1.2). The nearest free text, the 8-K EX-99 earnings release, is a press release, not a call.

### 2.6 Options (S8.2): what exists, the free stage, and the license need

**What option data the repository keeps (measured 2026-09-29).**

* **atx-vol** was removed from the tree on 2026-09-19 (commit `e4bdcf54`, "clean up"); `C:/atx/CLAUDE.md` still
  describes it. Its surface store (SurfaceDb) was built per session from an OPRA quote hive: `C:/atx-data/opra-hive`
  (616-name universe, 15:55 ET boards; per its ledger the 2024-2025 partitions were SPY-only) and `C:/atx-data/opra-all`
  (10 full-OPRA sessions, 2026-08-10..21). `C:/atx-data` is now empty. What remains: `C:/atx-scratch/opra-hive-tail`
  (3 sessions, 2026-07-29..31, 36 MB) and `C:/atx-scratch/surface-db/sp100-2026` (85 MB). **No per-security surface
  history exists.**
* `C:/Users/natha/Downloads/ORATS_SMV_Strikes_20240103.zip`: one ORATS strikes day (per strike: call/put volume and OI,
  bid/mid/ask IV, smoothed SMV vol, delta). Useful to cross-check a purchased product on 2024-01-03; not a history.
* **The panel's `iv_atm_*`** come from the prices stage, which reads the vendor file (SpiderRock TickerHistory3)
  columns `atmCenI_{5,10,21,42,63,126,252}d`: ATM implied volatility with the implied earnings move removed
  ("censored"), NULL outside [0.02, 5.0], at trading-day tenors (21 d = 30 calendar days). TickerHistory3 also carries
  `atmCenI` at 84-504 d, `atmCenH_*` (historical-earnings censored), `iEMove` / `hEMove` (implied / historical earnings
  move), `wkD1`/`shD1`/`qtrD1`/`lnD1` (ATM slopes) and `expiryCount`. It has **no delta-bucketed IV, no option volume
  and no open interest** (vendor dictionary; schema checked).

**Free stage `options/`** (`alpha_panel/options.py`, `python -m atx_db.alpha_panel.options`): one row per
(session_date, security_id) with a vendor ATM IV: `iv_atm_{21,63,126,252}d`, `term_slope_63_21`, `term_slope_252_21`
(= the panel characteristic `iv_term_slope`, Vasquez 2017), `atm_source = 'tickerhistory3'`; the licensed columns
`iv_call_25d_30d`, `iv_put_25d_30d`, `skew_25d_30d`, `call_volume`, `put_volume`, `call_oi`, `put_oi` stay NULL
(`skew_source = 'none'`) until the adapter below loads a purchase (`--licensed <licensed_options dir>`, full outer
join on (security_id, session_date)). Clock rule `options-clock-v1`: `atm_available_at` = session date 22:00
America/Chicago, the vendor's documented US delivery of its end-of-day surface products (T+0; SurfaceFixedTermHist
carries the same `atmCenI_*` fields), i.e. 03:00/04:00 UTC the next day, so a session's values are usable from the
second following session; `available_at` = the later of that and the licensed clock.

Built 2026-09-30 against panel v2 (71 s, 0.35 GiB peak under the guard): 10,656,780 rows, 9,036 lines,
2018-01-02..2026-09-18, 236 MB, no duplicate (session, security) key, no NULL clock. Coverage over the panel's
`member_equity` cells (manifest `coverage`):

| year | member cells | optionable (vendor ATM IV) | term slope, of optionable cells | term slope, of optionable lines | skew / volume / OI |
|---|---|---|---|---|---|
| 2018 | 451,143 | 96.2% | 99.9% | 99.96% | 0 |
| 2019 | 576,186 | 96.6% | 99.9% | 100% | 0 |
| 2020 | 580,695 | 95.7% | 99.9% | 99.9% | 0 |
| 2021 | 611,044 | 97.2% | 99.9% | 100% | 0 |
| 2022 | 584,009 | 98.4% | 99.9% | 100% | 0 |
| 2023 | 575,461 | 98.2% | 99.9% | 99.9% | 0 |
| 2024 | 573,063 | 97.9% | 99.8% | 100% | 0 |
| 2025 | 553,772 | 97.9% | 99.6% | 100% | 0 |
| 2026 (to 09-18) | 389,602 | 98.0% | 99.7% | 100% | 0 |

So the free part meets the S8.2 line (≥ 95% of optionable member lines) for the ATM level and term slope only; the
25-delta skew, option volume and open interest are 0% and need the license below.

**Clock note for the panel (not changed here).** The panel exports `iv_atm_*` with clock `vendor-eod-same-date`
(`export_impl.py`), i.e. usable at the next session. The vendor documents US delivery of its EOD surface history at
22:00 CT on the trading date (03:00/04:00 UTC next day), after the 22:00 UTC cutoff of the next session, so under the
delivery clock the values are usable one session later than the panel assumes. A live surface feed at the close
would support the panel's convention; the history file alone does not.

**License need.** The same vendor sells the missing fields, keyed by the same `securityID` (so identity is
`vendor_native`, no mapping loss):

| product (SpiderRock history, Parquet on S3) | fields needed | history in product | depth to buy |
|---|---|---|---|
| `OptionEODFeaturesHist` (one row per underlier and day) | `callVolume`, `putVolume`, `callOI`, `putOI` (OI one day delayed), `atmI_21d`, `atmI_252d`, `delta20Skew21D`, `vSlope_21d` | US from 2014-01-02 | 2019-01 onward (D7: one year before the 2020 score window); 2014 onward if 5-year volume/OI lookbacks are wanted |
| `SurfaceFixedGridHist` (per underlier, day and fixed term 5-504 trading days) | `volATM` and the nine call-delta vols `volD40`..`volU40` (10-90 delta) at terms 21 and 252 | US from 2010-01-04 | 2019-01 onward; terms 21 and 252 suffice |

Both are delivered at 22:00 CT on the trading date. Derivation in `licensed/options.py`: 25-delta call IV = mean of
the 20- and 30-delta call vols, 25-delta put IV = mean of the 70- and 80-delta call vols (put delta = call delta - 1,
carry ignored), both on the 21-trading-day term; skew = put - call; term slope = ATM(252) - ATM(21). Column spellings
and the grid's delta convention come from the public dictionaries and must be confirmed at purchase (the mock follows
them). Alternatives with the same fields: OptionMetrics IvyDB US (Volatility Surface at delta ±25 / 30 days; Option
Volume with volume and OI by call/put; `secid` -> CUSIP, mapped by `licensed-id-v1`), ORATS Core history.

Adapter: `licensed/options.py` (`options_daily`: key vendor id, ticker, session; PIT rule `options-pit-v1`:
snapshot = `srCloseTime` (~5 minutes before the close), `available_at` = trading date 22:00 America/Chicago).
Mock: `OptionEODFeaturesHist_2024.parquet` + `SurfaceFixedGridHist_2024.parquet` in the vendor layout.

## 3. Purchase decision points (D3, revisit before S8.3)

| # | product | history / cadence | unlocks | acceptance on real data | until then |
|---|---|---|---|---|---|
| 1 | I/B/E/S (or FactSet) Estimates: detail, summary, actuals, recommendations, targets | 2015+ (5-year revision lookback for the 2020+ score window), daily | consensus surprise, revisions, dispersion, recommendation changes: the largest missing alpha family | contract tests pass; `mapped_share` ≥ 0.98 on consensus; no fatal check | guidance (EVT S6.4), time-series SUE |
| 2 | Securities lending daily file (S&P Global / DataLend) | 2019+ (D7), daily | borrow fee, utilization, short-side constraints for the short book | contract tests pass; ≥ 95% of member_equity cells from 2020 | `borrow_proxy/` |
| 3 | Options EOD features + delta grid (SpiderRock, same securityID) | 2019+ (2014+ for 5-year volume/OI lookbacks), daily | 25-delta skew, option volume, open interest by call/put | contract tests pass; skew, volume and OI on ≥ 95% of optionable member lines per year (the `options/` manifest coverage) | `options/` ATM term structure |
| 4 | Official index constituents + weights (S&P 500, Russell 3000) | 2019+, daily | benchmark-relative risk, index-event studies | contract tests pass; weights sum to 1 per date | rule-based proxies (MKT S3.6) |
| 5 | GICS Direct history with publication dates | 2015+ | GICS itself (risk-model or mandate need) | contract tests pass; `structure_ok` 100% after the guard | SIC / NAICS / FF / text industries |
| 6 | Earnings-call transcripts with version creation times | 2019+ | call text features only | contract tests pass | none |

## 4. Running a real delivery (S8.3)

```bash
cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
# land files as served under data/raw/<source>/ with receipts.jsonl (history_mode per file), then:
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 -- \
    .venv/Scripts/python.exe -m atx_db.licensed load estimates --raw data/raw/ibes
.venv/Scripts/python.exe -m atx_db.licensed substitutes    # where the free stand-ins are and what they carry
.venv/Scripts/python.exe -m atx_db.licensed mock estimates --raw <scratch dir>   # a vendor-layout dry-run delivery
```

`load` materializes each normalized table as one pyarrow table (DuckDB itself is capped at 250 MB and spills to the
system temp directory); a multi-year detail history should be landed in yearly files and loaded year by year into
separate output directories, or the loader extended to stream by file, to stay inside a 0.6 GiB guard.
