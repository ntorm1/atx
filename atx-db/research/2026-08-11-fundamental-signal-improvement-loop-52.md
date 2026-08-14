# Fundamental signal improvement loop 52: Ball operating profitability closure

Status: preregistered for current-data validation and costed walk-forward; no
Loop 52 result inspected at registration time.

## Primary research and hypothesis

Ball, Gerakos, Linnainmaa, and Nikolaev construct an operating-profitability
measure that better matches current revenue and expenses and find a stronger
expected-return relation than gross profit or bottom-line earnings
(https://doi.org/10.1016/j.jfineco.2015.02.004).

ATX hypothesis: the existing literal Ball operating-profitability factor can
qualify as an independent sleeve rather than only informing router construction.

## Candidate and prior evidence

Candidate: `profitability_ball_operating_profitability`. The governed PIT
surface has 69,270 rows, 777 securities, and 176 dates. Loop 3 reported
21-day IC 0.02139, HAC t-statistic 2.6959, and a positive but small 0.056%
quintile spread. The factor is highly correlated with cash operating
profitability, so the frozen baseline-correlation gate is particularly relevant.

Loop 52 requires current 21-day positive IC, HAC >=2, positive quintile spread,
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

Admission uses unchanged gates: candidate OOS Sharpe >=0.50, deflated-Sharpe
probability >=0.95, marginal blend Sharpe >=0.05, positive stressed Sharpe,
turnover <=0.70, drawdown <=0.25, correlation <=0.70, minimum candidate/blend
deployment >=0.95, participation compliance, and every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop52-ball-operating-profitability-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The duplicate fast screen exceeded its hard cap; stored governed evidence from
the identical warehouse cutoff supplied the eligible 21-day IC/HAC/spread. The
costed decision artifact completed before the shell's 45-second output cap, so
the immutable JSON was inspected directly rather than rerunning the backtest.

- Candidate OOS Sharpe: 0.4790 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.2542 versus the 0.95 floor.
- Baseline Sharpe: 0.5352; blend Sharpe: 0.5675; marginal Sharpe: +0.0323
  versus the +0.05 floor.
- Doubled-cost blend Sharpe remained positive at 0.4818.
- Candidate average turnover was 0.0985 and maximum drawdown was 11.38%.
- Candidate minimum gross deployment was 0.8291 versus the 0.95 floor.
- Blend deployment was 100% and maximum participation was 0.0881.
- Baseline correlation was acceptable at 0.6405.

Failed gates were candidate OOS Sharpe, deflated-Sharpe probability, marginal
mega-alpha Sharpe, and candidate deployment. Evidence digest:
`148eccf42e935af4aad676891816578046967136ba977e16ba25ffa8ad3197d8`.

The factor is close on raw Sharpe but decisively fails multiple-testing and
deployment standards. Router v6 remains production.
