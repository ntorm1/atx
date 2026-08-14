# Fundamental signal improvement loop 53: operating-profitability closure

Status: preregistered for costed walk-forward from existing governed current
evidence; no Loop 53 backtest result inspected at registration time.

## Primary research and hypothesis

The profitability literature finds that operating profit matched to current
expenses predicts the cross-section of expected returns more strongly than
cruder income measures. Ball et al. report materially higher portfolio alpha
for operating than gross profitability
(https://doi.org/10.1016/j.jfineco.2015.02.004). A recent retrospective also
finds profitability subsumes broad quality-investing concepts
(https://doi.org/10.3386/w33601).

ATX hypothesis: the existing PIT `profitability_operating_profitability` factor
can clear standalone economics at $50 million even though it is already the
primary information source inside router v6.

## Frozen evidence and costed protocol

The governed same-cutoff evidence reports 21-day IC 0.0220, HAC t-statistic
2.97, positive 0.32% quintile spread, and 171 valid dates. This clears the
unchanged upstream IC/HAC/spread/breadth gate. A duplicate screen is omitted
because identical scans exceeded the hard cap in Loop 52.

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
`C:\atx\atx-factor\research\loop53-operating-profitability-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED AS AN ADDITIVE SLEEVE**. The production registry was not
created or changed.

The compact-output costed run completed in 20.2 seconds, eliminating the prior
console-serialization timeout. Results:

- Candidate OOS Sharpe: 0.6652, clearing the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.4659 versus the 0.95 floor.
- Baseline Sharpe: 0.5352; blend Sharpe: 0.5739; marginal Sharpe: +0.0388
  versus the +0.05 floor.
- Candidate/baseline correlation: 0.8694 versus the 0.70 ceiling.
- Doubled-cost blend Sharpe remained positive at 0.4863.
- Candidate turnover was 0.1012, maximum drawdown 8.12%, and minimum gross
  deployment 0.9877; all passed.
- Blend deployment was 100% and maximum participation was 0.0871; both passed.

Failed gates were deflated-Sharpe probability, marginal mega-alpha Sharpe, and
baseline correlation. Evidence digest:
`aa62233e2897a1409cb7f9999e8ed01736e215159177477f163800d33cdd0139`.

The factor is not an independent mega-alpha sleeve. Its higher standalone
Sharpe does, however, expose a separate production question: should it replace
rather than blend with router v6? The additive admission protocol cannot answer
that high-correlation replacement question, so a preregistered replacement
challenge is the next engine improvement. No replacement is made in this loop.
