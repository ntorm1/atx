# Fundamental signal improvement loop 46: low quarterly inventory change

Status: preregistered for current-data validation and costed walk-forward; no
Loop 46 result inspected at registration time.

## Primary research and hypothesis

Thomas and Zhang document future-return predictability in inventory changes
(https://doi.org/10.1023/A:1020221918065). Belo and Lin establish a broader
inventory-growth spread (https://doi.org/10.1093/rfs/hhr069). The mechanism is
consistent with high inventory investment forecasting lower expected returns.

ATX hypothesis: the governed low quarterly inventory-change factor is a more
focused and capacity-feasible quality sleeve than the broader Loop 44 accrual
composite.

## Candidate and prior evidence

Candidate: `investment_low_quarterly_inventory_change`, defined as negative
sequential-quarter inventory change divided by average current/prior assets,
then PIT winsorized and standardized. Loop 26 produced 50,543 rows across 421
securities and 173 dates with zero quality defects. Its prior 21-day
IC/HAC/spread were 0.0161/2.42/0.383%.

Loop 46 requires current 21-day positive IC, HAC >=2, positive quintile spread,
at least 36 dates with 20 names, and zero defects before costed testing.

## Frozen costed walk-forward

Baseline: `composite_operating_profitability_or_net_issuance` (router v6).
AUM is **$50 million**, independently fixed by the candidate-blind Loop 45
router capacity frontier. All prior $100 million decisions remain immutable.

- 21-trading-day horizon; expanding 60/12/12 walk-forward, one-period embargo,
  minimum three valid folds.
- Dollar-neutral gross 1.0, 5% name cap, minimum 20 names.
- 0.25 bps commission, 2 bps half spread, 10 bps square-root impact, 50 bps
  annual borrow, 10% maximum ADV participation.
- Candidate allocation 20%; trial count 32; doubled-cost stress test.

Admission requires candidate OOS Sharpe >=0.50, deflated-Sharpe probability
>=0.95, blend Sharpe improvement >=0.05, positive stressed blend Sharpe,
average turnover <=0.70, absolute blend drawdown <=0.25, absolute baseline
correlation <=0.70, candidate and blend minimum gross deployment >=0.95,
participation compliance, and valid participation in every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop46-quarterly-inventory-change-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The fast upstream screen completed in 15.878 seconds and reproduced 21-day IC
0.01614 and HAC t-statistic 2.4198. The previously persisted same-data quintile
spread was positive at 0.383%, so the candidate cleared the research gate.

The frozen $50 million costed walk-forward rejected the candidate:

- Candidate OOS Sharpe: 0.0626 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.0291 versus the 0.95 floor.
- Baseline Sharpe: 0.5352; blend Sharpe: 0.5381; marginal Sharpe: +0.0029.
- Doubled-cost blend Sharpe remained positive at 0.4436.
- Candidate average turnover was 0.2652 and maximum drawdown was 9.26%.
- Candidate minimum gross deployment was 0.6441 versus the 0.95 floor.
- Baseline and blend deployment were 100%; blend maximum participation was
  0.0882, below the 0.10 ceiling.
- Baseline correlation was only 0.0416.

Failed gates were candidate OOS Sharpe, deflated-Sharpe probability, marginal
mega-alpha Sharpe, and candidate deployment. Evidence digest:
`34c15cd51ee87c512a47e15e7bf40c94772dd3600d6fe2a8c7ee222b5548fc59`.

The key production result is that candidate-independent capacity calibration
removed the inherited baseline/blend failures without weakening any gate. The
remaining rejection is attributable to candidate economics and its own sparse
liquidity. Router v6 remains production.
