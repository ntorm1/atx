# Fundamental signal improvement loop 87: twin profitability-price momentum

Status: **production data feature complete; rejected from production and shadow alpha**.

## Research contract

Huang, Zhang, and Zhou's *Twin Momentum: Fundamental Trends Matter* forms a
fundamental-implied-return signal from trends in seven accounting variables and combines it with
12-minus-1-month price momentum. Its strongest portfolio buys stocks that are simultaneously in
the top fundamental- and price-momentum groups and shorts the bottom/bottom intersection:

- https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2894068

Loop 87 implements a deliberately parsimonious, return-fit-free adaptation named
`momentum_twin_profitability_trend_price_12_1`. The fundamental leg is the already-governed PIT
eight-quarter quarterly gross-profitability trend. The price leg is the split-adjusted return from
session t-252 through t-21. Within each formation cross-section, both inputs are percentile-ranked
and centered to [-1,1]. The raw score is:

`sign(F) * min(abs(F), abs(P))` when `F` and `P` have the same sign, and zero otherwise.

This continuous encoding preserves the paper's top/top and bottom/bottom confirmation tails while
allowing the preregistered portfolio search to test symmetric, asymmetric, raw-score, and
stock-level integrated constructions. It does not claim to reproduce the paper's fitted
seven-fundamental FIR forecast. The simpler specification avoids fitting returns before the
holdout and was chosen to keep the widened data work off the research critical path.

## Production feature

Migration 264 governs the feature and its direct dependencies. Each observation records the full
parent-factor lineage, both exact price endpoints, their availability timestamps, the reference
bar, raw and centered ranks, cross-sectional breadth, and the confirmation value. The loader reads
only price histories for securities present in the fundamental parent, so the 31.2-million-row
canonical price surface does not impose a full-universe scan on downstream feature computation.

The refresh completed in 51.6 seconds and materialized 68,986 rows across 609 securities and 155
formation dates from 2013-10-31 through 2026-06-15. Median raw formation-date breadth was 474.
The canonical bar store remains ready for 34,495 securities, but current governed fundamental
coverage—not price coverage—is now the limiting breadth constraint.

Integrity checks found 68,986 unique IDs and security/date keys, zero non-finite values, zero
missing lineage, and zero parent, reference, start-endpoint, end-endpoint, or output availability
violations. Migration 264 is applied and every stored migration checksum verifies.

## Predictive diagnostics

All IC estimates use governed adjusted-price forward returns and HAC inference. The signal is
positive at every horizon but statistically weak:

| Horizon | Mean rank IC | HAC t-stat | Conventional t-stat | Sign consistency | Dates | Mean names |
|---|---:|---:|---:|---:|---:|---:|
| 21 sessions | 0.0075 | 0.70 | 0.68 | 54.2% | 153 | 444.3 |
| 63 sessions | 0.0088 | 0.59 | 0.91 | 57.6% | 151 | 444.4 |
| 126 sessions | 0.0116 | 0.52 | 1.14 | 58.8% | 148 | 442.8 |
| 252 sessions | 0.0145 | 0.52 | 1.33 | 64.8% | 142 | 439.5 |

The monotonic rise in raw IC with horizon is consistent with slow-moving fundamental information,
but overlapping long-horizon labels substantially reduce effective evidence; none of the HAC
t-statistics clears even 1.0.

## Portfolio exploration and disposition

Before validation returns were read, the acceptance layer committed 36 configurations: six
portfolio variants, three allocations, and both separate-sleeve and stock-level integrated modes.
This increased the durable trial count from 450 to 486. Selection ended on 2021-02-26, followed by
one embargoed rebalance; untouched validation began on 2021-04-30.

No selection-period construction was feasible because every candidate sleeve had negative
standalone selection Sharpe. The diagnostic fallback was top-quintile-versus-universe at 10%
allocation using integrated stock-level ranks. In selection, its candidate Sharpe was -0.296,
baseline Sharpe 0.304, combined Sharpe 0.473, marginal Sharpe +0.170, turnover 0.104, and maximum
ADV participation 8.43%. Median signal breadth was 428 names.

The frozen fallback performed much better in the untouched 61-month validation:

| Validation | Candidate | Router v6 | Integrated 90/10 | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 0.692 | 0.597 | 0.923 | 0.815 |
| Annualized return | 2.61% | 2.01% | 2.49% | 2.19% |
| Maximum drawdown | -3.77% | -10.34% | -3.73% | -3.94% |
| Average turnover | 0.192 | 0.060 | 0.053 | 0.053 |
| Annualized cost drag | 0.37% | 0.29% | 0.29% | 0.58% |
| Maximum ADV participation | 2.63% | 9.21% | 8.08% | 8.08% |
| Probabilistic Sharpe | 93.14% | 89.83% | 97.67% | 96.14% |
| DSR probability (486 trials) | 7.91% | 5.49% | 17.92% | 12.34% |

The validation candidate/router correlation was 0.436. The integrated portfolio improved Sharpe
by 0.325, held a median 516 eligible names with median effective breadth 330, and produced positive
returns in five of six reporting folds (the last fold has only one observation).

Decision: **reject from production and shadow alpha**. The positive holdout is diagnostically
important, but selection offered no standalone support, median validation breadth missed the
1,000-name institutional floor, and the 486-trial DSR was only 17.9% for the combined book. Shadow
admission is prohibited because insufficient multiple-testing evidence and breadth were not the
only failures. The exposed holdout will not be used to tune this feature. The governed factor
remains available as a production data feature and a frozen candidate for genuinely new dates.

Artifact: `atx-factor/research/loop87-twin-momentum-exploration.json`.
Evidence SHA-256:
`f7a7e46055ff1b6663156e35b88fee0c0905e04ec64fb776918af930d31dbe6c`.

## Verification

- The pure confirmation test and split-adjusted endpoint-loader test passed; no full suite ran.
- Ruff and Python compilation passed for the feature, migration, build script, audit, and focused
  tests.
- Live governance checks confirm the definition, dependency edges, migration 264, and verified
  checksums.
- The feature integrity audit has zero duplicate, non-finite, missing-lineage, and point-in-time
  timing violations.
- The durable ledger contains 486 trials. Neither production nor shadow registry contains the
  rejected candidate.
