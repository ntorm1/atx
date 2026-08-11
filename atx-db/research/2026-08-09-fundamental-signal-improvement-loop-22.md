# Fundamental signal improvement loop 22: quarterly operating profitability

Date: 2026-08-09

## Research question

Can announcement-time quarterly operating profitability add timely fundamental information to the
production operating-profitability/net-issuance router without sacrificing its unusually strong
long-short tails?

## Primary-source basis

- Hou, Xue, and Zhang define quarterly operating profits-to-lagged assets (`Olaq`) as quarterly
  revenue (`REVTQ`) minus COGS (`COGSQ`) minus SG&A (`XSGAQ`) plus R&D (`XRDQ`, zero when
  missing), divided by total assets one quarter earlier (`ATQ`). Their canonical portfolios delay
  the fiscal quarter by at least four months and rebalance monthly:
  <https://www.nber.org/system/files/working_papers/w23394/w23394.pdf>.
- The authors' replication reports significant one-month `Olaq1` performance and materially
  stronger quarterly than annual operating-profitability results:
  <https://global-q.org/uploads/1/2/2/6/122679606/houxuezhang2020rfs.pdf>.
- The maintained global-q testing library continues to publish `Olaq1`, `Olaq6`, and `Olaq12` as
  profitability test portfolios: <https://global-q.org/testingportfolios.html>.

The formula, missing-R&D rule, quarterly duration, lagged-assets gap, age bound, and
cross-sectional treatment were fixed before return evaluation. No security return enters feature
construction.

## Point-in-time feature contract

The production feature is
`profitability_quarterly_operating_profitability_lagged_assets`, sourced as
`atx-db PIT quarterly operating profitability v1`:

`(revenue - COGS - SG&A + coalesce(R&D, 0)) / one-quarter-lagged total assets`.

SEC duration facts must span 70-115 days. Lagged assets must end 60-130 days before the numerator
quarter, be positive, and already be visible. The latest complete numerator must also be visible at
the monthly close and no more than 200 days old. Actual SEC availability replaces the paper's
conservative four-month delay; this is a point-in-time-safe timeliness improvement. Reported gross
profit is an algebraically identical fallback for `revenue - COGS`; SG&A remains mandatory and
only missing R&D is zero-filled.

The warehouse contains 36,272 production rows using revenue minus COGS and 20,533 using reported
gross profit. Report ages are 25-198 days (median 91); lagged-assets gaps are 63-122 days. A raw
audit exposed five obvious unit-scale errors, including quarterly operating profit above 200 times
assets. Forward migration `0222` therefore rejects absolute raw ratios above five before monthly
1%/99% winsorization and z-scoring. The post-guard raw range is -2.643 to 0.615.

## Production build

Added `atx_db.quarterly_operating_profitability`, migrations `0221` and `0222`, a standalone build
CLI, and three focused tests. A full historical refresh completes in 34 seconds and materializes
56,805 unique rows across 479 securities and 173 dates from 2012-04-30 through 2026-06-15. Monthly
breadth is 27-399 names. There are no duplicate keys, non-finite values, future-visible statements,
future periods, or lagged-assets timing violations; monthly scores have exact sample mean zero and
standard deviation one. Maximum lineage is 2,355 bytes.

## Standalone analysis

Run id: `loop22-quarterly-operating-profitability-production`. Deciles and split-adjusted forward
returns are used.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0211 | 2.09 | -0.324% | 50.9% | 0.27 |
| 63d | 0.0303 | 1.87 | -1.875% | 49.7% | 0.28 |
| 126d | 0.0353 | 1.46 | -5.872% | 53.0% | 0.33 |
| 252d | 0.0478 | 1.44 | -10.543% | 55.0% | 0.36 |

The positive rank IC ladder and increasing monotonicity show useful broad cross-sectional
information, but the literal low-profitability decile earns extreme rebound returns. Standalone
top/bottom turnover is 24.1%/35.2%, mean rank autocorrelation is 0.964, and the negative tail
spreads disqualify this feature as an independent long-short signal.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0115 | 0.0093 | -0.0094 | -0.0090 |
| 2016-2020 | 0.0273 | 0.0364 | 0.0484 | 0.0855 |
| 2021-2026 | 0.0202 | 0.0367 | 0.0489 | 0.0468 |
| 2023-2026 | 0.0377 | 0.0575 | 0.0784 | 0.1016 |

