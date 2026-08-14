# Fundamental signal improvement loop 59: tax-to-book income

Status: canonical current-tax data gap closed and production feature retained;
**rejected upstream from mega-alpha**.

## Research hypothesis

Lev and Nissim propose a tax fundamental based on estimated after-tax taxable
income divided by book income. They find that it predicts long-run earnings
growth, while noting that its return relation becomes weak after SFAS 109
(https://www.columbia.edu/~dn75/taxableincome.pdf). Hanlon separately finds
that large book-tax differences identify less persistent earnings
(https://doi.org/10.2308/accr.2005.80.1.137). More recent evidence argues that
apparent book-tax-difference mispricing may be subsumed by operating cash flow
to price (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3165163), making a
strict independent return test essential.

The local catalog contained no tax-to-book income signal. The frozen candidate
is:

`(current_tax / statutory_rate) * (1 - statutory_rate) / net_income`.

The statutory rate is 35% for fiscal years through 2017 and 21% thereafter.
Only positive annual current tax and positive annual net income from the same
accession are admitted; annual durations must be 330-400 days; ratios above ten
are removed; and no component is imputed. Higher tax-to-book income receives
the higher score. The formula, orientation, rate schedule, and filters were
fixed before return inspection and cannot be reversed post hoc.

The first gate requires positive IC at every 21/63/126/252-day horizon and HAC
t-statistics of at least 2.0 at two horizons including 126 or 252 days. Failure
stops all more expensive testing.

## Provider data improvement

`CurrentIncomeTaxExpenseBenefit` was present in raw SEC facts and in the
canonical mapping but absent from both the bitemporal revisions and canonical
statement-point partitions. A single-concept refresh produced:

- 54,468 `fundamental_fact_revisions` rows;
- 54,467 canonical `current_tax` statement points;
- 1,197 securities from 2007-11-03 through 2026-07-03.

The two transaction-wrapped refresh stages completed in 35 seconds. This closes
a reusable income-tax disclosure gap for tax-rate, cash-tax, book-tax, and
earnings-quality products independently of the alpha outcome.

## Production implementation

Added `atx_db.tax_to_book_income`, build command
`scripts/build_tax_to_book_income.py`, focused formula/governance tests, and
append-only migration `0246`. The feature reuses the governed asset-growth
monthly formation scaffold, enforces filing visibility, and joins current tax
and net income by exact accession and period.

The live build completed in 19.2 seconds and materialized 65,458 point-in-time
observations across 709 securities and 175 monthly dates from 2012-04-30 through
2026-06-15. Each row carries the selected statement-point identifiers,
availability timestamps, statutory rate, estimated taxable income, parent
lineage, and no-imputation/no-return-fitting contract.

## Evidence and decision

Run ID: `loop59-tax-to-book-income-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | -0.0067 | -1.17 | 171 | 376.7 |
| 63d | -0.0043 | -0.51 | 169 | 374.9 |
| 126d | -0.0023 | -0.21 | 166 | 372.2 |
| 252d | 0.0002 | 0.01 | 160 | 366.5 |

The literature-oriented score is negative from 21 through 126 days and
economically zero at 252 days. No horizon has meaningful inference, so the
candidate fails the frozen first gate. This modern result is consistent with
the original paper's warning that the post-SFAS 109 return relation was weak;
it does not authorize reversing the observed sign.

Decision: **reject `earnings_tax_to_book_income` from mega-alpha**. No full tail
run, 2021+ stability run, or $50 million Polars cost test was performed. There
is intentionally no costed decision artifact. Router v6 and the mega-alpha
registry are unchanged.

The canonical current-tax partition and governed factor remain available for
conditional earnings-quality research and for testing whether tax information
adds anything after controlling explicitly for cash-flow yield.

## Verification

- Two focused formula, rate-policy, orientation, lineage, and governance tests
  pass.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0246 with 220 numeric
  migrations.
- No full suite was run.
