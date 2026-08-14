# Fundamental signal improvement loop 44: quarterly working-capital accruals closure

Status: preregistered for current-data revalidation and costed walk-forward; no
Loop 44 backtest result inspected at registration time.

## Primary research and hypothesis

Sloan (1996) establishes the negative relation between accruals and subsequent
returns (https://doi.org/10.2308/tar-9608042309). Collins and Hribar show that
the accrual anomaly also holds in quarterly data and is distinct from PEAD
(https://doi.org/10.1016/S0165-4101(00)00015-X). Later U.S. evidence warns that
the anomaly has decayed (https://doi.org/10.1287/mnsc.1110.1320), making a
current costed test essential.

ATX hypothesis: the governed low quarterly operating-working-capital-accruals
factor is a distinct, investable quality sleeve for router v6.

## Candidate and upstream evidence

Candidate: `quality_low_quarterly_operating_working_capital_accruals`, defined
as the negative of `(dAR + dInventory - dDeferredRevenue - dAP)` divided by
lagged assets, then winsorized and standardized point in time. Loop 25 produced
56,805 rows, 479 securities, and 173 dates with zero quality defects. Its
21-day IC/HAC/spread were 0.0146/2.76/0.112%; mean correlation to router v6 was
only 0.065.

Loop 44 requires current 21-day positive IC, HAC >=2, positive quintile spread,
at least 36 dates with 20 names, and zero defects before costed testing.

## Frozen costed walk-forward

Baseline: `composite_operating_profitability_or_net_issuance` (router v6).

- 21-trading-day horizon; expanding 60/12/12 walk-forward with one-period
  embargo and at least three folds.
- Dollar-neutral gross 1.0, 5% name cap, minimum 20 names.
- AUM $100 million; 0.25 bps commission, 2 bps half spread, 10 bps square-root
  impact, 50 bps annual borrow, and 10% maximum ADV participation.
- Candidate allocation 20%; trial count 32; doubled-cost stress test.

Admission requires candidate OOS Sharpe >=0.50, deflated-Sharpe probability
>=0.95, blend Sharpe improvement >=0.05, positive stressed blend Sharpe,
average turnover <=0.70, absolute blend drawdown <=0.25, absolute baseline
correlation <=0.70, candidate and blend minimum gross deployment >=0.95,
participation compliance, and valid participation in every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop44-quarterly-accruals-mega-alpha-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The full diagnostic exceeded the 30-second command cap after persisting its
results, so the fast screen was used to confirm IC/HAC in 15.219 seconds. The
current 21-day IC was 0.01464, HAC t-statistic 2.7558, and persisted quintile
spread 0.261%, clearing the upstream gate.

The frozen costed walk-forward rejected the candidate:

- Candidate OOS Sharpe: 0.2405 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.0913 versus the 0.95 floor.
- Baseline Sharpe: 0.5803; blend Sharpe: 0.6191; marginal Sharpe: +0.0388
  versus the +0.05 floor.
- Doubled-cost blend Sharpe remained positive at 0.5143.
- Candidate average turnover was 0.2786 and maximum drawdown was 5.68%.
- Candidate and blend minimum gross deployment were 0.4163 and 0.8397 versus
  the 0.95 floor.
- Blend maximum ADV participation was 0.1246 versus the 0.10 ceiling.
- Baseline correlation was an attractive 0.1789.

Failed gates were candidate OOS Sharpe, deflated-Sharpe probability, marginal
mega-alpha Sharpe, blend participation, candidate deployment, and blend
deployment. Evidence digest:
`978b6cee9fb5360fe45fcaf250dadadf4dc9dea9e299c454cfa6804e6509832f`.

The sleeve is the best incremental candidate in Loops 40-44 but does not clear
the preregistered standard. It remains a useful research feature; router v6
remains production.
