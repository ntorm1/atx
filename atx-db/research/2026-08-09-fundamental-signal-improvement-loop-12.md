# Fundamental signal improvement loop 12: TTM cash-flow profitability and total accruals

Date: 2026-08-09

## Research question

Do directly reported operating cash flows improve the persistence and return signal in accounting
earnings, and can cash-flow profitability improve either the broad router or the revenue-confirmed
earnings sleeve?

## Primary-source basis

- Sloan shows that the cash component of earnings is more persistent than the accrual component and
  that prices do not immediately reflect that difference:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2598>.
- Richardson, Sloan, Soliman, and Tuna connect lower accrual reliability to lower earnings
  persistence and stock-price mispricing:
  <https://www.sciencedirect.com/journal/journal-of-accounting-and-economics/vol/39/issue/3>.
- Livnat and Santicchia test originally reported quarterly data using SEC filing dates and find
  that extreme quarterly accruals predict returns through the following four quarters. Their
  author-hosted paper is available at:
  <https://people.stern.nyu.edu/jlivnat/quarterly%20accruals%20faj%20final.pdf>.

## Existing-feature audit

Loop 03 already materialized `quality_low_operating_working_capital_accruals` from annual
balance-sheet changes. Its conventional low-accrual orientation had significantly negative IC in
this sample, so rebuilding that measure would be duplicative.

The cash-flow-statement construction was defined in the ratio library but had never been
materialized as a PIT factor. Available TTM coverage is broad:

| Metric | TTM rows | Securities | First period | Last period |
|---|---:|---:|---:|---:|
| Net income | 295,568 | 1,366 | 2008-06-30 | 2026-05-31 |
| Operating cash flow | 139,986 | 1,377 | 2008-12-26 | 2026-05-31 |

An initial router-scaffold prototype produced 96,485 complete observations across 1,109
securities and motivated an independent production implementation.

## Build

Added `atx_db.cash_flow_profitability`, migration `0208`, a standalone CLI, and two governed
features computed in a single query:

- `profitability_operating_cash_flow_to_assets`:
  `operating_cash_flow_ttm / average_total_assets`;
- `quality_low_total_accruals`:
  `(operating_cash_flow_ttm - net_income_ttm) / average_total_assets`.

The production loader:

- derives month-ends independently from daily bars;
- joins the interval-keyed `us_common_equity_liquid_v1` roster at the exact decision timestamp;
- selects only USD TTM revisions visible by that timestamp;
- requires net income and operating cash flow at the same TTM period end;
- matches positive current assets within 31 days and a visible prior asset observation 300-430
  days earlier;
- limits TTM age to 550 days and extreme absolute accrual ratios to 10;
- applies 1% winsorization and cross-sectional z-scoring;
- records the TTM IDs, asset IDs, all availability timestamps, and universe interval in lineage.

Live output for each factor: 86,960 rows, 863 securities, 175 dates, 2012-04-30 through
2026-06-15.

## Analysis

Run id: `loop12-cash-flow-profitability-production`.

### Rank IC

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Cash-flow profitability | 0.0232 / 2.95 | 0.0324 / 2.66 | 0.0380 / 2.36 | 0.0536 / 3.15 |
| Low total accruals | 0.0029 / 0.47 | 0.0046 / 0.51 | 0.0040 / 0.35 | 0.0006 / 0.04 |

The predictive content comes from cash-flow profitability, not from subtracting earnings. This is
consistent with cash flow carrying persistence information while the modern accrual anomaly is
weak, but it does not by itself establish a monotonic tradable factor.

### Tail-shape diagnostics

Cash-flow-profitability top-minus-bottom spreads are -0.42%, -0.82%, -0.83%, and -3.62% at
21/63/126/252 days despite positive rank IC. The pooled decile curve is U-shaped: the most negative
cash-flow-profitability decile has the highest raw forward return, consistent with a distressed or
deep-value tail, while ranks generally improve away from the center within monthly cross-sections.

Excluding the bottom 20% restores positive spreads of 0.25%/0.49%/0.92%/1.78%, but reduces IC to
0.0143/0.0194/0.0191/0.0328. That threshold was selected after inspecting the curve and is not
productionized. An absolute-extremes transform also gives positive spreads but negative IC, so it
is rejected.

The untransformed factor is highly persistent: top/bottom turnover is 16.3%/22.5% and mean rank
autocorrelation is 0.980.

### Subperiod stability

| Period | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0089 / 0.64 | 0.0059 / 0.26 | -0.0023 / -0.08 | 0.0095 / 0.31 |
| 2016-2020 | 0.0221 / 1.66 | 0.0329 / 1.50 | 0.0442 / 1.49 | 0.0842 / 3.01 |
| 2021-2026 | 0.0342 / 2.66 | 0.0511 / 2.88 | 0.0622 / 2.78 | 0.0554 / 3.41 |

Most predictive power appears after 2015. The earliest 126-day sign is slightly negative, although
economically near zero.

## Allocation tests

Cash-flow profitability has mean cross-sectional rank correlation 0.290 with the broad router.

### Sparse broad-router overlay

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Router | 0.02045 / 3.32 | 0.03316 / 4.40 | 0.04800 / 5.05 | 0.06791 / 6.44 |
| Router + 25% cash where available | 0.02194 / 3.37 | 0.03528 / 3.91 | 0.05027 / 4.39 | 0.07148 / 6.61 |
| Router + 50% cash where available | 0.02195 / 3.24 | 0.03515 / 3.65 | 0.04907 / 4.05 | 0.07008 / 6.52 |

The 25% overlay improves full-sample IC at every horizon, but it reduces every horizon in
2012-2015 and reduces recent 252-day IC. It is not promoted as an unconditional router replacement.

### Cash-backed earnings sleeve

On 25,352 common keys, an equal-weight cash-flow-profitability/revenue-confirmed-SUE blend produces
IC 0.0304/0.0517/0.0557/0.0532 with HAC 2.68/2.94/2.14/1.65. It improves the common-cohort earnings
sleeve at most horizons, especially after 2015, but turns negative at 126 and 252 days in
2012-2015. It remains a regime-aware research candidate rather than a production composite.

## Decision

Promote `profitability_operating_cash_flow_to_assets` as a production-quality, queryable model
feature with explicit non-linear-tail documentation. Do not treat it as a standalone top-minus-
bottom portfolio without a separately validated distress/value control.

Keep `quality_low_total_accruals` queryable as a diagnostic but do not promote it as alpha. Do not
alter the broad router and do not materialize either tested blend because their improvements are
not stable across subperiods.

## Verification

- Targeted cash-flow-profitability tests: 2 passed serially.
- New files: Ruff and Python compilation clean.
- Schema `0208`, migration checksums, dependency counts, checkpoint, duplicates, availability, and
  finiteness checks: passed.
- Full-suite execution was intentionally avoided.

## Next loop

Research the distressed low-cash-flow tail using point-in-time leverage, liquidity, and value
controls. The objective is to separate a compensated distress/value premium from the monotonic
cash-profitability effect before any non-linear transform or router allocation is productionized.
