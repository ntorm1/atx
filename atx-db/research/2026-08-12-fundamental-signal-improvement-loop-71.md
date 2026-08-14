# Fundamental signal improvement loop 71: earnings/revenue agreement recovery

Status: **rejected after preregistered construction selection and untouched validation**.

## Research and frozen candidate

Revenue surprise contains information about future and post-announcement returns beyond earnings
surprise (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=314962). Related anomaly evidence finds
the strongest return continuation when earnings, revenue, and price signals point in the same
direction (https://www.sciencedirect.com/science/article/pii/S0378426613003890). This supports
recovering a joint-confirmation feature while leaving its economic direction fixed.

Loop 71 recovered the existing governed `earnings_sue_revenue_agreement` feature. No formula,
direction, availability rule, or observation was changed. Its older fixed continuous-rank test had
candidate Sharpe 0.293, +0.036 marginal router Sharpe, and 0.300 router correlation, so the new
bounded construction selector tested whether concentrated agreement tails were more useful.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the durable trial count from 126 to 144. The constructions were continuous
rank, continuous raw score, symmetric quintile and decile tails, top quintile versus universe, and
universe versus bottom quintile. Selection ended 2020-08-31, September 2020 was embargoed, and
only the frozen winner was evaluated from 2020-10-30 onward.

Early history selected **symmetric decile tails at 30%**:

- candidate Sharpe: 0.733;
- router Sharpe: 0.287;
- 70/30 combined Sharpe: 0.623;
- marginal Sharpe: +0.336;
- turnover: 0.154;
- maximum ADV participation: 5.85%.

This was a feasible, economically interpretable winner selected without validation returns.

## Untouched validation

The validation segment contains 67 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.340 | 0.431 | 0.524 | 0.437 |
| Annualized return | 2.48% | 1.51% | 2.10% | 1.73% |
| Maximum drawdown | -11.16% | -10.34% | -7.65% | -7.91% |
| Average turnover | 0.336 | 0.060 | 0.152 | 0.152 |
| Annualized cost drag | 0.50% | 0.29% | 0.36% | 0.71% |
| Maximum ADV participation | 23.11% | 9.21% | 7.24% | 7.24% |
| Probabilistic Sharpe | 77.83% | 83.50% | 87.28% | 83.24% |
| DSR probability (144 trials) | 3.89% | 5.90% | 9.61% | 6.54% |

Candidate/router correlation was only 0.200 and adding the sleeve improved router Sharpe by 0.093.
Four of six validation folds were positive, and the combination remained profitable under doubled
costs. These diversification results are useful, but they do not overcome the standalone evidence:
candidate Sharpe was below the 0.50 floor, candidate DSR was far below 95%, and capacity stress
caused minimum gross exposure to fall to 0.464. The candidate therefore fails both statistical and
implementation gates.

Decision: **reject earnings/revenue agreement from mega-alpha**. It is not shadow eligible because
DSR is not the sole failed gate and its 77.83% probabilistic Sharpe is below the 95% shadow floor.
Router v6, the production registry, and the Loop 69 NOA shadow entry remain unchanged. The consumed
validation period must not be used to retune the feature or its decile construction.

Decision artifact:
`atx-factor/research/loop71-earnings-revenue-agreement-exploration.json`.
Evidence SHA-256:
`f84ce120320d1e3f7f9380ca77c429bfe3e1a3cf701787873ee308225d27e722`.

## Runtime correction and verification

The first evaluation exposed a floating-point boundary in the native Polars capped-proportional
allocator: on one date, requested gross and eligible capacity differed only in their final binary
digit. The solver now detects `target >= capacity - 1e-12` and assigns eligible names directly to
their caps. A regression reproduces a 0.30000000000000004 target against 0.1 and 0.2 caps.

- The same already-committed grid was resumed; idempotency prevented duplicate trials.
- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 144 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production entry was
  created.
- Ten targeted portfolio/backtest tests pass in 0.71 seconds; Ruff passes.
- No full test suite was run.
