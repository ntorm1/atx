# Fundamental signal improvement loop 15: continuous financial strength

Date: 2026-08-09

## Research question

Does replacing Piotroski's binary thresholds with a fixed, rank-standardized continuous score
recover useful information without fitting component weights to this evaluation sample?

## Primary-source basis

- Piotroski explicitly identifies binary thresholding as a potential information loss and reports
  a robustness specification that ranks each of the nine signals between zero and one and sums the
  annual ranks into `RANK_SCORE`:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=249455>.
- Asness, Frazzini, and Pedersen place quality measures on equal footing by cross-sectionally
  ranking each variable, standardizing its ranks, and averaging component z-scores. Their quality
  framework spans profitability, growth, and safety:
  <https://link.springer.com/article/10.1007/s11142-018-9470-2>.

These sources specify the transform before this sample is evaluated: orient each component so
higher is stronger, rank-standardize by date, and equal-weight the component scores.

## Component diagnosis

The nine continuous Piotroski dimensions were reconstructed from the production row lineage and
evaluated independently on the same complete-case cohort.

| Oriented component | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| ROA | 0.0028 / 0.22 | -0.0077 / -0.41 | -0.0091 / -0.35 | 0.0066 / 0.26 |
| CFO / beginning assets | 0.0008 / 0.07 | -0.0066 / -0.34 | -0.0110 / -0.40 | -0.0071 / -0.20 |
| Change in ROA | 0.0147 / 1.47 | 0.0304 / 2.02 | 0.0516 / 2.61 | 0.0715 / 3.57 |
| Low accruals | -0.0101 / -1.06 | -0.0140 / -0.92 | -0.0295 / -1.56 | -0.0420 / -1.78 |
| Low change in leverage | 0.0027 / 0.25 | -0.0044 / -0.28 | -0.0068 / -0.33 | -0.0045 / -0.21 |
| Change in liquidity | -0.0024 / -0.25 | -0.0012 / -0.08 | 0.0050 / 0.26 | 0.0220 / 0.86 |
| Low net issuance | 0.0155 / 1.34 | 0.0104 / 0.65 | 0.0225 / 1.24 | 0.0386 / 1.96 |
| Change in gross margin | 0.0051 / 0.50 | 0.0185 / 1.24 | 0.0384 / 1.97 | 0.0510 / 2.21 |
| Change in asset turnover | -0.0014 / -0.14 | -0.0069 / -0.46 | -0.0073 / -0.35 | -0.0150 / -0.56 |

Improving ROA and gross margin explain the constructive portion of the score. Low accruals reverses
in this selected modern cohort, and most leverage/liquidity/efficiency changes are weak. This
diagnosis was not used to select or weight components; doing so would fit the evaluation sample.

## Production build

Added `atx_db.continuous_financial_strength`, migrations `0211` and `0212`, a standalone CLI,
and targeted tests. The live features are:

- `quality_continuous_financial_strength`;
- `quality_continuous_financial_strength_high_book_to_market`.

For each date, the implementation calculates and orients:

```text
ROA
CFO / beginning assets
change in ROA
(CFO - net income) / beginning assets
negative change in long-term leverage
change in current ratio
low split-adjusted net share issuance
change in gross margin
change in asset turnover
```

Each component receives an average-tie cross-sectional rank z-score. The factor is the equal-weight
mean of all nine component scores, standardized again within date. Inputs remain complete case;
there is no imputation, return-fitted selection, or return-fitted weighting.

The first implementation reloaded and repeated the complete annual fact lineage and took more than
two minutes to refresh. Migration `0212` changes the dependency contract to reference the governed
Piotroski factor row and its already complete lineage. The optimized full refresh materializes
24,881 rows in 22.4 seconds. Maximum row lineage is 1,420 bytes and retains the upstream factor
value ID plus every raw and rank-standardized component.

Live coverage is identical to the underlying complete-case Piotroski surface:

