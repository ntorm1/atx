# Fundamental signal improvement loop 79: abnormal inventory growth

Status: **production data feature complete; frozen zero-capital shadow candidate**.

## Research and feature contract

Steinker and Hoberg report that abnormal year-over-year inventory growth relative to sales growth
predicts subsequent returns, with unusually high inventory accumulation associated with weaker
performance
(https://www.sciencedirect.com/science/article/pii/S0272696313000399). Thomas and Zhang likewise
identify inventory changes as a major driver of the accrual anomaly
(https://ideas.repec.org/a/spr/reaccs/v7y2002i2d10.1023_a1020221918065.html).

Loop 79 therefore added the governed point-in-time factor
`investment_low_abnormal_inventory_growth`:

`(inventory_t / inventory_t-4 - 1) - (revenue_t / revenue_t-4 - 1)`.

Lower abnormal growth is preferred. The implementation joins the latest inventory facts visible at
each existing governed quarterly revenue-growth decision timestamp. It requires positive,
same-quarter current/prior-year inventory and revenue pairs, imputes no missing inventory, rejects
absolute inventory or abnormal growth above 10, winsorizes each cross-section at 1%, and
standardizes only when at least 20 securities are present. Every output preserves source IDs,
timestamps, periods, values, formula, and preprocessing parameters in lineage.

Migration 255 registers the factor and its dependencies on `growth_quarterly_revenue_yoy` and the
`inventory` metric. The production refresh materialized 29,012 rows across 384 securities and 160
dates from 2013-04-30 through 2026-06-15. The panel has no duplicate IDs, non-finite values,
availability violations, or missing lineage.

## Data diagnostics

Forward-return information coefficients were positive at short horizons but did not persist:

| Horizon | Mean rank IC | HAC t-stat | Dates |
|---|---:|---:|---:|
| 21 trading days | 0.0151 | 1.46 | 158 |
| 63 trading days | 0.0130 | 0.90 | 156 |
| 126 trading days | -0.0043 | -0.21 | 153 |
| 252 trading days | -0.0196 | -0.73 | 147 |

The weak and non-monotonic standalone diagnostics made portfolio-level diversification and cost
tests decisive; the factor was not admitted from IC alone.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the cumulative construction count from 270 to 288. Selection ended
2020-12-31, January 2021 was embargoed, and only the frozen winner was evaluated beginning
2021-02-26. Losing variants remained blind to validation returns.

Selection chose **continuous cross-sectional rank at 30% allocation**. On selection data its
standalone Sharpe was 0.230, the router Sharpe was 0.269, and the combined Sharpe was 0.336, a
+0.067 improvement. Maximum estimated ADV participation was 6.69%.

## Untouched validation and decision

The untouched validation segment contains 63 monthly 21-trading-day observations through
2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.971 | 0.712 | 1.023 | 0.936 |
| Annualized return | 4.34% | 2.42% | 3.79% | 3.45% |
| Maximum drawdown | -6.70% | -10.34% | -7.43% | -7.67% |
| Average turnover | 0.195 | 0.060 | 0.114 | 0.114 |
| Annualized cost drag | 0.39% | 0.29% | 0.33% | 0.66% |
| Maximum ADV participation | 7.74% | 9.21% | 8.33% | 8.33% |
| Probabilistic Sharpe | 97.37% | 93.61% | 98.00% | 97.13% |
| DSR probability (288 trials) | 28.38% | 12.22% | 31.97% | 25.70% |

Candidate/router correlation was only 0.153. Five of six calendar folds were positive, the
combination maintained full gross deployment, doubled costs remained profitable, and the candidate
improved router Sharpe by +0.311. All admission gates passed except the multiple-testing-adjusted
DSR floor.

Decision: **admit the exact continuous-rank/30% construction to the zero-capital shadow registry,
not production**. It may be promoted only from new, genuinely untouched dates under the existing
shadow policy; the consumed interval cannot be reused for tuning or promotion. Production router
v6 and capital allocations remain unchanged.

Decision artifact:
`atx-factor/research/loop79-abnormal-inventory-growth-exploration.json`.
Evidence SHA-256:
`339ed1ebfa5d73573f8bb509ee454d624f40a5020b222fd55b47664182cfbf02`.

## Verification

- Three focused abnormal-inventory-growth tests passed; no full suite was run.
- Ruff passed on every touched Loop 79 Python file.
- Applied migration checksums verify and the live warehouse is at migration 255.
- Factor definition, dependency edges, point-in-time availability, uniqueness, finiteness, and
  lineage checks passed.
- The durable ledger contains 288 trials.
- The shadow registry contains this candidate and `quality_net_operating_assets`; no production
  registry entry was created.
