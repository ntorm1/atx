# Production acceptance case: CVX quarterly EPS growth

User question: "what was CVX YoY EPS growth for the past 3 quarters?"

Snapshot: 2026-09-20. Definition: reported GAAP diluted earnings per share in
USD/share for each discrete fiscal quarter. Growth is the current quarter's
reported EPS divided by the same quarter one year earlier, minus one. All
three prior-year denominators below are positive. Display growth as a percent;
do not substitute TTM growth or management-adjusted earnings.

## Independently verified source benchmark

| Quarter end | Quarter | Reported diluted EPS | Same quarter prior year | YoY growth |
| --- | --- | ---: | ---: | ---: |
| 2026-06-30 | Q2 2026 | 6.11 | 1.45 | 321.3793103448% |
| 2026-03-31 | Q1 2026 | 1.11 | 2.00 | -44.5000000000% |
| 2025-12-31 | Q4 2025 | 1.39 | 1.84 | -24.4565217391% |

Sources, inspected 2026-09-21:

- [Chevron Q2 2026 release, published 2026-07-31](https://chevroncorp.gcs-web.com/node/38551)
- [Chevron Q1 2026 release, published 2026-05-01](https://chevroncorp.gcs-web.com/news-releases/news-release-details/chevron-reports-first-quarter-2026-results)
- [Chevron Q4 2025 release, published 2026-01-30](https://chevroncorp.gcs-web.com/news-releases/news-release-details/chevron-reports-fourth-quarter-2025-results)
- [Chevron Q2 2026 Form 10-Q](https://www.sec.gov/Archives/edgar/data/93410/000009341026000167/cvx-20260630.htm)

The earnings releases explicitly separate reported diluted EPS from adjusted
diluted EPS. The source EPS values above are reported to two decimal places;
the growth benchmark is calculated from those reported values.

## A real quarter-construction trap

On 2026-09-22, the repaired source parser was executed against the locally
frozen official Q4 2025 SEC exhibit. It selected reported diluted EPS1.39,
prior-year-quarter EPS1.84 and period end2025-12-31 from the statement table;
it separately identified fiscal2025Q4. The filing's event date is2026-01-30.
This is a source-extraction check, not a populated production growth result.
The run used no warehouse writes and peaked at0.571GiB under the2GiB guard.

The [actual filing index](https://www.sec.gov/Archives/edgar/data/93410/000009341026000019/0000093410-26-000019-index.html)
labels `a12312025ex9918-k.htm` as EX-99.1. The directory JSON instead labels
files with values such as `text.gif`; it does not supply SEC exhibit types.
Generic source discovery must use the filing's typed document table rather
than infer an exhibit type from that directory field or a filename prefix.

Chevron's FY2025 diluted EPS is 6.63 and its nine-month 2025 diluted EPS is
5.27. Their difference is 1.36, while reported Q4 EPS is 1.39. The nine-month
figure is in the [Q3 2025 release](https://chevroncorp.gcs-web.com/node/37451).
Quarterly EPS must not be fabricated by subtracting cumulative EPS: share
denominators can differ. Reported quarter EPS, an explicitly labeled
reconstruction, and unavailable evidence must remain distinguishable.

## Measured warehouse gaps

After the memory guard stopped the archive loader, root inspected the live
warehouse read-only at 2026-09-21T23:58:15Z, with a second identity inspection
at 2026-09-22T00:00:30Z. Each used DuckDB 1GB/one thread under a 2GiB process
guard, sequentially; measured process-tree peaks were 0.455GiB and 0.283GiB.

- The four reported EPS inputs for Q1/Q2 2026 and their year-earlier quarters
  are present in `sec_company_facts` and match the benchmark. This is enough
  raw evidence to calculate those two growth values.
- Neither requested Q4 date has a direct 70-115-day `EarningsPerShareDiluted`
  fact in USD/share. FY2025 EPS 6.63 and nine-month EPS 5.27 are present, but
  their difference is not reported Q4 EPS. The existing monetary-only
  quarter-subtraction paths correctly exclude per-share items. Reported Q4
  source coverage must be supplied to pass this case; a fabricated residual
  is not a repair.
- `fundamental_statement_points`, `fundamental_standardized`, and
  `derived_metric_values` each have zero rows across the entire warehouse.
  Their activation remains outstanding, so no production metric query has
  passed this case. The existing quarterly metric code is
  `eps_diluted_q_growth_yoy`; `eps_diluted_growth_yoy` is the TTM variant.
- CVX raw facts carry owner
  `SEC-COMPANYFACTS-UNRESOLVED-CIK-0000093410`. Its 3,621 price rows, through
  2026-09-18, carry `SEC-CIK-0000093410`. The observed CIK identity history
  begins 2026-09-20 and does not establish fact-time linkage for earlier
  filings. Archive ingestion intentionally disallows current-ticker fallback;
  no later activation stage automatically repairs this linkage. Current
  identity evidence must not be backdated to manufacture a historical join.

## Executed SQL acceptance result

At 2026-09-22T00:18:09.931497Z, the read-only
[acceptance SQL](../../.superpowers/sdd/tier1-parity/quarterly-eps-acceptance.sql)
returned these three rows at the explicit 2026-09-20 22:00 UTC cutoff:

| Quarter | Raw current EPS | Raw prior EPS | Raw computed YoY | Stored quarterly growth | Diagnosis |
| --- | ---: | ---: | ---: | --- | --- |
| Q2 2026 | 6.11 | 1.45 | 321.3793103448% | NULL | Materialization missing |
| Q1 2026 | 1.11 | 2.00 | -44.5000000000% | NULL | Materialization missing |
| Q4 2025 | NULL | NULL | NULL | NULL | Both direct quarterly inputs missing |

The [machine-readable result](../../.superpowers/sdd/tier1-parity/quarterly-eps-acceptance-result.json)
retains accession, owner, period, stored and effective availability, revision,
and publication diagnostics. SQL SHA-256:
`fe886f41543f7bf42261dec61096a6935f2c608ee7e447e1ca6379b90c7af262`.
Execution used DuckDB 1GB/one thread inside the 2GiB process guard, with
measured native process-tree peak 0.466GiB. This is a diagnostic query over
current tables, not a replacement publisher or a passed production case.

Review found that the original query's publication-status label depended on
raw Company Facts inputs. That does not change the measured missing outputs
above, but would mislabel a future quarter supplied by an earnings exhibit.
The [versioned successor SQL](../../.superpowers/sdd/tier1-parity/quarterly-eps-acceptance-v2.sql)
separates publication status from the raw comparison. It is prepared for the
next run and has not yet executed; the original SQL and result remain frozen.

The general repairs are to activate existing materializers, ingest direct
reported-quarter EPS through a deterministic earnings-exhibit source, and
serve issuer accounting content with separate ticker-lookup and content
availability clocks. A CIK query must discover actual source owners; it must
not assume every filing uses the unresolved-owner namespace. Current ticker
lookup can select an issuer's history without certifying historical market
security association.

These are source and warehouse findings, not a completed production result.
The independent source benchmark remains the required output. Operator
evidence is in `companyfacts-archive7-stop-inspection.json`,
`cvx-identity-live-evidence.json`, and `cvx-eps-production-query-audit.md`
under `.superpowers/sdd/tier1-parity/` at the repository root.

## Filing event date is not the earnings period

Source verification on 2026-09-22 found another generic ingestion requirement.
The [SEC filing index](https://www.sec.gov/Archives/edgar/data/93410/000009341026000019/0000093410-26-000019-index.htm)
records the 8-K's filing date and period of report as **2026-01-30**. Its
[earnings exhibit](https://www.sec.gov/Archives/edgar/data/93410/000009341026000019/a12312025ex9918-k.htm)
reports **fourth-quarter 2025**, with the statement quarter ending
**2025-12-31** and reported diluted EPS1.39 (prior-year quarter1.84).

The source draft incorrectly used the submission's `report_date` as the
accounting `period_end`. That would reject the real exhibit even if a synthetic
fixture supplied December31 as its 8-K report date. The repair must identify
the accounting period from the exhibit's labeled fiscal quarter and explicit
statement header, retaining the filing/event date separately for provenance
and availability. Exact 13/14-week boundaries likewise need document evidence;
a filing event date or calendar-quarter guess is not a substitute.

This is a verified source-semantic defect in an isolated draft, not a live
warehouse repair or a newly measured EPS result. The focused integration
fixture must use the actual event/quarter-date distinction before activation.

After the writer finishes and the relevant stages are activated, run this
query through the warehouse's production path. Confirm the CVX security/CIK
mapping, exact quarter boundaries, reported diluted EPS item, same-quarter
prior-year match, as-of revision selection, and source lineage. Return the
three rows above, with known availability for both EPS inputs, or identify
the missing source or transformation explicitly. A source benchmark alone
does not pass this acceptance case.

## Current executable consumer: selected-lineage qualification

Use the generic [quarterly EPS desk reader](../scripts/read_quarterly_eps_growth.py)
for canonical output. It delegates numerical publication qualification to
`WarehouseReadService.issuer_content_range` (IQ2). The frozen v1/v2 SQL and
receipts above remain historical diagnostics; their coarse owner checks do
not replace exact selected-leaf qualification.

From `atx-db`, under the existing memory guard and after the controller grants
the single runtime slot:

```powershell
.\.venv\Scripts\python.exe scripts/read_quarterly_eps_growth.py `
  --db-path data/warehouse.duckdb --cik 0000093410 `
  --content-as-of 2026-09-20T22:00:00Z `
  --start 2025-10-01 --end 2026-07-01 --latest 3 `
  --output-json data/cvx-quarterly-eps-desk.json
```

Choose a fresh output filename. The reader opens one read-only transaction
with 256MB memory, one thread, bounded spill, and row/byte preflights. The
explicit period-end range is `[start,end)`. `--latest` defaults to 3 observed
qualified periods and includes NULL unavailable revisions, so a subsequent
invalidation never resurrects an older numeric state. It does not infer an
absent fiscal quarter or calculate prior-year dates itself. Growth ratios
and domain statuses come from the canonical publisher; `growth_percent`
only changes the display unit. Canonical growth uses
`(current - prior) / abs(prior)`: positive bases match `current / prior - 1`,
negative bases retain the signed change relative to the base's magnitude,
and zero/missing bases retain NULL and their original `value_status`.

Schema 323's selected-input columns and prerequisite tables must exist.
Missing schema, missing materialization, insufficient periods, ambiguous
owners, rejected selected lineage, and unavailable values produce explicit
JSON diagnoses and exit 2. Exit 0 means the requested count of unambiguous
numeric observations was returned, not that a production release passed.
Inspect `issuer_diagnostics`, including all rejection and truncation flags.

This reader requires an explicit CIK and performs no ticker lookup. Resolve
the ticker separately at an explicit `issuer_lookup_as_of` using the
[issuer-content service](ISSUER_CONTENT_QUERY.md); do not substitute that
clock for `--content-as-of`. Issuer accounting history is not historical
security association, and reconstructed event availability is not verified
historical-vintage data. This consumer does not repair source gaps or
materialize missing data.

The root-controlled live invocation on 2026-09-24 wrote
[`cvx-eps-desk1.json`](../../.superpowers/sdd/tier1-parity/cvx-eps-desk1.json)
with `schema_prerequisite_missing` and exit 2: the warehouse still lacked
`derived_metric_values.selected_input_refs_hash` and
`derived_metric_values.selected_input_refs_json`. It returned no numeric
rows and stopped at schema preflight. The process peaked at 0.642921GiB
under a 1GiB guard; the warehouse size and modification time were unchanged.
This verifies controlled prerequisite reporting, not the numerical CVX
acceptance case. The tiny real-IQ2 fixtures and corrected zero/negative-base
cases passed; they do not establish production source coverage.
