# Fundamental signal improvement loop 26: quarterly inventory investment

Date: 2026-08-09

## Research hypothesis

Loop 25's predeclared component attribution found that low inventory change was the only
working-capital leg with positive IC at all four production horizons. That is consistent with
Thomas and Zhang's finding that inventory change accounts for most of the negative
accrual-future-return relation. Belo and Lin likewise document a low-minus-high inventory growth
spread and interpret it through costly inventory investment.

The formulas were pinned to the Global-q testing-portfolio definitions before implementation:

- `Ivc`: inventory change divided by average current/prior total assets.
- `Ivg`: current inventory divided by prior inventory minus one.
- Firms carrying no inventory in either comparison period are excluded.

This loop adapts the horizon from annual to sequential quarterly reports and replaces the fixed June
delay with actual SEC knowledge timestamps. It does not fit a coefficient, sign, guard, or window to
returns.

Primary references:

- [Thomas and Zhang, *Inventory Changes and Future Returns*](https://papers.ssrn.com/abstract=295247)
- [Belo and Lin, *The Inventory Growth Spread*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1526726)
- [Global-q testing-portfolio technical document](https://global-q.org/uploads/1/2/2/6/122679606/portfoliostd_2025may.pdf)

## Point-in-time implementation

Added `atx_db.quarterly_inventory_investment`, migration `0229`, one dual-factor CLI, and three
focused tests. The governed source `atx-db PIT quarterly inventory investment v1` publishes:

- `investment_low_quarterly_inventory_change`
- `investment_low_quarterly_inventory_growth`

Both builders start with the exact complete current/prior inventory pair already selected in `Claq`.
The change variant adds a current total-assets fact from the same accession and period; the prior
asset value is the exact governed lagged-assets denominator. Every input must be visible by the
monthly decision time. Positive inventory and asset values are required, absolute raw values are
guarded at 5 for change and 10 for growth, and each raw measure is negated, monthly 1%/99%
winsorized, and z-scored.

The joint live build contains 50,543 change rows and 50,540 growth rows, each spanning 421
securities and 173 dates from 2012-04-30 through 2026-06-15. The original loader aggregated every
total-assets statement and took about 73 seconds before feature calculation. An exact-key direct
join reduced input loading to 6.3 seconds and full in-memory construction to 24.6 seconds with zero
raw or standardized value changes. The full transactional refresh remains dominated by replacing
100,000 JSON-lineage rows and the large-file checkpoint.

## Standalone analysis

Run id: `loop26-quarterly-inventory-investment-production`.

| Factor | Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---|---:|---:|---:|---:|---:|---:|
| Inventory change | 21d | 0.0161 | 2.42 | 0.383% | 55.6% | 0.47 |
| Inventory change | 63d | 0.0196 | 1.93 | 1.031% | 64.5% | 0.55 |
| Inventory change | 126d | 0.0242 | 2.04 | 5.464% | 63.3% | 0.66 |
| Inventory change | 252d | 0.0238 | 2.28 | 4.611% | 55.6% | 0.41 |
| Inventory growth | 21d | 0.0160 | 2.59 | 0.108% | 45.6% | 0.62 |
| Inventory growth | 63d | 0.0178 | 1.71 | 1.211% | 55.6% | 0.59 |
| Inventory growth | 126d | 0.0204 | 1.64 | 3.462% | 56.0% | 0.67 |
| Inventory growth | 252d | 0.0191 | 1.79 | 0.080% | 52.5% | 0.41 |

Average-assets inventory change is the stronger standalone feature: it wins mean IC at every
horizon, has HAC above two at 21/126/252 days, and produces much stronger 126/252-day tails.
Top/bottom turnover is 42.5%/42.6% with 0.785 rank autocorrelation. Inventory growth turns over at
42.4%/42.8% with 0.779 autocorrelation. Both therefore remain information features rather than
independent low-churn portfolios.

### Regime stability

| Variant and period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Change, 2012-2015 | 0.0093 | -0.0038 | -0.0024 | 0.0138 |
| Change, 2016-2020 | 0.0213 | 0.0306 | 0.0351 | 0.0293 |
| Change, 2021-2026 | 0.0160 | 0.0256 | 0.0329 | 0.0258 |
| Change, 2023-2026 | 0.0073 | 0.0102 | 0.0320 | 0.0082 |
| Growth, 2012-2015 | 0.0116 | -0.0029 | -0.0114 | 0.0072 |
| Growth, 2016-2020 | 0.0151 | 0.0237 | 0.0279 | 0.0169 |
| Growth, 2021-2026 | 0.0198 | 0.0270 | 0.0367 | 0.0318 |
| Growth, 2023-2026 | 0.0128 | 0.0112 | 0.0338 | 0.0084 |

The signal is weak in 2012-2015 and positive across every horizon since 2016. Growth is somewhat
better in the modern cohort; average-assets change retains the stronger full-history inference and
tail returns. This is a useful independent surface but not unanimous evidence for a router change.

### Distinctiveness

Inventory change has mean cross-sectional correlation of 0.820 with inventory growth, 0.434 with
quarterly low working-capital accruals, 0.265 with `Claq`, 0.050 with conservative annual asset
growth, and only 0.009 with production router v6. Inventory growth correlations are 0.426, 0.228,
0.092, and 0.047 respectively. The quarterly signal is not a repackaging of the annual investment
surface.

## Router comparison and decision

Each candidate changed only ordering inside the production primary router's fixed deciles.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Cash profitability (production v6) | 0.02281 | 0.03653 | 0.05306 | 0.07247 |
| Inventory change | 0.02271 | 0.03613 | 0.05296 | 0.07211 |
| Inventory growth | 0.02275 | 0.03606 | 0.05278 | 0.07188 |

Both alternatives trail v6 mean IC at every full-history horizon and at every 2023+ horizon. Their
tail spreads are close to v6 and their extreme-decile membership turnover is necessarily identical,
but neither clears the promotion threshold. No router migration was made; source
`atx-db governed cash-decile router v6` remains live with 114,684 rows.

## Verification

- Three focused tests pass, covering both formulas and orientations, zero/incomplete exclusions,
  variant-specific guards, migration definitions, and direct dependencies.
- Both live partitions have unique factor IDs and natural keys, no null/non-finite/future-knowledge
  rows, exact monthly sample z-score normalization, breadth of 25-360, and maximum lineage of
  1,520 bytes.
- The live schema is `0229` with 203 checksummed migrations. Migration checksums, schema drift,
  schema-contract v2 pin, checkpoint, Ruff, compilation, and changed-tree diff hygiene pass.
- Full-suite execution was intentionally avoided.

## Next loop

Test sales-adjusted inventory investment rather than inventory in isolation. The predeclared
hypothesis is that inventory growth in excess of sales growth is the adverse demand-mismatch
signal, while inventory growth supported by sales should not receive the same penalty. Use
seasonally matched quarterly periods where coverage permits and compare it with the two direct
inventory features before considering any router change.
