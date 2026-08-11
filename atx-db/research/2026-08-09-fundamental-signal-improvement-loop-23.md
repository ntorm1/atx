# Fundamental signal improvement loop 23: quarterly gross profitability

Date: 2026-08-09

## Research question

Does the broader quarterly gross-profits-to-lagged-assets signal (`Glaq`) avoid the distressed
bottom-tail reversal in quarterly operating profitability while adding a timelier and more complete
profitability surface?

## Primary-source basis

- Hou, Xue, and Zhang define `Glaq` as quarterly revenue (`REVTQ`) minus COGS (`COGSQ`),
  divided by one-quarter-lagged assets (`ATQ`), and sort monthly on a fiscal quarter delayed at
  least four months: <https://www.nber.org/system/files/working_papers/w23394/w23394.pdf>.
- The maintained global-q library publishes `Glaq1`, `Glaq6`, and `Glaq12` and documents its
  testing-portfolio contract through 2024: <https://global-q.org/testingportfolios.html> and
  <https://global-q.org/uploads/1/2/2/6/122679606/portfoliostd_2025may.pdf>.
- Novy-Marx's primary gross-profitability research argues that gross profits-to-assets has
  cross-sectional predictive power comparable to book-to-market:
  <https://www.nber.org/papers/w15940>.

The formula, timing, statement duration, lagged-assets gap, raw-value guard, and
cross-sectional standardization were fixed before return evaluation. No return enters feature
construction.

## Point-in-time feature contract and build

Added `atx_db.quarterly_gross_profitability`, migration `0225`, a standalone CLI, and three focused
tests. The governed factor is
`profitability_quarterly_gross_profitability_lagged_assets`, sourced as
`atx-db PIT quarterly gross profitability v1`:

`coalesce(revenue - COGS, reported gross profit) / one-quarter-lagged total assets`.

SEC duration facts must span 70-115 days; prior assets must end 60-130 days before the numerator
quarter, be positive, and be visible. Actual SEC availability replaces the paper's conservative
four-month delay. Reported gross profit is an algebraically identical fallback for revenue minus
COGS. Current-quarter age is at most 200 days, absolute raw ratios above five are rejected as
unit-scale errors, and each monthly cross-section is 1%/99% winsorized then z-scored.

The 26-second historical refresh materializes 86,527 unique rows across 681 securities and 173
dates from 2012-04-30 through 2026-06-15. Of those rows, 49,583 use revenue minus COGS and
36,944 use reported gross profit. Report age is 17-198 days (median 91); prior-assets gaps are
63-122 days. Monthly breadth is 40-579 names. There are no duplicate keys, non-finite values, or
future-dated outputs; the raw range is -2.622 to 1.733, monthly sample normalization is exact, and
lineage is at most 1,998 bytes.

## Standalone analysis

Run id: `loop23-quarterly-gross-profitability-production`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0054 | 0.61 | -0.470% | 49.1% | -0.15 |
| 63d | 0.0103 | 0.70 | -1.253% | 51.5% | -0.12 |
| 126d | 0.0143 | 0.67 | -2.715% | 51.2% | 0.02 |
| 252d | 0.0180 | 0.62 | -2.789% | 50.0% | 0.27 |

The broad surface does not compensate for its weak predictive content. All HAC t-statistics are
below one and every literal high-minus-low spread is negative. Top/bottom turnover is
23.3%/26.3%, mean rank autocorrelation is 0.980, and 172 rebalance transitions are available.

### Regime stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0103 | 0.0150 | 0.0063 | -0.0065 |
| 2016-2020 | 0.0069 | 0.0196 | 0.0359 | 0.0742 |
| 2021-2026 | -0.0008 | -0.0057 | -0.0093 | -0.0337 |
| 2023-2026 | -0.0007 | -0.0058 | -0.0043 | -0.0148 |

The signal's apparent full-history slope is entirely pre-2021. It is negative at every horizon in
both modern windows, so the quarterly update does not solve the staleness problem of annual gross
profitability.

### Distinctiveness and router trial

Mean cross-sectional correlation is 0.910 with annual gross profitability, 0.632 with quarterly
operating profitability, 0.394 with cash operating profitability, 0.328 with production router v5,
and 0.217 with annual operating profitability. The extremely high annual-gross correlation shows
that `Glaq` mostly increases coverage of an existing exposure rather than discovering a new one.

A coverage-neutral research router replaced `Olaq` with `Glaq` as the within-primary-decile
secondary key. Extreme-decile membership and turnover remain fixed by construction.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Primary only | 0.02219 | 0.03509 | 0.05171 | 0.07088 |
| Quarterly operating profitability | 0.02259 | 0.03620 | 0.05274 | 0.07234 |
| Quarterly gross profitability | 0.02204 | 0.03536 | 0.05132 | 0.07052 |

`Glaq` reduces 21-, 126-, and 252-day IC relative even to the primary-only router and trails `Olaq`
at every horizon. Although its one-year HAC is 7.11, that comes with lower mean IC and no broad
improvement. Production router v5 remains unchanged.

## Decision

Keep `Glaq` production-queryable as a governed experimental dataset, primarily for research,
coverage diagnostics, and future sector-aware work. Do not promote it as a trading signal and do
not change the v5 router. The result implies that SG&A and R&D treatment in `Olaq` contains
essential modern cross-sectional information that gross profit alone omits.

## Verification

- Formula/fallback, scale rejection, and isolated migration-governance tests pass.
- New Python files pass Ruff and compilation.
- Live schema is `0225` with 199 checksummed migrations. Checksum verification, schema-contract
  pin verification, and a DuckDB checkpoint pass.
- Full-suite execution was intentionally avoided.

## Next loop

Build quarterly cash-based operating profits-to-lagged assets (`Claq`) from `Olaq` minus quarterly
receivables and inventory accruals plus deferred-revenue and payable changes. Its published
replication evidence is stronger than `Olaq`, and the cash adjustment directly targets the
distressed/accrual-heavy bottom cohort that defeated standalone `Olaq` and `Glaq` tails.
