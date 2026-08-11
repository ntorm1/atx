# Fundamental signal improvement loop 27: sales-adjusted inventory growth

Date: 2026-08-09

## Research hypothesis

Inventory growth can be either an adverse demand mismatch or a rational response to expanding
sales. The predeclared hypothesis was therefore that year-over-year inventory growth unsupported
by year-over-year sales growth is the harmful component. This follows the abnormal-inventory-growth
literature and Abarbanell and Bushee's use of inventory/sales relations in fundamental analysis.

Primary references:

- [Abarbanell and Bushee, *Abnormal Returns to a Fundamental Analysis Strategy*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=40740)
- [Bendig et al., *The impact of inventory dynamics on long-term stock returns*](https://www.sciencedirect.com/science/article/pii/S0272696313000399)
- [Chen, Frank, and Wu, *U.S. Retail and Wholesale Inventory Performance from 1981 to 2004*](https://pubsonline.informs.org/doi/abs/10.1287/msom.1060.0129)

## Point-in-time implementation

Added `atx_db.quarterly_abnormal_inventory_growth`, migration `0230`, a standalone CLI, and three
focused tests. The governed factor is
`investment_low_quarterly_abnormal_inventory_growth`, sourced as
`atx-db PIT quarterly abnormal inventory growth v1`:

`(inventory_t / inventory_t-4 - 1) - (revenue_t / revenue_t-4 - 1)`.

Lower values are preferred. The builder starts from every governed `Claq` row and its linked
quarterly operating-profitability row, then selects the closest prior governed report 330-400 days
earlier for the same security. Both reports must be visible at the current monthly decision.
Inventory and revenue endpoints must be positive; component and final ratios receive predeclared
absolute guards of ten, followed by sign inversion, monthly 1%/99% winsorization, and z-scoring.

The history join finds 51,253 candidate pairs in 2.9 seconds. After the published positive-value and
scale contract, the live build has 28,999 rows across 384 securities and 160 dates from 2013-04-30
through 2026-06-15. The full transactional build completed in 23.5 seconds.

## Standalone analysis

Run id: `loop27-quarterly-abnormal-inventory-growth-production`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0133 | 1.29 | 0.041% | 53.2% | 0.38 |
| 63d | 0.0109 | 0.75 | -0.531% | 55.8% | 0.13 |
| 126d | -0.0053 | -0.25 | 0.038% | 57.5% | 0.25 |
| 252d | -0.0190 | -0.69 | -2.945% | 57.1% | 0.09 |

The predeclared combined hypothesis fails full-history production criteria. It has weak short-horizon
IC, reverses at longer horizons, and lacks monotonic tails. Top/bottom turnover is 35.7%/35.4% and
rank autocorrelation is 0.892.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2013-2015 | 0.0383 | 0.0416 | 0.0397 | 0.0197 |
| 2016-2020 | -0.0206 | -0.0327 | -0.0734 | -0.0703 |
| 2021-2026 | 0.0326 | 0.0374 | 0.0401 | 0.0161 |
| 2023-2026 | 0.0495 | 0.0557 | 0.0680 | 0.0310 |

The recent signal is strong, including HAC t-stats of 3.69/3.21/3.33/1.94 in 2023+, but the severe
2016-2020 reversal makes a production promotion unjustified. The feature remains queryable so
downstream users can study the regime shift without hiding an unsuccessful full-history result.

### Component attribution

The two formula legs were reconstructed on the exact 28,999-row cohort without changing their
literature-defined signs.

| Component | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| High YoY sales growth | 0.0261 / 2.18 | 0.0355 / 1.89 | 0.0400 / 1.50 | 0.0418 / 1.34 |
| Low YoY inventory growth | -0.0126 / -1.14 | -0.0280 / -1.50 | -0.0474 / -1.82 | -0.0559 / -1.51 |

The sales term contributes in the intended direction at every horizon. The year-over-year inventory
term has the opposite sign on this cohort: higher, not lower, inventory growth predicts returns.
That finding explains why subtracting inventory growth destroys the otherwise useful sales signal.
The sign is not flipped after observing returns; high YoY sales growth becomes the next loop's
separately predeclared feature.

### Distinctiveness

Mean cross-sectional correlation is 0.193 with quarterly inventory change, 0.262 with quarterly
inventory growth, 0.144 with quarterly low working-capital accruals, 0.116 with `Claq`, and 0.003
with production router v6.

## Router comparison and decision

The candidate again changed only within-primary-decile ordering.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Cash profitability (production v6) | 0.02281 | 0.03653 | 0.05306 | 0.07247 |
| Abnormal inventory growth | 0.02265 | 0.03565 | 0.05226 | 0.07120 |

The candidate trails v6 at every full-history horizon. In 2023+ it wins only at 21 days and trails at
63/126/252 days. Its 21/63/126/252-day tail spreads are 0.356%, 1.264%, 3.000%, and 8.310%, close
to but not better than v6 as a group. No router migration was made.

## Verification

- Three focused tests pass, covering formula/sign, component guards, deterministic lineage,
  migration definition, and both direct factor dependencies.
- The live partition has 28,999 unique IDs and natural keys, no null/non-finite/future-knowledge
  rows, exact monthly sample z-score normalization, breadth of 22-336, and maximum lineage of
  1,732 bytes.
- The live schema is `0230` with 204 checksummed migrations. Migration checksums, schema drift,
  schema-contract v2 pin, checkpoint, Ruff, compilation, and changed-tree diff hygiene pass.
- Full-suite execution was intentionally avoided.

## Next loop

Publish high same-quarter year-over-year sales growth directly from the governed pair and test it
outside the inventory-carrying subset where possible. Compare it with existing revenue-surprise and
expected-growth surfaces, assess whether the 2016-2020 failure belongs to the inventory penalty or
to sales growth itself, and require both full-history and modern evidence before any router change.
