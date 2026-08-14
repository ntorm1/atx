# Fundamental signal improvement loop 48: earnings/revenue agreement

Status: preregistered for current-data validation and costed walk-forward; no
Loop 48 result inspected at registration time.

## Primary research and hypothesis

Jegadeesh and Livnat find that revenue surprises predict future returns beyond
earnings surprises and that PEAD is stronger when revenue and earnings
surprises have the same sign
(https://doi.org/10.1016/j.jacceco.2005.10.003). Revenue agreement can therefore
identify the more persistent subset of earnings news.

ATX hypothesis: the existing PIT SUE/revenue-sign-agreement factor is a focused,
independent tactical sleeve that improves router v6 at the independently
calibrated $50 million AUM.

## Candidate and prior evidence

Candidate: `earnings_sue_revenue_agreement`. It retains SUE as the ranking value
only on exact security/date observations where PIT revenue surprise has the same
sign. Loop 11 produced 32,731 rows, 859 securities, and 175 dates, with monthly
breadth 42-359 and zero quality defects. Stored current evidence reports 21-day
IC 0.0238, HAC 2.42, and a 0.97% quintile spread.

Loop 48 requires current 21-day positive IC, HAC >=2, positive quintile spread,
at least 36 dates with 20 names, and zero defects before costed testing.

## Frozen costed walk-forward

Baseline: `composite_operating_profitability_or_net_issuance` (router v6). AUM
is $50 million from the candidate-independent Loop 45 capacity frontier.

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
`C:\atx\atx-factor\research\loop48-earnings-revenue-agreement-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The fast current-data screen completed in 8.725 seconds and reproduced 21-day
IC 0.02382 and HAC t-statistic 2.4246. The stored current-data quintile spread
was positive at 0.97%, clearing the upstream gate.

The frozen $50 million costed walk-forward rejected the candidate:

- Candidate OOS Sharpe: 0.2932 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.1185 versus the 0.95 floor.
- Baseline Sharpe: 0.4418; blend Sharpe: 0.4772; marginal Sharpe: +0.0355
  versus the +0.05 floor.
- Doubled-cost blend Sharpe remained positive at 0.3863.
- Candidate average turnover was 0.3022 and maximum drawdown was 13.67%.
- Candidate, baseline, and blend minimum deployment were effectively 100%.
- Blend maximum participation was 0.0731, below the 0.10 ceiling.
- Baseline correlation was acceptable at 0.3004.

Only three economic gates failed: candidate OOS Sharpe, deflated-Sharpe
probability, and marginal mega-alpha Sharpe. All capacity, turnover, drawdown,
stress, and correlation gates passed. Evidence digest:
`7ddddb9da01d56966251faadca28130cf0bf3f32c1842983d9a9bb40f5d291aa`.

This is the cleanest capacity result so far, but the signal is not strong enough
for admission. It remains a queryable tactical feature; router v6 remains
production.
