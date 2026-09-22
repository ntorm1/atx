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

After the writer finishes and the relevant stages are activated, run this
query through the warehouse's production path. Confirm the CVX security/CIK
mapping, exact quarter boundaries, reported diluted EPS item, same-quarter
prior-year match, as-of revision selection, and source lineage. Return the
three rows above, with known availability for both EPS inputs, or identify
the missing source or transformation explicitly. A source benchmark alone
does not pass this acceptance case.
