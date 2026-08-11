# Fundamental signal improvement loop 29: SUE and direct revenue-growth agreement

Date: 2026-08-09

## Research hypothesis

Jegadeesh and Livnat report stronger post-earnings-announcement drift when earnings and revenue
surprises point in the same direction. ATX already publishes a SUE sleeve gated by standardized
unexpected revenue. Loop 28 showed that direct same-quarter revenue growth carries more IC than
that standardized revenue transform on their common cohort. The predeclared test therefore retains
standardized unexpected EPS only when direct revenue growth has the same sign.

Primary references:

- [Jegadeesh and Livnat, *Post-Earnings-Announcement Drift: The Role of Revenue Surprises*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=903767)
- [Jegadeesh, *Revenue Growth and Stock Returns*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=314962)

## Pre-registration comparison

Before adding a factor definition, the sign gate was evaluated in memory from governed panel rows.
It retained 18,342 of 28,066 paired observations across 158 dates. On the exact 12,775-row overlap
with the existing agreement sleeve, both factors are rank-identical because each publishes SUE and
uses revenue only as a gate. Differences outside that overlap are therefore cohort selection, not
hidden weighting.

The candidate's 126/252-day spreads were 4.58%/3.38%, materially above the existing agreement's
2.54%/2.01%. That cleared the pre-registration threshold for productionizing a distinct feature.

## Point-in-time implementation

Added `atx_db.earnings_revenue_growth_agreement`, migration `0232`, a standalone CLI, and three
focused tests. The governed factor is `earnings_sue_revenue_growth_agreement`, sourced as
`atx-db PIT SUE revenue-growth agreement v1`:

`zscore(SUE | SUE * same-quarter revenue growth > 0)`.

The loader selects the latest governed SUE and revenue-growth values on the identical
security/monthly decision key. `available_at` is their maximum. Revenue growth only decides the
same-sign gate; raw SUE remains the ranking variable and is restandardized over the eligible
cross-section. No fitted coefficient or threshold is introduced.

The live build contains exactly the pre-registered 18,342 rows across 419 securities and 158 dates
from 2013-04-30 through 2026-06-15. It completed in 20 seconds.

## Standalone analysis

Run id: `loop29-sue-revenue-growth-agreement-production`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0206 | 1.56 | 0.849% | 53.8% | 0.20 |
| 63d | 0.0375 | 1.97 | 2.921% | 63.0% | 0.30 |
| 126d | 0.0495 | 1.80 | 4.582% | 66.9% | 0.15 |
| 252d | 0.0375 | 1.22 | 3.384% | 56.6% | 0.27 |

The feature has positive IC and positive tails at every horizon. Relative to the existing agreement
sleeve, it gives up some 21-day IC/spread, is similar at 63 days, and improves 126/252-day IC and
tail spreads. Top/bottom turnover is 42.7%/47.4% and rank autocorrelation is 0.910, so this remains
a selective earnings-event sleeve rather than a low-turnover standalone portfolio.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2013-2015 | 0.0443 | 0.0588 | 0.0497 | -0.0141 |
| 2016-2020 | 0.0012 | 0.0237 | 0.0432 | 0.0461 |
| 2021-2026 | 0.0264 | 0.0394 | 0.0556 | 0.0600 |
| 2023-2026 | 0.0531 | 0.0719 | 0.0987 | 0.1273 |

Modern evidence is exceptionally strong: 2023+ HAC t-stats are 2.06/2.30/3.48/4.70. The early
one-year reversal and muted 2016-2020 short horizon still argue against replacing a slow production
router with this event-driven sleeve.

Mean correlation is necessarily 1.0 with raw SUE and the existing agreement on their respective
common cohorts, because the gate does not alter SUE ranks. Correlation is 0.591 with direct revenue
growth and only 0.045 with production router v6.

## Router comparison and decision

The candidate changed only ordering inside production primary deciles.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Cash profitability (production v6) | 0.02281 | 0.03653 | 0.05306 | 0.07247 |
| SUE/revenue-growth agreement | 0.02224 | 0.03550 | 0.05241 | 0.07149 |

The agreement trails v6 at every full-history horizon and at three of four 2023+ horizons. Its
21/63/126/252-day router spreads are 0.356%, 1.261%, 2.992%, and 8.216%. No router migration was
made; v6 remains production.

## Verification

- Three focused tests pass, covering the same-sign gate, retained-SUE orientation, invalid inputs,
  lineage, migration definition, and both direct dependencies.
- The live partition has 18,342 unique IDs and natural keys, no null/non-finite/future-knowledge
  rows, exact monthly sample z-score normalization, breadth of 20-246, and maximum lineage of 586
  bytes.
- The live schema is `0232` with 206 checksummed migrations. Migration checksums, schema drift,
  schema-contract v2 pin, checkpoint, Ruff, compilation, and changed-tree diff hygiene pass.
- Full-suite execution was intentionally avoided.

## Next loop

Investigate whether gross-margin stability separates productive revenue growth from the extreme
growth names whose standalone long-horizon tails reverse. Predeclare a revenue-growth sleeve gated
by non-declining same-quarter gross margin, following classic fundamental-analysis signals, and
evaluate it in memory before creating another governed factor.
