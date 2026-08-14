# Fundamental signal improvement loop 74: Ball operating-profitability recovery

Status: **rejected after preregistered construction selection and untouched validation**.

## Research and frozen candidate

Ball, Gerakos, Linnainmaa, and Nikolaev define operating profitability to better match current
expenses with current revenue and find that it predicts returns more strongly than net income or
gross profit (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2414543). Their published paper
defines the numerator as revenue less cost of goods sold and SG&A, excluding R&D, and reports
predictability extending well beyond the initial holding period
(https://faculty.tuck.dartmouth.edu/images/uploads/faculty/joseph-gerakos/Ball%2C_Gerakos%2C_Linnainmaa%2C_et_al._2015.pdf).
Related evidence finds that removing accruals to form cash-based operating profitability can
further improve the investment opportunity set
(https://faculty.tuck.dartmouth.edu/images/uploads/faculty/joseph-gerakos/Ball%2C_Gerakos%2C_Linnainmaa%2C_et_al._2016.pdf).

Loop 74 recovered the existing governed `profitability_ball_operating_profitability` feature. Its
previous fixed portfolio produced 0.479 standalone Sharpe and was the strongest remaining
unconsumed profitability definition. No formula, direction, availability rule, or observation was
changed. The durable ledger confirmed that this factor had not entered the bounded construction
explorer.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the cumulative trial count from 180 to 198. The constructions were continuous
rank, continuous raw score, symmetric quintile and decile tails, top quintile versus universe, and
universe versus bottom quintile. Selection ended 2020-07-31, August 2020 was embargoed, and only
the frozen winner was evaluated from 2020-09-30 onward.

Early history selected **continuous rank at 30%**:

- candidate Sharpe: 0.637;
- router Sharpe: 0.403;
- 70/30 combined Sharpe: 0.572;
- marginal Sharpe: +0.169;
- turnover: 0.109;
- maximum ADV participation: 6.19%.

The winner was feasible and economically strong in selection, but neither asymmetric construction
was preferred.

## Untouched validation

The validation segment contains 68 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | -0.006 | 0.393 | 0.291 | 0.212 |
| Annualized return | -0.12% | 1.37% | 1.02% | 0.73% |
| Maximum drawdown | -9.60% | -10.34% | -9.58% | -9.80% |
| Average turnover | 0.042 | 0.060 | 0.055 | 0.055 |
| Annualized cost drag | 0.29% | 0.30% | 0.30% | 0.59% |
| Maximum ADV participation | 6.83% | 9.21% | 8.58% | 8.58% |
| Probabilistic Sharpe | 49.45% | 81.36% | 74.94% | 68.85% |
| DSR probability (198 trials) | 0.29% | 3.90% | 2.14% | 1.33% |

Candidate/router correlation was 0.621, below the redundancy ceiling, and all capacity,
deployment, turnover, and drawdown constraints passed. The economic edge did not persist: only
three of six folds were positive, candidate return was slightly negative, and the sleeve reduced
router Sharpe by 0.101. It therefore fails standalone Sharpe, multiple-testing evidence, and
marginal mega-alpha Sharpe.

Decision: **reject Ball operating profitability from mega-alpha**. It is not shadow eligible
because DSR is not the sole failure and probabilistic Sharpe is below the 95% shadow floor. The
explorer explicitly tested rank, raw, concentrated tails, and both asymmetric legs, so the result
does not rely on a single assumed portfolio mapping. Router v6, production state, and the Loop 69
NOA shadow candidate remain unchanged. The consumed validation interval must not be reused to
reverse or retune the factor.

Decision artifact:
`atx-factor/research/loop74-ball-operating-profitability-exploration.json`.
Evidence SHA-256:
`8bc38fbec39776b725339b6d5eda864b1009a73d2e0d20cce00f222e7b7dc6ac`.

## Verification

- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 198 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production entry was
  created.
- The one committed evaluation completed in 94 seconds; no duplicate process or scan was launched.
- No source changed and no full test suite was run.
