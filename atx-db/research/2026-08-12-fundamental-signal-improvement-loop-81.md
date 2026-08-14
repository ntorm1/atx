# Fundamental signal improvement loop 81: abnormal receivables growth

Status: **production data feature complete; rejected from alpha and shadow admission**.

## Research and feature contract

Fundamental-analysis research treats receivables growing disproportionately to sales as a warning
about collections, sales quality, or aggressive revenue recognition. Receivables contain
incremental information about future sales, earnings, and margins
(https://doi.org/10.1177/0148558X9300800406), while the broader accounting-anomaly literature
emphasizes out-of-sample power, risk, and transaction-cost controls for admitting such signals
(https://doi.org/10.1016/j.jacceco.2010.09.008).

ATX already uses the receivables-to-sales index inside its governed Beneish M-score. That eight-input
complete-case factor covers only 156 securities, however, and does not expose disproportionate
receivables growth independently. Loop 81 added the broader quarterly factor
`quality_low_abnormal_receivables_growth`:

`(receivables_t / receivables_t-4 - 1) - (revenue_t / revenue_t-4 - 1)`.

Lower abnormal receivables growth is preferred. The implementation joins the latest receivables
facts visible at the exact current and same-quarter-prior-year period ends already governed by the
quarterly revenue-growth lineage. It requires positive current/prior revenue and receivables,
imputes no missing balance, rejects absolute receivables growth or abnormal growth above 10,
winsorizes at 1%, and standardizes only monthly cross-sections with at least 20 names. Source IDs,
values, periods, timestamps, formula, and preprocessing parameters are retained in lineage.

Migration 257 registers the factor and dependencies on `growth_quarterly_revenue_yoy` and the `ar`
metric. The production refresh materialized 23,451 rows across 327 securities and 159 dates from
2013-04-30 through 2026-06-15, more than doubling the security breadth of the Beneish surface.

## Data diagnostics

Rank ICs had the expected positive sign at all horizons but were economically weak and
statistically insignificant:

| Horizon | Mean rank IC | HAC t-stat | Dates | Mean names |
|---|---:|---:|---:|---:|
| 21 trading days | 0.0072 | 0.64 | 157 | 146.0 |
| 63 trading days | 0.0106 | 0.65 | 155 | 145.0 |
| 126 trading days | 0.0129 | 0.71 | 152 | 142.7 |
| 252 trading days | 0.0042 | 0.28 | 146 | 137.8 |

The feature still entered the complete portfolio-construction search so weak average IC would not
skip an asymmetric or diversifying implementation.

## Preregistered construction search

Six constructions times 10%, 20%, and 30% router allocations were committed before returns were
loaded, increasing the durable trial count from 306 to 324. Selection ended 2021-01-29, February
2021 was embargoed, and only the frozen winner was evaluated beginning 2021-03-31. Losing variants
remained blind to validation.

Selection chose **low-abnormal-growth top quintile versus the universe at 30% allocation**. Its
standalone selection Sharpe was 0.056, and the router moved from 0.140 to 0.155, a +0.015
improvement. Maximum estimated ADV participation was 6.95%.

## Untouched validation and decision

The untouched validation segment contains 62 monthly 21-trading-day observations through
2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | -0.178 | 0.641 | 0.551 | 0.438 |
| Annualized return | -0.81% | 2.17% | 1.60% | 1.26% |
| Maximum drawdown | -8.41% | -10.34% | -7.39% | -7.61% |
| Average turnover | 0.201 | 0.060 | 0.110 | 0.110 |
| Annualized cost drag | 0.41% | 0.30% | 0.33% | 0.67% |
| Maximum ADV participation | 7.60% | 9.21% | 7.50% | 7.50% |
| Probabilistic Sharpe | 34.31% | 91.25% | 88.40% | 83.10% |
| DSR probability (324 trials) | 0.04% | 8.35% | 5.36% | 3.08% |

Candidate/router correlation was -0.144, but negative correlation did not create useful
diversification: only three of six folds were positive and combined Sharpe fell by 0.090. The
candidate failed standalone Sharpe, DSR, and marginal-router-improvement gates.

Decision: **reject abnormal receivables growth from production alpha and shadow**. The governed
feature remains a production-queryable data product for manipulation diagnostics and downstream
research. The consumed validation interval cannot be used to retune this construction; a future
test requires a materially distinct preregistered hypothesis, such as explicit sales-regime or
industry conditioning with governed historical classifications.

Decision artifact:
`atx-factor/research/loop81-abnormal-receivables-growth-exploration.json`.
Evidence SHA-256:
`cb1ef0ef661299608268a25fd19eed0f35eac23a8b27ccea5b7e68e162fecc54`.

## Verification

- Two focused pure-transform tests passed; no full suite was run.
- The new migration and governance assertions passed a transactional dry run against the current
  warehouse, and rollback restored migration 256 with no residual factor definition.
- Ruff passed on all six new/touched Loop 81 Python paths.
- Applied migration checksums verify and the live warehouse is at migration 257.
- The factor panel has no duplicate IDs, non-finite values, availability violations, or missing
  lineage.
- The durable construction count is 324.
- The shadow registry remains unchanged with the Loop 69 NOA and Loop 79 abnormal-inventory-growth
  candidates; no production entry was created.
