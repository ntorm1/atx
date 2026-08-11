# Fundamental signal improvement loop 28: same-quarter revenue growth

Date: 2026-08-09

## Research hypothesis

Loop 27's predeclared attribution found that high same-quarter year-over-year sales growth had
positive IC at every horizon, while its inventory penalty had the wrong sign. Jegadeesh documents
post-announcement drift after large revenue surprises and argues that analysts underreact to their
persistent implications. Jegadeesh and Livnat further show that revenue information strengthens
earnings-surprise drift when both signals agree.

Primary references:

- [Jegadeesh, *Revenue Growth and Stock Returns*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=314962)
- [Jegadeesh and Livnat, *Post-Earnings-Announcement Drift: The Role of Revenue Surprises*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=903767)
- [Abarbanell and Bushee, *Abnormal Returns to a Fundamental Analysis Strategy*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=40740)

The predeclared feature is simple same-quarter revenue growth. It is deliberately distinct from the
existing standardized unexpected revenue factor and introduces no return-fitted coefficient.

## Point-in-time implementation

Added `atx_db.quarterly_revenue_growth`, migration `0231`, a standalone CLI, and three focused
tests. The governed factor is `growth_quarterly_revenue_yoy`, sourced as
`atx-db PIT quarterly revenue growth v1`:

`revenue_t / revenue_t-4 - 1`.

The builder begins with the governed quarterly operating-profitability history and selects the
closest prior report 330-400 days earlier for the same security. Both rows must be visible by the
current monthly decision, both revenue endpoints must be positive, and absolute raw growth above
ten is rejected. The result is monthly 1%/99% winsorized and z-scored.

Of 56,805 current decisions, 51,253 have a prior-year governed row, 33,142 have positive revenue at
both endpoints, and 33,121 clear the scale guard. The monthly breadth contract leaves 33,088 live
rows across 439 securities and 161 dates from 2013-04-30 through 2026-06-15. The full build took
40 seconds.

## Standalone analysis

Run id: `loop28-quarterly-revenue-growth-production`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0225 | 2.00 | 0.358% | 49.7% | 0.09 |
| 63d | 0.0347 | 1.96 | 0.236% | 55.4% | 0.04 |
| 126d | 0.0479 | 1.81 | -0.585% | 60.4% | -0.07 |
| 252d | 0.0498 | 1.56 | -1.691% | 63.5% | 0.02 |

Mean IC increases monotonically with horizon and sign consistency reaches 69.6% at one year. The
extreme-decile result does not agree: top-minus-bottom returns reverse at 126/252 days and the
cross-decile curve is nearly flat. This is a useful broad rank feature, not an independent
extreme-growth strategy. Top/bottom turnover is 32.7%/34.2% and rank autocorrelation is 0.928.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2013-2015 | 0.0700 | 0.0948 | 0.1037 | 0.0645 |
| 2016-2020 | 0.0016 | 0.0081 | 0.0206 | 0.0468 |
| 2021-2026 | 0.0184 | 0.0294 | 0.0453 | 0.0440 |
| 2023-2026 | 0.0385 | 0.0507 | 0.0756 | 0.0984 |

The weak 2016-2020 interval remains positive rather than reversing, and the modern evidence is
strong. In 2023+, HAC t-stats are 2.50/3.19/3.65/5.45. Full-history and modern mean IC therefore
agree even though the tail portfolio is unsuitable.

### Existing revenue-signal comparison

The new factor overlaps the existing standardized unexpected revenue surface on 26,310 rows.

| Common-cohort factor | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Same-quarter revenue growth | 0.0252 | 0.0335 | 0.0507 | 0.0439 |
| Standardized unexpected revenue | 0.0218 | 0.0297 | 0.0354 | 0.0219 |

Simple growth wins every horizon, especially at 126/252 days. Its mean correlation is 0.590 with
standardized unexpected revenue, 0.539 with the SUE/revenue confirmation composite, 0.507 with the
SUE/revenue agreement signal, 0.149 with `Claq`, and -0.037 with production router v6. It adds
information beyond the existing revenue-surprise transform.

## Router comparison and decision

The candidate used revenue growth only as a within-primary-decile secondary.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Cash profitability (production v6) | 0.02281 | 0.03653 | 0.05306 | 0.07247 |
| Same-quarter revenue growth | 0.02255 | 0.03579 | 0.05282 | 0.07197 |

The revenue candidate trails v6 at every full-history horizon. It improves mean IC at every horizon
in 2023+, but those gains are small and its HAC and tail results do not improve as a group. Its
21/63/126/252-day router spreads are 0.355%, 1.260%, 2.988%, and 8.289%. No migration was made;
production remains v6.

## Verification

- Three focused tests pass, covering formula/orientation, positive denominators, scale rejection,
  deterministic lineage, migration definition, and the direct QOP dependency.
- The live output has 33,088 unique IDs and natural keys, no null/non-finite/future-knowledge rows,
  exact monthly sample z-score normalization, breadth of 23-372, and maximum lineage of 1,050
  bytes.
- The live schema is `0231` with 205 checksummed migrations. Migration checksums, schema drift,
  schema-contract v2 pin, checkpoint, Ruff, compilation, and changed-tree diff hygiene pass.
- Full-suite execution was intentionally avoided.

## Next loop

Test the paper's confirmation mechanism directly: combine standardized unexpected earnings with
same-quarter revenue growth only when their signs agree. Compare it on a common cohort with the
existing standardized-revenue agreement factor and require improved tail behavior, not just mean
rank IC, before considering a router change.
