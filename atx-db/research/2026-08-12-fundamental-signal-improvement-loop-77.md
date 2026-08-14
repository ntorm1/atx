# Fundamental signal improvement loop 77: Altman distress short-leg test

Status: **rejected after preregistered construction selection and untouched validation**.

## Research and frozen candidate

Campbell, Hilscher, and Szilagyi document that financially distressed U.S. stocks have delivered
anomalously low returns despite higher volatility and conventional factor risk
(https://doi.org/10.2139/ssrn.770805; published paper:
https://scholar.harvard.edu/files/campbell/files/campbellhilscherszilagyi_jf2008.pdf). This suggests
that distress predictiveness may be concentrated in the extreme short leg rather than monotonic
across the full financial-health distribution. The implementation also must treat short costs as a
first-class constraint: recent evidence finds that short-sale costs can eliminate many anomaly
returns that originate on the short side
(https://onlinelibrary.wiley.com/doi/10.1111/jofi.13501).

Loop 77 recovered the governed `distress_altman_z_score` feature, where higher scores denote
stronger financial health. The prior continuous-rank portfolio had low router correlation (0.366)
and positive but weak standalone Sharpe. The factor was unconsumed in the durable construction
ledger. No formula, direction, availability rule, or observation was changed.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the cumulative trial count from 234 to 252. Selection ended 2020-08-31,
September 2020 was embargoed, and only the frozen winner was evaluated from 2020-10-30 onward. The
phase-separated explorer completed in 10.49 seconds.

The literature-motivated **universe versus distressed bottom quintile** was feasible in early
history, with 0.417 candidate Sharpe and combined marginal Sharpe of +0.065, +0.112, and +0.132 at
10%, 20%, and 30% allocations. It was not the winner. Early history instead selected
**continuous rank at 30%**:

- candidate Sharpe: 0.580;
- router Sharpe: 0.444;
- 70/30 combined Sharpe: 0.690;
- marginal Sharpe: +0.247;
- turnover: 0.115;
- maximum ADV participation: 7.89%.

This choice was frozen before validation. The distressed-tail alternative was not examined on the
holdout after losing selection.

## Untouched validation

The validation segment contains 67 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | -0.214 | 0.431 | 0.182 | 0.117 |
| Annualized return | -1.61% | 1.51% | 0.73% | 0.43% |
| Maximum drawdown | -11.63% | -10.34% | -12.30% | -12.53% |
| Average turnover | 0.071 | 0.060 | 0.066 | 0.066 |
| Annualized cost drag | 0.30% | 0.29% | 0.30% | 0.60% |
| Maximum ADV participation | 8.08% | 9.21% | 8.77% | 8.77% |
| Probabilistic Sharpe | 30.64% | 83.50% | 66.42% | 60.77% |
| DSR probability (252 trials) | 0.04% | 4.10% | 0.89% | 0.57% |

Candidate/router correlation was 0.505 and all implementation constraints passed, so redundancy,
capacity, and turnover did not cause failure. Only three of six validation folds were positive;
candidate return was negative and adding it reduced router Sharpe by 0.249. The candidate fails
standalone Sharpe, DSR, and marginal mega-alpha Sharpe.

Decision: **reject Altman financial health from production mega-alpha**. It is not shadow eligible.
The expected distressed short leg was explicitly tested and lost on early data, preventing an
after-the-fact holdout choice. Router v6, production state, and the Loop 69 NOA shadow candidate
remain unchanged. The consumed validation period must not be used to promote the losing distress
tail or reverse the selected continuous construction.

Decision artifact: `atx-factor/research/loop77-altman-distress-exploration.json`.
Evidence SHA-256:
`44c55ac3520b8339f8e047c59ea992500c251cb448cdd0378460dfc8c912d0b4`.

## Verification

- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 252 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production entry was
  created.
- All losing constructions remained blind to validation returns.
- No source changed and no full test suite was run.
