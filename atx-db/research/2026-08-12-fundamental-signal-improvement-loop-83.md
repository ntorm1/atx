# Fundamental signal improvement loop 83: profitability trend

Status: **production data feature complete; alpha rejected after Loop 84 governance audit**.

## Research and feature contract

Akbas, Jiang, and Koch, *The Trend in Firm Profitability and the Cross Section of
Stock Returns*, estimate a rolling slope in quarterly gross profitability over the latest eight
quarters, controlling for quarterly seasonality. They report that the trajectory predicts future
profitability, earnings surprises, forecast errors, and returns beyond the profitability level and
earnings momentum (https://doi.org/10.2308/accr-51708). S&P Global's later Russell 3000 study
also reports significant long-short excess returns across six gross-profitability-trend variants
(https://www.spglobal.com/content/dam/spglobal/mi/en/documents/general/MI-Research-QR-Profitability-180605.pdf).

Loop 83 added `profitability_quarterly_gross_profitability_trend_8q`. It estimates the OLS
coefficient on actual elapsed-quarter time after calendar-quarter fixed effects, using the latest
eight distinct observations from the governed PIT quarterly gross-profitability parent. Every
parent observation must be visible by the monthly decision timestamp. The rolling history must
span 580--1,000 days and contain at least three seasonal quarters.

Two published-method adaptations are explicit in lineage. The governed parent scales gross
profit by one-quarter-lagged assets rather than contemporaneous assets. Also, because the parent
does not derive a standalone fiscal Q4 from annual duration filings, the implementation permits
eight visible quarterly observations over at most 1,000 days and uses actual elapsed time. This
expanded a December 2025 probe from 21 strict complete-history names to 521 without forward
filling or using future filings. Cross-sections are winsorized at 1% and standardized only with at
least 20 names.

Migration 259 governs the definition and dependency. The production refresh materialized 68,986
rows across 609 securities and 155 dates from 2013-10-31 through 2026-06-15.

## Data diagnostics

Average monotonic rank predictiveness was weak:

| Horizon | Mean rank IC | HAC t-stat | Conventional t-stat | Dates | Mean names |
|---|---:|---:|---:|---:|---:|
| 21 trading days | 0.0025 | 0.36 | 0.36 | 153 | 444.1 |
| 63 trading days | -0.0036 | -0.33 | -0.52 | 151 | 443.9 |
| 126 trading days | -0.0056 | -0.35 | -0.78 | 148 | 441.9 |
| 252 trading days | -0.0003 | -0.01 | -0.03 | 142 | 438.4 |

The longer labels remain IC-decay diagnostics only. Portfolio exploration used non-overlapping
monthly 21-trading-day returns.

## Preregistered construction search

The durable ledger committed 18 trials before portfolio results, raising the cumulative trial
count from 360 to 378. The search used six frozen variants, 10%/20%/30% allocations, the first
60% of common dates for selection, one embargo period, and the untouched final 40% for
validation. Allocation-aware capacity used the correct candidate sleeve AUM.

No selection-grid construction had positive standalone candidate Sharpe. The deterministic
fallback therefore froze the least-damaging grid point: **top profitability-trend quintile versus
the universe at 10% allocation**. Through 2021-02-26 its candidate Sharpe was -0.628; combined
Sharpe was 0.227 versus 0.265 for the router, a -0.038 marginal result. The March 2021 formation
date was embargoed and validation began 2021-04-30. This pronounced selection/validation regime
reversal is an additional practical reason to keep the candidate in shadow.

## Untouched validation and disposition

The validation segment contains 61 monthly observations:

| Validation | Candidate sleeve | Router v6 | 90/10 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 1.041 | 0.597 | 0.712 | 0.624 |
| Annualized return | 3.77% | 2.01% | 2.40% | 2.10% |
| Maximum drawdown | -2.77% | -10.34% | -9.50% | -9.72% |
| Average turnover | 0.137 | 0.060 | 0.067 | 0.067 |
| Annualized cost drag | 0.34% | 0.29% | 0.30% | 0.60% |
| Maximum ADV participation | 2.64% | 9.21% | 8.96% | 8.96% |
| Minimum gross deployment | 100.00% | 100.00% | 100.00% | 100.00% |
| Probabilistic Sharpe | 99.32% | 89.83% | 93.33% | 90.78% |
| DSR probability (378 trials) | 25.77% | 6.34% | 10.15% | 7.09% |

Candidate/router correlation was 0.122, combined Sharpe improved by +0.114, the doubled-cost
combination remained positive, and four of six folds were positive. The candidate passed every
production gate except the 95% multiple-testing-adjusted DSR probability floor. Its candidate
probabilistic Sharpe exceeded the 95% shadow threshold, so the policy admitted the frozen
construction to the canonical shadow registry.

The original Loop 83 policy classified the result as shadow because DSR was its only validation
failure. Loop 84 subsequently added the preregistered `positive_candidate_selection_v1` support
gate: at least one selection construction must have positive standalone candidate Sharpe while
meeting turnover and participation ceilings. Loop 83 had zero such constructions, so its shadow
admission was revoked with an auditable registry tombstone.

Final decision after the Loop 84 governance audit: **reject profitability trend from production
and shadow alpha**. The exposed holdout must not be used to retune the factor or construction.

Artifact: `atx-factor/research/loop83-profitability-trend-exploration.json`.
Evidence SHA-256:
`6475773bd41a3f3358a1a460e0e6640b4b4214efaddb3239c3ef69e0a1c8acb3`.

## Verification

- Three focused transform/governance tests passed before live apply; no full suite was run.
- The final migration-backed rerun was stopped after exceeding the bounded 120-second check;
  the final SQL contract was instead exercised against live data before and after materialization.
- Ruff and `git diff --check` passed on all Loop 83 code paths.
- Applied migration checksums verify and the warehouse is at migration 259.
- The factor panel has zero duplicate IDs, non-finite values, missing lineage, or parent
  availability violations.
- The Loop 83 run raised the durable ledger to 378 trials. Loop 84 later removed this candidate
  from the canonical shadow registry because its selection grid contained zero feasible trials;
  the historical artifact and revocation tombstone are retained.
