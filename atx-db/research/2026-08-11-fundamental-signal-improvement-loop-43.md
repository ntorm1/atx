# Fundamental signal improvement loop 43: standardized-unexpected-earnings closure

Status: preregistered for current-data revalidation and costed walk-forward; no
Loop 43 backtest result inspected at registration time.

## Primary research and hypothesis

Latane and Jones (1979) document the standardized-unexpected-earnings effect
(https://doi.org/10.1111/j.1540-6261.1979.tb02136.x). Rendleman, Jones, and
Latane (1987) further study size and serial-correlation explanations for excess
returns following SUE sorts
(https://doi.org/10.1111/j.1540-6288.1987.tb00322.x).

ATX hypothesis: the pure PIT time-series SUE factor is a cleaner tactical sleeve
than the Loop 42 confirmation composite and improves router v6 after costs.

## Candidate and upstream evidence

Candidate: `earnings_standardized_unexpected_eps`. Its governed construction
uses first-filed diluted EPS, a same-season prior-year comparison, and only
preceding seasonal changes in the volatility denominator. Loop 9 materialized
102,152 rows, 1,113 securities, and 176 dates with no duplicates or non-finite
values. Later common-cohort evidence reported 21-day IC 0.0224 and HAC 3.20.

Loop 43 requires current 21-day positive IC, HAC >=2, positive quintile spread,
at least 36 dates with 20 names, and zero quality defects before costed testing.

## Frozen costed walk-forward

Baseline: `composite_operating_profitability_or_net_issuance` (router v6).

- 21-trading-day horizon; expanding walk-forward.
- 60 train periods, 12 test periods, 12-period step, one-period embargo,
  minimum three valid folds.
- Dollar-neutral gross 1.0, 5% name cap, minimum 20 names.
- AUM $100 million; 0.25 bps commission, 2 bps half spread, 10 bps square-root
  impact, 50 bps annual borrow, 10% maximum ADV participation.
- Candidate allocation 20%; trial count 32; doubled-cost stress test.

Admission requires candidate OOS Sharpe >=0.50, deflated-Sharpe probability
>=0.95, blend Sharpe improvement >=0.05, positive stressed blend Sharpe,
average turnover <=0.70, absolute blend drawdown <=0.25, absolute baseline
correlation <=0.70, candidate and blend minimum gross deployment >=0.95,
participation compliance, and valid participation in every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop43-sue-mega-alpha-decision.json`. The production
registry may change only if every gate passes.

## Result

Decision: **REJECTED AT UPSTREAM GATE**. No costed walk-forward was run and the
production registry was not created or changed.

The current 21-day evaluation produced IC 0.01264, HAC t-statistic 1.9504,
quintile spread 0.167%, and spread hit rate 55.6%. Breadth remained strong: all
175 dates had at least 20 names, with a mean of 524.6. Top- and bottom-quintile
turnover were 0.388 and 0.396.

The positive spread and breadth passed, but HAC missed the preregistered 2.00
floor. The small miss is not rounded upward and no threshold was changed after
seeing the result. The factor remains queryable but is not admission-eligible.
