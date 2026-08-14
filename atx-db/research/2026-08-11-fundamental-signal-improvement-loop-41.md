# Fundamental signal improvement loop 41: cash-flow profitability mega-alpha closure

Status: preregistered for current-data revalidation and costed walk-forward; no
Loop 41 backtest result inspected at registration time.

## Primary research and hypothesis

Ball, Gerakos, Linnainmaa, and Nikolaev (2016) find that cash-based operating
profitability outperforms profitability measures containing accruals and
subsumes the accrual anomaly:
https://doi.org/10.1016/j.jfineco.2016.03.002.

ATX hypothesis: the existing point-in-time operating-cash-flow-to-assets signal
retains its positive predictive power and can add a capacity-feasible sleeve to
production router v6 after costs.

## Candidate and upstream evidence

Candidate: `profitability_operating_cash_flow_to_assets`. The live surface has
86,960 rows, 863 securities, 175 dates from 2012-04-30 through 2026-06-15, and
zero non-finite or future-availability rows.

Loop 12 reported 21/63/126/252-day IC of
0.02316/0.03240/0.03798/0.05365 and HAC t-statistics of
2.95/2.66/2.36/3.15. Loop 41 requires the current governed evaluation to retain
a positive 252-day IC, HAC >=2, a positive quintile spread, at least 36 dates
with 20 names, and zero quality defects before costed testing.

## Frozen costed walk-forward

Baseline: production router v6,
`composite_operating_profitability_or_net_issuance`.

- 21-trading-day holding horizon; chronological expanding walk-forward.
- Minimum 60 train periods, 12 test periods, 12-period step, one-period embargo,
  minimum three valid folds.
- Dollar-neutral gross 1.0, 5% name cap, minimum 20 names.
- AUM $100 million; 0.25 bps commission, 2 bps half spread, 10 bps square-root
  impact, 50 bps annual short borrow, 10% maximum ADV participation.
- Candidate allocation: 20%; trial count: 32; doubled-cost stress test.

Admission requires candidate OOS Sharpe >=0.50, deflated-Sharpe probability
>=0.95, blend Sharpe improvement >=0.05, positive stressed blend Sharpe,
average turnover <=0.70, absolute blend drawdown <=0.25, absolute baseline
correlation <=0.70, candidate and blend minimum gross deployment >=0.95,
participation compliance, and valid participation in every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop41-cash-flow-profitability-mega-alpha-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The current governed evaluation reproduced the positive average rank-IC
evidence in 16.302 seconds. The 21/63/126/252-day ICs were
0.02316/0.03240/0.03798/0.05365 with HAC t-statistics of
2.95/2.66/2.36/3.15. Breadth was strong: all 175 dates had at least 20 names,
with a mean of 496.9.

However, the candidate failed the preregistered upstream gate because the
21/63/126/252-day quintile long-short spreads were all negative:
-0.174%/-0.279%/-0.638%/-2.325%. This tail reversal is economically important
despite the positive average rank correlations. Top- and bottom-quintile
turnover were 0.143 and 0.189.

A costed run was inadvertently allowed to complete after the upstream spread
failure. It is retained as immutable supplementary rejection evidence, not as
an admission-eligible test. It also rejected the candidate decisively:

- Candidate OOS Sharpe: 0.2027 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.0731 versus the 0.95 floor.
- Baseline Sharpe: 0.5803; blend Sharpe: 0.5328; marginal Sharpe: -0.0474.
- Doubled-cost blend Sharpe remained positive at 0.4453.
- Candidate and blend minimum gross deployment were 0.5785 and 0.8381 versus
  the 0.95 floor.
- Blend maximum ADV participation was 0.1178 versus the 0.10 ceiling.
- Baseline correlation was acceptable at 0.6378, average candidate turnover
  was 0.1224, and candidate maximum drawdown was acceptable at 13.02%.

Failed costed gates were candidate OOS Sharpe, deflated-Sharpe probability,
marginal mega-alpha Sharpe, blend participation, candidate deployment, and
blend deployment. Evidence digest:
`1e1f079b515963edbdd33779a9133a4cc29cdafba2bf9e1e9ae2d39cac0f3473`.

The result demonstrates why ATX requires portfolio diagnostics and costed OOS
evidence instead of promoting factors from IC alone. Router v6 remains the
production factor.
