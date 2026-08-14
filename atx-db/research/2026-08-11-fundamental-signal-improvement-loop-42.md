# Fundamental signal improvement loop 42: earnings-confirmation mega-alpha closure

Status: preregistered for current-data revalidation and costed walk-forward; no
Loop 42 backtest result inspected at registration time.

## Primary research and hypothesis

Post-earnings-announcement drift is the tendency for returns to continue in the
direction of an earnings surprise after disclosure. Soffer and Lys (1999)
document delayed incorporation of predictable earnings information
(https://doi.org/10.1111/j.1911-3846.1999.tb00583.x), while Livnat, Qi, and Wu
show that analyst-based surprises generate stronger drift than simple
time-series surprises (https://doi.org/10.1111/j.1475-679X.2006.00196.x).

ATX hypothesis: the existing PIT combination of standardized unexpected EPS and
the first complete post-SEC-filing session reaction is an independent tactical
sleeve that improves production router v6 after realistic costs.

## Candidate and upstream evidence

Candidate: `earnings_sue_filing_confirmation`. It is already a governed,
queryable production feature with 98,485 rows, 1,113 securities, and 176 dates.
Its availability is the later of the earnings and post-filing-reaction inputs.

Loop 10 reported 21/63/126/252-day IC of
0.01346/0.01640/0.01922/0.00801. Its 21-day HAC t-statistic was 2.44, quintile
spread 0.256%, and router correlation on common keys only 0.018. Loop 42 requires
current 21-day positive IC, HAC >=2, positive quintile spread, at least 36 dates
with 20 names, and zero quality defects before costed testing.

## Frozen costed walk-forward

Baseline: `composite_operating_profitability_or_net_issuance` (router v6).

- 21-trading-day holding horizon; expanding walk-forward.
- Minimum 60 train periods, 12 test periods, 12-period step, one-period embargo,
  minimum three valid folds.
- Dollar-neutral gross 1.0, 5% name cap, minimum 20 names.
- AUM $100 million; 0.25 bps commission, 2 bps half spread, 10 bps square-root
  impact, 50 bps annual short borrow, 10% maximum ADV participation.
- Candidate allocation 20%; trial count 32; doubled-cost stress test.

Admission requires candidate OOS Sharpe >=0.50, deflated-Sharpe probability
>=0.95, blend Sharpe improvement >=0.05, positive stressed blend Sharpe,
average turnover <=0.70, absolute blend drawdown <=0.25, absolute baseline
correlation <=0.70, candidate and blend minimum gross deployment >=0.95,
participation compliance, and valid participation in every fold.

The immutable decision target is
`C:\atx\atx-factor\research\loop42-earnings-confirmation-mega-alpha-decision.json`.
The production registry may change only if every gate passes.

## Result

Decision: **REJECTED**. The production registry was not created or changed.

The governed revalidation retained the expected tactical signal. The
21/63/126/252-day ICs were 0.01346/0.01640/0.01922/0.00801 with HAC
t-statistics of 2.44/2.37/2.23/0.67. Corresponding quintile spreads were all
positive at 0.186%/0.687%/1.061%/1.315%. All 175 dates had at least 20 names;
mean breadth was 508.3.

The frozen costed walk-forward rejected the candidate:

- Candidate OOS Sharpe: 0.1387 versus the 0.50 floor.
- Candidate deflated-Sharpe probability: 0.0482 versus the 0.95 floor.
- Baseline Sharpe: 0.5803; blend Sharpe: 0.5982; marginal Sharpe: +0.0180
  versus the +0.05 floor.
- Doubled-cost blend Sharpe remained positive at 0.4951.
- Candidate average turnover was 0.3159 and maximum drawdown was 13.28%.
- Candidate and blend minimum gross deployment were 0.7167 and 0.8397 versus
  the 0.95 floor.
- Blend maximum ADV participation was 0.1284 versus the 0.10 ceiling.
- Baseline correlation was an acceptable 0.2591.

Failed gates were candidate OOS Sharpe, deflated-Sharpe probability, marginal
mega-alpha Sharpe, blend participation, candidate deployment, and blend
deployment. Evidence digest:
`3f071d2e86009fea028e4fca22aa1182ca0e5851a612668e3cacbf80bb52f9af`.

The feature remains useful as a queryable tactical diagnostic, but is not a
production mega-alpha sleeve. Router v6 remains the production factor.