| Factor | Rows | Securities | Dates | Coverage |
|---|---:|---:|---:|---|
| Continuous strength | 21,704 | 317 | 172 | 2012-03-30 to 2026-06-15 |
| Continuous strength, high B/M | 3,177 | 124 | 89 | 2019-02-28 to 2026-06-15 |

## Analysis

Run id: `loop15-continuous-financial-strength-production`. Quintiles are used because the factor is
continuous and the conditioned cohort averages only 35 names.

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Continuous strength | 0.0043 / 0.39 | 0.0021 / 0.12 | 0.0121 / 0.52 | 0.0342 / 1.43 |
| High-B/M continuous strength | 0.0160 / 0.75 | 0.0109 / 0.40 | 0.0040 / 0.11 | -0.0373 / -0.86 |

The full-cohort rank signal is modestly positive at one year, but neither surface is monotonic:

| Factor | 21d Q5-Q1 | 63d Q5-Q1 | 126d Q5-Q1 | 252d Q5-Q1 |
|---|---:|---:|---:|---:|
| Continuous strength | -0.347% | -0.844% | -1.727% | -1.733% |
| High-B/M continuous strength | -0.114% | -1.409% | -1.847% | -4.880% |

Standalone top/bottom-quintile turnover is low at 17.4%/17.6%, with 0.949 mean rank
autocorrelation. The high-value condition raises turnover to 35.5%/32.7% because value membership
itself changes.

### Subperiod stability

Standalone IC:

| Period | 21d | 63d | 126d | 252d |
|---|---:|---:|---:|---:|
| 2012-2015 | -0.0092 | -0.0241 | -0.0210 | 0.0270 |
| 2016-2020 | -0.0007 | -0.0015 | 0.0198 | 0.0554 |
| 2021-2026 | 0.0186 | 0.0246 | 0.0297 | 0.0163 |

Recent short/intermediate behavior improves, but the one-year effect is not stable: 2019-2022 is
positive (0.0466, HAC 1.99) while 2023-2026 is negative (-0.0172, HAC -0.74). The high-B/M
one-year IC is -0.0464 in 2021-2026 and -0.0606 in 2023-2026.

### Distinctiveness and router test

The standalone factor correlates 0.738 with binary Piotroski, 0.596 with cash-flow profitability,
0.279 with corrected Altman, -0.278 with book-to-market, and only 0.068 with the broad router.

Low correlation does not translate into an improved router. On the full panel, the baseline router
IC is 0.02219 / 0.03509 / 0.05172 / 0.07088. A 10% sparse overlay changes it to
0.02220 / 0.03481 / 0.05079 / 0.06947; a 25% overlay changes it to
0.02193 / 0.03401 / 0.04921 / 0.06723. Both overlays also weaken recent 63-252-day IC.

## Decision

Keep both continuous factors as production-queryable research features, but do not promote either
as a monotonic alpha and do not alter the router. The fixed all-nine transform provides a governed
benchmark and component surface; the evidence rejects treating equal-weight financial strength as
a validated long-short signal in this sample.

The strong individual trend legs are hypotheses for future out-of-sample designs, not permission to
retrofit this factor.

## Verification

- Continuous-strength tests plus Piotroski regression tests: 4 passed serially.
- New files: Ruff and Python compilation clean.
- Schema `0212`; compact dependency graph is one upstream factor for the standalone feature and
  two factor dependencies for the high-value feature.
- Migration checksums, checkpoint, duplicate-key, finiteness, availability, and upstream-lineage
  checks: passed.
- Scoped `atx-db` diff check: passed; full-suite execution was intentionally avoided.

## Next loop

Build the profitability subcomponent specified by Quality Minus Junk using independently motivated
profitability measures and its published rank-standardization rule. Keep it separate from
Piotroski's weaker leverage/liquidity legs and test whether the cleaner profitability construct
adds value beyond the existing cash-flow and broad-router features.
