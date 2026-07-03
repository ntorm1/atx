# ats-eqt — FINRA Off-Exchange Transparency Dataset

**Status:** Production, v1.0 (Phase 17a close)
**Audience:** ats-eqt engineering team (ingestion, storage, query, ops)
**Scope:** FINRA OTC Transparency program — weekly, monthly, and block-size
off-exchange (ATS dark-pool + non-ATS member-firm) trading volume per
security and per venue, across NMS Tier 1 / Tier 2 / OTC-equity tiers.
**Last updated:** 2026-05-23 (Phase 17a sprint close)

---

## 0. Executive summary

FINRA's OTC Transparency program makes the only public, free, structured
dataset of US off-exchange (dark-pool and member-firm) trading activity.
ats-eqt ingests four period types — weekly, monthly, and block-size summaries
— over the FINRA Query API and stores them as four bitemporal
`TableContract`s. A SQL convenience view (`offexchange_security_period_v`)
joins the security-period rollup to the internal sec master, enabling the
headline single-SQL-query:

```
ats-eqt offexchange ats-share --ticker AAPL --period weekly --week 2025-01-06
```

Output (deterministic from the bundled test fixture):

```
ticker            : AAPL
period_type       : weekly
summary_start_date: 2025-01-06
total_volume      : 66,925,813
ats_volume        : 21,246,912
non_ats_volume    : 45,678,901
ats_share_pct     : 31.7470%
```

---

## 1. Provenance

### FINRA OTC Transparency program

FINRA (the Financial Industry Regulatory Authority) operates the OTC
Transparency program under its mandate as a registered national securities
association and self-regulatory organisation for broker-dealers. The program
publishes weekly, monthly, and block-size aggregate trading volume statistics
for securities traded off US exchanges — the "dark pool + member firm" slice
of US equity market microstructure.

### Rule lineage

| Rule / Notice | Significance |
|---|---|
| **FINRA Rule 6110** | Transactions in equity securities — member-firm transaction-reporting obligations; basis for the reported volume data. |
| **FINRA Rule 6610** | Transaction reporting for OTC equity securities (non-NMS); extends reporting obligations to the OTC market. |
| **Regulatory Notice 15-48** (Dec 2015) | FINRA expanded OTC Transparency reporting to include ATS identity-level data (`weeklySummary` with per-ATS MPID), replacing the prior aggregated dark-pool aggregate. |
| **Regulatory Notice 16-14** (Apr 2016) | FINRA provided implementation guidance on the Tier 1 / Tier 2 / OTC-equity tiering and the initial publication-delay schedule. |
| **Regulatory Notice 19-29** (Sep 2019) | FINRA updated the publication-delay schedule: Tier-1 ≈ 14 calendar days; Tier-2 ≈ 28 days; OTCE ≈ 28 days. |

### FINRA Query API

- **Endpoint group:** `otcMarket`
- **Auth:** OAuth2 Public Credential (client-credentials flow); token endpoint
  `https://ews.fip.finra.org/fip/rest/ews/oauth2/access_token`.
- **Data endpoint:** `POST {base_url}/data/group/otcMarket/name/{datasetName}`
  with a JSON filter body carrying the date-range parameters.
- **Dataset names:**
  - `weeklySummary` — weekly period aggregates (ATS + non-ATS per symbol per MPID/firm)
  - `monthlySummary` — monthly period aggregates
  - `blockSummary` — block-size breakdowns (≥ 10,000 shares or $200,000)
  - `weeklySummaryHistoric` — bulk historic weekly data (operator backfill only)
- **Download cap:** 10 GB / month (free-tier). ats-eqt tracks cumulative
  bytes per run and emits a structured warning when the run crosses 80% of
  the monthly cap. See `FinraSettings.monthly_download_cap_bytes`.

---

## 2. The four contracts + the `_v` view

### `offexchange_volume`

The per-`(security, venue, period)` fact table. One row per (security /
period / venue / summary-type) combination as published by FINRA. Columns
include `volume_id` (synthetic PK), `security_id` (FK to `reference_security`
after symbol resolution), `mpid`, `venue_class` (`"ATS"` or `"non_ATS"`),
`total_share_quantity`, `total_trade_count`, `summary_type_code`,
`period_type`, `summary_start_date`, and the four bitemporal columns
(`knowledge_from`, `knowledge_to`, `valid_from`, `valid_to`).

```sql
-- All ATS dark-pool volume for AAPL in the week of 2025-01-06
SELECT mpid, total_share_quantity
FROM   offexchange_volume
WHERE  summary_start_date = '2025-01-06'
  AND  period_type        = 'weekly'
  AND  venue_class        = 'ATS'
  AND  security_id        = (
           SELECT security_id FROM reference_security
           WHERE  ticker = 'AAPL' LIMIT 1
       )
  AND  knowledge_to       = '9999-12-31 23:59:59.999999'
ORDER BY total_share_quantity DESC;
```

### `offexchange_venue`

