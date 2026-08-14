# Fundamental signal improvement loop 37: R&D-to-market-equity

Status: preregistered; no Loop 37 return results inspected at registration time.

## Primary research and hypothesis

Chan, Lakonishok, and Sougiannis (2001), *The Stock Market Valuation of
Research and Development Expenditures*, study R&D intensity and report a return
association that is particularly strong when R&D is scaled by market equity.
The primary publication is:

- https://doi.org/10.1111/0022-1082.00411

ATX hypothesis: among liquid US common equities that disclose positive R&D,
higher point-in-time annual R&D expense divided by contemporaneous market
capitalization predicts higher subsequent returns. Missing R&D is not treated as
zero because SEC non-disclosure is not equivalent to an economically verified
zero investment.

## Frozen data construction

- Numerator: latest positive finite 330--380-day R&D expense from an annual SEC
  filing visible at the monthly close. Filing accession, source point ID, fiscal
  period, and availability are retained.
- Denominator: positive finite component-lineaged market capitalization on the
  final eligible trading observation of each security/month.
- Universe: PIT `us_common_equity_liquid_v1`, joined by immutable `security_id`.
- Maximum annual-fundamental age: 550 days. All R&D and universe inputs must be
  visible by the market-cap decision close; no missing values are imputed.
- Cross section: 1%/99% winsorization followed by sample z-score by date;
  minimum 20 names.

The pre-return coverage audit found 58,815 eligible observations across 462
securities and 174 monthly dates from 2012-04-30 through 2026-06-15, with
21--381 names per date.

## Frozen sequential evaluation

Stage 1 runs adjusted-price rank IC and HAC inference at 21, 63, 126, and 252
trading days using `--screen-only`. Stage 2 is allowed only when the 252-day IC
is positive and HAC t-statistic is at least 2.0.

If Stage 1 passes, Stage 2 requires a positive 252-day top-minus-bottom quintile
spread, at least 20 names on at least 36 monthly dates, and zero PIT, key,
finiteness, or lineage defects. Only then may the governed costed `atx-factor`
walk-forward run. Mega-alpha admission remains frozen at candidate OOS Sharpe
>= 0.50, deflated Sharpe probability >= 0.95, at least +0.05 Sharpe improvement
for an 80/20 blend, positive stressed blend Sharpe, participation in every valid
fold, and passing deployment/turnover gates.

## Production implementation and quality

Migration `0239` governs `valuation_rd_to_market_equity` and its market-cap,
R&D metric, and universe dependencies. `atx_db.rd_intensity` performs filing-
accession resolution, PIT monthly joining, age/validity gates, winsorization,
sample standardization, lineage serialization, hashing, and insertion entirely
inside DuckDB. The live build loaded 58,686 rows in 15.95 seconds.

The production surface covers 461 securities and 174 monthly dates from
2012-04-30 through 2026-06-15, with 21--380 names per date. IDs and natural keys
are unique; values and scores are finite; all raw values are positive; JSON is
valid; and no availability date exceeds its decision date. Per-date means are
within 1.32e-15 of zero and sample standard deviations within 8.89e-16 of one.
The focused end-to-end SQL test passes.

## Stage 1 results and decision

Run id: `loop37-rd-to-market-equity-screen`. The optimized governed screen
completed in 4.18 seconds.

| Horizon | Rank IC | Naive t-stat | HAC t-stat | Sign consistency | Dates | Mean names |
|---:|---:|---:|---:|---:|---:|---:|
| 21 | 0.00058 | 0.06 | 0.06 | 48.54% | 171 | 338.7 |
| 63 | 0.00180 | 0.18 | 0.12 | 46.15% | 169 | 338.0 |
| 126 | 0.00499 | 0.52 | 0.25 | 46.39% | 166 | 337.0 |
| 252 | 0.02413 | 2.36 | 0.81 | 50.00% | 160 | 334.8 |

Decision: **reject Loop 37 from the mega-alpha portfolio**. The one-year IC is
positive and its non-overlap-naive t-stat exceeds two, but the preregistered
overlap-aware HAC statistic is only 0.81 versus the required 2.0. Stage 2 and
`atx-factor` are prohibited. This is precisely why the production gate uses HAC
rather than the misleading ordinary t-stat; router v6 remains unchanged.
