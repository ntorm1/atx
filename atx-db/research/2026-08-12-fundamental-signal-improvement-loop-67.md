# Fundamental signal improvement loop 67: cash-profitability level and growth

Status: production feature added; **predictive characteristic, rejected as a mega-alpha sleeve**.

## Research and frozen construction

Expected profitability predicts returns beyond current profitability, with cash-profitability
forecasts reported as the strongest tested definition
(https://doi.org/10.1016/j.econlet.2019.108547). Dynamic profitability evidence separately finds
that first-order profitability growth carries the strongest pricing power and that combining the
static level with its dynamics improves performance
(https://doi.org/10.1016/j.irfa.2022.102059). A local duplicate audit found no governed annual
cash-profitability change factor.

Before inspecting Loop 67 returns, the candidate was frozen as:

```text
growth = current annual cash operating profitability
       - immediately prior annual cash operating profitability
score  = 0.5 * z(current level) + 0.5 * z(growth)
```

Annual statement period ends must be 300--430 days apart. Both observations must already be known
at formation. No missing component is imputed and neither the component nor portfolio weights are
fit to returns. Growth and the final blend are winsorized 1% per tail and standardized by formation
date.

## Production implementation

Added `atx_db.cash_profitability_growth`, `scripts/build_cash_profitability_growth.py`, three
focused formula/orientation/lineage/governance tests, and append-only migration `0254`. Lineage
records both parent factor-value IDs, statement period ends, raw levels, the raw and standardized
change, availability timestamps, fixed weights, and decision timestamp.

The set-based build finished in 25.6 seconds with 59,337 observations across 719 securities and
171 formation dates from 2012-08-31 through 2026-06-15.

## Cross-sectional evidence

Run ID: `loop67-screen`.

| Horizon | Rank IC | HAC t-stat | Sign consistency | Dates | Mean names |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0224 | 2.72 | 54.5% | 167 | 333.2 |
| 63d | 0.0184 | 1.73 | 61.2% | 165 | 329.7 |
| 126d | 0.0196 | 1.58 | 57.4% | 162 | 324.2 |
| 252d | 0.0214 | 1.30 | 59.0% | 156 | 312.5 |

The signal is positive at every horizon and statistically strongest at one month. Under the old
two-significant-horizon rule it would have stopped here. That rule was too coarse: it conflated
cross-sectional predictiveness with one arbitrary portfolio implementation.

## Hardened feature-to-portfolio acceptance

Loop 67 introduced a bounded, leakage-resistant exploration layer in the separate Polars
`atx-factor` engine. Its design follows four findings:

- Multiple factor trials require much higher evidence than an unadjusted t-stat near two
  (https://www.nber.org/papers/w20592).
- Deflated Sharpe must account for selection bias, multiple tests, and non-normal returns
  (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551).
- Anomaly conclusions can change across continuous/equal-weighted, tail, breakpoint, and
  value-weighted implementations; microcaps can dominate naive equal-weight tests
  (https://academic.oup.com/rfs/article/33/5/2019/5236964).
- Turnover mitigation and realistic costs materially change implementable anomaly returns
  (https://doi.org/10.1093/rfs/hhv063).

The production workflow now commits four constructions times three allocations to a durable trial
ledger before evaluation: continuous rank, quintile tails, decile tails, and continuous raw score,
each at 10%, 20%, and 30% of the mega-alpha. Construction selection uses only the first 60% of
common dates. One monthly formation is embargoed. Only the frozen winner touches the final 40%.
The final decision includes the cumulative 78-trial DSR penalty, annual-fold stability, $50 million
ADV caps, trading and borrow costs, doubled-cost stress, gross deployment, drawdown, correlation,
and marginal mega-alpha Sharpe. Longer overlapping return horizons remain signal-decay diagnostics,
not falsely independent monthly portfolio returns.

Early history through 2020-09-30 selected **quintile tails at a 30% allocation**. Validation begins
2020-11-30 and contains 66 untouched monthly observations.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.250 | 0.547 | 0.491 | 0.410 |
| Annualized return | 1.07% | 1.91% | 1.81% | 1.50% |
| Maximum drawdown | -6.04% | -10.34% | -9.47% | -9.70% |
| Average turnover | 0.100 | 0.060 | 0.071 | 0.071 |
| Annualized cost drag | 0.33% | 0.30% | 0.31% | 0.61% |
| Maximum ADV participation | 7.59% | 9.21% | 8.54% | 8.54% |
| DSR probability (78 trials) | 3.30% | 13.50% | 10.63% | 7.56% |

The candidate is positive in four of six validation folds and is fully deployable at $50 million.
It nevertheless misses the 0.50 standalone Sharpe floor, the 95% deflated-Sharpe probability, and
the required +0.05 marginal mega-alpha Sharpe. Adding the selected sleeve reduces router Sharpe by
0.0557. Correlation with the router is 0.663, below the 0.70 ceiling, so lack of diversification is
not the mechanical rejection.

Decision: **reject `composite_cash_profitability_level_growth` from mega-alpha**. Retain it as a
governed predictive characteristic for downstream models and future genuinely out-of-sample data;
do not tune another allocation or tail definition on the validation period. Router v6 and the
mega-alpha registry remain unchanged. Full decision evidence is stored in
`atx-factor/research/loop67-cash-profitability-level-growth-exploration.json` and the committed grid
in `atx-factor/research/trial-ledger.json`.

## Verification

- Three focused atx-db tests passed in 1.35 seconds; Ruff passed.
- Eleven focused atx-factor portfolio/exploration/ledger/CLI tests passed in 4.62 seconds; Ruff
  passed.
- Migration `0254` is applied; the warehouse reports 228 numeric migrations.
- No full test suite was run.
