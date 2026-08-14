# Fundamental signal improvement loop 82: earnings acceleration and sleeve-capacity correction

Status: **production data feature complete; rejected from alpha and shadow admission**.

## Research and feature contract

He and Narayanamoorthy define earnings acceleration as the quarter-over-quarter change in
seasonally differenced earnings growth. They report that a high-minus-low acceleration strategy
predicts one-month and quarter-long returns, remains distinct from PEAD, profitability, accruals,
momentum, and investment anomalies, and is consistent with investors missing implications for
earnings two and three quarters ahead
(https://doi.org/10.1016/j.jacceco.2019.101238). Their primary price-deflated construction is:

`((EPS_t - EPS_t-4) / price_t-1) - ((EPS_t-1 - EPS_t-5) / price_t-2)`.

Loop 82 added `earnings_quarterly_acceleration` using first-filed diluted quarterly EPS. Each EPS
and price is normalized by the cumulative split index so intervening stock splits do not create
false acceleration. Quarterly sequences must satisfy 60–130-day adjacent and 350–380-day seasonal
gaps; all prior EPS and price inputs must have been visible when quarter t was filed. The latest
signal no older than 150 days is attached to the governed monthly SUE decision grid, but the SUE
score itself is not an acceleration input. Cross-sections are winsorized at 1% and standardized
only with at least 20 names. Full first-filed statement, split, price, availability, and decision
lineage is retained.

Migration 258 registers the factor and dependencies on the governed SUE decision factor,
first-filed diluted EPS, and market close. The production refresh materialized 52,370 rows across
881 securities and 160 dates from 2013-07-31 through 2026-06-15.

## Data diagnostics

The characteristic was strongest near the paper's quarter-long window but faded thereafter:

| Horizon | Mean rank IC | HAC t-stat | Conventional t-stat | Dates | Mean names |
|---|---:|---:|---:|---:|---:|
| 21 trading days | 0.0106 | 1.43 | 1.35 | 156 | 302.8 |
| 63 trading days | 0.0163 | 1.52 | 2.03 | 154 | 304.3 |
| 126 trading days | 0.0032 | 0.32 | 0.40 | 151 | 306.3 |
| 252 trading days | -0.0060 | -0.49 | -0.74 | 145 | 311.0 |

The 63-day label remains an IC-decay diagnostic. `atx-factor` uses monthly formation dates, so
monthly 63-day returns overlap and cannot be treated as independent investable periods. Loop 82
hardened the CLI to reject `explore-candidate --horizon-days 63` during argument parsing, before a
trial can be committed. The legitimate portfolio test used non-overlapping 21-trading-day returns,
consistent with the paper's documented one-month strategy.

## Allocation-capacity defect and correction

The first 18-trial run revealed that the candidate's standalone book severely underdeployed while
the blend remained fully invested. Tracing the weight path found that every candidate construction
was capacity-clipped as if it managed the full $50 million, even when its candidate allocation was
only 10%, 20%, or 30%. This distorted candidate weights, allocation comparisons, and deployment
gates.

The explorer now uses capacity model `allocation_scaled_aum_v1`:

- a candidate tested at allocation `a` is constructed and costed at `a * total AUM`;
- the blended portfolio is normalized and capacity-tested at total AUM;
- the capacity-model version is part of the durable preregistration specification;
- a candidate that cannot deploy its own sleeve budget remains ineligible even when the baseline
  can absorb unused capital.

A focused regression verifies all three candidate allocations use $5M/$10M/$15M against a $50M
portfolio. The invalid horizon guard and allocation-aware behavior passed nine focused explorer/CLI
tests. The initial 18 trials remain counted; the corrected semantics were preregistered as 18 new
trials, raising the cumulative count from 324 to 360. Because the initial run exposed the holdout,
the corrected run is explicitly a non-admissible engineering replay.

## Corrected selection and validation replay

Selection ended 2021-01-29, February 2021 was embargoed, and the frozen winner remained the
**top acceleration quintile versus the universe at 30% allocation**. Under correct $15M sleeve
capacity, selection candidate Sharpe was 0.467 and combined Sharpe improved from 0.210 to 0.429,
a +0.219 gain. Maximum full-portfolio ADV participation was 7.66%.

The validation segment contains 62 monthly observations beginning 2021-03-31.

| Corrected replay | Candidate sleeve | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.050 | 0.641 | 0.732 | 0.601 |
| Annualized return | 0.13% | 2.17% | 1.93% | 1.57% |
| Maximum drawdown | -8.26% | -10.34% | -7.06% | -7.34% |
| Average turnover | 0.220 | 0.060 | 0.122 | 0.122 |
| Annualized cost drag | 0.38% | 0.30% | 0.35% | 0.70% |
| Maximum ADV participation | 10.09% | 9.21% | 7.18% | 7.18% |
| Average gross deployment | 82.18% | 100.00% | 100.00% | 100.00% |
| Minimum gross deployment | 36.57% | 100.00% | 100.00% | 100.00% |
| Probabilistic Sharpe | 54.44% | 91.25% | 94.25% | 90.40% |
| DSR probability (360 trials) | 0.23% | 7.89% | 10.77% | 6.24% |

Candidate/router correlation was -0.404 and combined Sharpe improved by +0.091, but this did not
rescue the candidate. Only two of six folds were positive, standalone Sharpe was nearly zero, DSR
was negligible, maximum participation marginally breached 10%, and the selected sleeve could not
deploy its own $15M budget. It failed standalone Sharpe, DSR, fold-stability, and gross-deployment
gates.

Decision: **reject earnings acceleration from production and shadow alpha**. The governed feature
remains a production-queryable data product. The corrected replay could not have admitted the
candidate because the holdout was already exposed, but its four independent failures make the
rejection robust without relying on that restriction. Reconsideration requires genuinely new
dates or a materially distinct, preregistered construction; the current holdout cannot be retuned.

Corrected replay artifact:
`atx-factor/research/loop82-earnings-acceleration-capacity-corrected-replay.json`.
Evidence SHA-256:
`77d4b7f5f4e80c0065dae69d652f3682c64df3431921efacb1d4cf30ecfa6dba`.

The pre-fix diagnostic artifact is retained for audit at
`atx-factor/research/loop82-earnings-acceleration-exploration.json`; it is not admissible evidence.

## Verification

- Two focused earnings-acceleration transform tests passed; no full suite was run.
- Migration 258 and its governance assertions passed a transactional dry run before live apply.
- Nine focused explorer and CLI tests passed after the capacity correction.
- Ruff passed on all Loop 82 factor, migration, CLI, explorer, and test paths.
- Applied migration checksums verify and the live warehouse is at migration 258.
- The factor panel has no duplicate IDs, non-finite values, availability violations, or missing
  lineage.
- The durable ledger contains 360 trials and no invalid 63-day exploration spec.
- The canonical shadow registry remains unchanged with the Loop 69 NOA and Loop 79
  abnormal-inventory-growth candidates; no replay shadow or production entry was created.
