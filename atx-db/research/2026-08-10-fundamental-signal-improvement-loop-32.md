# Fundamental signal improvement loop 32: change in asset turnover

Date: 2026-08-10

## Pre-registered research hypothesis

Operating-efficiency improvement can forecast future profitability and returns beyond the level
of profitability. Fairfield and Yohn report that changes in asset turnover and profit margin,
rather than their levels, help forecast changes in return on assets. Soliman finds that the
DuPont decomposition contains incremental information used incompletely by market participants.
Nissim and Penman motivate separating operating assets and liabilities from financing items when
measuring operating profitability.

Primary references:

- [Fairfield and Yohn, *Using Asset Turnover and Profit Margin to Forecast Changes in Profitability*](https://doi.org/10.1023/A:1012430513430)
- [Soliman, *The Use of DuPont Analysis by Market Participants*](https://publications.aaahq.org/accounting-review/article/83/3/823/2928/The-Use-of-DuPont-Analysis-by-Market-Participants)
- [Nissim and Penman, *Ratio Analysis and Equity Valuation*](https://business.columbia.edu/sites/default/files-efs/pubfiles/1063/nissimpenmanratio.pdf)

This document freezes the candidate definitions and decision rules before any Loop 32 return
screen is run.

## Candidate definitions

The primary candidate is the broad, annual change in total-asset turnover:

`delta_ato_t = revenue_t / total_assets_(t-1) - revenue_(t-1) / total_assets_(t-2)`.

This is the existing point-in-time Piotroski turnover convention and uses beginning assets. The
current and prior annual durations must each be 330-400 days, their fiscal endpoints must be
300-430 days apart, all denominators and revenues must be positive and finite, and every input
must have been visible at the monthly formation date. The newest annual observation may be no
more than 550 days old. No return-fitted parameter is permitted.

Two variants are secondary and cannot silently replace the primary candidate:

1. Approximate net-operating-asset turnover change uses
   `noa_proxy = stockholders_equity + long_term_debt - cash_and_short_term_investments` and the
   same beginning-denominator change formula. ATX currently lacks broad point-in-time coverage for
   all short-term debt, preferred stock, minority interest, operating liabilities, and financial
   assets required for exact net operating assets. This variant must therefore remain labelled a
   proxy and must require positive finite denominators.
2. Quarterly same-quarter turnover change uses quarterly revenue divided by lagged point-in-time
   assets and compares the same fiscal quarter one year earlier. It is exploratory because the
   cited hypothesis is annual and quarterly balance-sheet coverage may materially narrow the
   universe.

The production score, if the primary survives, is the per-formation-date sample z-score of the
raw change after symmetric 1% cross-sectional winsorization. Cross-sections require at least 20
securities. Industry-neutral results may be reported as a robustness check, but they cannot
replace the broad pre-registered result.

## Evaluation contract

The factor is evaluated on the existing governed monthly US-equity universe and fixed forward
total-return horizons of 21, 63, 126, and 252 trading days. The screen reports rank IC, Newey-West
HAC inference, decile Q10-Q1 spreads, hit rate, tail monotonicity, turnover, rank persistence,
coverage, modern-regime stability, and correlation with incumbent features and production router
v6. Missing observations are not imputed.

The primary candidate advances to governance only if it has positive full-history rank IC at all
four horizons, HAC t-statistics of at least 2.0 at two or more horizons including 126 or 252 days,
positive and directionally coherent long-horizon tails, no material modern-regime sign reversal,
and useful incremental exposure versus the production router. A secondary variant may be governed
only if it independently satisfies the same rules; it cannot rescue a failed primary through
post-hoc relabelling.

Any governed candidate must then pass the separate `atx-factor` walk-forward admission test with
transaction costs, capacity constraints, deflated-Sharpe control, and explicit baseline/blend
comparison. Portfolio admission requires every configured mega-alpha gate; a useful database
feature may still be retained after portfolio rejection.

## Status

Pre-registration was frozen before Loop 32 return inspection. The production evaluation is now
complete. Neither construction is admitted to the production router or mega-alpha registry.

## Primary total-assets result

The primary total-asset-turnover change retained 77,948 rows across 907 securities and 175 monthly
formation dates from 2012-04-30 through 2026-06-15. Breadth ranged from 45 to 621 names.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0085 | 1.40 | 0.676% | 56.7% | 0.418 |
| 63d | 0.0101 | 1.22 | 2.126% | 60.4% | 0.321 |
| 126d | 0.0180 | 1.78 | 4.053% | 64.5% | 0.370 |
| 252d | 0.0288 | 1.97 | 8.259% | 72.5% | 0.491 |

The economics are coherent, including positive IC and tails at every horizon, but the candidate
does not satisfy the frozen inference rule: no horizon reaches a HAC t-statistic of 2.0. It is
therefore rejected rather than rounded into a pass. The standalone implementation remains
research code and was not added to `factor_definition` or the live factor partition.

## Secondary NOA-proxy implementation

The secondary formula is:

`revenue_t / noa_proxy_(t-1) - revenue_(t-1) / noa_proxy_(t-2)`, where
`noa_proxy = stockholders_equity + lt_debt - cash_st_inv`.

Added `atx_db.noa_proxy_turnover_change`, migration `0235`, a standalone build CLI, and four focused
tests. Each leg is an exact-accession annual filing observation. All components must be present,
finite, positive after constructing the denominator, and visible by the governed monthly close;
missing debt is not treated as zero. Lineage explicitly carries `is_exact_noa=false`, the omitted
NOA components, every statement-point ID and timestamp, and the no-imputation/no-return-fitting
contract.

An initial fast secondary screen reused the primary loader and therefore inherited an unrelated
`total_assets` coverage requirement. That 22,995-row subset appeared to pass the inference gate.
The production loader correctly removed that requirement and expanded to 35,552 rows across 505
securities and 175 dates, with breadth of 25-311. The broader production partition is the
authoritative test; the narrower result is quarantined and cannot support admission.

## Authoritative production analysis

Run id: `loop32-noa-proxy-production-evaluation`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0064 | 0.79 | 0.612% | 53.2% | 0.127 |
| 63d | 0.0134 | 1.14 | 1.876% | 61.5% | 0.055 |
| 126d | 0.0236 | 1.78 | 3.718% | 65.1% | -0.079 |
| 252d | 0.0203 | 1.03 | 7.793% | 71.3% | -0.261 |

The signal again has positive average IC and spreads, but it fails the required HAC threshold at
every horizon and its pooled decile ladder reverses at 126 and 252 days. Top/bottom turnover is
17.7%/17.9%, mean rank autocorrelation is 0.924, and there are 174 rebalance pairs.

Modern regimes remain positive but do not repair full-history inference. The 2021+ IC ladder is
0.0189/0.0261/0.0351/0.0305 with HAC t-stats of 1.73/1.62/1.86/1.43. The 2023+ ladder is
0.0169/0.0193/0.0373/0.0467 with HAC t-stats of 1.28/0.88/1.38/1.63. A planned Fama-French 12
industry-neutral robustness check had zero knowledge-date-safe classification matches in the live
warehouse, so no neutral result is reported.

Cross-sectional mean correlation is 0.020 with production router v6, -0.052 with conservative
asset growth, -0.015 with quarterly operating-profitability level, 0.016 with quarterly
profitability change, and 0.115 with continuous financial strength. The exposure is distinct, but
distinctness does not substitute for reliability.

## Costed mega-alpha decision

The separate Polars engine ran nine disjoint 12-month test folds, totaling 108 out-of-sample
observations at $100 million AUM. The candidate produced a 0.066 Sharpe, 0.159% annualized return,
-13.21% maximum drawdown, and 0.0296 deflated-Sharpe probability. Router v6 produced a 0.580
Sharpe. An 80/20 router/candidate blend reached 0.592 Sharpe, only +0.0118 marginal Sharpe versus
the required +0.05; doubled costs reduced it to 0.493.

The candidate also reached only 20.0% minimum gross deployment. The blend's 12.56% maximum ADV
participation exceeded the 10% ceiling and its 83.8% minimum gross deployment missed the 95%
floor. Candidate/baseline OOS return correlation was 0.165.

Decision: **reject from the mega-alpha portfolio**. It fails candidate Sharpe, deflated Sharpe,
marginal improvement, participation, candidate deployment, and combined deployment gates. Router
v6 and the mega-alpha registry are unchanged. The immutable decision artifact is
`C:\atx\atx-factor\research\loop32-noa-proxy-mega-alpha-decision.json`, evidence digest
`26c346cbf1e0d4bc7c033b2ccc3391f7f825662ed09e4911d98830500eaf0fbd`.

Migration `0235` had already been applied after the narrower preliminary screen, so it remains
immutable. The proxy partition is retained as a governed, reproducible research feature, not as a
production-router or mega-alpha constituent. This preserves the failed experiment and prevents a
future analyst from rediscovering only its favorable restricted subset.

## Verification

- Four focused feature/governance tests and the checksum-registration test pass. The full suite
  was intentionally not run; an earlier broader targeted lane timed out during repeated fresh
  schema bootstrap, after which the lane was narrowed and run serially.
- Ruff passes for both research implementations, the production module, CLI, migration, registry,
  and focused tests.
- The live partition has 35,552 unique IDs and natural keys, no null/non-finite/future-knowledge
  rows, exact monthly sample z-score normalization, and maximum lineage of 2,847 bytes.
- Live schema is `0235` with 209 distinct checksummed migrations.

## Next loop

Loop 32 supplies a useful low-correlation diagnostic but no admissible mega-alpha. The next signal
loop should first repair or backfill the PIT classification surface and should avoid screening a
secondary construction through a loader that imposes unrelated primary-candidate fields.
