# Fundamental signal improvement loop 65: Beneish manipulation risk

Status: production feature added; **rejected upstream from mega-alpha**.

## Research and duplicate avoidance

Beneish's original eight-variable model combines receivables growth, gross-margin
deterioration, asset quality, sales growth, depreciation, SG&A, accruals, and
leverage to detect earnings-manipulation pressure
(https://diyinvestor.de/wp-content/uploads/2024/09/Beneish-M-Score-paper-1999.pdf).
Beneish, Lee, and Nichols subsequently report that firms with higher manipulation
probability earn lower returns across size, value, momentum, accrual, and short-
interest partitions
(https://www.gsb.stanford.edu/faculty-research/publications/earnings-manipulation-expected-returns).

A June 2026 U.S. replication is especially relevant to the current loop: it reports
that the 1999 M-score retains a -4.61% annual value-weighted spread alpha with a
-2.53 t-statistic even while traditional accrual and Dechow-Dichev signals no longer
produce significant modern alpha
(https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6881278).

ATX already had the exact eight-variable calculation in its governed formula
library, but had never materialized it as a PIT factor. No prior loop or factor
partition duplicated the candidate. The frozen raw score is the negative M-score:

```text
M = -4.84 + 0.920*DSRI + 0.528*GMI + 0.404*AQI + 0.892*SGI
    + 0.115*DEPI - 0.172*SGAI + 4.679*TATA - 0.327*LVGI
factor score = -M
```

Higher factor values identify lower manipulation risk. The current and prior annual
statements must be exact-accession filings visible at formation and separated by
300-430 days. Revenue, receivables, COGS, assets, current assets, PP&E, D&A, SG&A,
liabilities, net income, and operating cash flow are required; no value is imputed.
D&A precedence is cash-flow D&A, income-statement D&A, then depreciation. Scores are
winsorized 1% per tail and cross-sectionally standardized.

The first gate requires positive IC at every 21/63/126/252-day horizon and HAC
t-statistics of at least 2.0 at two horizons including 126 or 252 days. The sign was
frozen before return inspection and cannot be reversed after observing results.

## Production implementation

A pre-return feasibility query found 9,795 strict PIT monthly observations across
156 securities and clean 364-371-day statement pairs over 2012-2026.

Added `atx_db.beneish_m_score`, `scripts/build_beneish_m_score.py`, focused exact-
formula/orientation/governance tests, and append-only migration `0252`. Lineage
stores the M-score, all eight indices, both years of component values and point IDs,
accessions, availability timestamps, filing periods, D&A precedence, and the
decision timestamp.

The production transaction committed 8,830 observations across 156 securities and
110 breadth-qualified rebalance dates from 2013-05-31 through 2026-06-15. The shell
wrapper crossed its 45-second deadline while the spawned builder retained the
warehouse lock. The exact builder was allowed to finish and its committed partition
was inspected directly; it was not restarted or duplicated.

## Evidence and decision

Run ID: `loop65-beneish-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | -0.0251 | -1.60 | 108 | 79.9 |
| 63d | -0.0266 | -1.39 | 106 | 79.5 |
| 126d | -0.0341 | -1.51 | 103 | 79.1 |
| 252d | -0.0471 | -1.44 | 97 | 78.1 |

The low-manipulation score is incorrectly signed at every horizon and becomes more
negative with horizon. Reversing it after inspection would violate the frozen
research contract; in any event, the reverse sign would still have absolute HAC
t-statistics below 2.0 at every horizon.

Decision: **reject `quality_low_beneish_m_score` from mega-alpha**. No full tail
run, 2021+ stability run, or $50 million Polars capacity test was performed. There
is intentionally no costed decision artifact. Router v6 and the mega-alpha registry
remain unchanged. The governed M-score remains valuable as a forensic-accounting,
data-quality, and misstatement-risk product feature.

## Verification

- Two focused exact-formula, orientation, lineage, and governance tests pass in
  7.22 seconds.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0252 with 226 applied numeric
  migrations.
- No full suite was run.
