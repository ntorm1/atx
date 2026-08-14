# Standardized fundamentals release research — 2026-08-14

## Outcome

Migration 0271 and the new set-based materializer turn the previously empty
`fundamental_standardized` contract into a deployable, revision-complete data product.
The live 33 GB warehouse was inspected read-only and was not migrated or mutated.

## External design evidence

- [FactSet Fundamentals](https://developer.factset.com/api-catalog/factset-fundamentals-api)
  separates a metric dictionary from observations, exposes annual/quarterly/LTM
  periodicities, supports restatement selection through `updateType`, and advertises more
  than 1,400 items. ATX therefore treats item metadata, periodicity, and revision choice as
  first-class contract fields rather than presentation-only labels.
- [FactSet Report Builder](https://developer.factset.com/api-catalog/factset-fundamentals-report-builder-api)
  documents annual and interim statements, four dynamic industry templates, line-item
  hierarchy, units/scaling, and restatement flags. This supports keeping industry routing,
  display hierarchy, measurement units, and revision state outside raw tag names.
- The February 2026 [WRDS Compustat webinar](https://wrds-www.wharton.upenn.edu/documents/2180/WRDS_SP_Webinar_Feb_002.pdf)
  distinguishes overwritten current Compustat data from Snapshot/PIT products that retain
  preliminary, original, and restated values with effective and through dates. ATX now
  retains every standardized revision and emits `available_at`, exclusive `valid_to`,
  revision sequence/count, prior value, delta, and original/restated type.
- The [SEC Financial Statement Data Sets readme](https://www.sec.gov/files/financial-statement-data-sets.pdf)
  says the data is as filed, uses accession numbers to relate the filing tables, preserves
  presentation rows, and does not scale the numeric value. ATX preserves accession,
  taxonomy/concept, raw measurement unit, and input item IDs on standardized output.
- [XBRL US DQC rules](https://xbrl.us/home/priorities/data-quality/rules-guidance/)
  are versioned and effective-dated. The standardization rules likewise retain active and
  valid date windows and every build pins a deterministic rule-set SHA-256.

## Warehouse audit

Read-only measurements before this release:

| Surface | Rows | Relevant breadth |
|---|---:|---:|
| `fundamental_statement_points` | 5,510,203 | 43 canonical metrics; 1,598 issuers |
| `fundamental_ttm_points` | 1,589,240 | 12 metrics; 1,396 issuers |
| `fundamental_xbrl_metric` | 202 | supplemental proof surface |
| `fundamental_standardized` | 0 | public product was empty |
| `fundamental_item` | 0 | committed seed existed but was not operationally loaded |
| `fundamental_item_alias` | 0 | committed aliases existed but were not operationally loaded |

The live raw `sec_company_facts` surface is materially broader than its stale derived
tables: 14,306,942 rows across 2,720 issuers and 123 concepts, while
`fundamental_fact_revisions` contains 5,551,022 rows across 1,598 issuers and only 48
concepts. A governed rebuild is therefore part of release finalization rather than an
optional optimization.

The committed dictionary contains 234 items and 123 source aliases. The original refresh
loaded millions of candidate rows into pandas, grouped them in Python, filtered to current
source revisions, and classified every duration fact as annual. That combination explains
both the empty production surface and incorrect quarterly/PIT semantics.

## Implemented contract

- 450 active rules: 130 annual, 130 quarterly, 130 TTM, and 60 instant, including
  share-class, public-float, utility, and broker-dealer surfaces.
- A DuckDB set-based candidate, routing, combination, and revision pipeline. Full builds do
  not materialize the fact set in Python.
- Duration classification separates 70–120 day discrete quarters from 330–380 day annual
  periods; instant and constructed TTM facts retain their own bases.
- Cumulative YTD statements are converted into discrete Q2, Q3, and Q4 observations by
  subtracting the latest visible preceding cumulative period. Events from either input
  create PIT revisions, and a directly reported quarter takes precedence from its own
  availability timestamp onward.
- All source revisions are retained. Each standardized chain has one latest row plus
  `revision_group_id`, `revision_sequence`, `revision_count`, `previous_value`, deltas,
  `update_type`, and successor `valid_to`.
- Raw `unit` and canonical `unit_type` are distinct fields. Input codes, input item IDs,
  accession, filing date, rule ID, and producing run remain queryable lineage.
- `fundamental_standardization_builds` records scope, rule digest, input/output/exception
  counts, basis distribution, status, timestamps, and failure evidence.
- Standardized public schema version 2.0.0 exposes the new measurement and revision fields.
- `atx-db refresh-standardized-fundamentals` provides a bounded-memory operator path with
  optional repeated `--symbol` filters.

## Validation

The focused pure-transform, migration, and set-based suite covers alias priority,
combination rules, unmapped extensions, two-vintage restatements, exclusive validity,
quarter classification, unit preservation, manifests, quality checks, and ratio consumers.

A disposable copy of the current schema was populated from a read-only slice of the live
warehouse for AAPL, MSFT, and XOM:

| Measure | Result |
|---|---:|
| candidate inputs | 18,609 |
| standardized revisions | 17,951 |
| revision chains | 7,876 |
| chains with multiple revisions | 5,032 |
| distinct standardized items | 44 |
| annual / quarterly / instant / TTM rows | 2,743 / 4,415 / 6,820 / 3,973 |
| unmapped exceptions | 0 |
| chains without exactly one latest row | 0 |

That first slice deliberately reused the live derived statement surfaces. Rebuilding the
same three issuers from raw `sec_company_facts` exposed 102 concepts and produced 29,474
fact revisions, 29,051 statement points spanning 90 canonical metrics, 2,128 fiscal-period
rows, and 13,733 TTM revisions spanning 43 metrics. This confirms that the live derived
surfaces are stale rather than that the raw source lacks breadth.

The rebuilt slice then produced the following standardized result with the 450-rule set:

| Measure | Before YTD derivation | With PIT YTD derivation | With quarterly combinations |
|---|---:|---:|---:|
| standardized revisions | 34,803 | 41,202 | 41,368 |
| distinct standardized items | 92 | 92 | 92 |
| annual rows | 5,124 | 5,124 | 5,124 |
| quarterly rows | 7,586 | 13,985 | 14,151 |
| instant rows | 11,375 | 11,375 | 11,375 |
| TTM rows | 10,718 | 10,718 | 10,718 |
| unmapped exceptions | 0 | 0 | 0 |

The 6,399 added quarterly revisions comprise 982 Q2, 1,038 Q3, and 4,379 Q4
observations across all three issuers and 41 items. The result has zero duplicate
revision-group/availability events and zero chains without exactly one latest row.
Allowing multi-input rules to consume those components adds 166 more quarterly revisions
and yields 190 PIT `net_change_in_debt` observations across two issuers. The rule sums debt
issuance with sign-normalized repayments; treating repayments as a second subtraction would
invert the accounting convention.

The repository fast lane collected 1,444 tests: 1,439 passed and five slow tests were
skipped by the default profile. Ruff and strict mypy passed for the changed runtime/API
surface. Remaining warnings are the known TestClient deprecation, a deliberate NumPy
overflow guard test, and two pre-existing pandas UTC deprecations.

## Remaining parity work

This is the first materially populated standardized release, not the end of content parity.
The next content tranches must expand the rebuilt 90 upstream statement metrics across the
234-item governed dictionary and then 300+ core reported items; add bank, insurer, REIT,
utility, and broker-dealer template delivery; validate dimensions/consolidation/currency;
and publish filing-level
reconciliation samples and coverage SLOs. FactSet-scale breadth (1,400+ items) and
Compustat-grade PIT history remain product targets, not current claims.
