# Fundamental signal improvement loop 40: Altman strength mega-alpha closure

Status: preregistered for current-data revalidation and costed walk-forward; no
Loop 40 backtest result inspected at registration time.

## Primary research and hypothesis

Altman (1968) established the multivariate financial-ratio bankruptcy model.
Campbell, Hilscher, and Szilagyi (2008) show that distressed stocks earn
anomalously low returns rather than a simple risk premium. Primary publications:

- https://doi.org/10.1111/j.1540-6261.1968.tb00843.x
- https://doi.org/10.1111/j.1540-6261.2008.01416.x

ATX hypothesis: the existing corrected point-in-time Altman financial-strength
score remains positively predictive after current-data revalidation and can add
an independent sleeve to production router v6 after costs.

## Candidate and upstream evidence

Candidate: `distress_altman_z_score`. Its corrected public-company formula uses
market equity divided by total liabilities, not the broken legacy total-debt
substitution. The live surface has 30,875 unique rows, 441 securities, 173 dates,
and zero non-finite or future-availability rows.

Loop 13 reported 21/63/126/252-day IC of
0.02503/0.04274/0.04962/0.06890, HAC t-statistics of
2.30/2.44/2.14/2.39, and positive spreads at every horizon. Loop 40 requires a
current full governed evaluation to reproduce the 252-day positive IC, HAC >=2,
positive quintile spread, at least 36 dates with 20 names, and zero quality
defects before proceeding.

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

Admission requires every engine gate: candidate OOS Sharpe >=0.50, deflated
Sharpe probability >=0.95, blend Sharpe improvement >=0.05, positive stressed
blend Sharpe, average turnover <=0.70, absolute blend drawdown <=0.25, absolute
baseline correlation <=0.70, candidate and blend minimum gross deployment
>=0.95, participation compliance, and valid participation in every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop40-altman-mega-alpha-decision.json`. The atomic
mega-alpha registry may be created or changed only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The current governed upstream evaluation reproduced the prior evidence in
10.876 seconds. The 21/63/126/252-day rank ICs were
0.02503/0.04274/0.04962/0.06890 and the corresponding HAC t-statistics were
2.30/2.44/2.14/2.39. Quintile long-short spreads were positive at every horizon:
0.314%/0.641%/1.169%/3.298%. All 173 dates had at least 20 names; mean breadth
was 178.5 names. Top- and bottom-quintile turnover were 0.151 and 0.179.

The frozen costed walk-forward then rejected the candidate:

- Candidate OOS Sharpe: 0.1860 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.0629 versus the 0.95 floor.
- Baseline Sharpe: 0.5620; 20% blend Sharpe: 0.5276; marginal Sharpe: -0.0344.
- Doubled-cost blend Sharpe remained positive at 0.4402.
- Candidate and blend minimum gross deployment were 0.3220 and 0.8381 versus
  the 0.95 floor.
- Blend maximum ADV participation was 0.1186 versus the 0.10 ceiling.
- Baseline correlation was acceptable at 0.3664, average turnover was 0.1298,
  and candidate maximum drawdown was acceptable at 16.85%.

Failed gates were candidate OOS Sharpe, deflated-Sharpe probability, marginal
mega-alpha Sharpe, blend participation, candidate deployment, and blend
deployment. Evidence digest:
`2a8470036b72530660092e5cf9f77b38f744ab16e5e49bb86945693cee774dfe`.

This closes the attractive in-sample/PIT IC result without promoting a weak or
capacity-infeasible production sleeve. The existing router v6 remains the
production factor.
