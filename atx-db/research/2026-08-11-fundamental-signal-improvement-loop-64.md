# Fundamental signal improvement loop 64: low RSST total accruals

Status: production feature added; **rejected upstream from mega-alpha**.

## Research, pivots, and duplicate avoidance

The loop began with cash-based operating profitability because Ball, Gerakos,
Linnainmaa, and Nikolaev find that it outperforms accrual-inclusive profitability
measures and subsumes the traditional accrual anomaly
(https://doi.org/10.1016/j.jfineco.2016.03.002). The catalog audit found complete
annual and quarterly production implementations from earlier loops, so that
candidate was not rebuilt or retested.

The second candidate was the cash conversion cycle. Wang defines quarterly CCC as
365 times average inventory over COGS plus average receivables over sales minus
average payables over COGS, and reports that low industry-adjusted CCC predicts
higher returns
(https://www.ivey.uwo.ca/media/3789366/the-cash-conversion-cycle-spread.pdf).
The raw formula already exists in the ATX ratio library. However, the published
signal adjusts CCC by the Fama-French 48-industry median. A warehouse audit proved
that the current SIC/FF12 classifications were first known on 2026-08-09 and have
zero PIT coverage on the 2012-2026 factor scaffold. Backdating today's industry was
rejected as lookahead, and the CCC candidate was deferred rather than weakened.

The final preregistered candidate was Richardson-Sloan-Soliman-Tuna comprehensive
accruals. Richardson et al. relate less reliable accruals to lower earnings
persistence and significant mispricing
(https://papers.ssrn.com/sol3/papers.cfm?abstract_id=521062). A June 2026 U.S.
replication reports that traditional total accruals and Dechow-Dichev accrual
quality no longer earn significant modern alpha, while the broader RSST measure
retains significant equal-weighted alpha
(https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6881278). This made RSST the
best distinct, implementable candidate for the loop.

The frozen construction is:

```text
NOA = total assets - cash and short-term investments - total liabilities
      + short-term debt + long-term debt
RSST accruals = (NOA(t) - NOA(t-1)) / average(total assets(t), total assets(t-1))
factor score = -RSST accruals
```

Lower comprehensive accruals are preferred. Two consecutive exact-accession annual
balance sheets, a 300-430-day period gap, a maximum reporting age of 550 days, and
all five non-imputed components are required. Values are winsorized 1% per tail and
cross-sectionally standardized. The first gate requires positive IC at all four
21/63/126/252-day horizons and HAC t-statistics of at least 2.0 at two horizons,
including 126 or 252 days. Failure stops all more expensive testing.

## Production implementation

The duplicate audit found the existing level-NOA factor but no annual change-in-NOA
or RSST feature. A pre-return query found 29,626 feasible monthly observations
across 361 securities, with clean annual gaps of 336-371 days.

Added `atx_db.rsst_accruals`, `scripts/build_rsst_accruals.py`, focused formula and
governance tests, and append-only migration `0251`. The builder uses the governed
NOA factor as its monthly universe scaffold, independently resolves exact-accession
annual statements and amendments visible at each formation date, and records both
years of component values, point IDs, accessions, availability timestamps, NOA, and
assets in lineage. Missing balance-sheet components are never filled with zero.

The production build completed in 28.5 seconds and committed 29,607 observations
across 361 securities and 172 rebalance dates from 2012-04-30 through 2026-06-15.

## Evidence and decision

Run ID: `loop64-rsst-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | 0.0032 | 0.49 | 170 | 171.9 |
| 63d | 0.0021 | 0.20 | 168 | 171.4 |
| 126d | -0.0000 | -0.00 | 165 | 170.8 |
| 252d | -0.0076 | -0.41 | 159 | 169.6 |

The signal is economically negligible at one and three months, flat at six months,
and incorrectly signed at one year. It fails both the frozen sign and inference
conditions.

Decision: **reject `quality_low_rsst_accruals` from mega-alpha**. No full tail run,
2021+ stability run, or $50 million Polars capacity test was performed. There is
intentionally no costed decision artifact. Router v6 and the mega-alpha registry
remain unchanged. The governed feature remains useful for earnings-quality,
misstatement-risk, and accounting-research products.

## Verification

- Two focused formula, orientation, lineage, and governance tests pass in 2.55
  seconds.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0251 with 225 applied numeric
  migrations.
- No full suite was run.
