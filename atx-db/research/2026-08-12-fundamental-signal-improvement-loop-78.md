# Fundamental signal improvement loop 78: quarterly inventory-change construction test

Status: **rejected after preregistered construction selection and untouched validation**.

## Research and frozen candidate

Thomas and Zhang find that the negative relation between accruals and future abnormal returns is
driven largely by inventory changes, with extreme inventory-growth firms subsequently
underperforming (https://ideas.repec.org/a/spr/reaccs/v7y2002i2d10.1023_a1020221918065.html).
Operations research using quarterly inventories similarly reports monotonically weaker stock
performance as abnormal year-over-year inventory growth rises, consistent with demand/supply
mismatches and future profitability reversals
(https://www.sciencedirect.com/science/article/pii/S0272696313000399). These findings imply a
potentially asymmetric short leg among unusually high-inventory-growth firms.

Loop 78 recovered the governed `investment_low_quarterly_inventory_change` feature, oriented so
higher values mean lower inventory growth. Its prior fixed portfolio was weak but highly distinct
from router v6 (correlation 0.042) and positive in five of nine folds. The feature was unconsumed in
the durable construction ledger. No formula, direction, availability rule, or observation changed.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the cumulative trial count from 252 to 270. Selection ended 2020-07-31,
August 2020 was embargoed, and only the frozen winner was evaluated from 2020-09-30 onward. The
phase-separated explorer completed in 19.11 seconds.

The literature-motivated **universe versus high-inventory bottom quintile** was feasible but weak
in selection: candidate Sharpe was 0.048 and its best router improvement was +0.041 at a 20%
allocation. Early history instead selected **continuous raw score at 30%**:

- candidate Sharpe: 0.275;
- router Sharpe: 0.403;
- 70/30 combined Sharpe: 0.559;
- marginal Sharpe: +0.156;
- turnover: 0.180;
- maximum ADV participation: 6.31%.

The losing high-inventory short construction remained blind to validation returns.

## Untouched validation

The validation segment contains 68 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.203 | 0.393 | 0.415 | 0.330 |
| Annualized return | 1.08% | 1.37% | 1.63% | 1.28% |
| Maximum drawdown | -10.41% | -10.34% | -8.52% | -8.82% |
| Average turnover | 0.254 | 0.060 | 0.132 | 0.132 |
| Annualized cost drag | 0.44% | 0.30% | 0.35% | 0.69% |
| Maximum ADV participation | 15.99% | 9.21% | 8.16% | 8.16% |
| Probabilistic Sharpe | 67.06% | 81.36% | 80.35% | 75.74% |
| DSR probability (270 trials) | 1.43% | 3.15% | 5.09% | 3.14% |

Candidate/router correlation remained only 0.096 and four of six folds were positive. At its
actual 30% allocation, the combined portfolio respected capacity, deployed fully, survived doubled
costs, and improved router Sharpe by 0.023. The candidate's hypothetical standalone book would
breach the 10% participation ceiling and had severe negative skew (-2.58) and excess kurtosis
(15.98); these non-normal returns are reflected in the low DSR. Standalone Sharpe, DSR, and
marginal router improvement all fail their admission floors.

Decision: **reject quarterly inventory change from production mega-alpha**. It is not shadow
eligible. Both the economically expected short tail and the full continuous surface were explored
without holdout tuning; neither produced decision-grade evidence. Router v6, production state, and
the Loop 69 NOA shadow candidate remain unchanged. The consumed validation interval must not be
used to promote the losing tail or retune the selected raw-score construction.

Decision artifact:
`atx-factor/research/loop78-quarterly-inventory-change-exploration.json`.
Evidence SHA-256:
`9f808bf0c3906fe3dc26118092e48c6409733f84452d7e3fd263d6ae75aa1ef4`.

## Verification

- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 270 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production entry was
  created.
- All losing constructions remained blind to validation returns.
- No source changed and no full test suite was run.
