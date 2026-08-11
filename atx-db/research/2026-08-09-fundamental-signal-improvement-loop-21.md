# Fundamental signal improvement loop 21: point-in-time q5 rolling WLS

Date: 2026-08-09

## Research question

Can the q5 expected-growth forecast transfer to SEC-based ATX data when its coefficients are
learned from point-in-time realized investment growth instead of importing the paper's historical
Compustat average slopes?

## Primary-source basis

- Hou, Mo, Xue, and Zhang motivate expected investment growth as a separate expected-return
  dimension and construct it with cross-sectional forecasts rather than security returns:
  <https://www.nber.org/papers/w24709>.
- The current global-q specification defines the target as the one-year-ahead change in
  investment-to-assets and the predictors as `ln(q)`, cash operating profitability, and four-quarter
  change in ROE. Annual inputs are at least four months old and all regression variables are
  winsorized monthly at 1%/99%:
  <https://global-q.org/uploads/1/2/2/6/122679606/factorstd_2025feb.pdf>.
- The published implementation estimates monthly market-equity-weighted cross-sectional WLS,
  lags predictors twelve months behind the latest observable investment-growth target, and applies
  the average slopes from the prior 120 months with a 30-month minimum:
  <https://global-q.org/uploads/1/2/2/6/122679606/houmoxuezhang2020rf.pdf>.
- The maintained global-q library continues to publish expected-growth factor and benchmark
  returns through 2024:
  <https://global-q.org/factors.html>.

No security return enters model fitting. The target, lag, weighting, winsorization, slope-window,
and minimum-history rules were fixed before evaluation.

## Point-in-time target and estimation contract

For training month `t`, the target is:

`I/A_t - I/A_t_minus_1`

where both annual asset-growth decisions are governed factor rows already visible at `t`. Each
training observation uses `ln(q)`, cash operating profitability, and delta ROE from the same security
twelve calendar months earlier. Current training-month market equity is the WLS weight. The target
and all three predictors are capped at their monthly 1st/99th percentiles.

The feasibility audit forms 46,484 leak-free training observations across 707 securities and 159
calendar months from April 2013 through June 2026. Every month has at least 39 candidate names;
there are zero target or predictor timing violations. Raw target changes range from -18.19 to 18.66,
confirming the need for the predeclared caps.

The paper's broad CRSP sample does not specify a small-sample guard. An initial exact audit exposed
one 34-name monthly design with condition number 4,318 and a nonsensical delta-ROE slope of -85.6.
The production contract therefore requires at least 100 names and condition number no greater than
1,000. This safeguard was chosen from model identifiability, not return performance. It removes the
unstable cross-section; retained model months actually contain 125-514 names and have condition
numbers no greater than 311.

## Production build

Added `atx_db.expected_growth_rolling`, migration `0220`, a standalone build CLI, three focused
tests, and the governed `expected_growth_model_slopes` table. The factor is
`investment_q5_expected_growth_rolling_wls`, sourced as
`atx-db PIT q5 rolling WLS expected growth v1`.

The slope table persists 133 reconstructable monthly models from 2014-04-30 to 2026-06-15. Every
row records coefficients, observation count, condition number, weighted R-squared, maximum input
availability, a deterministic training-sample hash, and compact lineage. Average ATX slopes are
0.0244 for log q, 0.1016 for cash profitability, and 0.1972 for delta ROE, versus published averages
of -0.029/0.516/0.771. Weighted monthly R-squared ranges from 0.001 to 0.431.

Each forecast averages only slope dates strictly before formation and within the prior 120 months.
At least 30 eligible slopes are required. This produces 42,612 rows across 623 securities and 114
dates from 2017-02-28 to 2026-06-15. Forecast windows contain 30-110 slope months. The complete
slope-plus-factor refresh takes 22.2 seconds.

No persisted slope or factor row is duplicate, non-finite, future-available, or uses a same/future
model date. Predictor lags are exactly twelve calendar months. Factor dates have at least 36 names,
exact sample z-scores, and lineage no larger than 1,391 bytes; slope lineage is at most 652 bytes.

## Standalone analysis

