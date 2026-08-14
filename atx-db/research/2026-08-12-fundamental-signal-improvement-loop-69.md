# Fundamental signal improvement loop 69: net operating assets asymmetric recovery

Status: **not accepted into production mega-alpha; admitted to zero-capital shadow monitoring**.

## Research and frozen candidate

The loop researched fundamental anomalies whose payoff can be concentrated in one portfolio leg.
Financial-distress evidence shows that long/short anomaly profitability can reside predominantly in
one extreme and that distressed/high-risk shorts can behave differently from the broad universe
(https://www.sciencedirect.com/science/article/pii/S0304405X12002176). A broader accounting-anomaly
study likewise finds institutional implementation failures concentrated in short legs
(https://www.lehigh.edu/~xuy219/research/RAST_2024.pdf).

A local evidence audit selected the existing governed `quality_net_operating_assets` factor rather
than mining a new formula. Hirshleifer, Hou, Teoh, and Zhang's NOA anomaly predicts higher returns
for firms with low cumulative operating investment; the standard definition subtracts cash and
financial claims from operating assets and liabilities
(https://www.federalreserve.gov/pubs/ifdp/2012/1070/ifdp1070.htm). Later work continues to report a
negative NOA/return relation (https://doi.org/10.1002/rfe.1098).

This was the strongest plausible recovery candidate from prior ATX decisions:

- former continuous-rank costed Sharpe 0.594;
- router correlation -0.163;
- former 20% blend Sharpe 0.653, improving router Sharpe by 0.211;
- every economic gate passed; only the former DSR threshold failed.

The governed factor formula, orientation, availability policy, and observations were unchanged.
Higher ATX scores continue to mean lower NOA and therefore higher expected returns.

## Preregistered 18-trial search

Before loading returns, six constructions times three allocations were committed to
`atx-factor/research/trial-ledger.json`, raising the cumulative research count to 108:

1. continuous cross-sectional rank;
2. continuous raw standardized value;
3. symmetric quintile tails;
4. symmetric decile tails;
5. top quintile long versus the broad non-top universe;
6. broad non-bottom universe versus the bottom quintile short;

each at 10%, 20%, and 30% router allocation. Selection ended 2020-08-31, September was embargoed,
and only the frozen winner touched returns from 2020-10-30 onward.

The selection sample identified **top quintile versus universe** as the strongest construction,
confirming that NOA's useful ATX payoff is predominantly in the low-NOA long leg:

| Selection construction | Candidate Sharpe | 20% combined Sharpe | 20% marginal Sharpe |
|---|---:|---:|---:|
| Top quintile vs universe | 1.074 | 0.626 | +0.339 |
| Symmetric quintile tails | 0.918 | 0.588 | +0.302 |
| Continuous raw | 0.953 | 0.582 | +0.295 |
| Symmetric decile tails | 0.761 | 0.578 | +0.291 |
| Continuous rank | 0.868 | 0.511 | +0.224 |
| Universe vs bottom quintile | 0.160 | 0.334 | +0.047 |

The 30% top-versus-universe variant had higher gross economics but breached the frozen 10% ADV
participation ceiling at 12.28%, so the selector correctly chose the feasible 20% allocation.

## Untouched validation

The final sample contains 67 monthly 21-trading-day observations through 2026-04-30.

| Validation result | NOA candidate | Router v6 | 80/20 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.671 | 0.431 | 0.650 | 0.566 |
| Annualized return | 3.76% | 1.51% | 2.28% | 1.97% |
| Maximum drawdown | -5.80% | -10.34% | -9.20% | -9.40% |
| Average turnover | 0.066 | 0.060 | 0.064 | 0.064 |
| Annualized cost drag | 0.30% | 0.29% | 0.30% | 0.59% |
| Maximum ADV participation | 13.71%* | 9.21% | 8.16% | 8.16% |
| DSR probability (108 trials) | 11.86% | 7.09% | 16.02% | 11.70% |

`*` The candidate diagnostic represents deploying the full $50 million into the sleeve. The actual
20% combined allocation passes the 10% participation ceiling and remains fully deployed.

The candidate improves router Sharpe by **+0.2186**, has only 0.098 correlation with the router,
survives doubled costs, and is positive in four of six validation folds. Standalone probabilistic
Sharpe is 97.35%, above 95%. Every frozen economic and operational gate passes.

The only failed production gate is deflated-Sharpe probability: 11.86% versus 95%. With 108
committed research trials and only 67 untouched months, promoting the result would ignore the
selection burden. Decision: **do not add NOA to production mega-alpha**.

## Shadow disposition and acceptance hardening

An ordinary rejection would also be wrong: the construction has robust positive untouched
economics, material diversification, and operational feasibility. The engine now distinguishes
three dispositions:

- `accepted`: every production gate passes;
- `shadow`: every economic/operational gate and 95% probabilistic Sharpe pass, while cumulative
  trial-adjusted DSR alone remains insufficient;
- `rejected`: any economic, stability, cost, capacity, correlation, or ordinary statistical gate
  fails.

Loop 69 is the first `shadow` candidate. It is recorded atomically in
`atx-factor/research/shadow-alpha-registry.json` with **zero capital**, frozen construction and
allocation, validation boundary, evidence digest, and promotion policy. It may be retested only
after genuinely new formation dates arrive. Promotion will still require every production gate;
validation-period tuning is prohibited. Router v6 and the production mega-alpha registry remain
unchanged.

Decision artifact:
`atx-factor/research/loop69-net-operating-assets-exploration.json`.
Evidence SHA-256:
`2364e9270f5e5ab595c4c30c7dfe040e5088bf6450cb13e45d4c4b73cb808524`.

## Verification

- Twenty-four targeted exploration, shadow-registry, registry, CLI, portfolio, backtest, and
  mega-alpha tests pass across two bounded groups.
- Ruff passes on the changed engine and tests.
- Trial ledger contains 108 cumulative trials; the shadow registry contains one zero-capital
  candidate.
- No full test suite was run.
