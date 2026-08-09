# Fundamental signal improvement loop 14: production Piotroski financial strength

Date: 2026-08-09

## Research question

Can the original nine-signal Piotroski F-score be reconstructed point in time from the internal
warehouse, and does it separate winners from losers in the high book-to-market portfolio for the
available US liquid-equity sample?

## Primary-source basis

- Piotroski defines nine binary signals across profitability, leverage/liquidity/funding, and
  operating efficiency, then sums them to a 0-9 F-score. The intended application is explicitly
  within the highest book-to-market portfolio:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=249455>.
- The published Journal of Accounting Research paper is identified by DOI 10.2307/2672906:
  <https://www.jstor.org/stable/2672906>.

The paper uses positive ROA, positive CFO, improving ROA, CFO greater than earnings, falling
long-term leverage, improving current ratio, no common-equity issuance, improving gross margin,
and improving asset turnover. ROA and turnover use beginning assets; leverage uses average assets.

## Legacy audit

`quality_piotroski_f_score` existed as a governed definition but had no live values. Its legacy
dependency graph referenced derived inputs that were not materialized. A second ratio subsystem
contained tested score arithmetic but depended on narrow XBRL metric names: the live broad
statement warehouse uses `lt_debt` and `shares_outstanding`, not `long_term_debt` and
`common_shares_outstanding`.

The production audit found sufficient exact annual history:

- 4,925 complete annual observations across 402 securities;
- 1,951 observations across 350 securities with the additional beginning-asset period needed for
  exact current and prior ROA/turnover calculations;
- 99,300 live point-in-time book-to-market rows and 130,195 split-adjusted net-issuance rows for
  monthly decision alignment and the equity-offering proxy.

No TTM substitution or missing-signal imputation was necessary.

## Production build

Added `atx_db.piotroski`, migration `0210`, `scripts/build_piotroski.py`, and targeted tests.
Migration `0210` replaces the empty legacy definition and dependency graph and adds the explicitly
conditioned `quality_piotroski_high_book_to_market` feature.

The loader:

- accepts only annual 10-K/20-F/40-F duration facts with 330-380-day windows;
- selects statement revisions only when every input was visible at the monthly decision;
- requires complete current and prior annual values plus the preceding beginning-asset value;
- calculates current/prior ROA and asset turnover using beginning assets;
- calculates current/prior leverage using average assets;
- uses the production split-adjusted net-share-issuance factor for `EQ_OFFER`, with unchanged
  shares or net repurchases scoring one;
- ranks book-to-market over the full governed point-in-time book-to-market cross-section before
  applying the 80th-percentile condition;
- records each annual fact ID, accession, availability time, upstream factor ID, signal Boolean,
  ratio, and conditioning decision in row lineage;
- fails closed on incomplete inputs, stale annual data, insufficient date breadth, or non-finite
  ratios.

The equity-offering leg is an explicit implementation proxy: it measures no *net* split-adjusted
share issuance rather than directly reading gross common-equity proceeds.

Live output:

| Factor | Rows | Securities | Dates | Coverage |
|---|---:|---:|---:|---|
| `quality_piotroski_f_score` | 21,704 | 317 | 172 | 2012-03-30 to 2026-06-15 |
| `quality_piotroski_high_book_to_market` | 3,177 | 124 | 89 | 2019-02-28 to 2026-06-15 |

The conditional history starts in 2019 because earlier complete-case high-value cohorts do not
meet the ten-name production breadth floor.

## Analysis

Comparable long-horizon run id: `loop14-piotroski-production-long`.

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Standalone F-score | 0.0125 / 1.22 | 0.0113 / 0.61 | 0.0241 / 0.92 | 0.0354 / 1.22 |
| High-B/M F-score | 0.0057 / 0.28 | -0.0099 / -0.38 | -0.0160 / -0.54 | -0.0590 / -2.14 |

The standalone score is weakly positive in average cross-sectional rank IC, but it is not a
monotonic long-short alpha. A score-tail comparison of F-score 8-9 minus F-score 1-3 is negative:

| Cohort | 21d spread / HAC | 63d spread / HAC | 126d spread / HAC | 252d spread / HAC |
|---|---:|---:|---:|---:|
| Standalone | -1.273% / -2.17 | -3.908% / -2.23 | -5.816% / -2.07 | -7.962% / -1.84 |
| High B/M | -0.350% / -0.29 | -5.740% / -1.71 | -8.760% / -2.45 | -26.670% / -6.32 |

The exact paper portfolio cannot be reproduced responsibly in this sample. There are no F-score
zero observations; score one appears for only three standalone securities and two conditional
securities. Only 31 standalone dates and 22 conditional dates contain even one score-1 name beside
an 8/9 name, and no date contains two names in each exact paper tail. The 1-3 adaptation above is
therefore reported as a diagnostic, not presented as a replication.

Generic decile spreads are also not decision-grade for this discrete feature because the evaluator
must split tied scores using deterministic security-ID order. Rank IC is tie-aware, and the raw
score-tail analysis is the appropriate secondary check.

### Subperiod stability

Standalone F-score IC is strong early, reverses in the middle period, and becomes weakly positive
recently:

| Period | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0485 / 1.96 | 0.0676 / 1.79 | 0.1198 / 2.62 | 0.1603 / 7.00 |
| 2016-2020 | -0.0113 / -0.73 | -0.0366 / -1.12 | -0.0489 / -1.11 | -0.0316 / -0.67 |
| 2021-2026 | 0.0129 / 0.96 | 0.0170 / 0.83 | 0.0259 / 0.88 | 0.0072 / 0.27 |

The high-B/M one-year reversal is concentrated in 2019-2022 (IC -0.0826, HAC -2.03). It is smaller
but still negative in 2023-2026 (IC -0.0209, HAC -0.90).

### Distinctiveness

The standalone score has low mean correlation with the broad router (-0.010), corrected Altman
(0.095), and book-to-market (-0.143), and moderate correlation with cash-flow profitability
(0.277). It is therefore structurally distinct, but distinctiveness alone does not justify routing
capital to a non-monotonic signal.

## Decision

Promote `quality_piotroski_f_score` as a production-quality, queryable financial-strength feature,
not as a standalone monotonic alpha. Keep `quality_piotroski_high_book_to_market` queryable as an
explicit research-conditioned surface, but do not route, blend, or market it as validated alpha.
Do not alter the broad production router.

The negative high-value tail is a useful product result: the internal data layer can reproduce the
accounting heuristic without silently claiming that a 1976-1996 return result persists in a modern
liquid-equity sample.

## Verification

- New Piotroski tests: 2 passed serially.
- Existing ratio-level Piotroski tests: 7 passed serially.
- Relevant legacy factor-catalog tests were updated for the corrected dependency contract.
- New files: Ruff and Python compilation clean.
- Schema `0210`; 12 governed dependency edges across the two factors.
- Both live surfaces contain no duplicate keys, non-finite values, or future-available rows.
- Runtime packaging now declares `lxml`, which three imported modules already required but the
  clean-install dependency set omitted.
- Full-suite execution was intentionally avoided.

## Next loop

Decompose the nine binary legs using point-in-time continuous changes, identify which components
drive the modern tail reversal, and test a robust continuous financial-strength feature without
fitting weights to the evaluation sample.
