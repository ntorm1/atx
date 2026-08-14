# Fundamental signal improvement loop 62: organization capital

Status: production feature added; **rejected upstream from mega-alpha**.

## Research hypothesis

Eisfeldt and Papanikolaou construct organization capital by accumulating SG&A
through a perpetual-inventory model and report that firms with high
organization capital relative to assets earn materially higher average returns
(https://doi.org/10.1111/jofi.12034). Their mechanism is that organization
capital is partly embodied in key talent, giving shareholders a riskier claim on
its cash flows.

The local research and factor catalogs contained no organization-capital,
capitalized-SG&A, or equivalent perpetual-inventory feature. The paper's frozen
construction is:

`OC_t = (1 - 0.15) * OC_t-1 + SG&A_t / CPI_t`

with initialization

`OC_0 = (SG&A_1 / CPI_1) / (0.10 + 0.15)`.

ATX scales the resulting real stock by current assets divided by current CPI.
Higher organization capital to assets receives the higher score. At least five
consecutive visible annual observations are required, annual gaps must be
300-430 days, the latest filing may be no older than 550 days, the ratio may not
exceed 20, and SG&A is never zero-imputed. This last choice is stricter than the
paper and avoids treating disclosure absence as a real zero. All constants and
filters were frozen before return inspection.

The paper ranks firms relative to 17 industry peers. ATX does not yet have
historical point-in-time SIC/FF17 assignments, and applying current industry
labels to prior formation dates would leak future classification. This loop
therefore evaluates the production-safe unneutralized ratio across eligible
liquid U.S. common equities. A strict within-industry replication remains
dependent on filing-time industry history.

The first gate requires positive IC at every 21/63/126/252-day horizon and HAC
t-statistics of at least 2.0 at two horizons including 126 or 252 days. Failure
stops all more expensive testing.

## Production implementation

Canonical SG&A already covered 134,134 points across 765 securities, total
assets covered 180,873 points across 1,484 securities, and governed CPIAUCSL
observations covered 1947 through 2026. No source refresh was needed.

Added `atx_db.organization_capital`, build command
`scripts/build_organization_capital.py`, focused calculation/governance tests,
and append-only migration `0249`. At every formation date the feature:

- joins SG&A and assets by exact filing accession;
- selects only filing and CPI observations visible by the decision timestamp;
- resolves amendments independently for each historical period;
- enforces consecutive annual history and applies the recursive capital stock;
- records every SG&A point, CPI observation, filing timestamp, initialization
  parameter, asset denominator, and parent factor in lineage.

The live build completed in 25.0 seconds and materialized 59,607 point-in-time
observations across 514 securities and 167 monthly dates from 2012-12-31 through
2026-06-15. Histories contain 5-19 annual observations. Raw organization-capital
ratios range from 0.0048 to 13.7896 with a 0.7082 median, remain inside the
frozen bound, and have no duplicate natural keys.

## Evidence and decision

Run ID: `loop62-organization-capital-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | -0.0118 | -1.12 | 163 | 359.9 |
| 63d | -0.0131 | -0.70 | 161 | 358.4 |
| 126d | -0.0190 | -0.69 | 158 | 356.1 |
| 252d | -0.0157 | -0.40 | 152 | 351.7 |

The published high-organization-capital orientation is negative at every
horizon and fails every frozen sign and inference requirement. The observed
negative relation may partly reflect between-industry accounting composition,
which is exactly why the original study used industry-relative ranks, but that
diagnostic is not permission to reverse the signal or use a lookahead industry
classification.

Decision: **reject `intangibles_high_organization_capital` from mega-alpha**.
No full tail run, 2021+ stability run, or $50 million Polars cost test was
performed. There is intentionally no costed decision artifact. Router v6 and
the mega-alpha registry are unchanged.

The governed capital stock remains useful for intangible-adjusted valuation,
productivity, and future within-industry research after point-in-time industry
history is added.

## Verification

- Two focused recursive-stock, CPI scaling, orientation, lineage, and governance
  tests pass in 1.4 seconds.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0249 with 223 numeric
  migrations.
- No full suite was run.
