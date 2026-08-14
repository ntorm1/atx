# Fundamental signal improvement loop 76: filing-confirmed earnings-surprise long leg

Status: **rejected after asymmetric construction selection and untouched validation**.

## Research and frozen candidate

Post-earnings-announcement drift is stronger when earnings news contains persistent cash-flow
information. Shivakumar finds that unexpected cash flows predict future returns beyond the earnings
surprise itself and that decomposing earnings news outperforms an earnings-only strategy
(https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1468-5957.2006.01425.x). Complementary evidence
shows that qualitative and detailed fundamental information around the announcement adds to the
reported earnings number and can predict subsequent drift
(https://www.federalreserve.gov/pubs/ifdp/2008/951/default.htm). Liquidity research also warns that
much published PEAD is concentrated in costly illiquid stocks, making explicit capacity and impact
gates essential (https://business.columbia.edu/faculty/research/liquidity-and-post-earnings-announcement-drift).

The remaining-feature audit selected the governed `earnings_sue_filing_confirmation` surface. Its
prior fixed portfolio had low router correlation (0.259), positive marginal Sharpe (+0.018), and
five of nine positive folds, but weak standalone Sharpe. Other unconsumed features had either
negative prior portfolio evidence or failed upstream IC gates. No formula, direction, availability
rule, or observation was changed.

## Preregistered search

Six constructions times 10%, 20%, and 30% router allocations were committed before loading
returns, increasing the durable trial count from 216 to 234. Selection ended 2020-07-31, August
2020 was embargoed, and only the frozen winner was evaluated from 2020-09-30 onward. The optimized
phase-separated explorer completed in 13.75 seconds.

Early history selected **top quintile versus universe at 30%**:

- candidate Sharpe: 0.505;
- router Sharpe: 0.403;
- 70/30 combined Sharpe: 0.527;
- marginal Sharpe: +0.124;
- turnover: 0.172;
- maximum ADV participation: 7.38%.

The result localizes the economically useful expression to filing-confirmed positive surprises
financed by a broad short universe; neither symmetric tail nor continuous mapping was preferred.

## Untouched validation

The validation segment contains 68 monthly 21-trading-day observations through 2026-04-30.

| Validation result | Candidate | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.301 | 0.393 | 0.483 | 0.387 |
| Annualized return | 0.95% | 1.37% | 1.71% | 1.35% |
| Maximum drawdown | -6.37% | -10.34% | -8.73% | -9.01% |
| Average turnover | 0.287 | 0.060 | 0.142 | 0.142 |
| Annualized cost drag | 0.46% | 0.30% | 0.35% | 0.70% |
| Maximum ADV participation | 9.73% | 9.21% | 8.00% | 8.00% |
| Probabilistic Sharpe | 75.75% | 81.36% | 86.30% | 81.10% |
| DSR probability (234 trials) | 1.95% | 3.48% | 5.44% | 3.31% |

Candidate/router correlation was only 0.121. The 70/30 combination improved router Sharpe by
0.091, survived doubled costs, deployed fully, and respected the participation ceiling, although
the standalone sleeve came within 27 basis points of that ceiling. Three of six validation folds
were positive. Standalone candidate Sharpe missed the 0.50 admission floor and its cumulative-trial
DSR probability was far below 95%.

Decision: **reject filing-confirmed earnings surprise from production mega-alpha**. It is not
shadow eligible because DSR is not the sole failed gate. The asymmetric discovery is retained in
the immutable artifact, demonstrating that the feature was not discarded after one arbitrary
portfolio mapping, but current standalone evidence is insufficient for capital or shadow status.
Router v6, production state, and the Loop 69 NOA shadow candidate remain unchanged. Only new
untouched formation dates may support re-evaluation; the consumed holdout cannot be used to adjust
the construction or allocation.

Decision artifact:
`atx-factor/research/loop76-earnings-filing-confirmation-exploration.json`.
Evidence SHA-256:
`bb66720be9be7f500101bfde91b810cab4ba1321638754811a3d66a4b950fabb`.

## Verification

- The atomic decision records `disposition: rejected` and `shadow_eligible: false`.
- The trial ledger contains 234 cumulative trials.
- The shadow registry still contains only `quality_net_operating_assets`; no production entry was
  created.
- All six constructions were evaluated only on selection dates before validation weights existed.
- No source changed and no full test suite was run.
