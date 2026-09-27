# Fundamental events artifact: output contract (`atx.fundamental-events/v1`)

Producer: `atx-engine/tools/build_fundamental_events.py` (mega-alpha T20). Consumer: the `fund` and `grp`
field groups of `atx-engine/tools/prepare_research_fields.py` (T21) and the PIT audit (T25).
Rulings: `v4-prereg.md` §R2. This file is the stable contract; any change is announced to the controller
before it is committed.

Values are **modeled / unaccepted** (not F.1-accepted), derived from the SEC CompanyFacts extraction CF-R
(archive snapshot 2026-09-20) and the FSDS v2 SUB acceptance clock. The CIK scope is the T19 rehearsal identity
bridge (`rehearsal_identity=true`); a CIK outside that list never appears.

## 1. Directory layout (publish order)

```
<out>/
  run.json                    written by `prepare`: input pins, CIK-list SHA, parameters, code hashes
  cik_scope.txt               pinned normalized copy of the CIK scope (one integer per line)
  clock.parquet               accession clock table (all SUB filers; FSDS SUB 2009q2..2024q4 only)
  sic_events.parquet          SIC event table (section 4)
  prepare.receipt.json
  events/batch-NNNN.parquet   per CF-R batch event rows (same schema as section 3)
  events/batch-NNNN.receipt.json
  fundamental_events.parquet  written by `finalize`: all batches, sorted (cik, accepted_utc, accession)
  manifest.json               written LAST by `finalize` (section 5); readers bind to it by SHA-256
```

A consumer reads only `manifest.json`, `fundamental_events.parquet` and `sic_events.parquet`, and verifies the
two parquet SHA-256s listed in the manifest. `manifest.json` absent means the artifact is incomplete.

## 2. Clock, seal and visibility

- **Event clock** `accepted_utc` of a row is the clock of its accession (filing):
  - FSDS SUB `accepted_utc` joined by accession number (`clock_basis = "fsds_accepted_utc"`);
  - else FC1 = `filed` 00:00 UTC + 46 h (`clock_basis = "cf_fc1"`).
- A fact enters the producer's knowledge at its accession's clock. Restatements are **latest-clock-wins**: a
  restated value enters at the restating filing's clock and is never backdated.
- **Seal:** no accession with clock >= 2025-01-01T00:00:00Z contributes anything; no FSDS SUB quarter after
  2024q4 is read; CF-R rows with `filed_date >= 2025-01-01` are dropped at the parquet read.
- **Consumer visibility rule (T21, declared by §R2):** a row is visible at session `d` iff
  `accepted_utc < d 22:00 UTC` (the role close mark), and it is usable from the next session
  (`--fund-lag-sessions 1`).

## 3. `fundamental_events.parquet` (one row per filing event)

One row per in-scope CIK and per accession of form 10-K, 10-K/A, 10-Q, 10-Q/A, 10-KT, 10-KT/A, 10-QT, 10-QT/A,
20-F, 20-F/A, 40-F, 40-F/A (us-gaap facts only) that contributed at least one retained fact, whose clock is in
[2014-06-01, 2025-01-01). Earlier accessions update the knowledge state but emit no row. Each row is a complete
snapshot of everything known about the CIK at that clock, **anchored at the fiscal period `period_end`**.

**Consumer selection rule (row-level, latest-clock-wins):** for a CIK and mark, take the latest visible row
(max `accepted_utc`, tie by `accession`). Every item is that row's value (NaN stays NaN; do not fall back to an
older row per item, so paired items always share one anchor). The whole row is stale, and every item NaN,
when `date(d) - period_end > staleness_days`.

| column | type | meaning |
|---|---|---|
| `cik` | int64 | SEC CIK |
| `accession` | string | accession number `##########-YY-######` |
| `accepted_utc` | timestamp[us, tz=UTC] | event clock (section 2) |
| `clock_basis` | string | `fsds_accepted_utc` or `cf_fc1` (FC1 fallback, labelled) |
| `filed` | date32 | SEC filing date |
| `form` | string | form of this accession |
| `report_period` | date32, nullable | this accession's own report period (FSDS `period` snapped to its nearest core fact end within 10 d; else its latest core fact end); null when it carries no core fact (core = total assets, stockholders' equity, net income, revenue, operating cash flow) |
| `period_end` | date32 | anchor A: max `report_period` over all accessions applied so far (monotone per CIK); every item is for the fiscal period ending at A |
| `fiscal_year` | int32, nullable | DEI fiscal year of the accession that set the anchor (FSDS `fy`, else CF `fiscal_year`) |
| `fiscal_period` | string, nullable | DEI fiscal period (`FY`, `Q1`..`Q4`) of that accession |
| `fiscal_year_end` | string, nullable | FSDS `fye` (`MMDD`) of that accession |
| `staleness_days` | int32 | 200 if the CIK has a 10-Q/10-QT (or /A) accession with clock in (clock - 400 d, clock]; else 400 (annual-only filer) |
| 28 item columns | float64 | section 3.1; NaN = not derivable at A (never null) |
| `zero_filled` | string | comma-separated, sorted item names whose value used a zero-filled component (section 3.2); `""` if none |
| `n_facts` | int32 | retained facts carried by this accession |
| `n_restated` | int32 | facts of this accession that replaced a different known value for the same metric, period and concept rank |

