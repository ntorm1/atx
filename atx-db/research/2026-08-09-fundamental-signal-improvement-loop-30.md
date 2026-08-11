# Fundamental signal improvement loop 30: revenue growth with gross-margin confirmation

Date: 2026-08-09

## Research hypothesis

Revenue growth can be less informative when it is bought through deteriorating unit economics.
Classic fundamental analysis treats sales growth without gross-margin support as a possible sign of
competition or an adverse cost structure. Loop 30 therefore predeclared a direct revenue-growth
sleeve that retains only companies whose same-quarter gross margin did not decline year over year.

Primary references:

- [Abarbanell and Bushee, *Fundamental Analysis, Future Earnings, and Stock Prices*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=40740)
- [CFA Institute Research Foundation, *The Usefulness of Fundamental Analysis*](https://rpc.cfainstitute.org/sites/default/files/-/media/documents/book/rf-publication/2004/rf-v2004-n3-3927-pdf.pdf)
- [Jegadeesh, *Revenue Growth and Stock Returns*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=314962)

## Pre-registration and data guard

The initial in-memory gate retained revenue-growth observations when current gross margin was at
least the prior-year same-quarter gross margin. Raw source reconstruction exposed extreme margins
from sparse accounting identities (gross-margin changes ranged from roughly -899 to +59), so the
pre-registration was amended before productionization with a symmetric absolute gross-margin guard
of 5.0 on both endpoints. This removed 128 of 33,088 eligible revenue-growth observations and left
17,059 production candidates. The threshold is a data-validity bound, not a fitted return parameter.

The guarded pre-registration produced 21/63/126/252-day rank IC of
0.02105/0.02882/0.04486/0.05682 and positive top-minus-bottom spreads at every horizon. That cleared
the production feature threshold.

## Point-in-time implementation

Added `atx_db.quarterly_revenue_margin_confirmation`, migration `0233`, a standalone CLI, and three
focused tests. The governed factor is `growth_quarterly_revenue_margin_confirmation`, sourced as
`atx-db PIT quarterly revenue margin confirmation v1`:

`zscore(revenue_growth_score | gross_margin_t - gross_margin_t_4 >= 0)`.

The loader starts from the governed quarterly revenue-growth feature and joins the exact current and
prior quarterly-operating-profitability input IDs recorded in its lineage. Gross profit is rebuilt
from revenue minus COGS, reported gross profit, or operating income plus SG&A minus R&D, in that
order. It rejects non-finite values and gross margins outside +/-5.0. `available_at` is the maximum
of the revenue-growth, current-quarter, and prior-quarter inputs. Revenue growth remains the ranking
variable; gross-margin change is only a gate, and the retained monthly cross-section is re-zscored.

The live build exactly matched pre-registration: 17,059 rows across 432 securities and 154 dates
from 2013-04-30 through 2026-06-15. It completed in 36 seconds.

## Standalone analysis

Run id: `loop30-quarterly-revenue-margin-confirmation-production-evaluation`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0211 | 1.43 | 0.599% | 52.0% | 0.370 |
| 63d | 0.0288 | 1.37 | 1.965% | 60.7% | 0.333 |
| 126d | 0.0449 | 1.73 | 5.705% | 63.3% | 0.358 |
| 252d | 0.0568 | 1.79 | 15.174% | 63.8% | 0.285 |

The gate fixes the direct revenue-growth factor's negative long-horizon tail spread while retaining
positive IC at every horizon. Top/bottom turnover is 31.1%/39.0%, mean rank autocorrelation is
0.945, and there are 153 rebalance pairs.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2013-2015 | 0.0315 | 0.0580 | 0.0521 | 0.0274 |
| 2016-2020 | 0.0150 | 0.0220 | 0.0493 | 0.0953 |
| 2021-2026 | 0.0212 | 0.0199 | 0.0367 | 0.0339 |
| 2023-2026 | 0.0483 | 0.0418 | 0.0691 | 0.0848 |

The recent regime is strongest: 2023+ HAC t-stats are 2.81/2.03/4.14/7.92. Evidence is positive
in every reported regime and horizon, although early cross-sections are narrow and full-history HAC
strength remains below 2.0.

The factor has correlation 1.0 with direct revenue growth on their common gated cohort because the
gate does not change within-cohort growth ordering. Correlations are 0.591 with standardized
unexpected revenue, 0.228 with quarterly operating profitability, 0.146 with quarterly cash
profitability, and -0.094 with production router v6. The feature therefore adds a well-defined
eligibility cohort, not a new continuous ranking transformation.

## Router comparison and decision

The candidate changed only ordering inside production primary deciles; it used the secondary key on
16,980 of the router's unchanged 114,684 rows.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|---:|
| Cash profitability (production v6) | 0.02281 | 0.03653 | 0.05306 | 0.07247 |
| Revenue/margin confirmation | 0.02255 | 0.03555 | 0.05231 | 0.07126 |

The candidate trails v6 at all four full-history horizons and all four 2023+ horizons. Its
21/63/126/252-day router spreads are 0.356%, 1.262%, 2.996%, and 8.258%, versus v6's
0.352%, 1.272%, 2.997%, and 8.333%. No router migration was made; v6 remains production.

## Verification

- The two pure-computation tests and the single governed migration test pass; the full suite was
  intentionally not run.
- The live partition has 17,059 unique IDs and natural keys, no null/non-finite/future-knowledge
  rows, valid JSON lineage, exact monthly sample z-score normalization, breadth of 20-268, and
  maximum lineage of 1,090 bytes.
- The live schema is `0233` with 207 checksummed migrations. Migration checksums, schema drift,
  schema-contract v2 pin, and checkpoint pass.
- Ruff passes on the implementation, migration, CLI, and focused test.

## Next loop

Investigate a second unit-economics confirmation that is not rank-identical to direct revenue
growth. Predeclare a gross-profit-growth feature using same-quarter prior-year gross profit, compare
it directly with revenue growth and gross-margin-gated revenue growth, and productionize it only if
its long-horizon improvement is not explained solely by the existing gate.
