# Fundamental signal improvement loop 80: quarterly gross-margin change

Status: **production data feature complete; rejected from alpha and shadow admission**.

## Research and feature contract

Abarbanell and Bushee's fundamental-analysis strategy includes changes in gross margin and reports
that accounting signals contain information about future returns
(https://www.jstor.org/stable/248340). Later work finds that gross-margin changes provide
incremental information distinct from revenue and earnings surprises
(https://ideas.repec.org/a/eme/rafpps/v14y2015i3p239-261.html).

Loop 80 first considered net payout and shareholder yield, but did not duplicate them: ATX already
has a governed strict net-payout feature whose sparse complete-case coverage was rejected in Loops
8 and 50. The remaining production gap was the direct gross-margin-change surface. The existing
revenue/margin feature is a binary gate on revenue growth and does not expose margin change as an
independent signal.

The new governed factor `profitability_quarterly_gross_margin_change_yoy` is:

`gross_margin_t - gross_margin_t-4`.

Higher same-quarter year-over-year expansion is preferred. It reuses exact current and prior-year
quarterly statements already linked in governed revenue-growth lineage, and therefore introduces
no looser period matching. Both statements must be visible at the monthly decision. Missing
components are not imputed; absolute margins and margin changes above 5 are rejected; each monthly
cross-section is winsorized at 1% and standardized only with at least 20 names. Full statement IDs,
periods, values, timestamps, formula, and preprocessing parameters are retained in lineage.

Migration 256 registers the factor and its dependencies on quarterly revenue growth and quarterly
operating profitability. The production refresh materialized 32,960 rows across 439 securities and
161 dates from 2013-04-30 through 2026-06-15.

## Data diagnostics

The signal direction was positive at every horizon, but none of the HAC statistics approached the
upstream evidence floor:

| Horizon | Mean rank IC | HAC t-stat | Dates | Mean names |
|---|---:|---:|---:|---:|
| 21 trading days | 0.0096 | 1.10 | 159 | 202.6 |
| 63 trading days | 0.0070 | 0.58 | 157 | 201.2 |
| 126 trading days | 0.0108 | 0.64 | 154 | 197.7 |
| 252 trading days | 0.0029 | 0.13 | 148 | 190.5 |

The feature was still passed to construction exploration so weak average IC would not prematurely
discard a diversifying or asymmetric portfolio surface.

## Preregistered construction search

Six constructions times 10%, 20%, and 30% router allocations were committed before returns were
loaded, increasing the durable trial count from 288 to 306. Selection ended 2020-12-31, January
2021 was embargoed, and only the frozen winner was evaluated beginning 2021-02-26. Losing variants
remained blind to validation.

Selection chose **continuous cross-sectional rank at 10% allocation**. The selection-period
standalone Sharpe was only 0.001; the router moved from 0.232 to 0.239, a +0.007 improvement.
Maximum estimated ADV participation was 7.16%. The small selected allocation correctly reflected
the weak early evidence.

## Untouched validation and decision

The untouched validation segment contains 63 monthly 21-trading-day observations through
2026-04-30.

| Validation result | Candidate | Router v6 | 90/10 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.735 | 0.712 | 0.786 | 0.700 |
| Annualized return | 2.43% | 2.42% | 2.71% | 2.40% |
| Maximum drawdown | -7.19% | -10.34% | -10.24% | -10.47% |
| Average turnover | 0.195 | 0.060 | 0.072 | 0.072 |
| Annualized cost drag | 0.39% | 0.29% | 0.30% | 0.60% |
| Maximum ADV participation | 7.85% | 9.21% | 9.05% | 9.05% |
| Probabilistic Sharpe | 94.58% | 93.61% | 95.19% | 93.23% |
| DSR probability (306 trials) | 12.32% | 11.87% | 15.53% | 11.38% |

Candidate/router correlation was 0.127, five of six folds were positive, doubled costs remained
profitable, the portfolio deployed fully, and combined Sharpe improved by +0.074. However, this was
a sharp reversal from the nearly flat selection result, included a -1.13 Sharpe 2022 fold, and
failed the multiple-testing-adjusted DSR gate. Standalone probabilistic Sharpe was 94.58%, just
below the 95% threshold required for a DSR-only failure to enter shadow.

Decision: **reject the selected gross-margin-change construction from production and shadow**. The
governed feature remains a production-queryable data product. The consumed validation interval may
not be reused to tune its construction; a future reconsideration requires new observations or a
materially distinct, preregistered economic hypothesis.

Decision artifact:
`atx-factor/research/loop80-quarterly-gross-margin-change-exploration.json`.
Evidence SHA-256:
`a8c29176897fdca2cca0022af78cda6678bbb3917e2b39c3880867b9e9aac291`.

## Verification

- Three focused feature/governance tests passed; no full suite was run.
- Ruff passed on all six new/touched Loop 80 Python paths.
- Applied migration checksums verify and the live warehouse is at migration 256.
- The factor panel has no duplicate IDs, non-finite values, availability violations, or missing
  lineage.
- The durable ledger contains 306 trials.
- The shadow registry remains unchanged with the Loop 69 NOA and Loop 79 abnormal-inventory-growth
  candidates; no production entry was created.