The venue dimension table. One row per MPID (ATS MPID or non-ATS CRD/firm
identifier), carrying `mpid`, `venue_name`, `venue_class`, and the ATS/firm
identity metadata. Upserting venues is idempotent (keyed on `mpid`).

```sql
-- All registered ATS venues
SELECT mpid, venue_name
FROM   offexchange_venue
WHERE  venue_class = 'ATS'
ORDER BY mpid;
```

### `offexchange_security_period`

The derived per-`(security, period)` rollup. One row per (security / period)
combination, carrying `ats_share_quantity`, `non_ats_share_quantity`,
`total_share_quantity`, `ats_share_pct` (as `Decimal(38, 6)`),
`restatement_detected` (bool), and `rows_metadata_amended` (int). Produced
by `SecurityPeriodMaterializer.materialize`; not ingested directly from FINRA.

```sql
-- ATS market-share for all securities in the week of 2025-01-06
SELECT security_id, ats_share_quantity, non_ats_share_quantity, ats_share_pct
FROM   offexchange_security_period
WHERE  summary_start_date = '2025-01-06'
  AND  period_type        = 'weekly'
  AND  knowledge_to       = '9999-12-31 23:59:59.999999'
ORDER BY ats_share_pct DESC;
```

### `offexchange_quality_report`

The per-run QA scorecard. One row per ingest run, carrying `rows_total`,
`rows_new`, `rows_amended`, `rows_metadata_amended`, `rows_skipped`,
`symbol_resolution_rate` (`Decimal(38, 6)`), `venue_upserts`,
`restatement_detected` (bool), and the run's knowledge timestamp.

```sql
-- Latest quality reports ordered by run time
SELECT summary_start_date, rows_total, symbol_resolution_rate, restatement_detected
FROM   offexchange_quality_report
WHERE  knowledge_to = '9999-12-31 23:59:59.999999'
ORDER BY knowledge_from DESC
LIMIT  10;
```

### `offexchange_security_period_v` (SQL view)

A `SqlView` (not a `TableContract`) that LEFT JOINs `offexchange_security_period`
to `reference_security` on `security_id`, projecting the ticker and security
name alongside the rollup columns. This is the "single-SQL-query" surface: one
statement resolves ticker → security_id and returns the ATS-share headline.

```sql
-- Single SQL: ATS-share headline for AAPL, week of 2025-01-06
SELECT ticker, ats_share_quantity, non_ats_share_quantity,
       total_share_quantity, ats_share_pct
FROM   offexchange_security_period_v
WHERE  ticker             = 'AAPL'
  AND  summary_start_date = '2025-01-06'
  AND  period_type        = 'weekly'
  AND  knowledge_to       = '9999-12-31 23:59:59.999999';
```

---

## 3. The ATS / non-ATS taxonomy + `summaryTypeCode` mapping

FINRA's OTC Transparency program distinguishes two venue classes:

| `summaryTypeCode` (FINRA API) | `venue_class` (ats-eqt) | Description |
|---|---|---|
| `ATS_W_SMBL` | `ATS` | Alternative Trading System — a registered SEC dark pool with an assigned MPID. Identity is the ATS's unique MPID (e.g., `UBSA` = UBS ATS, `CROS` = Citadel Connect). |
| `OTC_W_SMBL` | `non_ATS` | Non-ATS member firm — a FINRA-registered broker-dealer that internalises or crosses orders off-exchange but is not a registered ATS. Identity is the firm's CRD number (converted to an MPID-like string) rather than an ATS MPID. |

**MPID vs. firm-CRD identity distinction.** For ATS rows, `mpid` is the
four-character ATS identifier assigned by FINRA/DTCC. For non-ATS rows,
`mpid` in the raw FINRA data is typically a firm abbreviation derived from
the firm's CRD number rather than a formally registered MPID. The
`offexchange_venue` table stores both; the `venue_class` column is the
authoritative discriminator.

---

## 4. Publication-delay model

FINRA publishes off-exchange data on a tiered delay schedule set by
Regulatory Notice 19-29:

| Tier | `FinraSettings` field | Typical delay |
|---|---|---|
| NMS Tier 1 (large-cap) | `tier1_delay_days` (default: 14) | ≈ 14 calendar days after the reporting week/month ends |
| NMS Tier 2 (mid-cap) | `tier2_delay_days` (default: 28) | ≈ 28 calendar days |
| OTC Equity (OTCE) | `otce_delay_days` (default: 28) | ≈ 28 calendar days |

ats-eqt's live smoke test (`test_offexchange_live_finra.py`) computes the
most-recent published period as `today - tier1_delay_days`, snapped to the
Monday of that calendar week. The ingest CLI and JobKinds accept an explicit
`--week` / `--month` argument; the live test uses the auto-computed period.

---

## 5. Restatement semantics

FINRA occasionally republishes a previously-released period with corrections
(volume adjustments, late-reporting corrections, ATS restatements). ats-eqt
represents republications via the bitemporal amend chain:

