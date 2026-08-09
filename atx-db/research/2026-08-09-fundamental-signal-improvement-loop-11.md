# Fundamental signal improvement loop 11: revenue-confirmed earnings surprise

Date: 2026-08-09

## Research question

Does first-filed quarterly revenue news identify more persistent standardized unexpected earnings,
and should revenue be blended into SUE or used only as a confidence gate?

## Primary-source basis

- Jegadeesh and Livnat find that revenue surprises contain information about future returns beyond
  earnings surprises, that analysts incorporate the information slowly, and that the strongest
  six-month return spread occurs when revenue and earnings surprises are considered jointly:
  <https://www.sciencedirect.com/science/article/pii/S0165410106000061>.
- The authors define standardized unexpected revenue growth using a seasonal random walk with
  drift and an eight-season estimation window. Their paper also reports that revenue-confirmed
  earnings changes are more persistent and that the drift largely develops over six months. An
  author-hosted research index records both the journal article and the related Financial Analysts
  Journal study: <https://pages.stern.nyu.edu/~jlivnat/research.htm>.

The production implementation uses SEC first-filed values and conservative filing availability,
not preliminary earnings-announcement timestamps or analyst estimates.

## Data audit

First-revision, quarter-like statement coverage before investability screens:

| Metric | Quarter-like rows | Securities | Seasonal pairs | Eight-history eligible rows | Eligible securities |
|---|---:|---:|---:|---:|---:|
| Revenue | 52,847 | 1,298 | 46,850 | 36,735 | 1,228 |
| Operating cash flow | 20,642 | 1,388 | 18,515 | lower and frequently YTD-derived | — |
| Diluted EPS | 70,266 | 1,352 | 63,490 | reference series | — |

Revenue has sufficient depth for the paper's strict eight-season design. True quarterly operating
cash flow is much thinner because SEC cash-flow statements are commonly year-to-date, so a
quarterly cash-surprise factor was rejected for this loop. Of the eligible revenue observations,
USD accounts for 53,082 first-revision quarter-like rows; the production factor is USD-only.

## Build

### Standardized unexpected revenue

Added `earnings_standardized_unexpected_revenue` with:

- first-filed USD quarterly revenue only;
- quarter durations of 70-115 days and same-quarter matches 350-380 days apart;
- seasonal random walk with drift:
  `((revenue_q - revenue_q-4) - mean(prior 8 seasonal changes)) / std(prior 8 changes)`;
- exactly eight prior seasonal observations, positive historical volatility, 150-day freshness;
- $100 million market-cap and $1 million ADV21 screens;
- 1% cross-sectional winsorization, z-scoring, deterministic IDs, and complete nested lineage;
- governed migration `0205` and standalone CLI.

Raw revenue rather than revenue per share is used intentionally. Within-security standardization is
invariant to a constant scale and avoids injecting noisy point-in-time share counts and split
adjustments. This is an explicit production approximation to the paper's per-share measure.

Live output: 59,666 rows, 914 securities, 176 dates, 2012-03-30 through 2026-06-15.

### Equal-weight confirmation

Added `earnings_sue_revenue_confirmation` as an exact-key, later-availability intersection of SUE
and revenue surprise, equal weighted and re-standardized. It is governed by migration `0206`.

Live output: 54,752 rows, 868 securities, and 176 dates.

### Same-sign agreement sleeve

Common-cohort analysis showed that revenue magnitude diluted the stronger SUE rank. The production
promotion therefore uses revenue as a gate rather than an equal-weight input. Added
`earnings_sue_revenue_agreement` with:

- exact `(security_id, as_of_date)` SUE/revenue intersection;
- a strict `sue * revenue_surprise > 0` gate;
- SUE retained as the ranking value and re-standardized within the confirmed cohort;
- no row, rather than a neutral zero, when signs disagree or revenue history is unavailable;
- later upstream availability and auditable IDs for both inputs;
- governed migration `0207` and standalone CLI.

Live output: 32,731 rows, 859 securities, 175 dates, with 42/187/359 minimum/mean/maximum names per
date.

## Analysis

### Standalone and equal-weight factors

