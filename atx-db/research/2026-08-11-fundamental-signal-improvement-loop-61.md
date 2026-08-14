# Fundamental signal improvement loop 61: operating-cost inflexibility

Status: production feature added; **rejected upstream from mega-alpha**.

## Research and duplicate avoidance

Novy-Marx's operating-leverage proxy scales COGS plus SG&A by assets and links
higher operating leverage to higher expected returns
(https://doi.org/10.1093/rof/rfq019). A local audit found that Loop 39 had
already implemented and rejected that exact construction as
`risk_operating_leverage`; it was not retested.

Loop 61 pivoted before implementation to Taussig's distinct operating-cost
flexibility measure (https://doi.org/10.3390/risks12100161). That study defines
SDOC as the sample standard deviation of the natural log of annual operating
cost, where operating cost is COGS plus SG&A, over a rolling five-year window.
It reports a negative return relation: lower SDOC represents less flexible cost
structures, greater risk, and higher expected return, beyond the level-based
operating-leverage proxy.

The frozen ATX candidate is therefore:

`-stddev_samp(ln(COGS + SG&A), five annual observations)`.

Each cost requires positive, finite COGS plus SG&A from one exact accession.
Five visible annual observations must span 1300-1650 days, the newest may be no
older than 550 days, SDOC above five is excluded, and no cost is imputed. Lower
SDOC receives the higher score. The formula and orientation were fixed before
return inspection and cannot be reversed post hoc.

The first gate requires positive IC at every 21/63/126/252-day horizon and HAC
t-statistics of at least 2.0 at two horizons including 126 or 252 days. Failure
stops all more expensive testing. This overlap correction is especially
important for annual signals carried across monthly formation dates.

## Production implementation

The local catalog contained no SDOC or operating-cost-flexibility feature.
Canonical COGS and SG&A already had sufficient coverage; a pre-return audit
found valid five-year histories across 558 securities, so no source refresh was
needed.

Added `atx_db.operating_cost_inflexibility`, build command
`scripts/build_operating_cost_inflexibility.py`, focused formula/governance
tests, and append-only migration `0248`. At every formation date the feature
independently enforces filing visibility, resolves amendments by period end,
selects the newest five distinct annual observations, and records all values,
statement-point IDs, accessions, and availability timestamps in lineage.

The live build completed in 19.9 seconds and materialized 41,960 point-in-time
observations across 436 securities and 172 monthly dates from 2012-04-30 through
2026-06-15.

## Evidence and decision

Run ID: `loop61-operating-cost-inflexibility-screen`.

| Horizon | Rank IC | Conventional t | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0095 | 1.23 | 1.13 | 170 | 242.2 |
| 63d | 0.0141 | 1.68 | 1.08 | 168 | 240.3 |
| 126d | 0.0227 | 2.67 | 1.25 | 165 | 237.4 |
| 252d | 0.0302 | 2.93 | 0.94 | 159 | 231.4 |

The candidate is directionally coherent and strengthens with horizon, but no
HAC statistic reaches 2.0. The apparently significant conventional 126- and
252-day statistics are not reliable because adjacent monthly observations reuse
the same annual filings and forward-return windows overlap. The frozen
serial-correlation-aware gate correctly rejects that false precision.

Decision: **reject `risk_operating_cost_inflexibility` from mega-alpha**. No
full tail run, 2021+ stability run, or $50 million Polars cost test was
performed. There is intentionally no costed decision artifact. Router v6 and
the mega-alpha registry are unchanged.

The governed feature remains a promising research control and may be revisited
only under a predeclared independent extension, such as genuine filing-time
industry exclusions or non-overlapping annual inference—not by relaxing the
failed gate after seeing these results.

## Verification

- Two focused five-year SDOC, orientation, lineage, and governance tests pass in
  2.0 seconds.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0248 with 222 numeric
  migrations.
- No full suite was run.
