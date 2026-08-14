# Fundamental signal improvement loop 70: quarterly operating accruals recovery

Status: **rejected after asymmetric construction selection and untouched validation**.

## Research and frozen candidate

Operating accruals are a classic earnings-quality signal: high accruals imply that a larger share of
reported earnings is non-cash and historically predict lower future returns. Chan, Chan, Jegadeesh,
and Lakonishok report a reliable negative relation between accruals and subsequent returns
(https://www.nber.org/papers/w8308). Quarterly evidence finds that markets overestimate the
persistence of the accrual component and that quarterly accrual mispricing is distinct from
post-earnings-announcement drift
(https://doi.org/10.1016/S0165-4101(00)00015-X). Portfolio research describes the conventional
trade as long the lowest-accrual firms and short the highest-accrual firms
(https://doi.org/10.2469/faj.v64.n3.7).

Loop 70 recovered the existing governed
`quality_low_quarterly_operating_working_capital_accruals` feature. No formula, direction,
availability rule, or observation was changed. The prior fixed continuous-rank test had low router
correlation (0.179), positive standalone Sharpe (0.240), and +0.039 marginal router Sharpe, making
it plausible that an isolated long or short leg could improve implementation.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the durable trial count from 108 to 126. The constructions were continuous
rank, continuous raw score, symmetric quintile and decile tails, top quintile versus universe, and
universe versus bottom quintile. Selection ended 2020-07-31, August 2020 was embargoed, and only
the frozen winner was evaluated from 2020-09-30 onward.

Early history selected **universe versus bottom quintile at 30%**. Because higher ATX factor scores
mean lower accruals, its bottom quintile is the high-accrual short extreme. Selection results were:

- candidate Sharpe: 0.160;
- router Sharpe: 0.403;
- 70/30 combined Sharpe: 0.506;
- marginal Sharpe: +0.103;
- turnover: 0.170;
- maximum ADV participation: 6.10%.

The selector therefore found the economically expected high-accrual short leg and a feasible
allocation without using validation returns.

## Untouched validation

The validation segment contains 68 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | -0.596 | 0.393 | 0.090 | 0.004 |
| Annualized return | -2.50% | 1.37% | 0.28% | -0.06% |
| Maximum drawdown | -15.62% | -10.34% | -10.62% | -10.89% |
| Average turnover | 0.217 | 0.060 | 0.125 | 0.125 |
| Annualized cost drag | 0.42% | 0.30% | 0.34% | 0.69% |
| Maximum ADV participation | 8.88% | 9.21% | 8.53% | 8.53% |
| Probabilistic Sharpe | 5.41% | 81.36% | 58.21% | 50.41% |
| DSR probability (126 trials) | 0.0002% | 5.29% | 0.94% | 0.49% |

Candidate/router correlation was only 0.239, so redundancy did not cause failure. Instead, the
high-accrual short payoff reversed: only one of six validation folds was positive, candidate Sharpe
fell to -0.596, and adding the sleeve reduced router Sharpe by 0.303. The candidate failed
standalone Sharpe, DSR, marginal mega-alpha Sharpe, and positive-fold stability.

Decision: **reject quarterly operating working-capital accruals from mega-alpha**. The result is not
shadow eligible because ordinary statistical, economic, and stability gates all fail. The governed
feature remains available in atx-db for research and model attribution, but its consumed validation
period must not be used to reverse the sign or tune another portfolio. Router v6, production
registry, and the Loop 69 NOA shadow construction remain unchanged.

Decision artifact:
`atx-factor/research/loop70-quarterly-operating-accruals-exploration.json`.
Evidence SHA-256:
`d1bde8884bd6d763b5eb8fd5e62457e1c9ce56bb02f34c2dbaf0965582546a68`.

## Verification

- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 126 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production registry
  exists.
- No full test suite was run.
