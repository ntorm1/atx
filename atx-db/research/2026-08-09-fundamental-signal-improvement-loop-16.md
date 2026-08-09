# Fundamental signal improvement loop 16: QMJ profitability

Date: 2026-08-09

## Research question

Does the published Quality Minus Junk profitability construction provide a cleaner and more
predictive point-in-time quality signal than the broader nine-leg financial-strength score?

## Primary-source basis

- Asness, Frazzini, and Pedersen define profitability from gross profits over assets, return on
  equity, return on assets, cash flow over assets, gross margin, and low accruals. They rank each
  characteristic cross-sectionally, standardize the ranks, and average the component scores:
  <https://link.springer.com/article/10.1007/s11142-018-9470-2>.
- Ball, Gerakos, Linnainmaa, and Nikolaev find that cash-based operating profitability has more
  explanatory power for expected returns than accrual-heavy measures and largely subsumes the
  accrual anomaly:
  <https://www.sciencedirect.com/science/article/pii/S0304405X16300307>.
- Ball, Gerakos, Linnainmaa, and Nikolaev show that consistently deflated income-statement
  profitability measures have similar predictive power, making denominator consistency an
  important implementation choice:
  <https://www.sciencedirect.com/science/article/abs/pii/S0304405X15000203>.

The transform and equal weights were fixed before looking at returns. The warehouse uses reported
operating cash flow rather than QMJ's synthetic cash-flow proxy. That adaptation is explicit in the
definition and every row's lineage; it is independently motivated by the cash-profitability paper.

## Point-in-time input audit

The governed Piotroski row contains the exact current annual values and statement-point IDs for net
income, operating cash flow, gross profit, revenue, and total assets. Its decision lineage also
contains the exact book-to-market factor-value ID used at formation. Resolving book equity through
that ID produces:

- 21,704 candidate Piotroski rows;
- 21,704 rows with positive, visible book equity;
- 20,855 rows whose book-equity fiscal period exactly matches the other annual inputs.

The 849 mismatches are excluded rather than accepting adjacent-year equity or imputing a value.
Thus 96.1% of the candidate surface is eligible under the stricter same-period contract.

## Production build

Added `atx_db.qmj_profitability`, migration `0213`, a standalone build CLI, and focused tests. The
production feature is `quality_qmj_profitability`.

For each formation date, the implementation calculates:

```text
gross profit / total assets
net income / book equity
net income / total assets
reported operating cash flow / total assets
gross profit / revenue
(reported operating cash flow - net income) / total assets
```

Each component receives an average-tie cross-sectional rank z-score. The factor is the equal-weight
mean of the six component scores, standardized again within date. It is complete case: no
imputation, return-fitted selection, or return-fitted weighting is allowed.

The full refresh materializes 20,855 rows across 315 securities and 172 dates from 2012-03-30 to
2026-06-15 in 14.1 seconds. The exported liquid-universe panel contains 20,820 rows. Maximum lineage
is 1,462 bytes and retains the two upstream factor-value IDs, the exact book-equity statement point,
all raw components, and all rank-standardized components.

## Analysis

Run id: `loop16-qmj-profitability-production`. The evaluation uses quintiles and split-adjusted
forward returns.

| Horizon | Rank IC | HAC t-stat | Q5-Q1 spread | Hit rate | Quintile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | -0.0004 | -0.04 | -0.532% | 47.9% | -0.70 |
| 63d | -0.0069 | -0.38 | -1.492% | 48.5% | -0.70 |
| 126d | -0.0042 | -0.16 | -2.443% | 47.0% | -0.50 |
| 252d | 0.0116 | 0.33 | -4.998% | 39.2% | -0.30 |

The factor is highly persistent: top- and bottom-quintile turnover are 10.5% and 12.1%, and mean
rank autocorrelation is 0.985. Persistence does not compensate for the absent monotonic return
relationship.

### Component diagnosis

| Component | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Gross profit / assets | -0.0003 / -0.03 | 0.0043 / 0.27 | 0.0128 / 0.61 | 0.0260 / 0.83 |
| Return on equity | 0.0118 / 0.97 | 0.0063 / 0.34 | 0.0160 / 0.76 | 0.0427 / 2.12 |
| Return on assets | 0.0031 / 0.25 | -0.0103 / -0.53 | -0.0082 / -0.32 | 0.0133 / 0.51 |
| Operating cash flow / assets | -0.0020 / -0.17 | -0.0139 / -0.74 | -0.0173 / -0.65 | -0.0053 / -0.16 |
| Gross margin | -0.0096 / -0.71 | -0.0138 / -0.64 | -0.0143 / -0.45 | -0.0192 / -0.45 |
| Low accruals | -0.0054 / -0.54 | -0.0076 / -0.48 | -0.0220 / -1.06 | -0.0346 / -1.42 |

ROE is the only statistically credible long-horizon leg. Gross profitability is constructive but
weak. Low accruals again reverses in this cohort, and the equal-weight composite combines the useful
ROE leg with several negative legs. These results diagnose the published construction; they were
not used to change its fixed weights.

### Stability and high book-to-market research cohort

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2012-2015 | -0.0219 | -0.0672 | -0.1020 | -0.0726 |
| 2016-2020 | 0.0083 | 0.0241 | 0.0588 | 0.1033 |
| 2021-2026 | 0.0065 | 0.0069 | 0.0064 | -0.0208 |
| 2023-2026 | 0.0067 | -0.0009 | -0.0092 | -0.0428 |

The apparent full-sample one-year benefit is concentrated in 2016-2020 and reverses recently. An
offline test within the governed high-book-to-market cohort is positive at all horizons (0.0131,
0.0230, 0.0384, and 0.0369), but HAC t-stats are only 0.49-0.71 and the cohort averages 33-35 names.
That is insufficient evidence to materialize another conditioned factor.

### Distinctiveness and router test

Mean cross-sectional correlation is 0.140 with the broad router, 0.253 with cash operating
profitability, 0.769 with operating-cash-flow-to-assets, 0.299 with Piotroski, 0.675 with continuous
financial strength, 0.467 with Altman, and -0.465 with book-to-market.

On the exact overlapping QMJ cohort, the baseline router IC is 0.01739 / 0.03306 / 0.04727 /
0.04252. A 10% QMJ overlay lowers it to 0.01528 / 0.02821 / 0.03915 / 0.03415; a 25% overlay
lowers it to 0.00971 / 0.01634 / 0.02373 / 0.02507. In 2021-2026, the baseline is 0.00883 /
0.01934 / 0.03683 / 0.03478, while the 10% overlay falls to 0.00685 / 0.01398 / 0.02642 /
0.01646. Every tested horizon weakens.

## Decision

Keep `quality_qmj_profitability` as a production-queryable, provenance-complete research feature.
Do not promote it as a monotonic alpha, do not materialize the high-book-to-market variant, and do
not alter the production router. The published equal-weight composite fails both standalone spread
and incremental-router tests in this panel.

ROE's independent result is a pre-specified clue for a later loop, not permission to reweight QMJ
after seeing the sample.

## Verification

- Two focused QMJ tests plus two Piotroski regression tests passed serially; Ruff and Python
  compilation passed.
- Schema `0213`; exactly two governed factor dependencies and no duplicated factor-value IDs.
- All values are finite; migration checksums pass; maximum lineage is bounded at 1,462 bytes.
- Full-suite execution was intentionally avoided.

## Next loop

Research investment and asset-growth signals. They are a separate published return-predictive
dimension, require exact point-in-time balance-sheet changes, and should be tested independently
before considering a quality-investment interaction or router overlay.
