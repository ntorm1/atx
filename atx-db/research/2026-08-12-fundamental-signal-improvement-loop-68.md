# Fundamental signal improvement loop 68: cash-profitability closure

Status: existing production feature re-evaluated under hardened construction selection;
**rejected from mega-alpha after untouched holdout reversal**.

## Research and candidate recovery

Loop 68 began with research into DuPont margin and asset-turnover changes. The local duplicate
audit found that exact annual signals were already implemented and tested in Loops 32 and 33, so
no duplicative factor was built. The loop instead ranked current stored factor evidence to find a
production feature that the former single-construction acceptance gate could have skipped.

The recovered candidate is `profitability_cash_operating_profitability`. Ball et al. show that
cash-based operating profitability predicts returns more strongly than earnings measures distorted
by accrual accounting (https://doi.org/10.1016/j.jfineco.2015.02.004), and replication work includes
cash operating profitability among its central profitability signals
(https://academic.oup.com/rfs/article/33/5/2019/5236964). ATX's stored evidence is positive at all
four horizons:

| Horizon | Rank IC | HAC t-stat |
|---:|---:|---:|
| 21d | 0.0204 | 2.75 |
| 63d | 0.0249 | 2.22 |
| 126d | 0.0258 | 1.80 |
| 252d | 0.0254 | 1.26 |

The factor had not received a standalone bounded construction-grid/untouched-holdout test. The
related Ball operating-profitability factor had produced a 0.479 Sharpe and positive marginal
router improvement under the former fixed 20% continuous-rank protocol, making this a justified
closure candidate. No formula, direction, or feature parameter was changed.

## Preregistered portfolio exploration

Before loading returns, the existing Loop 67 grid was committed as another 12 trials in
`atx-factor/research/trial-ledger.json`, raising the cumulative count to 90. The grid contained
continuous rank, continuous raw score, symmetric quintile tails, and symmetric decile tails at
10%, 20%, and 30% router allocations. Selection used only the first 60% of common formation dates,
ending 2020-07-31. August 2020 was embargoed. Only the frozen winner was evaluated from
2020-09-30 onward.

Early history selected **continuous rank at 30%**:

- Candidate selection Sharpe: 0.606.
- Router selection Sharpe: 0.403.
- Combined selection Sharpe: 0.555, a +0.152 improvement.

This makes the final result a meaningful regime/holdout test rather than a candidate that never
worked in the first place.

## Untouched validation result

The validation segment contains 68 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | -0.084 | 0.393 | 0.268 | 0.188 |
| Annualized return | -0.42% | 1.37% | 0.93% | 0.63% |
| Maximum drawdown | -7.99% | -10.34% | -9.65% | -9.87% |
| Average turnover | 0.045 | 0.060 | 0.055 | 0.055 |
| Annualized cost drag | 0.29% | 0.30% | 0.30% | 0.59% |
| Maximum ADV participation | 7.59% | 9.21% | 8.59% | 8.59% |
| DSR probability (90 trials) | 0.38% | 6.62% | 3.45% | 2.20% |

The candidate is fully deployed at $50 million and correlation with router v6 is an acceptable
0.649. It is positive in only three of six validation folds. The 70/30 combination reduces router
Sharpe by 0.1246. Failed gates are standalone Sharpe, 95% deflated-Sharpe probability, and +0.05
marginal mega-alpha Sharpe.

Decision: **reject `profitability_cash_operating_profitability` from mega-alpha**. Retain the PIT
feature in atx-db because its cross-sectional IC is real and it remains useful as a model input, but
do not tune its portfolio on the consumed validation sample. Router v6 and the production registry
remain unchanged. Full evidence is in
`atx-factor/research/loop68-cash-operating-profitability-exploration.json`.

## Acceptance-layer improvement

The research audit also found evidence that accounting-anomaly implementation failures are often
concentrated in the short leg, and that the short leg carries material institutional and trading
frictions (https://www.lehigh.edu/~xuy219/research/RAST_2024.pdf). The portfolio engine therefore
now preregisters two additional dollar-neutral constructions for **future** candidates:

- top quintile long versus the broad non-top universe;
- broad non-bottom universe versus the bottom quintile short.

These isolate one predictive extreme without introducing unhedged market exposure. They were not
run retroactively on Loop 68 because its validation returns had already been observed. Future grids
contain six constructions times three allocations (18 committed trials).

## Verification

- Twenty-four targeted portfolio, exploration, ledger, CLI, backtest, mega-alpha, and replacement
  tests pass, including exact asymmetric-book weights, long-leg-only construction selection,
  chronological embargo, and holdout reversal.
- Ruff passes on the changed engine and tests.
- No full test suite was run.
