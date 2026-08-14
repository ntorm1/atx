# Fundamental signal improvement loop 88: earnings persistence

Status: **production data feature complete; rejected from production and shadow alpha**.

## Research contract

Cao and Narayanamoorthy show that post-earnings-announcement drift depends on both the magnitude
and persistence of an earnings surprise. Their central result is that lower ex-ante earnings
volatility is associated with stronger PEAD, and that this effect is not concentrated in stocks
with the highest trading frictions:

- https://onlinelibrary.wiley.com/doi/full/10.1111/j.1475-679X.2011.00425.x
- https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1573556

Loop 88 implements `earnings_sue_low_volatility_persistence`. It reuses the governed PIT SUE
feature and the historical standard deviation already computed solely from seasonal EPS changes
strictly preceding the current surprise. At each decision date it centers the SUE percentile to
[-1,1] and multiplies it by the cross-sectional percentile of low prior earnings volatility:

`(2 * rank_cs(SUE) - 1) * rank_cs(-prior seasonal EPS-change volatility)`.

The continuous multiplier retains every issuer with a valid volatility history rather than using
a hard low-volatility intersection, preserving breadth and allowing the portfolio layer to decide
how aggressively to concentrate. No returns are fitted and no holdout-dependent parameters exist.

## Production feature and breadth work

Migration 265 governs the factor and its SUE dependency. Lineage embeds the complete governed SUE
lineage, the strictly prior volatility estimate and observation count, both cross-sectional ranks,
the formula, and the missing-data policy.

The feature refresh completed in 35.4 seconds and materialized 102,152 rows for 1,113 securities
over 176 formation dates from 2012-03-30 through 2026-06-15. Median raw formation breadth was 585.

In parallel, the SEC companyfacts breadth path was converted from a monolithic load into bounded,
resumable raw batches. It now supports deterministic offsets, excluding already committed CIKs,
and deferring the expensive global catalog/revision/statement/period rebuild until all raw batches
finish. The identifier enrichment path was also changed from one DuckDB query per distinct filing
timestamp to one set-based bitemporal join per issuer. Three focused tests verify resolved,
unresolved, and null-availability behavior.

The first pre-optimization batch was stopped after its useful commits rather than allowed to block
research. It preserved 409 new issuers and 1,224,430 new facts, increasing raw companyfacts breadth
from 1,598 to 2,007 SEC CIKs and rows from 10,963,558 to 12,187,988. The local SEC ticker map has
8,021 issuers and the canonical price surface has 34,495 securities, so fundamentals—not prices—
remain the institutional breadth bottleneck. Further raw batches use the set-based resolver and run
outside the signal-research critical path.

## Predictive diagnostics

The signal has positive rank IC at all horizons, peaking around one quarter:

| Horizon | Mean rank IC | HAC t-stat | Conventional t-stat | Sign consistency | Dates | Mean names |
|---|---:|---:|---:|---:|---:|---:|
| 21 sessions | 0.0106 | 1.72 | 1.69 | 54.3% | 173 | 523.3 |
| 63 sessions | 0.0131 | 1.55 | 2.39 | 60.2% | 171 | 526.3 |
| 126 sessions | 0.0131 | 1.24 | 2.31 | 59.5% | 168 | 525.6 |
| 252 sessions | 0.0096 | 0.71 | 1.68 | 58.6% | 162 | 523.4 |

This supports the published persistence mechanism at the characteristic level, although HAC
evidence is below the production research threshold.

## Portfolio exploration and disposition

The acceptance layer committed all 36 configurations before reading validation returns, raising
the durable multiple-testing count from 486 to 522. Selection ended on 2020-06-30, followed by one
embargoed rebalance; untouched validation began on 2020-08-31.

Thirty-two selection configurations were feasible. The winner was the top-quintile-versus-
universe construction at 20% allocation using integrated stock-level ranks. Selection candidate,
baseline, and combined Sharpes were 0.374, 0.437, and 0.722; marginal Sharpe was +0.285, turnover
0.134, and maximum ADV participation 8.38%. Median selection signal breadth was 542.

The frozen construction failed in the untouched 69-month validation:

| Validation | Candidate | Router v6 | Integrated 80/20 | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | -0.468 | 0.395 | 0.520 | 0.422 |
| Annualized return | -1.56% | 1.37% | 1.59% | 1.28% |
| Maximum drawdown | -8.83% | -10.34% | -5.82% | -6.05% |
| Average turnover | 0.229 | 0.059 | 0.066 | 0.066 |
| Annualized cost drag | 0.40% | 0.30% | 0.31% | 0.61% |
| Maximum ADV participation | 4.51% | 9.21% | 8.54% | 8.54% |
| Probabilistic Sharpe | 12.31% | 81.66% | 88.51% | 83.62% |
| DSR probability (522 trials) | 0.0007% | 2.06% | 3.80% | 2.21% |

Candidate/router correlation was 0.501. The combined book improved baseline Sharpe by 0.125 and
reduced drawdown, but that diversification result cannot rescue a negative standalone sleeve.
Only one of six validation folds was positive. Median validation signal breadth was 557 names and
median effective portfolio breadth was 356.

Decision: **reject from production and shadow alpha**. It failed standalone validation Sharpe,
fold stability, DSR, and the 1,000-name production breadth floor. Candidate PSR was only 12.3%, so
the shadow exception does not apply. The exposed validation is frozen and will not be reused for
retuning. The factor remains available as a governed production data feature.

Artifact: `atx-factor/research/loop88-earnings-persistence-exploration.json`.
Evidence SHA-256:
`01e844561a5486af53ca57f720482aad56fe236824a6298f73f8ebc39928f585`.

## Verification

- Pure persistence weighting, deterministic target-window resume, loaded-target skip, and three
  set-based PIT identifier-resolution tests passed; no full suite ran.
- Ruff and Python compilation passed for the feature, migration, build/audit scripts, breadth
  loader, and focused tests.
- The live warehouse is at migration 265 and every applied checksum verifies.
- Feature integrity has 102,152 unique IDs and security/date keys, with zero non-finite values,
  missing lineage, late parents/outputs, invalid volatility estimates, or insufficient histories.
- The evidence digest recomputes exactly. The durable ledger contains 522 trials and one Loop 88
  experiment; neither production nor shadow registry contains the rejected candidate.
