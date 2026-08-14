# Fundamental signal improvement loop 35: gross-profit enterprise yield

Status: preregistered; no return results inspected at registration time.

## Primary literature and hypothesis

Novy-Marx (2013), *The Other Side of Value: The Gross Profitability Premium*,
reports that gross profits scaled by assets predict the cross-section of average
returns. Loughran and Wellman (2011) report that low enterprise multiples predict
higher average returns. The primary sources are:

- https://doi.org/10.1093/rfs/hhs116
- https://doi.org/10.1017/S0022109011000445

Loop 35 combines the two independently published ideas without fitting a weight:
annual gross profit divided by enterprise value. The hypothesis is that replacing
the accounting-asset denominator with the total capital-provider price creates a
cash/debt-aware profitability yield that predicts higher subsequent returns.
This synthesis is an ATX inference, not a formula claimed by either paper.

## Frozen point-in-time construction

- Numerator: the exact positive annual gross-profit value and source ID already
  recorded in the governed `profitability_gross_profitability` PIT lineage.
- Denominator: positive component-lineaged `enterprise_value` on the same monthly
  decision date.
- Availability: both source factors, the universe record, and enterprise value
  must be visible by that month's market close. `available_at` is their maximum.
- Maximum age: 550 days from annual fiscal period end to decision date.
- Universe: `us_common_equity_liquid_v1`; immutable `security_id` joins only.
- Cross section: 1%/99% winsorization, then date-wise z-score; minimum 20 names.
- Missing, zero, negative, stale, non-finite, or unlineaged inputs are omitted,
  never imputed.

## Frozen evaluation and admission gates

Evaluate adjusted forward returns at 21, 63, 126, and 252 trading days with
date-wise Spearman rank IC, Newey-West/HAC inference, equal-weight top-minus-
bottom quintile spreads, monotonicity, breadth, and turnover.

The candidate advances to the costed `atx-factor` walk-forward only if all hold:

1. 252-day IC is positive;
2. 252-day HAC t-statistic is at least 2.0;
3. 252-day top-minus-bottom spread is positive;
4. at least 20 names exist on at least 36 monthly dates;
5. PIT timing, keys, values, and lineage have zero quality violations.

Mega-alpha admission remains frozen at candidate OOS Sharpe >= 0.50, deflated
Sharpe probability >= 0.95, at least +0.05 Sharpe improvement for an 80/20 blend,
positive stressed blend Sharpe, participation in every valid fold, and passing
deployment/turnover gates. Any upstream failure is an explicit rejection and
precludes post-hoc `atx-factor` testing.

## Production implementation and quality

Migration `0237` governs `valuation_gross_profit_enterprise_yield` and records
its enterprise-value dataset, annual gross-profitability factor, and PIT universe
dependencies. The materializer is a direct DuckDB pipeline: exact same-decision-
date joining, lineage extraction, validity/age gates, winsorization, sample
z-scoring, JSON lineage, hashing, and insertion all remain set based. The live
write loaded 14,531 rows in 5.31 seconds.

The surface covers 149 securities and 173 monthly dates from 2012-03-30 through
2026-06-15, with 21--121 names per date. Factor IDs and natural keys are unique;
values are positive before standardization and finite afterward; JSON is valid;
no availability date exceeds its decision date. Per-date standardized means are
within 8.2e-16 of zero and sample standard deviations within 6.7e-16 of one.

## Governed evaluation and decision

Run id: `loop35-gross-profit-enterprise-yield-eval`. The bounded evaluator
persisted the complete IC/HAC stage before its 60-second cap was reached in the
quantile-spread stage:

| Horizon | Rank IC | HAC t-stat | Sign consistency | Dates | Mean names |
|---:|---:|---:|---:|---:|---:|
| 21 | -0.00405 | -0.29 | 51.76% | 170 | 83.8 |
| 63 | -0.01400 | -0.71 | 54.17% | 168 | 83.4 |
| 126 | -0.01313 | -0.48 | 52.73% | 165 | 82.8 |
| 252 | 0.00647 | 0.21 | 48.43% | 159 | 81.5 |

Decision: **reject Loop 35 from the mega-alpha portfolio**. The preregistered
one-year inference gate fails first and decisively (HAC t=0.21 versus 2.0), while
all shorter-horizon ICs are negative. Quantile completion cannot reverse that
failure, so it was not rerun and the costed `atx-factor` stage is prohibited by
the frozen protocol. Router v6 remains unchanged.
