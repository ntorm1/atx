# Fundamental signal improvement loop 85: gross-margin trend and institutional breadth

Status: **production data feature complete; rejected from production and shadow alpha**.

## Research and feature contract

Akbas, Jiang, and Koch decompose gross profitability into asset turnover and gross margin and
apply their eight-quarter seasonally controlled trend regression to both components. They report
that the component trends contain incremental information for future returns
(https://doi.org/10.2308/accr-51708). S&P Global independently reports that gross-profitability
trend remains significant after controlling for price and earnings momentum and is robust across
six constructions (https://www.spglobal.com/market-intelligence/en/news-insights/research/the-gross-profitability-trend-is-your-friend).

Loop 85 added `profitability_quarterly_gross_margin_trend_8q`. On the exact governed PIT history
grid shared with Loops 83 and 84, it calculates quarterly resolved gross profit divided by positive
revenue and estimates the coefficient on actual elapsed-quarter time after calendar-quarter fixed
effects. A reported-gross-profit fallback is permitted only when the same governed parent has a
revenue denominator. Every parent row must be visible at the monthly decision timestamp. The
factor requires eight observations and at least three seasonal quarters; absolute gross margin is
capped at 5 and absolute slope at 1 before 1% cross-sectional winsorization and standardization.

Migration 261 governs both parent dependencies. The production refresh materialized 39,882 rows
across 571 securities and 153 dates from 2013-11-29 through 2026-06-15.

## Data diagnostics

Gross-margin trajectory had strong, monotonic horizon persistence:

| Horizon | Mean rank IC | HAC t-stat | Conventional t-stat | Sign consistency | Dates | Mean names |
|---|---:|---:|---:|---:|---:|---:|
| 21 trading days | 0.0175 | 2.17 | 1.98 | 60.3% | 151 | 257.7 |
| 63 trading days | 0.0290 | 2.27 | 3.40 | 60.4% | 149 | 255.4 |
| 126 trading days | 0.0417 | 2.44 | 4.97 | 64.4% | 146 | 250.2 |
| 252 trading days | 0.0646 | 3.28 | 7.36 | 72.9% | 140 | 239.7 |

The longer horizons are IC-decay diagnostics only. Portfolio exploration used non-overlapping
monthly 21-trading-day returns.

## Breadth hardening and data-expansion audit

Loop 85 made breadth a production admission requirement rather than a descriptive statistic:

- production now requires median validation signal breadth of at least 1,000 names;
- a factor with at least 100 names may remain shadow-eligible if institutional breadth and/or DSR
  are its only failures and its candidate PSR is at least 95%;
- exploration evidence reports eligible signal names, nonzero portfolio holdings, and the
  gross-weight effective number of bets;
- the complete breadth gate is committed in the durable trial specification before results.

The live breadth funnel at 2026-06-15 shows 1,578 issuers with recent statement coverage but only
937 securities with a bar in the trailing 30 days, 775 with both recent prices and statements, and
662 governed liquid-universe members. Recent daily bars are therefore the primary constraint, not
SEC fundamentals. The funnel is persisted at `research/research-breadth-funnel.json`.

A non-mutating background scan of the 3.54 GB compressed local ten-year archive read 31,598,499
rows and found 12,218 vendor security IDs on its latest date, 2026-06-15. This proves the local
source can materially widen prices without a network wait. Its evidence is at
`research/broad-bar-archive-breadth.json`. The broad canonical projection load was launched as a
hidden background job after all Loop 85 warehouse verification; downstream universe and factor
refreshes remain separate governed steps after it completes.

## Preregistered construction search

The new breadth-aware gate committed 18 trials before portfolio results, raising the durable total
from 396 to 414. The search used six frozen variants, 10%/20%/30% allocations, the first 60% of
common dates for selection, one embargo period, and the untouched final 40% for validation. All
18 selection trials met the existing positive-candidate, turnover, and participation support rule.

The frozen winner was **gross-margin-trend quintile tails at 30% allocation**. Through 2021-03-31
its candidate Sharpe was 0.583 and combined Sharpe improved from 0.142 to 0.406, a +0.265 gain.
Maximum participation was 8.66%. April 2021 was embargoed and validation began 2021-05-28.

## Untouched validation and disposition

The validation segment contains 60 monthly observations:

| Validation | Candidate sleeve | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.590 | 0.631 | 0.682 | 0.602 |
| Annualized return | 2.60% | 2.12% | 2.63% | 2.31% |
| Maximum drawdown | -9.75% | -10.34% | -11.62% | -11.87% |
| Average turnover | 0.160 | 0.060 | 0.098 | 0.098 |
| Annualized cost drag | 0.35% | 0.29% | 0.31% | 0.63% |
| Maximum ADV participation | 7.83% | 9.21% | 8.08% | 8.08% |
| Minimum gross deployment | 100.00% | 100.00% | 100.00% | 100.00% |
| Probabilistic Sharpe | 88.60% | 90.90% | 91.77% | 89.27% |
| DSR probability (414 trials) | 6.58% | 7.04% | 9.44% | 6.75% |

Candidate/router correlation was 0.497, combined Sharpe improved by only +0.051, doubled-cost
Sharpe remained positive, and three of five folds were positive. Median validation signal breadth
was 475 names. The candidate portfolio held a median 190 names with median effective breadth
184.6; the combination held a median 642 names with median effective breadth 434.8.

The candidate failed the 1,000-name production breadth floor and 95% DSR floor. Its candidate PSR
was only 88.6%, below the 95% shadow threshold, so under-breadth development status did not rescue
it.

Decision: **reject gross-margin trend from both production and shadow alpha**. The governed data
feature remains production-queryable. Reconsideration requires genuinely new formation dates
after breadth expansion; the exposed holdout must not be retuned.

Artifact: `atx-factor/research/loop85-gross-margin-trend-exploration.json`.
Evidence SHA-256:
`1c54e2d9b6f6637ce8ac9f096063f912a8bc772af0507e08ac5dbd77ad79ac15`.

## Verification

- Two focused transform tests and a transactional migration dry run passed; no full suite ran.
- Thirteen focused explorer, CLI, and shadow-registry tests passed after breadth hardening.
- Ruff passed on the feature, governance, breadth tools, explorer, registry, and tests.
- Applied migration checksums verify and the warehouse is at migration 261.
- The factor has zero duplicate IDs, non-finite values, missing lineage, or parent-availability
  violations.
- The durable ledger contains 414 trials, the exploration evidence digest recomputes exactly, and
  the shadow registry was unchanged by the rejected candidate.
