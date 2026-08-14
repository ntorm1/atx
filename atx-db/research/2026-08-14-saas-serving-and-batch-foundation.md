# SaaS serving and batch foundation — 2026-08-14

## Outcome

ATX now has a versioned, allow-listed, point-in-time data API over the existing
warehouse plus a usable local control plane. This milestone is a serving foundation,
not a claim of full FactSet or Compustat content parity.

Implemented customer methods:

- metadata discovery for datasets, schemas, versions, fields, units, and natural keys;
- historical interval symbology resolution;
- synchronous `[start,end)` range requests with `as_of`, `latest` versus
  `first_reported` vintage selection, field/item/basis projection, and a 50,000-row cap;
- durable batch submission, account-scoped status/list/cancel/download, and background
  processing;
- Parquet, Arrow IPC, CSV, and JSON Lines artifacts with Zstandard/Gzip options;
- atomic publication, pinned request metadata, record and byte counts, expiry, and
  SHA-256 data manifests.

Implemented control-plane capabilities:

- account, key, schema-entitlement, usage-event, and batch-job persistence;
- high-entropy keys stored only as prefix plus SHA-256 digest;
- per-key scopes, active/expiry/revocation checks, schema allowlists, and synchronous
  row ceilings;
- one-time key issuance and account/grant/revocation commands through `atx-db-control`;
- in-memory adapters for isolated development and DuckDB adapters for restart-safe local
  deployments. PostgreSQL, a distributed quota store, a queue, and object storage remain
  production deployment work.

## Research translated into contract decisions

The method families and artifact metadata follow Databento's separation of metadata,
time series, symbology, and batch delivery. In particular, a batch request is normalized
and pinned before execution, and the result records request parameters, schema version,
counts, sizes, timestamps, and checksum. Historical identifiers remain intervals rather
than being rewritten to the current ticker. Sources:

- [Databento historical API](https://databento.com/docs/api-reference-historical/timeseries/timeseries-get-range-to-file)
- [Databento binary encoding and metadata](https://databento.com/docs/standards-and-conventions/databento-binary-encoding)
- [Databento symbology](https://databento.com/docs/standards-and-conventions/symbology)

The fundamentals contract separates metric/field metadata from observations and exposes
vintage selection because FactSet documents a metric dictionary, periodicity, update or
restatement modes, segments, and batch behavior. Compustat-like reporting choices must be
explicit dimensions rather than accidental deduplication. Sources:

- [FactSet Fundamentals API](https://developer.factset.com/api-catalog/factset-fundamentals-api)
- [CRSP/Compustat merged database guide](https://wrds-www.wharton.upenn.edu/documents/402/CRSP-Compustat_Merged_Database_Data_Guide_9efDcmD.pdf)

SEC accession, context, unit, dimensions, filing time, and as-filed evidence remain the
raw authority. Normalization and quality are separate versioned layers because the SEC
explicitly warns that as-filed datasets can contain redundancies and inconsistencies.
XBRL DQC rule versions and effective dates are retained as governed validation content.
Sources:

- [SEC EDGAR APIs](https://www.sec.gov/search-filings/edgar-application-programming-interfaces)
- [SEC Financial Statement Data Sets](https://www.sec.gov/files/financial-statement-data-sets.pdf)
- [XBRL US DQC rules and guidance](https://xbrl.us/home/priorities/data-quality/rules-guidance/)

## Schema work

Migrations 0267–0268 add and index the public contract and SaaS control tables. Migration
0269 closes point-in-time contract gaps on eight pre-existing 13F amendment research
tables by adding/backfilling `as_of_date` and, where needed, canonical `available_at`.
All producing loaders now populate those fields; availability for outcome-bearing rows is
conservatively the maximum time of the signal and required future evidence.

The initial public schemas are:

- `ATX.US.FUNDAMENTALS/reported`
- `ATX.US.FUNDAMENTALS/standardized`
- `ATX.US.FUNDAMENTALS/ratios`
- `ATX.US.EQUITIES/ohlcv-1d`

Only declared source tables and fields can enter generated SQL. Customers cannot provide
table names, identifiers, expressions, or SQL fragments.

## Verification

- strict Ruff checks passed for all new API, batch, control, migration, and modified 13F
  source files;
- strict mypy passed for the entire `atx_db.api` package;
- focused API/migration/import/module/PIT integration lane: 45 passed;
- complete standard fast lane: 1,420 passed, 5 skipped (1,425 collected) in 317.7 seconds;
- format tests read back real Parquet/Zstandard, Arrow/Zstandard, CSV/Gzip, and
  JSONL/Zstandard artifacts;
- durable end-to-end test provisions and revokes a key, enforces schema entitlement,
  persists job state and usage, downloads an artifact, validates its checksum, and reads
  the expected restated value;
- a read-only smoke query against the 33 GB live warehouse returned a real AAPL reported
  revenue revision chain without mutating the live file.

Known warnings are an upstream Starlette `TestClient`/httpx deprecation, one deliberate
NumPy overflow guard test, and two pre-existing pandas UTC deprecations.

## Next delivery tranche

1. Add cost estimation, monthly byte quotas, distributed request-rate enforcement, and
   idempotency keys.
2. Move job execution behind a queue interface and add object-store multipart artifacts,
   signed URLs, cleanup, retry policy, and worker leases.
3. Add cursor pagination and direct Arrow/JSONL streaming for medium synchronous work.
4. Publish a Python SDK with retries, pandas/polars/Arrow helpers, batch wait/download,
   and OpenAPI compatibility fixtures.
5. Begin the content-parity program: standardized table population, a 300+ core-item
   dictionary, industry templates, issuer breadth, reconciliation samples, and visible
   coverage/quality releases.
