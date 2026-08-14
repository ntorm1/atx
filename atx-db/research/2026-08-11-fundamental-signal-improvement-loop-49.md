# Fundamental signal improvement loop 49: quarterly cash profitability closure

Status: preregistered for current-data validation and costed walk-forward; no
Loop 49 result inspected at registration time.

## Primary research and hypothesis

Ball, Gerakos, Linnainmaa, and Nikolaev show that cash-based operating
profitability is a stronger return predictor than operating profitability with
accruals and can subsume the accrual anomaly
(https://doi.org/10.1016/j.jfineco.2016.03.002).

ATX hypothesis: the quarterly PIT adaptation, already used only as router v6's
secondary within-decile key, may also qualify as an independent $50 million
mega-alpha sleeve.

## Candidate and prior evidence

Candidate: `profitability_quarterly_cash_operating_profitability_lagged_assets`,
defined as quarterly operating profit adjusted for operating-working-capital
changes, divided by lagged assets. Loop 24 produced 56,805 rows, 479 securities,
and 173 dates with zero quality defects. Its 21-day IC/HAC/spread were
0.0240/2.76/0.098%; correlation to the then-production router was 0.303.

Loop 49 requires current 21-day positive IC, HAC >=2, positive quintile spread,
at least 36 dates with 20 names, and zero defects before costed testing.

## Frozen costed walk-forward

Baseline: `composite_operating_profitability_or_net_issuance` (router v6). AUM
is $50 million from the candidate-independent Loop 45 frontier.

- 21-trading-day horizon; expanding 60/12/12 walk-forward, one-period embargo,
  minimum three folds.
- Dollar-neutral gross 1.0, 5% name cap, minimum 20 names.
- 0.25 bps commission, 2 bps half spread, 10 bps square-root impact, 50 bps
  annual borrow, 10% maximum ADV participation.
- Candidate allocation 20%; trial count 32; doubled-cost stress test.

Admission uses the unchanged gates: candidate OOS Sharpe >=0.50,
deflated-Sharpe probability >=0.95, marginal blend Sharpe >=0.05, positive
stressed Sharpe, turnover <=0.70, drawdown <=0.25, correlation <=0.70, minimum
candidate/blend deployment >=0.95, participation compliance, and every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop49-quarterly-cash-profitability-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The redundant refreshed screen exceeded its 25-second cap and left a DuckDB
worker alive; that exact orphan was terminated. The governed stored evaluation
already covers the same warehouse cutoff and supplies the preregistered 21-day
IC 0.0240, HAC 2.76, positive 0.098% spread, and valid breadth. No evidence
threshold was changed.

After removing the orphan, the frozen $50 million costed run completed and
rejected the candidate:

- Candidate OOS Sharpe: 0.2647 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.0951 versus the 0.95 floor.
- Baseline Sharpe: 0.5352; blend Sharpe: 0.5127; marginal Sharpe: -0.0225.
- Doubled-cost blend Sharpe remained positive at 0.4297.
- Candidate average turnover was 0.2191 and maximum drawdown was 16.37%.
- Candidate minimum gross deployment was 0.8007 versus the 0.95 floor.
- Blend deployment was 100% and maximum participation was 0.0819.
- Baseline correlation was acceptable at 0.6067.

Failed gates were candidate OOS Sharpe, deflated-Sharpe probability, marginal
mega-alpha Sharpe, and candidate deployment. Evidence digest:
`730d5440a93147eed6d077c0eca909f9750d10931bb09d7120f912695c5a1df7`.

The standalone sleeve does not qualify. Its existing use as a secondary rank
inside router v6 remains unchanged because this experiment did not test or
invalidate that constrained role.
