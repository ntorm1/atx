# Fundamental signal improvement loop 33: annual profit-margin change

Date: 2026-08-10

## Pre-registered research hypothesis

Profit-margin improvement is the cost-control/pricing-power leg of the DuPont decomposition and
may forecast future profitability and returns beyond the current profitability level. Fairfield
and Yohn report that changes in profit margin and asset turnover, unlike their levels, help
forecast one-year-ahead changes in return on assets. Soliman studies the richer net-operating-asset
DuPont decomposition and finds incremental information that market participants process
incompletely.

Primary references:

- [Fairfield and Yohn, *Using Asset Turnover and Profit Margin to Forecast Changes in Profitability*](https://doi.org/10.1023/A:1012430513430)
- [Soliman, *The Use of DuPont Analysis by Market Participants*](https://doi.org/10.2308/accr.2008.83.3.823)
- [Nissim and Penman, *Ratio Analysis and Equity Valuation*](https://business.columbia.edu/sites/default/files-efs/pubfiles/1063/nissimpenmanratio.pdf)

This document freezes definitions and decision rules before any Loop 33 return inspection.

## Candidate definitions

The primary candidate is the broad annual change in net profit margin:

`delta_net_margin_t = net_income_t / revenue_t - net_income_(t-1) / revenue_(t-1)`.

Two variants are secondary and cannot silently replace the primary:

1. Operating-margin change replaces net income with operating income.
2. Gross-margin change replaces net income with gross profit. This is the continuous version of
   one Piotroski dimension, but it is screened on a broad standalone input set rather than through
   the complete nine-input F-score surface.

For every variant, the numerator and revenue must come from the same exact accession and fiscal
endpoint. Current and prior flows must each span 330-400 inclusive days, their fiscal endpoints
must be 300-430 days apart, revenue must be positive and finite, numerator values may be negative
but must be finite, and every input must be visible at the governed monthly formation timestamp.
The newest annual observation may be no more than 550 days old. Missing components are not
imputed and no return-fitted parameter is allowed.

Each raw change is symmetrically winsorized at 1% and sample-zscored within formation month.
Cross-sections require at least 20 securities. Financial firms remain in the broad pre-registered
test; an industry-neutral or nonfinancial result may be reported only as robustness and cannot
replace it.

## Evaluation contract

The candidates are evaluated on the governed monthly US-equity universe using fixed 21, 63, 126,
and 252-trading-day forward total-return targets. The report includes rank IC, Newey-West HAC
inference, decile Q10-Q1 spreads, hit rate, tail monotonicity, turnover, rank persistence, breadth,
modern-regime stability, and correlations with incumbent features and production router v6.

A candidate advances beyond research-only code only if it has positive full-history rank IC at
all four horizons, HAC t-statistics of at least 2.0 at two or more horizons including 126 or 252
days, positive directionally coherent long-horizon tails, no material modern-regime sign reversal,
and useful incremental exposure versus router v6. Each secondary candidate must independently
satisfy the same rule; it cannot rescue the primary through post-hoc relabelling.

Any surviving factor must pass the separate `atx-factor` walk-forward admission test with costs,
capacity constraints, deflated-Sharpe control, and explicit baseline/blend comparison. Portfolio
admission requires every configured mega-alpha gate.

## Status

Pre-registration was frozen before Loop 33 return inspection. The screen is complete and all three
variants are rejected before governance.

## Point-in-time research implementation

Added `atx_db.annual_margin_change` and three focused pure tests. One SQL pass pivots revenue, net
income, operating income, and gross profit by exact accession and annual fiscal endpoint, then
creates independent variant events. Current and prior observations are selected separately for
each security, formation month, and variant under the governed universe and knowledge timestamps.

The pure transform enforces positive revenue, finite numerators, 300-430-day consecutive annual
periods, 550-day maximum current age, minimum monthly breadth of 20, symmetric 1% winsorization,
and sample z-scoring. Every candidate carries exact statement-point lineage. No rows were written
to the warehouse and no migration was created.

## Pre-registered screen

| Candidate | Rows | Securities | Dates | Min/max breadth |
|---|---:|---:|---:|---:|
| Net-margin change | 73,101 | 891 | 175 | 41 / 580 |
| Operating-margin change | 69,702 | 798 | 175 | 44 / 561 |
| Gross-margin change | 50,862 | 555 | 173 | 34 / 473 |

### Net-margin change

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | -0.0011 | -0.21 | -0.138% | 45.6% | 0.418 |
| 63d | 0.0007 | 0.07 | -0.723% | 43.8% | 0.139 |
| 126d | 0.0056 | 0.45 | -1.953% | 45.2% | 0.152 |
| 252d | 0.0201 | 1.39 | -0.857% | 47.5% | 0.333 |

The primary fails the all-positive IC condition, every inference threshold, and positive-tail
condition. It is rejected.

### Operating-margin change

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0000 | 0.00 | 0.092% | 49.1% | 0.370 |
| 63d | 0.0020 | 0.21 | -0.171% | 46.2% | 0.370 |
| 126d | 0.0084 | 0.65 | -1.167% | 48.8% | 0.345 |
| 252d | 0.0169 | 1.15 | -1.177% | 55.6% | 0.309 |

The secondary has a positive but economically negligible average rank relationship and negative
tails at every horizon beyond 21 days. It is rejected.

### Gross-margin change

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | -0.0016 | -0.25 | -0.800% | 45.0% | 0.115 |
| 63d | -0.0007 | -0.07 | -1.347% | 40.2% | 0.285 |
| 126d | 0.0059 | 0.57 | -1.561% | 43.4% | 0.285 |
| 252d | 0.0141 | 1.52 | 2.181% | 49.4% | 0.418 |

The second secondary fails the all-positive IC rule and has negative short/intermediate tails. It
is rejected.

## Stability and incremental exposure

All variants have roughly 20% top/bottom monthly turnover and 0.91-0.93 mean rank persistence.
Cross-sectional mean correlations with router v6 are only 0.024 for net margin, 0.046 for operating
margin, and 0.028 for gross margin. Net and operating changes are highly redundant with each other
(0.827), while gross-margin change is less redundant at 0.212/0.365.

The 2021+ and 2023+ operating-margin slices are materially stronger. For 2021+, IC is
0.0187/0.0353/0.0582/0.0502 and HAC is 1.83/2.27/3.05/2.36. For 2023+, IC is
0.0200/0.0392/0.0707/0.0752 and HAC is 1.88/2.46/3.41/3.09. Net-margin evidence also improves in
2023+. These are post-sample regime observations and cannot override failed full-history gates or
negative full-history tails. They are retained only as monitoring hypotheses.

## Mega-alpha decision

Decision: **reject all Loop 33 variants from the mega-alpha portfolio**. Each fails the frozen
upstream feature gate, so none is governed or sent to the costed standalone admission engine.
Running a costed optimizer after selecting only the favorable modern regime would be a post-hoc
test and is intentionally prohibited. Router v6 and the mega-alpha registry remain unchanged.

## Verification

- Three focused transform tests pass; Ruff passes for the research module and tests.
- The full test suite was intentionally not run.
- The live warehouse remains schema `0235`; Loop 33 made no database writes.

## Next loop

Repair the missing point-in-time classification match before relying on industry-neutral DuPont
tests. A future revisit may pre-register an operating-margin signal specifically for a modern
regime, but the present full-history candidates must remain rejected.
