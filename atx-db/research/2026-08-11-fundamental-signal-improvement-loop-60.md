# Fundamental signal improvement loop 60: within-year inventory volatility

Status: production feature added; **rejected upstream from mega-alpha**.

## Research and duplicate avoidance

Thomas and Zhang find that inventory change drives much of the accrual anomaly
(https://papers.ssrn.com/sol3/papers.cfm?abstract_id=295247), while later
operations research finds that abnormal inventory growth predicts lower
long-run returns (https://doi.org/10.1016/j.jom.2013.05.002).

The initial Loop 60 target was inventory growth relative to sales. A catalog
audit found exact prior implementations: Loops 25-27 and 46 already tested
quarterly inventory change, inventory growth, and sales-adjusted abnormal
inventory growth. Repeating them would add selection bias without new evidence.

The loop therefore pivoted before implementation to Steinker and Hoberg's
distinct within-year inventory-volatility result. They report that companies
with high within-year quarterly inventory volatility earn higher long-run
returns, with the original sample focused on U.S. manufacturers. The frozen
candidate is the unit-free coefficient of variation:

`stddev_samp(inventory_q0..q3) / mean(inventory_q0..q3)`.

Four positive, point-in-time-visible quarterly observations must span 240-310
days; the latest observation must be no older than 200 days; ratios above five
are excluded; and no quarter is imputed. Higher volatility receives the higher
score. The formula and orientation were fixed before return inspection and
cannot be reversed post hoc.

The warehouse currently lacks historical point-in-time SIC classifications.
Using today's SIC to filter old formation dates would introduce lookahead, so
the production-safe evaluation covers liquid U.S. common equities that report
inventory rather than claiming a manufacturing-only replication. Historical
filing-time SIC remains a provider backlog item.

The first gate requires positive IC at every 21/63/126/252-day horizon and HAC
t-statistics of at least 2.0 at two horizons including 126 or 252 days. Failure
stops all more expensive testing.

## Production implementation

The existing canonical inventory partition already contained 94,592 statement
points across 905 securities, so no source refresh was needed. Added
`atx_db.inventory_volatility`, build command
`scripts/build_inventory_volatility.py`, focused formula/governance tests, and
append-only migration `0247`.

The feature deduplicates visible filing revisions independently at every
formation date, selects four distinct recent period ends, verifies their
calendar span, and retains statement-point IDs, accessions, values, and
availability timestamps in lineage. It reuses the governed asset-growth monthly
formation scaffold.

The live build completed in 23.3 seconds and materialized 81,585 point-in-time
observations across 611 securities and 175 monthly dates from 2012-04-30 through
2026-06-15.

## Evidence and decision

Run ID: `loop60-inventory-volatility-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | -0.0044 | -0.88 | 171 | 470.5 |
| 63d | -0.0029 | -0.41 | 169 | 469.3 |
| 126d | -0.0065 | -0.65 | 166 | 467.5 |
| 252d | -0.0161 | -1.14 | 160 | 463.9 |

The literature-oriented score is negative at every horizon and becomes most
negative at one year. Although the conventional one-year t-statistic is -2.91,
the overlap-robust HAC statistic is only -1.14. The candidate fails every sign
requirement and every frozen inference requirement.

Decision: **reject `operations_high_inventory_volatility` from mega-alpha**.
The negative relation is diagnostic evidence for this broader, point-in-time
safe inventory-reporting universe, not permission to reverse the candidate
after inspection. No full tail run, 2021+ stability run, or $50 million Polars
cost test was performed. Router v6 and the mega-alpha registry are unchanged.

The governed feature remains available for risk, supply-chain, and future
manufacturing-conditional research once filing-time industry history exists.

## Verification

- Two focused coefficient-of-variation, orientation, lineage, and governance
  tests pass in 1.2 seconds.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0247 with 221 numeric
  migrations.
- No full suite was run.
