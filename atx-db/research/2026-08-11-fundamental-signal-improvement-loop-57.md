# Fundamental signal improvement loop 57: net debt financing

Status: canonical data gap closed and production feature retained;
**rejected upstream from mega-alpha**.

## Research hypothesis

Spiess and Affleck-Graves document substantial long-run underperformance after
straight and convertible debt offerings and interpret issuance as a potential
signal of overvaluation
(https://doi.org/10.1016/S0304-405X(99)00031-8). Bradshaw, Richardson, and Sloan
also report that changes in debt are negatively related to future returns within
their broader external-financing analysis
(https://doi.org/10.1016/j.jacceco.2006.03.004).

The frozen candidate is:

`-(lt_debt_issued + signed_lt_debt_repaid) / prior_total_assets`.

The warehouse stores issuance as a nonnegative inflow and repayment as a
nonpositive signed outflow. Both components must be present in the same annual
accession with a 330-400-day duration; observations violating those sign
contracts are removed; beginning assets must be positive and visible; and no
missing component is imputed. Net debt raising therefore receives a negative
score and net repayment a positive score. The orientation was frozen before
return inspection and cannot be reversed post hoc.

The staged gate first requires positive IC at every 21/63/126/252-day horizon
and HAC t-statistics of at least 2.0 at two horizons including 126 or 252 days.
Failure stops the loop before tail, modern-regime, and costed testing.

## Provider data improvement

The concepts were present in raw `sec_company_facts` but absent from the silver
revision and canonical statement partitions. The existing concept-scoped
bitemporal pipeline was used to refresh only
`ProceedsFromIssuanceOfLongTermDebt` and `RepaymentsOfLongTermDebt`:

- `fundamental_fact_revisions`: 56,187 issuance and 68,873 repayment rows;
- `fundamental_statement_points`: 56,144 canonical `lt_debt_issued` and 68,828
  canonical signed `lt_debt_repaid` rows.

Each refresh was transaction-wrapped and completed atomically. The shell print
deadline elapsed after 30 seconds, but immediate read-only partition inspection
verified the complete results. This closes a real FactSet/Compustat-style data
coverage gap independently of the signal outcome.

## Production implementation

Added `atx_db.net_debt_financing`, build command
`scripts/build_net_debt_financing.py`, two focused calculation/governance tests,
and append-only migration `0244`. The feature reuses the governed asset-growth
formation/accession/prior-assets scaffold and exact-accession joins the two
canonical debt flows.

The live build completed in 19 seconds and materialized 32,105 rows across 422
securities and 174 monthly dates from 2012-04-30 through 2026-06-15. Every row
carries both statement-point IDs, availability timestamps, signed flows, net
financing, parent factor lineage, and the no-imputation/no-return-fitting
contract.

## Evidence and decision

Run ID: `loop57-net-debt-financing-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | -0.0083 | -1.27 | 170 | 186.1 |
| 63d | -0.0171 | -1.68 | 168 | 185.5 |
| 126d | -0.0145 | -1.23 | 165 | 184.5 |
| 252d | -0.0107 | -0.65 | 159 | 182.6 |

The literature-oriented score has negative average rank IC at every horizon and
fails every inference requirement. The observed sign suggests that, in this
warehouse sample, firms with more net debt raising subsequently outperform net
repayers; that is a diagnostic observation, not permission to reverse the
candidate after inspection.

Decision: **reject `financing_low_net_debt_financing` from mega-alpha**. The
first-stage IC gate fails, so no full tail run, 2021+ stability run, or $50
million Polars test was performed. There is intentionally no costed decision
artifact. Router v6 and the mega-alpha registry are unchanged.

The governed feature and canonical debt-flow partitions remain available for
capital-structure, credit, and conditional research. Retaining the failed
orientation prevents future analysts from unknowingly sign-flipping a tested
hypothesis.

## Verification

- Two focused formula, sign-policy, lineage, and governance tests pass.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0244 with 218 numeric
  migrations.
- No full suite was run.
