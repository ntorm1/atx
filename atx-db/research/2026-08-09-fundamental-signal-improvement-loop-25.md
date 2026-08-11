# Fundamental signal improvement loop 25: quarterly working-capital accruals

Date: 2026-08-09

## Research hypothesis

Quarterly working-capital accruals should expose the non-cash wedge inside operating profit sooner
than annual accrual measures. Collins and Hribar document a quarterly accrual anomaly distinct from
post-earnings-announcement drift, while Wu, Zhang, and Zhang interpret working-capital accruals as
investment whose expected-return relation follows investment theory. The maintained production
hypothesis is therefore simple and return-independent: lower newly disclosed operating
working-capital investment is higher quality.

Primary references:

- [Collins and Hribar, *Earnings-based and Accrual-based Market Anomalies: One Effect or Two?*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=166455)
- [Wu, Zhang, and Zhang, *The Accrual Anomaly: Exploring the Optimal Investment Hypothesis*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1024129)
- [Global-q testing-portfolio technical document](https://global-q.org/uploads/1/2/2/6/122679606/portfoliostd_2025may.pdf)

No return-fitted coefficient, sign, window, or threshold was introduced.

## Point-in-time implementation

Added `atx_db.quarterly_working_capital_accruals`, migration `0228`, a standalone CLI,
and focused tests. The governed factor is
`quality_low_quarterly_operating_working_capital_accruals`, sourced as
`atx-db PIT low quarterly working-capital accruals v1`:

`(dAR + dInventory - dDeferredRevenue - dAP) / one-quarter-lagged total assets`.

The published score negates the raw ratio because lower accruals are preferred, then applies
monthly 1%/99% winsorization and cross-sectional z-scoring. Absolute raw ratios above five are
rejected. The builder reads the exact statement periods, knowledge timestamp, balance changes,
missing-pair decisions, and lagged-assets denominator already governed in the quarterly cash
profitability (`Claq`) lineage. This creates one explicit factor dependency and prevents a second
fact-selection implementation from drifting away from `Claq`.

The live refresh produced 56,805 rows across 479 securities and 173 dates from 2012-04-30 through
2026-06-15. It completed in approximately 33 seconds.

## Standalone analysis

Run id: `loop25-low-quarterly-working-capital-accruals-production`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0146 | 2.76 | 0.112% | 53.8% | 0.48 |
| 63d | 0.0221 | 2.81 | 1.717% | 58.6% | 0.56 |
| 126d | 0.0201 | 2.06 | 5.600% | 60.8% | 0.59 |
| 252d | 0.0138 | 1.40 | 8.670% | 58.8% | 0.52 |

The factor has positive mean IC and positive long-short tails at every horizon. Its tails are much
better behaved than standalone quarterly operating or cash profitability, but it is a comparatively
fast signal: top/bottom turnover is 44.3%/44.0% and mean rank autocorrelation is 0.759. It is useful
as an independently queryable quality feature, not yet as the production router's extreme-decile
definition.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0023 | 0.0130 | 0.0202 | 0.0114 |
| 2016-2020 | 0.0194 | 0.0198 | 0.0158 | 0.0259 |
| 2021-2026 | 0.0186 | 0.0310 | 0.0245 | 0.0018 |
| 2023-2026 | 0.0129 | 0.0198 | 0.0169 | -0.0255 |

The 21/63/126-day relation remains positive in the modern sample, but the recent 252-day reversal
argues against replacing a stable slow router with this faster standalone feature.

### Component attribution

Each leg was reconstructed from the same governed lineage with the production orientation and
lagged-assets denominator.

| Component | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Low inventory change | 0.0148 / 2.37 | 0.0184 / 1.86 | 0.0239 / 2.09 | 0.0252 / 2.61 |
| High deferred-revenue change | 0.0043 / 0.96 | 0.0067 / 1.17 | 0.0065 / 0.82 | 0.0123 / 1.18 |
| Low receivables change | 0.0033 / 0.66 | 0.0039 / 0.53 | -0.0046 / -0.53 | -0.0026 / -0.32 |
| High payables change | 0.0021 / 0.35 | 0.0034 / 0.50 | 0.0064 / 0.73 | -0.0074 / -0.90 |

Low inventory change is the only leg with positive IC at every horizon and HAC support at both the
short and long ends. This internal result independently matches Thomas and Zhang's finding that the
negative accrual-return relation is mainly attributable to inventory changes. It becomes the next
loop's predeclared hypothesis rather than a fitted component weight.

### Distinctiveness

Mean monthly cross-sectional correlation is 0.478 with `Claq`, -0.047 with quarterly operating
profitability, 0.065 with production router v6, 0.079 with annual low working-capital accruals,
0.152 with annual low total accruals, and 0.048 with conservative annual asset growth. The new
feature is not a duplicate of the existing annual accrual or investment surfaces.

## Router comparison and decision

The experiment replaced only the secondary ordering inside the production primary signal's fixed
deciles. Primary profitability/fallback routing and all extreme-decile memberships remained fixed.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Primary only | 0.02219 | 0.03509 | 0.05171 | 0.07088 |
| Quarterly operating profitability | 0.02259 | 0.03620 | 0.05274 | 0.07234 |
| Quarterly cash profitability (production v6) | 0.02281 | 0.03653 | 0.05306 | 0.07247 |
| Low quarterly working-capital accruals | 0.02284 | 0.03644 | 0.05282 | 0.07177 |

The accrual secondary wins mean IC only at 21 days. It trails v6 at 63/126/252 days and trails v6
at every horizon in 2023+. Its 21/63/126/252-day spreads are -0.121%, 0.394%, 2.697%, and 7.559%,
also below v6 at every horizon. Because the objective is a durable production improvement rather
than selecting an isolated metric, no router migration was made. V6 remains production.

## Verification

- Three focused factor tests pass, covering formula/orientation, missing-change inheritance,
  scale rejection, deterministic identity, and migration metadata.
- The live output has 56,805 unique IDs and natural keys, no null/non-finite/future-knowledge rows,
  exact per-date sample z-score normalization, monthly breadth of 27-399, and lineage no larger
  than 1,116 bytes.
- The live schema is `0228` with 202 checksummed migrations. Migration checksums, schema drift,
  schema-contract v2 pin, and a DuckDB checkpoint all pass.
- Full-suite execution was intentionally avoided.

## Next loop

Build a direct low quarterly inventory-change factor from the governed balance snapshots. Compare
average-assets scaling with inventory-growth scaling using literature-defined formulas only, examine
sector availability, and test whether the stronger isolated leg improves production v6 without
sacrificing recent-period stability or extreme-decile returns.

Next-loop primary references:

- [Thomas and Zhang, *Inventory Changes and Future Returns*](https://papers.ssrn.com/abstract=295247)
- [Belo and Lin, *The Inventory Growth Spread*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1526726)
- [Global-q testing portfolios, including inventory changes and inventory growth](https://global-q.org/testingportfolios.html)
