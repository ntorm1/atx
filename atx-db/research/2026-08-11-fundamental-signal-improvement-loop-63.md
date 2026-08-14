# Fundamental signal improvement loop 63: large R&D increases

Status: production feature added; **rejected upstream from mega-alpha**.

## Research and duplicate avoidance

Chan, Lakonishok, and Sougiannis find that high R&D relative to market equity
identifies a subset of R&D firms with high subsequent returns
(https://doi.org/10.1111/0022-1082.00411). The local audit found that Loop 37
had already implemented and rejected that exact R&D-to-market construction as
`valuation_rd_to_market_equity`; it was not retested.

Loop 63 pivoted before implementation to the distinct R&D-increase event in
Eberhart, Maxwell, and Siddique
(https://doi.org/10.1111/j.1540-6261.2004.00644.x). They report positive
long-run abnormal returns and operating performance after firms make unexpected,
economically significant increases in R&D.

The frozen binary candidate requires all five published criteria:

1. current R&D / sales is greater than 5%;
2. current R&D / average current-prior assets is greater than 5%;
3. annual R&D growth is greater than 5%;
4. annual growth in R&D / sales is greater than 5%;
5. annual growth in R&D / average assets is greater than 5%.

Qualifying observations receive one and other fully observed R&D firms receive
zero. The current, prior, and two-year-lag filings must form a consecutive
300-430-day annual sequence and be visible at formation; the newest filing may
be no older than 550 days. R&D, revenue, and assets must all be positive, and no
value is imputed. The event definition and positive orientation were frozen
before return inspection.

The first gate requires positive IC at every 21/63/126/252-day horizon and HAC
t-statistics of at least 2.0 at two horizons including 126 or 252 days. Failure
stops all more expensive testing.

## Production implementation

The catalog contained no R&D-increase feature. A pre-return feasibility audit
found 4,537 strict three-year observations across 550 securities, including 312
historical qualifying events, so no source refresh was required.

Added `atx_db.rd_increase`, build command `scripts/build_rd_increase.py`,
focused five-criterion/governance tests, and append-only migration `0250`. The
feature resolves annual R&D and revenue within an exact accession, joins the
same filing's assets, independently resolves amendments at each formation date,
and stores all three years of point IDs, accessions, timestamps, component
ratios, growth rates, and criterion outcome in lineage.

The production transaction atomically committed 35,602 observations across 425
securities and 173 monthly dates from 2012-04-30 through 2026-06-15. It contains
2,256 qualifying event rows; monthly event breadth ranges from 2 to 26 with a
median of 12. The wrapper crossed its 45-second print deadline at 48.3 seconds,
but the exact process had exited and direct target-partition inspection verified
the complete committed result. The build was not repeated.

## Evidence and decision

Run ID: `loop63-rd-increase-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | 0.0014 | 0.18 | 170 | 205.2 |
| 63d | -0.0088 | -0.70 | 168 | 203.3 |
| 126d | -0.0177 | -0.99 | 165 | 200.4 |
| 252d | -0.0236 | -0.97 | 159 | 194.1 |

The event is effectively flat at one month and becomes increasingly negative
through one year. It fails the frozen sign condition at three horizons and has
no positive inference support. The literature-oriented event cannot be reversed
or redefined after seeing these results.

Decision: **reject `intangibles_large_rd_increase` from mega-alpha**. No full
tail run, 2021+ stability run, or $50 million Polars cost test was performed.
There is intentionally no costed decision artifact. Router v6 and the
mega-alpha registry are unchanged.

The governed event remains useful for innovation-investment, operating-
performance, and corporate-event research despite its failed standalone return
forecast in the ATX universe.

## Verification

- Two focused five-criterion, orientation, lineage, and governance tests pass in
  1.4 seconds.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0250 with 224 numeric
  migrations.
- No full suite was run.
