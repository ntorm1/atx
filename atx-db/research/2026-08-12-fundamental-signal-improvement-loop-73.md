# Fundamental signal improvement loop 73: operating-profitability recovery

Status: **rejected after preregistered construction selection and untouched validation**.

## Research and frozen candidate

Operating profitability matched to current expenses predicts the cross-section of expected returns
more strongly than gross profitability; published long-short tests report larger alpha for the
operating measure (https://www.sciencedirect.com/science/article/pii/S0304405X15000203). Cash-based
operating profitability can further improve the investment opportunity set by separating accruals
from cash generation (https://www.sciencedirect.com/science/article/pii/S0304405X16300307), while
expected-profitability work supports using recent firm profitability as a forward-looking return
characteristic (https://www.sciencedirect.com/science/article/pii/S0165176519302691).

The initial audit considered quarterly profitability change, but its governed earlier portfolio
evidence was strongly negative. The strongest unconsumed candidate was instead
`profitability_operating_profitability`: its prior fixed portfolio produced 0.665 standalone Sharpe,
but additive admission failed because it overlapped router v6. Loop 73 therefore froze this existing
feature and tested whether a different portfolio construction could retain its economics with less
router redundancy. No formula, direction, availability rule, or observation was changed.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the durable trial count from 162 to 180. The constructions were continuous
rank, continuous raw score, symmetric quintile and decile tails, top quintile versus universe, and
universe versus bottom quintile. Selection ended 2020-07-31, August 2020 was embargoed, and only
the frozen winner was evaluated from 2020-09-30 onward.

Early history selected **continuous raw score at 30%**:

- candidate Sharpe: 0.409;
- router Sharpe: 0.403;
- 70/30 combined Sharpe: 0.488;
- marginal Sharpe: +0.085;
- turnover: 0.105;
- maximum ADV participation: 6.81%.

The winner was feasible and improved the early router, but selection did not prefer either
asymmetric construction.

## Untouched validation

The validation segment contains 68 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.447 | 0.393 | 0.438 | 0.361 |
| Annualized return | 1.89% | 1.37% | 1.63% | 1.33% |
| Maximum drawdown | -10.11% | -10.34% | -10.51% | -10.73% |
| Average turnover | 0.051 | 0.060 | 0.057 | 0.057 |
| Annualized cost drag | 0.29% | 0.30% | 0.30% | 0.59% |
| Maximum ADV participation | 8.58% | 9.21% | 8.26% | 8.26% |
| Probabilistic Sharpe | 85.40% | 81.36% | 84.27% | 79.74% |
| DSR probability (180 trials) | 4.76% | 4.16% | 5.00% | 3.37% |

Four of six validation folds were positive, full deployment was maintained, and the combined book
remained profitable under doubled costs. Nevertheless, candidate Sharpe missed the 0.50 floor,
the 180-trial DSR probability was far below 95%, and the +0.046 marginal Sharpe missed the +0.05
floor. Candidate/router correlation remained 0.832, above the 0.70 additive ceiling. The last
partial fold was sharply negative, which also explains the decline from earlier full-window
evidence.

Decision: **reject operating profitability as an additive mega-alpha sleeve**. It is not shadow
eligible because DSR is not the sole failure and candidate probabilistic Sharpe is below the 95%
shadow floor. This does not skip a potentially useful high-correlation signal: the engine already
contains a separate replacement challenge, and the frozen operating-profitability replacement was
previously rejected because its multiple-testing evidence failed even though raw Sharpe exceeded
the incumbent. Router v6, production state, and the Loop 69 NOA shadow candidate remain unchanged.
The consumed validation interval must not be used to choose another construction or allocation.

Decision artifact:
`atx-factor/research/loop73-operating-profitability-exploration.json`.
Evidence SHA-256:
`ebdf1b248828a62e297c6ce4511f27936277d020f70b9a7d64a55c7ee4413d36`.

## Verification

- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 180 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production entry was
  created.
- All six constructions, including both asymmetric legs, were evaluated only in the selection
  segment before the final winner touched validation.
- No source changed and no full test suite was run.