The recent regime is strong at every horizon. Its HAC t-stat ladder is 2.29/3.09/3.52/3.64, which
supports using the signal as a secondary ordering feature while the full-history tail reversal
argues against letting it choose the extreme baskets.

### Distinctiveness

Mean cross-sectional correlation is only 0.163 with the production router, -0.071 with conservative
asset growth, 0.095 with four-quarter delta ROE, 0.315 with quarterly ROE, 0.423 with cash operating
profitability, 0.438 with annual operating profitability, and 0.435 with gross profitability. Its
0.613 correlation with rolling q5 expected growth is economically coherent because the rolling
model directly uses cash profitability and delta ROE.

## Decile-preserving production router

A plain 5-20% overlay improved average IC but weakened the production router's full-history tail
spreads and increased bottom-decile turnover. The promoted construction is instead lexicographic:

1. On the governed `us_common_equity_liquid_v1` decision-date cohort, operating profitability—or
   low net issuance when operating profitability is absent—selects the primary decile.
2. Visible quarterly operating profitability orders names only inside that primary decile.
3. When the quarterly feature is missing, the primary within-decile rank is retained.

This has no fitted weight: the proven router owns the coarse portfolio assignment and the fresh
quarterly signal is only a secondary key. Migration `0223` introduced the construction. Its first
implementation incorrectly formed buckets on all raw factor rows before the production panel's
universe/knowledge-date filter. The live diagnostic caught higher tail turnover, so migration
`0224` and source `atx-db governed decile-preserving conditional router v5` move bucket formation
onto the exact governed panel cohort. The corrected load takes 16 seconds and the full build takes
42 seconds, producing 114,684 unique rows across 954 securities and 175 dates.

Run id: `loop22-governed-decile-router-v5-production`.

| Horizon | v3 IC | v5 IC | v3 HAC | v5 HAC | v3 spread | v5 spread |
|---:|---:|---:|---:|---:|---:|---:|
| 21d | 0.02219 | 0.02259 | 3.57 | 3.57 | 0.357% | 0.349% |
| 63d | 0.03509 | 0.03620 | 4.50 | 4.55 | 1.259% | 1.245% |
| 126d | 0.05171 | 0.05274 | 5.37 | 5.33 | 2.981% | 2.983% |
| 252d | 0.07088 | 0.07234 | 6.93 | 7.08 | 8.262% | 8.315% |

V5 improves IC at all horizons and improves the 63- and 252-day HAC evidence. Top/bottom turnover
remains exactly 16.19%/19.25%; one-year hit rate rises from 74.38% to 75.63%, and one-year decile
monotonicity rises from 0.697 to 0.733. Mean rank autocorrelation is 0.969. In 2023+, v5 IC is
0.0308/0.0468/0.0674/0.0855 and one-year HAC is 2.91. The router is therefore promoted to v5.

## Decision

Promote the quarterly operating-profitability dataset as a production-queryable, point-in-time
feature, but do not trade its standalone extreme deciles. Promote governed decile-preserving router
v5 as the production composite: it extracts the quarterly feature's broad and recent rank
information while retaining the established router's long-short portfolio selection and turnover.

## Verification

- Six focused quarterly-feature/router tests pass, including formula/fallback, scale rejection,
  migration governance, primary/fallback routing, and explicit extreme-decile preservation.
- New and changed Python files pass Ruff and compilation; repository-local diff checks pass.
- Live schema is `0224` with 198 checksummed migrations. Migration checksum verification, schema
  contract pin verification, and a DuckDB checkpoint pass.
- The final router has 114,684 natural keys, 55,729 quarterly-secondary rows, no non-finite or
  future-dated output, exact monthly sample normalization, and lineage no larger than 1,319 bytes.
- Full-suite execution was intentionally avoided.

## Next loop

Research the companion quarterly gross-profits-to-lagged-assets signal (`Glaq`) and quarterly
cash-based operating profitability (`Claq`). Test whether one supplies a cleaner bottom tail than
`Olaq`, and whether cash-versus-accrual divergence can identify rather than merely suppress the
low-profitability rebound cohort.
