# Fundamental signal improvement loop 58: tax expense momentum

Status: canonical tax-expense data gap closed and production feature retained;
**rejected upstream from mega-alpha**.

## Research and duplicate avoidance

The loop began with annual margin change and DuPont decomposition research.
Soliman documents the forecasting value of separating profitability into
margin and turnover components (https://doi.org/10.2308/accr.2008.83.3.823).
The local research catalog audit found that loop 33 had already implemented and
rejected net-, operating-, and gross-margin change candidates. Repeating that
test would add selection bias without adding evidence, so the loop pivoted
before implementation.

Thomas and Zhang study tax expense momentum and report that seasonally
differenced quarterly tax expense predicts future returns
(https://doi.org/10.1111/j.1475-679X.2011.00409.x). The frozen candidate is:

`(income_tax_q - income_tax_q_minus_4) / assets_q_minus_4`.

Higher tax-expense surprise receives the higher score. The implementation
requires discrete quarterly tax observations separated by 300-430 days, uses
the prior-quarter accession's total assets, excludes zero surprises and
nonpositive assets, and performs no missing-component imputation. The
literature orientation and all filters were frozen before return inspection;
the observed return evidence cannot be used to reverse the sign post hoc.

The first gate requires positive IC at every 21/63/126/252-day horizon and HAC
t-statistics of at least 2.0 at two horizons including 126 or 252 days. A
failure stops all more expensive testing.

## Provider data improvement

`IncomeTaxExpenseBenefit` existed in raw SEC company facts but had no canonical
`income_tax` statement-point partition. The existing concept-scoped bitemporal
pipeline was used rather than a broad warehouse rebuild:

- `fundamental_fact_revisions`: 269,452 selected tax revisions;
- `fundamental_statement_points`: 269,441 canonical `income_tax` rows across
  1,429 securities, including 128,739 discrete-quarter observations;
- coverage runs from 2006-12-31 through 2026-07-05.

The revision refresh completed in 41.6 seconds. The statement-point command
crossed its 49-second shell deadline, then its exact worker completed and
released the database normally; partition checks verified the atomic result and
no orphan process remained. This closes a reusable FactSet/Compustat-style
income-statement coverage gap independently of the alpha decision.

## Production implementation

Added `atx_db.tax_expense_momentum`, build command
`scripts/build_tax_expense_momentum.py`, focused calculation/governance tests,
and append-only migration `0245`. The feature reuses the governed asset-growth
monthly formation scaffold, selects only point-in-time-visible filings, and
exact-accession joins the denominator.

The live build completed in 18.5 seconds and materialized 101,591 observations
across 885 securities and 175 monthly dates from 2012-04-30 through 2026-06-15.
Every row records current/prior tax facts, prior assets, availability timestamps,
parent formation lineage, and the no-imputation/no-return-fitting contract.

## Evidence and decision

Run ID: `loop58-tax-expense-momentum-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | 0.0059 | 1.08 | 171 | 584.5 |
| 63d | -0.0008 | -0.10 | 169 | 583.0 |
| 126d | -0.0052 | -0.46 | 166 | 578.7 |
| 252d | -0.0035 | -0.23 | 160 | 570.0 |

Only the shortest horizon is positive, and even there inference is weak. IC is
negative at 63, 126, and 252 days, so the candidate fails the frozen first gate.

Decision: **reject `earnings_tax_expense_momentum` from mega-alpha**. No full
tail run, 2021+ stability run, or $50 million Polars cost test was performed.
There is intentionally no costed decision artifact. Router v6 and the
mega-alpha registry are unchanged.

The canonical tax partition and governed feature remain useful for effective
tax-rate, deferred-tax, earnings-quality, and conditional interaction research.
The failed orientation is retained to prevent accidental rediscovery or
post-inspection sign reversal.

## Verification

- Two focused formula, orientation, lineage, and governance tests pass.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0245.
- No full suite was run.
