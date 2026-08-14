# Provider coverage metadata and SLO contract — 2026-08-14

## Outcome

Migration 0274 adds an honest, customer-visible availability contract for every public ATX
schema. It separates measured coverage from institutional targets and never equates “the
table exists” with “provider parity is achieved.”

The release includes:

- `api_schema_coverage_slo`, a versioned target registry for history, security breadth,
  item breadth, and freshness;
- `api_schema_coverage_snapshot`, an append-only measurement history;
- `v_api_schema_coverage_current`, the latest measurement joined to its pinned SLO;
- `metadata.get_dataset_range`, with dataset and per-schema inclusive-start/exclusive-end
  ranges;
- `metadata.get_dataset_condition`, with `available`, `degraded`, `pending`, and `missing`
  conditions;
- `metadata.get_schema_coverage`, with measured counts, target values, and machine-readable
  failed-SLO evidence;
- `atx-db refresh-provider-coverage` and an orchestrated `provider_schema_coverage` dataset
  job that runs after standardized fundamentals, ratios, and daily bars.

## External design evidence

Databento's official [Historical API metadata reference](https://databento.com/docs/api-reference-historical/basics/authentication?historical=http&live=http)
defines stable dataset and schema identifiers, per-schema dataset ranges, inclusive starts,
exclusive ends, and the four conditions `available`, `degraded`, `pending`, and `missing`.
ATX follows those discoverability semantics while reporting fundamentals coverage at schema
level rather than pretending irregular filing periods are daily market sessions.

Databento's [DBN metadata convention](https://databento.com/docs/standards-and-conventions/databento-binary-encoding)
stores enough request metadata to reproduce a historical extraction. ATX already places the
normalized request and release metadata in every batch artifact; the new coverage snapshot
adds the exact range/condition state used at discovery time.

The SEC [Financial Statement Data Sets documentation](https://www.sec.gov/file/financial-statement-data-sets)
states that the flattened XBRL data is as filed, covers quarterly and annual primary
financial-statement data, is updated quarterly, and is not a substitute for the filings.
ATX therefore measures raw/reporting coverage independently from standardized, industry,
reconciliation, and ratio products, and retains explicit source lineage.

## Condition semantics

| Condition | ATX meaning |
|---|---|
| `available` | The schema has records and passes every target in its pinned SLO version. |
| `degraded` | The schema has deliverable records but fails at least one history, breadth, item, or freshness target. |
| `pending` | The governed schema relation exists but contains no deliverable records yet. |
| `missing` | The schema relation itself is absent from the measured release. |

Conditions are measurements, not marketing overrides. A later snapshot can move in either
direction, prior snapshots remain queryable, and each failure contains metric, observed
value, threshold, and comparator.

## Initial institutional targets

The v1 targets deliberately describe the intended provider-grade product rather than the
current proof slice:

| Schema family | History target | Securities | Items/rules | Freshness |
|---|---:|---:|---:|---:|
| reported fundamentals | 2009 onward / 15 years | 2,500 | 90 | 120 days |
| standardized fundamentals | 2009 onward / 15 years | 2,500 | 200 | 120 days |
| industry-standardized | 2009 onward / 15 years | 2,500 | 200 | 120 days |
| reconciliation | 2009 onward / 15 years | 2,500 | 15 | 120 days |
| ratios | 2009 onward / 15 years | 2,500 | 50 | 120 days |
| daily equities | 2010 onward / 10 years | 5,000 | not applicable | 7 days |

Targets are effective-dated and versioned. Tightening one creates a new SLO version; it must
not reinterpret a historical snapshot silently.

## Disposable-slice measurement

The live warehouse remained read-only. Applying migration 0274 and refreshing coverage in
the isolated AAPL/MSFT/XOM database produced:

| Schema | Condition | Records | Securities | Items/rules | Measured range | Failed targets |
|---|---|---:|---:|---:|---|---|
| reported | degraded | 29,051 | 3 | 90 | 2006-09-30 to 2026-07-24 exclusive | security breadth |
| standardized | degraded | 41,368 | 3 | 92 | 2006-09-30 to 2026-07-24 exclusive | security and item breadth |
| industry-standardized | degraded | 41,368 | 3 | 92 | 2006-09-30 to 2026-07-24 exclusive | security and item breadth |
| reconciliation | degraded | 2,994 | 3 | 10 | 2007-09-29 to 2026-07-01 exclusive | security and rule breadth |
| ratios | pending | 0 | 0 | 0 | none | none until data exists |
| daily equities | pending | 0 | 0 | n/a | none | none until data exists |

The long history in the three-issuer rebuild passes the 2009/15-year target. The degraded
condition correctly exposes that a deep proof slice is not broad provider coverage.

## Verification and next work

Focused tests cover all six seeded SLOs, empty-schema pending state, measured range and
breadth, target-failure evidence, a versioned target reaching available state, endpoint
authentication, exclusive-end delivery, and structural quality checks.

The repository gate collected 1,444 tests: 1,439 passed and five slow tests were skipped by
the default profile. Ruff and strict mypy passed on the changed runtime, migration, API,
quality, and test surfaces.

Next work is to refresh this snapshot after the full raw rebuild, expose condition history
in release manifests and the customer portal, and add per-period/industry/item coverage
matrices so users can distinguish a globally available schema from a sparse requested
slice.