Run id: `loop21-q5-rolling-wls-production`. Deciles and split-adjusted forward returns are used.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0173 | 1.10 | -0.382% | 50.9% | -0.09 |
| 63d | 0.0352 | 1.46 | -0.658% | 59.1% | -0.14 |
| 126d | 0.0336 | 0.96 | -3.385% | 61.7% | -0.05 |
| 252d | 0.0334 | 0.69 | -1.744% | 51.5% | -0.03 |

The rolling model creates meaningful average rank predictability, especially at 63 days, but
overlapping-horizon HAC evidence remains below 2 and all literal high-minus-low spreads are
negative. Top/bottom-decile turnover is 33.7%/39.0%, mean rank autocorrelation is 0.976, and 113
rebalances are available.

### Fair comparison with imported slopes

On the rolling factor's identical 2017+ date/name surface:

| Model | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Published-slope proxy | 0.0117 | 0.0034 | -0.0050 | 0.0052 |
| Point-in-time rolling WLS | 0.0173 | 0.0352 | 0.0336 | 0.0334 |

Local accounting-target estimation dominates imported coefficients at every horizon. This validates
the governed model layer even though the resulting security signal does not clear trading-tail gates.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2017-2020 | 0.0377 | 0.0580 | 0.0585 | 0.0877 |
| 2021-2026 | 0.0020 | 0.0176 | 0.0134 | -0.0157 |
| 2023-2026 | 0.0313 | 0.0549 | 0.0519 | 0.0477 |

The factor is strong before 2021 and again from 2023, but the 2021-2022 transition makes the full
modern window weak. Recent 63-day HAC is 2.45; recent 21/126/252-day HAC values are
1.68/1.72/1.61. This regime dependence precludes unconditional production promotion.

### Distinctiveness

Mean cross-sectional correlation is 0.616 with cash operating profitability, 0.598 with the
published-slope proxy, 0.360 with delta ROE, 0.331 with operating profitability, 0.292 with the
production router, -0.092 with conservative asset growth, and -0.047 with low net issuance.
Rolling WLS is materially less cash-dominated than the imported proxy and adds distinct rank
information, but its tails remain unsuitable.

## Router overlay trial

Coverage-neutral research panels blend 5%, 10%, or 20% rolling expected growth into the production
router when available and otherwise retain the baseline.

| 2017+ router | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Baseline | 0.02809 | 0.04041 | 0.05681 | 0.07262 |
| 5% rolling WLS | 0.02749 | 0.04015 | 0.05419 | 0.06821 |
| 10% rolling WLS | 0.02710 | 0.03974 | 0.05285 | 0.06536 |
| 20% rolling WLS | 0.02626 | 0.03882 | 0.05062 | 0.06148 |

Every weight reduces full-history IC and HAC. At 5%, Q10-Q1 spreads fall from
0.703%/1.980%/4.568%/11.905% to 0.595%/1.768%/3.939%/10.972%, bottom turnover rises from
17.1% to 21.2%, and rank autocorrelation falls from 0.970 to 0.967.

The 5% overlay modestly improves several 2023+ statistics, including one-year spread from 3.898%
to 4.431%, but that regime-local gain does not offset the broad degradation. No production router
row or definition is changed.

## Decision

Promote the rolling WLS machinery and `expected_growth_model_slopes` to the production data/model
layer: it is point-in-time, reconstructable, non-return-fitted, numerically gated, and materially
outperforms the imported-slope benchmark on common dates. Keep
`investment_q5_expected_growth_rolling_wls` production-queryable but experimental for trading.
Do not use its extreme deciles and do not alter the production router.

## Verification

- Synthetic WLS recovery, strictly-prior forecast, and isolated migration/table governance tests
  passed.
- New and changed Python files pass Ruff and compilation.
- Live schema is `0220`; migration checksums verify, the slope dataset/table/fields are cataloged,
  and five direct dependencies are recorded.
- Target-lag, availability, condition-number, sample-hash, duplicate-key, finiteness, prior-window,
  minimum-history, minimum-breadth, normalization, and lineage-size checks passed.
- Full-suite execution was intentionally avoided.

## Next loop

Return to a timelier operating signal. Research global-q quarterly operating profits-to-lagged assets
(`Olaq`) and quarterly gross profits-to-lagged assets (`Glaq`). Build an announcement-time-safe
quarterly operating-profitability feature and test whether it preserves the production router's
strong tails while adding the modern responsiveness seen in qROE and rolling expected growth.
