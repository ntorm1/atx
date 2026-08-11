# Fundamental signal improvement loop 24: quarterly cash profitability

Date: 2026-08-09

## Research question

Can quarterly cash-based operating profits-to-lagged assets (`Claq`) remove the accrual-driven
distress tail in `Olaq` and improve the production router without changing its extreme baskets?

## Primary-source basis

- Hou, Xue, and Zhang define `Claq` as quarterly revenue minus COGS minus SG&A plus R&D,
  minus the quarterly change in receivables, minus the change in inventory, plus the change in
  deferred revenue, plus the change in payables, all divided by one-quarter-lagged assets. Missing
  balance changes and missing R&D are zero:
  <https://www.nber.org/system/files/working_papers/w23394/w23394.pdf>.
- The current global-q technical document retains the formula, four-month conservative accounting
  delay, and 1-/6-/12-month testing portfolios:
  <https://global-q.org/uploads/1/2/2/6/122679606/portfoliostd_2025may.pdf>.
- The original replication reports statistically significant `Claq1`, `Claq6`, and `Claq12`
  performance, with particularly strong Fama-MacBeth evidence:
  <https://global-q.org/uploads/1/2/2/6/122679606/houxuezhang2020rfs.pdf>.

The formula, missing-change rule, raw guard, and cross-sectional treatment were fixed before
evaluation. No return enters construction.

## Point-in-time contract and build

Added `atx_db.quarterly_cash_profitability`, migration `0226`, a standalone CLI, and three focused
tests. The factor is
`profitability_quarterly_cash_operating_profitability_lagged_assets`, sourced as
`atx-db PIT quarterly cash profitability v1`:

`(quarterly operating profit - dAR - dInventory + dDeferredRevenue + dAP) / lagged assets`.

The feature begins with each governed `Olaq` decision, including its exact quarterly operating
profit, lagged-assets denominator, statement periods, and monthly knowledge timestamp. It then
selects one statement-coherent latest visible balance snapshot for the numerator quarter and one
for the prior quarter. A balance change is calculated only when both endpoints exist; otherwise
that published change is zero. Absolute raw ratios above five are rejected, followed by monthly
1%/99% winsorization and z-scoring.

The first row-wise fact join exceeded three minutes and was terminated before replacing any output.
The production implementation pre-pivots the four balance metrics once per filing/period and then
selects the current/prior snapshots. Input loading fell to 4.1 seconds, full in-memory calculation
to 10.8 seconds, and the live refresh to 21 seconds.

The build preserves all 56,805 `Olaq` rows across 479 securities and 173 dates. Complete current
and prior pairs exist on 40,110 rows for receivables, 50,550 for inventory, 2,823 for deferred
revenue, and 46,488 for payables; 56,130 rows have at least one complete adjustment. The remaining
675 observations correctly reduce to `Olaq` under the published missing-change rule.

There are no duplicate keys, non-finite values, or future-dated outputs. The raw range is -2.619 to
0.680, monthly breadth is 27-399, sample normalization is exact, and lineage is at most 2,762
bytes.

## Standalone analysis

Run id: `loop24-quarterly-cash-profitability-production`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0240 | 2.76 | 0.098% | 56.7% | 0.36 |
| 63d | 0.0350 | 2.57 | -0.202% | 53.8% | 0.41 |
| 126d | 0.0407 | 2.15 | -1.033% | 62.0% | 0.36 |
| 252d | 0.0493 | 1.91 | -2.414% | 65.6% | 0.07 |

Cash adjustment improves `Olaq` IC at every horizon and changes its one-month spread from -0.324%
to +0.098%. It shrinks the one-year tail reversal from -10.543% to -2.414%, but does not eliminate
it at longer horizons. Standalone top/bottom turnover rises to 28.8%/41.5% and mean rank
autocorrelation falls to 0.905, so `Claq` remains unsuitable as an independent extreme-decile
strategy.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0078 | 0.0051 | -0.0037 | -0.0047 |
| 2016-2020 | 0.0319 | 0.0368 | 0.0443 | 0.0823 |
| 2021-2026 | 0.0248 | 0.0517 | 0.0622 | 0.0479 |
| 2023-2026 | 0.0361 | 0.0673 | 0.0907 | 0.0846 |

The modern evidence is materially stronger than both `Olaq` and `Glaq`. In 2023+, HAC t-stats are
2.18/3.94/5.38/4.13. The cash adjustment therefore targets the intended modern accrual problem
rather than importing a legacy-only effect.

### Distinctiveness

Mean cross-sectional correlation is 0.829 with `Olaq`, 0.523 with rolling q5 expected growth,
0.389 with annual cash profitability, 0.303 with production router v5, 0.246 with quarterly ROE,
and only 0.079 with four-quarter delta ROE. `Claq` remains recognizably operating profitability but
changes enough ranks to add independent information.

## Production router v6

A governed-cohort research panel compared primary-only ordering, `Olaq`, and `Claq` as the full
within-primary-decile secondary key. The primary router still determines every decile; no fitted
weight is introduced.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Primary only | 0.02219 | 0.03509 | 0.05171 | 0.07088 |
| `Olaq` (v5) | 0.02259 | 0.03620 | 0.05274 | 0.07234 |
| `Claq` (v6) | 0.02281 | 0.03653 | 0.05306 | 0.07247 |

`Claq` improves full-history IC and HAC over v5 at all horizons. Migration `0227` promotes source
`atx-db governed cash-decile router v6`.

Run id: `loop24-governed-cash-decile-router-v6-production`.

| Horizon | v5 HAC | v6 HAC | v5 spread | v6 spread |
|---:|---:|---:|---:|---:|
| 21d | 3.57 | 3.64 | 0.349% | 0.352% |
| 63d | 4.55 | 4.62 | 1.245% | 1.272% |
| 126d | 5.33 | 5.42 | 2.983% | 2.997% |
| 252d | 7.08 | 7.17 | 8.315% | 8.333% |

Top/bottom turnover remains exactly 16.19%/19.25%; one-year hit rate is 75.0%, mean rank
autocorrelation is 0.969, and the production surface remains 114,684 rows across 954 securities
and 175 dates. V6 uses `Claq` on 55,729 rows. Recent v6 and v5 are close—each wins individual
horizons—but the full-history improvement is unanimous and the cash construction has the stronger
standalone modern evidence.

## Decision

Promote `Claq` as a production-queryable profitability feature and promote governed cash-decile
router v6. Do not use standalone `Claq` extreme deciles; use its rank information only inside the
primary router's established baskets.

## Verification

- Three focused `Claq` tests and three router tests pass, covering formula/signs, missing-change
  behavior, scale rejection, migration dependencies, primary/fallback routing, and explicit
  extreme-decile preservation.
- New and changed Python files pass Ruff and compilation.
- Live schema is `0227` with 201 checksummed migrations. Checksum verification, schema-contract
  pin verification, and a DuckDB checkpoint pass.
- Router v6 has 114,684 natural keys, no non-finite or future-dated output, and lineage no larger
  than 1,325 bytes.
- Full-suite execution was intentionally avoided.

## Next loop

Research quarterly accruals and cash-flow-to-assets directly. Decompose the successful `Claq-Olaq`
rank change into receivables, inventory, deferred-revenue, and payables legs, then test whether a
standalone low-accrual quality feature can improve the production router without duplicating the
cash-profitability secondary.
