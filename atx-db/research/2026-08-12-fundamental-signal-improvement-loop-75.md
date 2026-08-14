# Fundamental signal improvement loop 75: quarterly cash-profitability long leg

Status: **rejected after asymmetric construction selection and untouched validation; explorer
runtime reduced by 73%**.

## Research and frozen candidate

Ball, Gerakos, Linnainmaa, and Nikolaev show that cash-based operating profitability removes the
accrual component and outperforms accrual-inclusive profitability measures in predicting the
cross-section of returns
(https://www.sciencedirect.com/science/article/pii/S0304405X16300307). Their evidence also finds
that adding the cash-profitability factor improves the attainable Sharpe ratio more than adding
separate profitability and accrual factors. Quarterly portfolio research independently reports
that net operating cash flow is a stronger signal of next-quarter returns than quarterly accruals
(https://www.tandfonline.com/doi/abs/10.2469/faj.v64.n3.7).

Loop 75 recovered the existing governed
`profitability_quarterly_cash_operating_profitability_lagged_assets` feature. No formula,
direction, availability rule, or observation was changed. Its previous fixed symmetric portfolio
had positive but subscale performance, so the new search tested whether its edge was concentrated
in one side of the cross-section.

## Explorer performance hardening

Loop 74 exposed avoidable work in the Polars explorer: every losing construction built and
capacity-normalized weights across the final holdout even though only the first 60% was used for
selection. The engine now physically separates the phases:

1. baseline and all six candidate constructions build weights only on selection dates;
2. the winner is frozen from selection metrics;
3. only the winner and baseline build validation-date weights;
4. the embargo, costs, capacity normalization, trial ledger, ranking, and gates are unchanged.

This makes holdout isolation structural rather than merely relying on per-date operators. A new
regression records every portfolio-build call and proves that none spans both phases. Five targeted
exploration tests pass and Ruff passes. The production-sized evaluation fell from 94 seconds in
Loop 74 to 25 seconds in Loop 75, a 73% reduction, while retaining the identical decision schema
and atomic evidence path.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the durable trial count from 198 to 216. Selection ended 2020-07-31, August
2020 was embargoed, and only the frozen winner was evaluated from 2020-09-30 onward.

Early history selected **top quintile versus universe at 30%**:

- candidate Sharpe: 0.385;
- router Sharpe: 0.403;
- 70/30 combined Sharpe: 0.509;
- marginal Sharpe: +0.106;
- turnover: 0.146;
- maximum ADV participation: 6.38%.

The selector therefore found that the signal's useful expression was the strong-cash-profitability
long extreme financed by a broad short universe, rather than a symmetric long/short sort.

## Untouched validation

The validation segment contains 68 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.322 | 0.393 | 0.451 | 0.367 |
| Annualized return | 1.45% | 1.37% | 1.64% | 1.32% |
| Maximum drawdown | -10.06% | -10.34% | -8.71% | -8.95% |
| Average turnover | 0.149 | 0.060 | 0.089 | 0.089 |
| Annualized cost drag | 0.36% | 0.30% | 0.32% | 0.63% |
| Maximum ADV participation | 8.51% | 9.21% | 7.19% | 7.19% |
| Probabilistic Sharpe | 77.65% | 81.36% | 85.16% | 80.26% |
| DSR probability (216 trials) | 2.11% | 3.67% | 4.58% | 2.95% |

Candidate/router correlation was only 0.400. Four of six validation folds were positive, all
capacity and deployment gates passed, the 70/30 book improved router Sharpe by 0.059, and the
combination survived doubled costs. Standalone candidate Sharpe nevertheless missed the 0.50
floor and cumulative-trial DSR was far below 95%.

Decision: **reject quarterly cash operating profitability from production mega-alpha**. It is not
shadow eligible because DSR is not the sole failed gate. This is a useful portfolio-discovery
result rather than a skipped feature: the asymmetric long leg was explicitly found and frozen, but
current evidence is not strong enough to allocate capital. Router v6, production state, and the
Loop 69 NOA shadow candidate remain unchanged. Only genuinely new untouched formation dates may
support a future re-evaluation; the consumed interval cannot be used to tune the allocation.

Decision artifact:
`atx-factor/research/loop75-quarterly-cash-profitability-exploration.json`.
Evidence SHA-256:
`9215b50d100e1de801725dcfb4bd8f28426ebe25be0090016a460b3d57b5d166`.

## Verification

- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 216 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production entry was
  created.
- Targeted explorer tests: 5 passed; touched-file Ruff checks pass.
- No full test suite was run.