- **First ingest** of a period: all rows are `NEW`; `knowledge_to = OPEN_END`.
- **Restatement re-ingest** (payload changed): the prior open row is closed
  (`knowledge_to` set to the restatement `as_of`); a new row is inserted
  with the corrected payload. The `offexchange_security_period` row sets
  `restatement_detected = true`.
- **Metadata-only AMEND** (P17a-6 addition): when FINRA advances
  `lastUpdateDate` on a row but the payload (share quantities, trade count,
  venue identity) is unchanged, the pipeline classifies the row as
  `METADATA_AMEND` rather than `SKIP` or `AMEND`. No new volume row is
  written; only the `finra_last_update_date` column is updated in-place.
  This prevents amend storms when FINRA touches hundreds of rows' metadata
  without changing the underlying data.
- The `rows_metadata_amended` counter in `offexchange_quality_report` tracks
  the count of metadata-only amends per run. A run with `rows_metadata_amended > 0`
  and `rows_amended == 0` indicates FINRA metadata-touched but data-stable
  rows — benign and expected.

**Metadata-only AMEND vs. real restatement:** a real restatement changes
`total_share_quantity`, `total_trade_count`, or `venue_class`. A
metadata-only AMEND changes only `lastUpdateDate` (and possibly
`finra_last_update_date`). The `rows_metadata_amended` counter lets
operators distinguish the two in the quality report.

---

## 6. The single-SQL headline-query example

The following reproduces the deterministic AAPL result from the bundled
FINRA weekly fixture (Phase 17a-5 acceptance values):

```
$ ats-eqt offexchange ats-share --ticker AAPL --period weekly --week 2025-01-06

ticker            : AAPL
period_type       : weekly
summary_start_date: 2025-01-06
total_volume      : 66,925,813
ats_volume        : 21,246,912
non_ats_volume    : 45,678,901
ats_share_pct     : 31.7470%
```

ATS breakdown: UBSA (UBS ATS) = 12,345,678 shares; CROS (Citadel Connect) =
8,901,234 shares. Non-ATS: one member firm = 45,678,901 shares. This result
comes from the bundled fixture (`tests/fixtures/offexchange/weekly_2025-01-06.json`)
and is asserted deterministically by `test_offexchange_e2e_sqlview.py`.

---

## 7. Ingest cadence + the JobKind invocation

Phase 17a ships five `offexchange.*` JobKinds on the Phase 12 scheduler
substrate:

| `JobKind.name` | Schedule | Purpose |
|---|---|---|
| `offexchange.weekly.ingest` | Every Monday 08:00 | Ingest the most-recent published `weeklySummary` period |
| `offexchange.weekly.materialize` | Every Monday 09:00 | Materialize `offexchange_security_period` for the latest weekly ingested period |
| `offexchange.weekly.quality` | Every Monday 09:30 | Write the QA scorecard for the latest weekly run |
| `offexchange.monthly.ingest` | 1st of each month 08:00 | Ingest the most-recent published `monthlySummary` period |
| `offexchange.monthly.materialize` | 1st of each month 09:00 | Materialize for the latest monthly ingested period |

All five are registered via the Phase 12 `_builtin.py` populate-on-import
pattern and are available to the job scheduler immediately on startup.

The CLI equivalents (manual invocation):

```bash
ats-eqt offexchange ingest --period weekly --week 2025-01-06
ats-eqt offexchange materialize --period weekly --week 2025-01-06
ats-eqt offexchange quality --period weekly --week 2025-01-06
ats-eqt offexchange status
ats-eqt offexchange ats-share --ticker AAPL --period weekly --week 2025-01-06
```

---

## 8. What we DON'T ingest

**FINRA short-interest / short-volume / Reg SHO threshold data** are
explicitly out of scope for this module. Per ADR 0015, those datasets
are assigned to the planned pricing module. See
[`pricing_market_data.md`](pricing_market_data.md) for the pricing-module
ownership of Reg SHO / short-interest data.

The off-exchange transparency program covers aggregate *volume* data only —
not trade-by-trade data, not quote data, not consolidated-tape data. The
FINRA CAT (Consolidated Audit Trail) and the NYSE/NASDAQ consolidated tapes
are separate data sources, not ingested by ats-eqt.

---

## 9. Operator instructions for the full historical backfill

The `OffExchangeBackfillRunner` (P17a-3) ships a `WeeklySummaryHistoric`
runner capable of fetching the full ~2013-present FINRA weekly history. This
is an operator-only run, not executed in the sprint or CI.

```bash
ats-eqt offexchange backfill --start 2013-01-01 --end <today> --period-type weekly
```

The historic dataset (`weeklySummaryHistoric`) covers weekly periods from
approximately January 2014. The runner writes each fetched period through
the normal ingest pipeline (idempotent; re-runs produce only SKIPs for
already-written rows). At 10 GB/month FINRA API cap, the full backfill may
need to be staged across multiple months. Monitor
`offexchange_quality_report.symbol_resolution_rate` per period to verify
coverage. The `OffExchangeBackfillRunner` accepts a `--dry-run` flag that
fetches and parses but does not write to storage.
