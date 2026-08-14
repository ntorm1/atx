# Industry routing and accounting reconciliation — 2026-08-14

## Outcome

Migrations 0272 and 0273 add two customer-facing, revision-complete fundamentals products:

- `industry-standardized` v1.0 dynamically enriches standardized observations with the
  statement template that was knowable at each decision time.
- `reconciliation` v1.0 evaluates governed accounting identities at each input or
  classification event and publishes exact weighted-input evidence, tolerance, status,
  applicability, and revision lineage.

The live warehouse was used read-only. All migrations, routing refreshes, and reconciliation
queries were executed in an isolated disposable slice database.

## External design evidence

- [FactSet Fundamentals](https://developer.factset.com/api-catalog/factset-fundamentals-api)
  exposes periodicity and update selection alongside a large metric dictionary. This
  supports separate governed item, value-revision, and delivery-schema contracts.
- [FactSet Report Builder](https://developer.factset.com/api-catalog/factset-fundamentals-report-builder-api)
  documents dynamic, industry-specific annual and interim statement templates. ATX keeps
  industry routing as PIT data rather than baking issuer type into canonical item codes.
- The February 2026 [WRDS Compustat webinar](https://wrds-www.wharton.upenn.edu/documents/2180/WRDS_SP_Webinar_Feb_002.pdf)
  distinguishes overwritten current data from Snapshot/PIT products with preliminary,
  original, and restated observations plus effective/through dates. Both new ATX products
  therefore preserve classification and value events instead of updating rows in place.
- The SEC [EDGAR XBRL Guide](https://www.sec.gov/files/edgar/filer-information/specifications/xbrl-guide-2024-07-08.pdf)
  describes calculation relationships and weights between reported concepts. ATX stores
  each reconciliation as ordered weighted canonical terms and retains the exact
  standardized observation IDs used for every result.
- [XBRL US DQC rule 0004](https://xbrl.us/data-rule/dqc_0004/) checks balance-sheet
  calculation relationships and documents permitted liability/equity decompositions.
  ATX implements the total-liabilities-and-equity and liabilities-plus-equity equations as
  hard checks, while the narrower stockholders-equity subset is a diagnostic because
  noncontrolling-interest scope can create a legitimate difference.
- The FASB's [2026 taxonomy technical guide](https://xbrl.fasb.org/resources/annualrelease/2026/GAAP_Financial_Reporting_Taxonomy_and_Data_Quality_Committee_Rules_Taxonomy_Technical_Guide.pdf)
  describes annual taxonomy releases and the accompanying DQC rules taxonomy. ATX rules
  are correspondingly versioned, cited, active/inactive, and effective-dated.

## Implemented industry contract

The router now derives economic intervals from every primary SIC classification revision,
retains knowledge-time route revisions, and emits an `ALL` fallback for securities without
a visible current SIC. Each security has exactly one latest route. The six templates are:

| Code | Template |
|---|---|
| `ALL` | general industrial / common items |
| `BK` | bank |
| `IS` | insurance |
| `RT` | real estate / REIT |
| `UT` | utility |
| `BD` | broker-dealer |

The dynamic statement view combines value-availability and route-availability events. A
later classification revision can therefore produce `classification_update` without
rewriting or falsely restating the underlying standardized value.

## Implemented reconciliation contract

The governed registry contains 19 rules across annual, quarterly, TTM, and instant bases:

- assets against total liabilities/equity;
- assets against liabilities plus equity including noncontrolling interests;
- the DQC 0004 stockholders-equity subset as a diagnostic;
- total debt from current and long-term components;
- gross profit, free cash flow, and net change in debt for three duration bases;
- bank net interest income and insurance combined ratio for three duration bases.

Every result carries `lhs_value`, `rhs_value`, residual, relative residual, effective
tolerance, rule version/citation, ordered item IDs, ordered standardized IDs, full weighted
input JSON, industry applicability, and an original/restated/classification/metadata
revision chain. When an issuer leaves an applicable industry template, the product emits a
`not_applicable` classification revision so a stale bank or insurer result cannot remain
visible in later PIT queries.

Statuses have deliberately separate meanings:

- `reconciled`: within the rule's absolute-or-relative tolerance;
- `mismatch`: an applicable hard identity outside tolerance;
- `diagnostic_difference`: an applicable comparison whose scope can legitimately differ;
- `not_applicable`: inputs exist, but the rule is not applicable to the PIT template.

## Measured validation

Copying the live classification history into the disposable database produced exactly one
latest route for all 46,830 securities:

| Template | Latest securities |
|---|---:|
| `ALL` | 45,454 |
| `BK` | 621 |
| `IS` | 151 |
| `RT` | 205 |
| `UT` | 184 |
| `BD` | 215 |

All six template definitions had complete required-item mappings. The AAPL/MSFT/XOM raw
rebuild contains 41,368 standardized revisions and produced 2,994 reconciliation result
revisions:

| Status | Result revisions |
|---|---:|
| reconciled | 2,826 |
| diagnostic difference | 156 |
| hard mismatch | 12 |

The 156 diagnostic differences are all the narrower stockholders-equity equation; the
same observations reconcile when equity including noncontrolling interests is used. The 12
hard differences are three quarterly and nine TTM gross-profit comparisons. The product
has zero duplicate group/availability events and zero chains without exactly one latest
revision. Among latest applicable results, the hard-mismatch rate is 0.189%, below the 5%
warning threshold.

Focused tests cover rule signs, seed idempotence, a clean identity becoming a mismatch after
a restatement, diagnostic scope handling, classification-driven retirement to
`not_applicable`, and all four warehouse quality checks.

The repository gate collected 1,444 tests: 1,439 passed and five slow tests were skipped by
the default profile. Ruff and strict mypy passed on the changed runtime, migration, API,
quality, and test surfaces.

## Remaining parity work

This tranche establishes semantics and a measurable release gate; it does not claim full
provider breadth. Next work must expand issuer and historical coverage, add bank/insurance/
REIT/utility/broker-dealer reconciliation corpora, validate filing calculation networks and
dimensions at scale, publish analyst exception workflows and coverage SLOs, and compare
licensed baselines where redistribution rights permit.