Run id: `loop11-revenue-confirmation-production`.

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Revenue surprise | 0.0072 / 0.96 | 0.0077 / 0.72 | 0.0146 / 0.98 | 0.0107 / 0.60 |
| 50% SUE / 50% revenue | 0.0146 / 1.86 | 0.0170 / 1.65 | 0.0204 / 1.40 | 0.0147 / 0.75 |

Revenue news is directionally positive but not independently significant. The equal-weight
composite improves on revenue alone but is not promotion-grade.

### Exact common cohort

On 54,752 common SUE/revenue keys, mean component correlation is 0.198.

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| SUE | 0.0224 / 3.20 | 0.0276 / 2.94 | 0.0263 / 2.01 | 0.0222 / 1.33 |
| Revenue surprise | 0.0119 / 1.51 | 0.0149 / 1.33 | 0.0193 / 1.30 | 0.0142 / 0.78 |
| 50% SUE / 50% revenue | 0.0189 / 2.41 | 0.0235 / 2.25 | 0.0260 / 1.76 | 0.0196 / 1.05 |
| SUE where signs agree | 0.0281 / 2.79 | 0.0415 / 3.10 | 0.0425 / 2.37 | 0.0304 / 1.47 |

The usable insight is confirmation, not averaging. A 90%/10% magnitude blend was also inferior to
unmodified SUE at most horizons, while the same-sign gate delivered a material rank-IC lift.

### Governed agreement factor

Run id: `loop11-revenue-agreement-production`.

| Horizon | Mean rank IC | HAC t-stat | Spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.02382 | 2.42 | 0.968% | 55.9% | 0.430 |
| 63d | 0.03816 | 2.84 | 2.872% | 62.5% | 0.370 |
| 126d | 0.04423 | 2.60 | 2.541% | 60.0% | 0.539 |
| 252d | 0.03333 | 1.70 | 2.014% | 55.3% | 0.552 |

Top/bottom turnover is 45.8%/46.7%, mean rank autocorrelation is 0.903, and all production rows are
finite, unique, and available no later than their decision date.

### Subperiod stability

| Period | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0397 / 2.62 | 0.0367 / 1.81 | 0.0299 / 1.05 | -0.0179 / -0.63 |
| 2016-2020 | 0.0150 / 0.76 | 0.0358 / 1.22 | 0.0524 / 1.41 | 0.0515 / 1.42 |
| 2021-2026 | 0.0211 / 1.40 | 0.0415 / 2.77 | 0.0466 / 3.16 | 0.0559 / 2.72 |

The 21-126-day signs are positive in every subperiod. The sole reversal is the earliest 252-day
slice, so the governed production use remains a 21-126-day high-conviction sleeve.

## Router allocation test

A sparse 10% agreement overlay, applied only where the sleeve exists and otherwise leaving the
broad router unchanged, produced the following same-panel comparison:

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Broad router | 0.02045 / 3.32 | 0.03316 / 4.40 | 0.04800 / 5.05 | 0.06791 / 6.44 |
| Router + 10% sparse agreement | 0.02094 / 3.28 | 0.03466 / 4.52 | 0.04864 / 5.03 | 0.06838 / 6.12 |

IC rises slightly at every horizon, but HAC weakens at three horizons. The overlay remains a
research allocation candidate rather than replacing the broad router.

## Decision

Promote `earnings_standardized_unexpected_revenue` as a queryable fundamental event feature and
`earnings_sue_revenue_agreement` as the preferred high-conviction earnings sleeve for 21-126-day
forecasts.

Keep `earnings_sue_revenue_confirmation` queryable but experimental. Do not promote the equal-weight
blend or alter the broad production router: revenue is useful as a direction gate, not as an equal
magnitude input, and the sparse router overlay does not improve inference consistently.

## Verification

- Revenue-surprise targeted tests: 2 passed.
- Equal-weight confirmation targeted tests: 2 passed.
- Agreement-sleeve targeted tests: 2 passed.
- New files: Ruff and Python compilation clean.
- Schema `0207`, migration checksums, checkpoint, duplicate, availability, and finiteness checks:
  passed.
- Full-suite execution was intentionally avoided.

## Next loop

Research accrual-based earnings quality using annualized quarterly balance-sheet changes and TTM
cash-flow realization. This avoids forcing sparse reported quarterly operating cash flow into an
event-surprise model while still testing whether confirmed earnings are cash backed.
