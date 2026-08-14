# Fundamental signal improvement loop 51: quarterly revenue growth closure

Status: preregistered for current-data validation and costed walk-forward; no
Loop 51 result inspected at registration time.

## Primary research and hypothesis

Jegadeesh and Livnat find that revenue surprises contain information about
future stock returns beyond earnings surprises
(https://doi.org/10.1016/j.jacceco.2005.10.003). Revenue growth is not identical
to analyst surprise, but a PIT seasonal growth signal tests whether recently
disclosed top-line momentum carries a related continuation effect without
requiring licensed forecasts.

ATX hypothesis: governed quarterly year-over-year revenue growth is a distinct,
capacity-feasible tactical sleeve that improves router v6 at $50 million.

## Candidate and prior evidence

Candidate: `growth_quarterly_revenue_yoy`, using exact quarter revenue divided
by the closest same-season prior report 330-400 days earlier, with both facts
visible by the monthly decision. Loop 28 produced 33,088 rows, 439 securities,
and 161 dates with zero null, non-finite, future-knowledge, or duplicate rows.
Its prior 21-day IC/HAC/spread were 0.02251/2.0031/0.358%.

Loop 51 requires current 21-day positive IC, HAC >=2, positive quintile spread,
at least 36 dates with 20 names, and zero defects before costed testing. The HAC
threshold is literal; a value below 2.0 will not be rounded upward.

## Frozen costed walk-forward

Baseline: `composite_operating_profitability_or_net_issuance` (router v6). AUM
is $50 million from the candidate-independent Loop 45 capacity frontier.

- 21-trading-day horizon; expanding 60/12/12 walk-forward, one-period embargo,
  minimum three folds.
- Dollar-neutral gross 1.0, 5% name cap, minimum 20 names.
- 0.25 bps commission, 2 bps half spread, 10 bps square-root impact, 50 bps
  annual borrow, 10% maximum ADV participation.
- Candidate allocation 20%; trial count 32; doubled-cost stress test.

Admission uses unchanged gates: candidate OOS Sharpe >=0.50, deflated-Sharpe
probability >=0.95, marginal blend Sharpe >=0.05, positive stressed Sharpe,
turnover <=0.70, drawdown <=0.25, correlation <=0.70, minimum candidate/blend
deployment >=0.95, participation compliance, and every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop51-quarterly-revenue-growth-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The current fast screen completed in 7.684 seconds. The 21-day IC was 0.02251,
HAC t-statistic 2.0031, with 159 dates and 203.4 mean names. This literally
cleared the frozen HAC floor; the stored current-data quintile spread was
positive at 0.358%.

The frozen $50 million costed walk-forward rejected the candidate:

- Candidate OOS Sharpe: 0.0040 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.0189 versus the 0.95 floor.
- Baseline Sharpe: 0.5841; blend Sharpe: 0.5070; marginal Sharpe: -0.0772.
- Doubled-cost blend Sharpe remained positive at 0.4278.
- Candidate average turnover was 0.2292 and maximum drawdown was 20.15%.
- Candidate minimum gross deployment was 0.6629 versus the 0.95 floor.
- Blend deployment was 100% and maximum participation was 0.0909.
- Baseline correlation was acceptable at 0.3482.

Failed gates were candidate OOS Sharpe, deflated-Sharpe probability, marginal
mega-alpha Sharpe, and candidate deployment. Evidence digest:
`665eae870dbe4731f615d1cf3d8f6c79fbce0b890ad52b2ccdfb33d63d2a81ad`.

The factor remains production-queryable but is not a tradeable mega-alpha
sleeve. Router v6 remains production.
