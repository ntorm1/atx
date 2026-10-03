# Announcement actuals and seasonal surprises

Stage `ea_actuals`, schema `atx.alpha-panel.ea-actuals/v1`. This initial publication
is experimental until the binding 200-release manual precision audit passes.
All failed gates remain in its manifest; extraction qualification alone is not
independent financial correctness certification.

`releases.parquet` and `ann_features.parquet` have one original 8-K item 2.02 row
per `(cik, accession)`, including releases with no readable/qualified source.
`releases_manifest.json` pins the parse publication independently of later feature
and validation manifests; `earnings_calendar_v2` consumes that stable publication.

| Column | Type / unit | Definition and source | Clock / staleness |
|---|---|---|---|
| cik, accession | BIGINT, VARCHAR | SEC issuer and original filing identity | filing |
| filing_date | DATE | EDGAR filing date | filing |
| available_at | UTC TIMESTAMP | Resolved EDGAR acceptance; unknown or ambiguous clocks are NULL | strict daily cutoff |
| source_available_at, acceptance_clock, clock_reason | TIMESTAMP, VARCHAR | Original source clock and qualification reason | lineage only; unresolved source clock never promotes a feature |
| period_end, period_basis, period_days | DATE, VARCHAR, INTEGER | Release's own explicit current period, without later filing labels; exact day duration is unknown unless disclosed | acceptance |
| saleq | DOUBLE, source currency | Explicit GAAP quarterly sales/revenues table cell, monetary scale applied | acceptance; 400-day consumer limit |
| niq | DOUBLE, source currency | Reported quarterly net income/loss table cell; parent/consolidated basis needs independent validation | acceptance; 400-day consumer limit |
| epsfiq | DOUBLE, currency per reported diluted share | Total diluted EPS, including discontinued operations; does **not** mean Compustat EPSFX continuing-operations EPS | acceptance; no ADR/share conversion invented |
| currency | VARCHAR | Explicit declaration in/near the table; an unqualified `$` symbol cannot supply USD | acceptance |
| *_seasonal_comparative | DOUBLE, corresponding source unit | Same-release prior-year quarterly comparison | acceptance |
| source_doc, doc_sha256, evidence | VARCHAR[] | Source filenames, verified content hashes and exact table/row/column/cell/scale evidence | lineage |
| reason | VARCHAR | Missing/ambiguous period, monetary units/currency, table duration, conflicting documents/rows or missing objects | acceptance |
| saleq_surprise, epsfiq_surprise | DOUBLE | Actual minus seasonal random walk, from visible comparable inputs | acceptance |
| sue_rev_ann, sue_eps_ann | DOUBLE, dimensionless | Surprise divided by sample standard deviation of the eight prior visible quarterly surprises; current surprise excluded | acceptance, 400 days |
| *_reason | VARCHAR | Missing seasonal history, fewer than eight surprises, zero scale or unqualified EPS share basis | acceptance |

Revenue SUE requires same currency and issuer. Every period uses the latest
visible vintage at that row's cutoff. A same-release EPS comparative does not
establish the share basis of eight older surprises: EPS SUE remains missing until
a source-backed historical split/share bridge qualifies that entire denominator.
No implicit zeros, periodic-report actuals, annual/YTD-to-quarter differences or
monetary/currency guesses are admitted by the first parser.

`filing_lags.parquet` is separate: `(cik, accession)` plus `filing_accession`,
`announcement_available_at`, `available_at` and `ea_lag_days`. The lag runs from
the announcement ET date to the first matching original 10-Q/10-QT ET date.
Its feature clock is that later report's resolved acceptance.

`reaction_returns.parquet` has `(cik, accession, security_id)`, reaction session,
window end, `ea_ret_3d`, availability and reason. It compounds the three reaction
session total returns, using identity on reaction session. All price source
partitions and rows end before 2023 before arithmetic. The full window must end
before 2023. Later/incomplete/guarded windows remain NULL. Its conservative
availability is the third session at 22:00 UTC, never the original release clock.

Daily visibility requires availability strictly before the previous session at
22:00 UTC. Labels remain physically separate. Final audit covers member_equity,
univ_all_common and univ_r3000p cells per 2020-2025 year and rejects stale or
incomplete producer hashes. It also remeasures Task3's unchanged first-reported
10-Q denominator against the new source-backed 8-K calendar, after the final
fundamentals_v11/compustat publication.

Build order, under the shared <=1 GiB guard and controller's data slot:

1. `python -m atx_db.alpha_panel.ea_actuals --phase parse`
2. `python -m atx_db.alpha_panel.earnings_calendar_v2 --phase parse`
3. `python -m atx_db.alpha_panel.earnings_calendar_v2 --phase build`
4. `python -m atx_db.alpha_panel.ea_actuals --phase build`
5. `python -m atx_db.alpha_panel.ea_actuals --phase audit`

The 200-release sample freezes lowest SHA256 of
`cik/accession/t9-precision-200-v1` over all original 2.02 releases 2020-2025,
independent of extraction success. It retains missing cases and source hashes.
Manual precision remains FAIL until all 200 rows are independently examined
against their original releases and later 10-Qs within 1%. Initial bounded source
probe: 20 cached original 8-K documents selected independently by accession hash,
0 formally qualified tables. This is a parser/source-qualification diagnostic,
not the gate sample or a source-absence estimate. No real stage build or final
coverage measurement has completed at this checkpoint.
