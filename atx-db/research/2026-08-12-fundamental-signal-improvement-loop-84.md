# Fundamental signal improvement loop 84: asset-turnover trend and selection support

Status: **production data feature complete; admitted to shadow alpha, not production alpha**.

## Research and feature contract

Akbas, Jiang, and Koch decompose quarterly gross profitability into asset turnover and gross
margin, construct trends in each component using the same seasonally controlled rolling
regression as their headline profitability trend, and report incremental return predictiveness
(https://doi.org/10.2308/accr-51708). Soliman independently documents that changes in asset
turnover predict future earnings changes and studies delayed equity-return responses through the
DuPont decomposition (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1101981).

Loop 84 added `efficiency_quarterly_asset_turnover_trend_8q`. For each governed monthly
decision it extracts revenue and lagged assets from the exact eight parent QGP observations
selected by Loop 83's PIT history grid, calculates quarterly revenue/lagged assets, and estimates
the coefficient on actual elapsed-quarter time after calendar-quarter fixed effects. All parent
factor rows must be visible by the decision timestamp. The factor requires eight observations,
at least three observed seasonal quarters, finite asset turnover no greater than 20 in absolute
value, and a finite slope no greater than 5 in absolute value. Cross-sections are winsorized at 1%
and standardized with at least 20 names.

Reusing the governed history grid avoids another quadratic historical self-join while preserving
the exact filing accessions, period ends, values, parent factor IDs, and timestamps. Migration 260
governs dependencies on both the history grid and quarterly gross-profitability parent. The
production refresh materialized 40,087 rows across 574 securities and 153 dates from 2013-11-29
through 2026-06-15.

## Data diagnostics

Predictiveness was positive at every horizon and strengthened with horizon:

| Horizon | Mean rank IC | HAC t-stat | Conventional t-stat | Dates | Mean names |
|---|---:|---:|---:|---:|---:|
| 21 trading days | 0.0174 | 1.67 | 1.69 | 151 | 259.0 |
| 63 trading days | 0.0201 | 1.32 | 2.10 | 149 | 256.7 |
| 126 trading days | 0.0367 | 1.65 | 3.61 | 146 | 251.4 |
| 252 trading days | 0.0561 | 1.50 | 4.40 | 140 | 240.8 |

The longer horizons are IC-decay diagnostics only. Portfolio exploration used non-overlapping
monthly 21-trading-day returns.

## Selection-support hardening

Loop 83 exposed a governance gap: when every selection construction was infeasible, the explorer
still evaluated its deterministic least-bad fallback and could classify a strong validation result
as shadow. Loop 84 introduced `positive_candidate_selection_v1`:

- a feasible selection trial requires positive standalone candidate Sharpe plus turnover and
  participation within their ceilings;
- the explorer records the feasible-trial count and the support-model version in its evidence;
- zero feasible selection trials adds `selection_no_feasible_construction`, preventing both
  production and shadow admission;
- the complete acceptance gate and selection-support version are now part of durable
  preregistration specifications;
- unsupported fallbacks may still run for diagnostics without becoming admissible evidence.

The shadow registry was upgraded to schema version 2 with auditable revocation tombstones. Loop
83 profitability trend, which had zero feasible selection trials, was removed from shadow without
altering its historical result artifact. The audit artifact is
`atx-factor/research/loop84-selection-support-audit.json`, SHA-256
`33592a70c4c0f6b7c2053351586ce779badf35152e77d8adc8c0b71de2db22b3`.

## Preregistered construction search

The new gate semantics and candidate specification committed 18 trials before portfolio results,
raising the durable total from 378 to 396. The search used six frozen variants, 10%/20%/30%
allocations, the first 60% of common dates for selection, one embargo period, and the untouched
final 40% for validation. Fifteen of 18 selection trials were feasible.

The frozen winner was **top asset-turnover-trend quintile versus the universe at 30% allocation**.
Through 2021-03-31 its candidate Sharpe was 0.403 and combined Sharpe improved from 0.142 to
0.343, a +0.202 marginal gain. Maximum full-portfolio participation was 7.08%. April 2021 was
embargoed and validation began 2021-05-28.

## Untouched validation and disposition

The validation segment contains 60 monthly observations:

| Validation | Candidate sleeve | Router v6 | 70/30 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | 1.015 | 0.631 | 1.010 | 0.923 |
| Annualized return | 4.21% | 2.12% | 3.56% | 3.24% |
| Maximum drawdown | -2.72% | -10.34% | -7.90% | -8.13% |
| Average turnover | 0.138 | 0.060 | 0.093 | 0.093 |
| Annualized cost drag | 0.34% | 0.29% | 0.31% | 0.62% |
| Maximum ADV participation | 5.71% | 9.21% | 8.08% | 8.08% |
| Minimum gross deployment | 100.00% | 100.00% | 100.00% | 100.00% |
| Probabilistic Sharpe | 99.36% | 90.90% | 98.40% | 97.56% |
| DSR probability (396 trials) | 22.64% | 7.21% | 25.46% | 19.73% |

Candidate/router correlation was 0.126, combined Sharpe improved by +0.380, doubled-cost Sharpe
remained 0.923, and all five complete validation folds were positive. The candidate passed every
production gate except the 95% multiple-testing-adjusted DSR probability floor. Candidate PSR
exceeded the 95% shadow threshold, so the frozen construction entered the canonical shadow
registry.

Decision: **shadow quarterly asset-turnover trend at 30%; do not admit it to production alpha**.
Promotion requires new untouched formation dates and every production gate. The exposed holdout
must not be used for retuning.

Exploration artifact:
`atx-factor/research/loop84-asset-turnover-trend-exploration.json`.
Evidence SHA-256:
`6ed54544b8ba13e0a4ba4597a5805f42ab0e8cb81cfcb149a8b556560ddb8b13`.

## Verification

- Two focused transform tests passed; migration 260 passed a transactional governance dry run.
- Thirteen focused explorer, CLI, and shadow-registry tests passed; no full suite was run.
- Ruff passed on all Loop 84 feature and acceptance paths.
- Applied migration checksums verify and the live warehouse is at migration 260.
- The factor panel has zero duplicate IDs, non-finite values, missing lineage, or parent
  availability violations.
- The durable ledger contains 396 trials. The schema-v2 shadow registry contains three active
  candidates: Loop 69 net operating assets, Loop 79 abnormal inventory growth, and Loop 84
  asset-turnover trend; Loop 83 is retained as a revoked tombstone.