### 3.1 Items (all float64; money in USD as reported, shares in shares)

Notation: A = `period_end`; `bal(X, T, tol)` = instant metric X at the balance date nearest T within `tol` days;
`ttm(X, E)` = trailing-twelve-month value ending within 7 d of E (tolerance 20 d when E is a lag date);
`q(X, E)` = discrete fiscal quarter ending within 7 d of E (20 d at lag dates). Lag dates: `lag4` = A - 365 d,
`lag1q` = A - 91 d, `lag1q_lag4` = A - 456 d, `lag8` = A - 730 d; all +-20 d.

| item | definition |
|---|---|
| `be` | book equity at A: `stockholders_equity`, else `equity_incl_minority` - `minority_int_bs` (0 if absent); minus `pref_stock` (0 if absent) |
| `at` | `total_assets` at A |
| `at_lag4` | `at` at the balance date nearest `lag4` |
| `lt` | `total_liabilities` at A; else `at` - `equity_incl_minority`; else `at` - `stockholders_equity` (same date) |
| `che` | `cash_and_st_investments` at A; else `cash` + `st_investments` (0 if absent); else `cash_st_inv` |
| `debt` | `st_debt` + `lt_debt` at A, each 0 if absent provided `at` exists at A (zero-fill flagged); NaN if `at` absent |
| `sale_ttm` | `ttm(revenue, A)` |
| `gp_ttm` | `ttm(gross_profit, A)`; else `sale_ttm` - `ttm(cogs, A)` |
| `oi_ttm` | `ttm(operating_income, A)` |
| `ni_ttm` | `ttm(net_income, A)` |
| `ni_q` | `q(net_income, A)` |
| `ni_q_lag4` | `q(net_income, lag4)` |
| `be_lag1q` | `be` at the equity balance date nearest `lag1q` (opening equity of A's quarter; `roe_q = ni_q / be_lag1q`) |
| `be_lag1q_lag4` | `be` at the equity balance date nearest `lag1q_lag4` (for `droe`) |
| `cfo_ttm` | `ttm(operating_cash_flow, A)`; else `ttm(cfo_continuing, A)` |
| `capx_ttm` | `ttm(capital_expenditures, A)`; NaN when not reported |
| `xrd_ttm` | `ttm(rd_expense, A)`; NaN when not reported |
| `dvc_ttm` | `ttm(common_div_paid, A)`, else `ttm(dividends_paid, A)`; 0 (flagged) when neither is derivable but `cfo_ttm` is |
| `prstkc_ttm` | `ttm(share_repurchases, A)`; 0 (flagged) when not derivable but `cfo_ttm` is |
| `sstk_ttm` | `ttm(stock_issuance, A)`; 0 (flagged) when not derivable but `cfo_ttm` is |
| `txt_q` | `q(income_tax, A)` |
| `txt_q_lag4` | `q(income_tax, lag4)` |
| `shrs_q` | weighted-average diluted shares (else basic; same concept for the pair) for the direct fiscal-quarter fact ending at A, else the fiscal-year fact ending at A (non-additive: never differenced) |
| `shrs_q_lag4` | the same concept and duration class ending near `lag4`; the pair is dropped (both NaN) when `abs(log10(shrs_q/shrs_q_lag4)) >= 2` or either <= 0 (XBRL scale error) |
| `noa` | `at - che - lt + debt` at A (Hirshleifer-Hou-Teoh-Zhang 2004; operating assets minus operating liabilities) |
| `noa_lag4` | the same at the balance date nearest `lag4` |
| `sue` | seasonal-random-walk SUE of the quarter ending at A on **first-reported** quarterly `net_income` (split-invariant): `(NI_q - NI_q-4) / sd(previous <= 8 seasonal differences)`, >= 4 required, sd > 0 |
| `fscore` | Piotroski (2000) F-score 0..9 at A; all 9 signals required, else NaN (section 3.3) |

Canonical metrics are the `atx-db` statement map (`atx_db.statement_map_seed.default_statement_map_rows()`,
industry template `ALL`, active, non-derived, us-gaap), imported read-only and pinned by code hash. Within one
metric, the concepts are merged per (start, end) key by `concept_priority` (a better-ranked concept overrides;
the same rank is overridden by a later clock), except that the taxonomy total is ranked ahead of the seed's
component concepts (manifest `parameters.precedence_overrides`): `revenue` takes `Revenues` before ASC 606
`RevenueFromContractWithCustomer*` (so `sale_ttm` and the `gp_ttm` fallback use total revenue; contract revenue
remains the fallback), `cash` takes cash and cash equivalents before `Cash`, `st_debt` takes `DebtCurrent` before
its components. The seed's `value_multiplier` (a cash-flow presentation sign) is
not applied: payments (`capx`, `dvc`, `prstkc`) are positive as reported in XBRL.

**Period arithmetic.** Durations of 80-100 d are quarters, 350-380 d fiscal years (covers 52/53-week years).
`q` = the direct quarter fact, else the difference of two facts with the same start whose ends are 80-100 d apart
(6M - 3M, 9M - 6M, FY - 9M). `ttm` = (1) the direct fiscal-year fact; else (2) current YTD + prior FY - prior-year
YTD (same start as that FY, end within 20 d of YTD end - 365); else (3) four chained quarters.

### 3.2 Zero-fill (presence) rules

Only these; each use is listed in `zero_filled`: `debt` (and `noa`, `noa_lag4`, `fscore` when their debt
component was zero-filled), `dvc_ttm`, `prstkc_ttm`, `sstk_ttm` (and `fscore` via `sstk_ttm`). Everything else
is NaN when not reported.

### 3.3 F-score signals (1 each; Piotroski 2000)

With `roa(E) = ni_ttm(E) / at(E - 1y)`, `lev(E) = ltd(E) / ((at(E) + at(E - 1y)) / 2)` (`ltd` = `lt_debt`,
zero-filled when `at` exists), `cr(E) = current_assets(E) / current_liabilities(E)`, `gm(E) = gp_ttm(E) /
sale_ttm(E)`, `turn(E) = sale_ttm(E) / at(E - 1y)`: `roa(A) > 0`; `cfo_ttm(A) > 0`; `roa(A) > roa(lag4)`;
`cfo_ttm(A) > ni_ttm(A)`; `lev(A) <= lev(lag4)`; `cr(A) > cr(lag4)`; `sstk_ttm(A) <= 0`; `gm(A) > gm(lag4)`;
`turn(A) > turn(lag4)`. Needs balances at A, `lag4`, `lag8`; any missing input or non-positive denominator
gives NaN.

## 4. `sic_events.parquet`

One row per in-scope CIK and FSDS SUB accession (any form) with a parseable SIC (100..9999), accepted before the
seal. Sorted (cik, accepted_utc, accession).

| column | type | meaning |
|---|---|---|
| `cik` | int64 | SEC CIK |
| `accession` | string | accession number |
| `accepted_utc` | timestamp[us, tz=UTC] | FSDS `accepted_utc`, else FC1 (`filed` + 46 h) |
| `clock_basis` | string | `fsds_accepted_utc` or `cf_fc1` |
| `filed` | date32 | filing date |
| `form` | string | form |
| `sic` | int32 | SEC SIC code as of that filing |

Consumer rule (T21): latest row with `accepted_utc` < mark; stale (NaN group) when
`date(d) - date(accepted_utc) > 550`.

## 5. `manifest.json`

`schema = "atx.fundamental-events/v1"`, `status = "complete"`, `values_label = "modeled_unaccepted"`,
`rehearsal_identity = true`, `seal = "2025-01-01"`, `emit_from = "2014-06-01"`; `files` (name ->
`{sha256, bytes, rows}`) for `fundamental_events.parquet` and `sic_events.parquet`; `items` (ordered list of the
28 item names) and `item_units`; `inputs` (CF-R dir, manifest SHA, archive SHA and every batch parquet SHA; FSDS
dir, manifest SHA and every SUB quarter SHA read; CIK list path, SHA and count); `code_sha256` (this tool,
`export_fundamental_fields.py`, `atx_db/statement_map_seed.py`, `atx_db/seeds/statement_map.csv`) and
`concept_map_sha256`; `parameters` (tolerances, staleness rule, zero-fill rules); `counts` (rows, CIKs, FC1 rows,
restated facts, zero-fill uses per item, share pairs rejected, per-year rows and per-item finite fraction);
`caveats` (list of strings).
