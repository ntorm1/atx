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
membership), `effective_from <= d < effective_to`.

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
| `ticker_dated` | a ticker interval (canonical form: `.`, `/`, `-`, whitespace stripped, case-sensitive) contains `id_date` |
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

Substitute: `events/guidance.parquet` (lane EVT, S6.4): management guidance ranges from 8-K EX-99 releases for
consensus; the panel's time-series `sue` for the consensus surprise. No free substitute for detail, recommendations
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
announced before F) and applies to sessions `F <= d < effective_to`; without a publication date it is known at F
00:00 ET (`floor`). **Structure guard** against the classic GICS backfill (today's code applied to years before its
structure existed): a code used before its structure start has `effective_from` clipped to that start
(`structure_guard`, `vintage_risk`); a code used after its discontinuation fails `structure_ok` (reported). Seed
breaks (`GICS_STRUCTURE`, extend from the vendor's structure file): Real Estate sector 60 from 2016-09-01; Media &
Entertainment group 5020 from 2018-10-01; Transaction & Payment Processing 40201060 from 2023-03-20; Data Processing
& Outsourced Services 45102020 discontinued 2023-03-17.

The vendor price file (TickerHistory3) has a `GICS` column, but it is empty in every lake prices row 2018-2026
(0 non-null of 21.9M rows, measured 2026-09-29), so there is no free GICS history to load.

Substitute: `classification/` (lane MKT, S7.1): SIC -> NAICS (approximate) and Fama-French industries per CIK at the
filing clock; text industries (lane TXT, S7.2) for peer sets.

### 2.4 Index constituents and weights (`indexes.py`, stage `licensed_indexes/`)

Product: S&P DJI (500/400/600/1500) and FTSE Russell (1000/2000/3000) constituent history and end-of-day files.

| table | key | PIT rule (`index-pit-v1`) |
|---|---|---|
| `constituents` | index, CUSIP, ticker, effective_from, snapshot | a membership spell is known at its **announcement** (date + time ET; 23:59:59 ET without a time) and applies to `effective_from <= d < effective_to`; without an announcement it is known at the effective date's 09:30 ET open (`floor`). Spells of delisted names are kept with their end date |
| `weights` | index, CUSIP, ticker, date | the close file of D (snapshot D 16:00 ET) is visible at **D 23:59:59 ET** (evening delivery); weights normalized to fractions per (index, date) and checked to sum to 1 |

A provider history that drops dead names or re-keys them to today's identifiers surfaces as `unmapped` /
`cusip_undated` rows in the validation stats.

Substitute: `indexes/` (lane MKT, S3.6): rule-based Russell 1000/2000/3000 proxies, S&P 500 change list from public
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

## 3. Purchase decision points (D3, revisit before S8.3)

| # | product | history / cadence | unlocks | acceptance on real data | until then |
|---|---|---|---|---|---|
| 1 | I/B/E/S (or FactSet) Estimates: detail, summary, actuals, recommendations, targets | 2015+ (5-year revision lookback for the 2020+ score window), daily | consensus surprise, revisions, dispersion, recommendation changes: the largest missing alpha family | contract tests pass; `mapped_share` ≥ 0.98 on consensus; no fatal check | guidance (EVT S6.4), time-series SUE |
| 2 | Securities lending daily file (S&P Global / DataLend) | 2019+ (D7), daily | borrow fee, utilization, short-side constraints for the short book | contract tests pass; ≥ 95% of member_equity cells from 2020 | `borrow_proxy/` |
| 3 | Options EOD features + delta grid (SpiderRock, same securityID) | 2019+ (2014+ for 5-year volume/OI lookbacks), daily | 25-delta skew, option volume, open interest by call/put | see the Options section | `options/` ATM term structure |
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
