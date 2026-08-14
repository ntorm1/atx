# Fundamental signal improvement loop 72: continuous financial-strength recovery

Status: **rejected after preregistered construction selection and untouched validation**.

## Research and frozen candidate

Piotroski's original financial-statement score separates strong from weak value firms using nine
predefined profitability, funding, liquidity, and operating-efficiency signals
(https://www.anderson.ucla.edu/documents/areas/prg/asam/2019/F-Score.pdf). International evidence
finds that the score remains return-predictive and reports that more of the premium comes from the
strong-fundamentals long leg than the weak-fundamentals short leg
(https://link.springer.com/article/10.1057/s41260-020-00157-2). Research using quarterly FSCORE
also reports return continuation from average recent fundamental strength that is distinct from
price and earnings momentum
(https://www.sciencedirect.com/science/article/abs/pii/S0890838919300848).

Loop 72 recovered the existing governed `quality_continuous_financial_strength` feature. Its nine
annual components are rank-standardized, oriented so higher means stronger, and equal weighted;
there are no return-fitted component weights. No formula, direction, availability rule, or
observation was changed. The earlier monotonic test could not rule out the documented long-leg
asymmetry, and this feature had not entered the durable exploration ledger.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the cumulative trial count from 144 to 162. The constructions were continuous
rank, continuous raw score, symmetric quintile and decile tails, top quintile versus universe, and
universe versus bottom quintile. Selection ended 2020-08-31, September 2020 was embargoed, and
only the frozen winner was evaluated from 2020-10-30 onward.

Early history selected **symmetric decile tails at 30%**, rather than the hypothesized isolated
strong-fundamentals long leg:

- candidate Sharpe: 0.390;
- router Sharpe: 0.328;
- 70/30 combined Sharpe: 0.373;
- marginal Sharpe: +0.045;
- turnover: 0.094;
- maximum ADV participation: 6.99%.

The winner was feasible but its early-history router improvement was already below the production
marginal-Sharpe floor.

## Untouched validation

The validation segment contains 67 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | -0.061 | 0.431 | 0.206 | 0.136 |
| Annualized return | -0.79% | 1.51% | 0.83% | 0.51% |
| Maximum drawdown | -11.51% | -10.34% | -10.40% | -10.68% |
| Average turnover | 0.152 | 0.060 | 0.092 | 0.092 |
| Annualized cost drag | 0.35% | 0.29% | 0.32% | 0.63% |
| Maximum ADV participation | 6.48% | 9.21% | 7.22% | 7.22% |
| Probabilistic Sharpe | 44.30% | 83.50% | 68.29% | 62.35% |
| DSR probability (162 trials) | 0.24% | 5.47% | 1.57% | 0.99% |

Candidate/router correlation was 0.447. The candidate lost money, only three of six folds were
positive, and adding it reduced router Sharpe by 0.225. Candidate average gross exposure was
0.911 and minimum gross exposure fell to 0.599, so the discrete complete-case surface also failed
the deployment gate. The result fails standalone Sharpe, DSR, marginal mega-alpha Sharpe, and
minimum gross deployment.

Decision: **reject continuous financial strength from mega-alpha**. It is not shadow eligible
because DSR is not the sole failed gate and its probabilistic Sharpe is below the 95% shadow floor.
The hardened selector explicitly evaluated both asymmetric legs, so this is no longer a rejection
based only on an assumed monotonic rank portfolio. Router v6, production state, and the Loop 69 NOA
shadow candidate remain unchanged. The consumed validation interval must not be reused to select
component weights, reverse the signal, or retune the portfolio.

Decision artifact:
`atx-factor/research/loop72-continuous-financial-strength-exploration.json`.
Evidence SHA-256:
`fb9011b75d831fdefdcbb2e2b282820e3b781cf24821a96c49c7dfd631918b51`.

## Verification

- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 162 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production entry was
  created.
- The full 18-choice evaluation completed after the Loop 71 allocator regression passed.
- No full test suite was run.
