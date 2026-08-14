# Fundamental signal improvement loop 38: low abnormal capital investment

Status: preregistered; no Loop 38 return results inspected at registration time.

## Primary research and hypothesis

Titman, Wei, and Xie (2004), *Capital Investments and Stock Returns*, report
that firms with unusually high capital investment subsequently earn lower
returns. The primary publication is:

- https://doi.org/10.1017/S0022109000003173

ATX hypothesis: lower current trailing-twelve-month capital expenditure relative
to the same company's preceding three annual TTM observations predicts higher
subsequent US common-equity returns.

## Frozen point-in-time construction

- Current investment: absolute value of the latest negative signed TTM capital-
  expenditure cash outflow visible at the monthly close, with fiscal end no more
  than 200 days old.
- Historical benchmark: mean absolute TTM capex at one, two, and three years
  before the current fiscal end. For each lag, select the closest end date within
  +/-100 days of 365.25 times the lag and the latest revision visible at the
  current decision. All three lags are required.
- Raw preferred score: `1 - current_capex / mean(prior_1y, prior_2y, prior_3y)`.
  Thus lower abnormal investment ranks higher.
- Data-validity guard: reject observations whose absolute abnormal-investment
  ratio exceeds 10. This threshold was frozen before returns and removes unit/
  taxonomy discontinuities rather than fitting performance.
- Decision dates: final eligible positive-market-cap observation per security/
  month. Universe is PIT `us_common_equity_liquid_v1`; immutable security IDs
  only. No missing historical year is imputed.
- Cross section: 1%/99% winsorization and sample z-score by date; minimum 20
  names. Complete source IDs, periods, values, and availability remain in JSON
  lineage.

The pre-return coverage audit found 72,498 eligible observations across 662
securities and 175 monthly dates from 2012-04-30 through 2026-06-15, with
37--515 names per date.

## Frozen sequential evaluation

Stage 1 evaluates adjusted-price rank IC and HAC inference at 21, 63, 126, and
252 trading days. Stage 2 is allowed only when 252-day IC is positive and HAC
t-statistic is at least 2.0.

If Stage 1 passes, Stage 2 additionally requires a positive 252-day top-minus-
bottom quintile spread, at least 20 names on at least 36 monthly dates, and zero
PIT, key, value, or lineage violations. Only then may the costed Polars
`atx-factor` walk-forward run. Mega-alpha admission remains frozen at candidate
OOS Sharpe >= 0.50, deflated Sharpe probability >= 0.95, at least +0.05 Sharpe
improvement for an 80/20 blend, positive stressed blend Sharpe, participation in
every valid fold, and passing deployment/turnover gates.

## Production implementation and quality

Migration `0240` governs `investment_low_abnormal_capex` and its market-cap,
capital-expenditure, and universe dependencies. `atx_db.abnormal_capex` resolves
current and each annual-lag revision as of the exact monthly decision, requires
all three lags, applies the preregistered validity bound, standardizes the cross
section, preserves source lineage, and inserts entirely inside DuckDB.

The first implementation carried every wide market-cap column and nested lineage
through the historical self-join and exceeded the 45-second write cap. The
production path now projects only required fields before that join; it loaded all
72,498 rows in 34.07 seconds. The surface covers 662 securities and 175 monthly
dates from 2012-04-30 through 2026-06-15, with 37--515 names per date. IDs and
natural keys are unique; values, scores, and JSON are valid; availability is PIT;
the abnormal-ratio guard has no violations; and date-wise standardized moments
are exact to floating-point tolerance. The focused three-lag integration test
passes.

## Stage 1 results and decision

Run id: `loop38-low-abnormal-capex-screen`. Wall time: 8.24 seconds.

| Horizon | Rank IC | HAC t-stat | Sign consistency | Dates | Mean names |
|---:|---:|---:|---:|---:|---:|
| 21 | -0.00448 | -0.72 | 49.12% | 171 | 417.1 |
| 63 | -0.00493 | -0.56 | 48.52% | 169 | 415.0 |
| 126 | -0.00464 | -0.36 | 54.82% | 166 | 412.0 |
| 252 | 0.00047 | 0.03 | 49.38% | 160 | 406.0 |

Decision: **reject Loop 38 from the mega-alpha portfolio**. The one-year effect
is economically indistinguishable from zero and its HAC t-statistic is 0.03
versus the required 2.0; shorter-horizon ICs have the wrong sign. Stage 2 and
`atx-factor` are prohibited. Router v6 remains unchanged.
