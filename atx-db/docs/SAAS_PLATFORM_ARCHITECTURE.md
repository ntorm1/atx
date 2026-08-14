# ATX Data Cloud architecture

Status: implementation baseline, 2026-08-14  
Contract version: v1  
Scope: US listed equities, issuer fundamentals, reference data, corporate actions,
ownership, estimates, market data, public alternative data, and quant-ready factors.

## Product north star

ATX is a data provider, not merely a database. A systematic equity team must be able
to replace its FactSet Fundamentals and S&P Compustat research inputs without writing
issuer-specific XBRL logic or accidentally introducing look-ahead, restatement, or
survivorship bias.

The product contract is:

1. Every delivered value has a stable instrument identity, economic observation time,
   first-availability time, revision identity, source, unit, and reproducible formula or
   normalization lineage.
2. Raw evidence remains immutable. Normalized statements, ratios, factors, and wide
   panels are versioned projections over raw filings; they never erase the source fact.
3. Historical queries are point-in-time by construction. `as_of` is an information-time
   cutoff, not a convenience filter applied after selecting the latest row.
4. The same dataset/schema request is available synchronously for bounded work and as an
   asynchronous, checksummed batch artifact for institutional work.
5. Dataset, schema, field, formula, symbology, entitlement, usage, quality, and release
   metadata are queryable products.

## Research basis

The delivery model adapts Databento's separation of **metadata**, **time series**,
**symbology**, and **batch** APIs. Databento defines a schema as a record format supported
by a named dataset, permits direct streaming or batch downloads, records the complete
request in file metadata, and meters record and byte sizes. Its historical API uses
RPC-style method families such as `timeseries.get_range`; its symbology deliberately
preserves historical symbols rather than rewriting old observations to today's ticker.
See the [Databento historical API](https://databento.com/docs/api-reference-historical/timeseries/timeseries-get-range-to-file),
[DBN metadata convention](https://databento.com/docs/standards-and-conventions/databento-binary-encoding),
and [symbology convention](https://databento.com/docs/standards-and-conventions/symbology).

The fundamentals model adapts FactSet's metric dictionary, periodicity, restatement
selection (`updateType`), segment endpoints, and asynchronous batch behavior. FactSet's
public description advertises more than 1,400 items and explicitly separates metric
metadata from fundamental observations; parity therefore means a governed item dictionary
and field definitions, not a pile of XBRL tags. See the
[FactSet Fundamentals API](https://developer.factset.com/api-catalog/factset-fundamentals-api).

Compustat parity requires explicit reporting dimensions. WRDS documents that a firm/date
can have multiple rows distinguished by population source, data format, industry format,
consolidation, and sometimes fiscal-year-end. ATX models those choices as issuer/listing,
accounting basis, industry template, consolidation scope, source taxonomy, and vintage
rather than silently choosing one row. See the
[CRSP/Compustat merged database guide](https://wrds-www.wharton.upenn.edu/documents/402/CRSP-Compustat_Merged_Database_Data_Guide_9efDcmD.pdf).

The SEC remains the authoritative raw source. Its Financial Statement Data Sets define
submission (`SUB`), tag (`TAG`), numeric fact (`NUM`), and presentation (`PRE`) relations,
with accession number as the filing join key and dimensions in the numeric fact key. The
SEC also states that those datasets are as filed and may contain redundancies,
inconsistencies, and discrepancies. ATX therefore retains full accession/context evidence
and runs normalization and quality rules as separate layers. See the
[SEC Financial Statement Data Sets readme](https://www.sec.gov/files/financial-statement-data-sets.pdf)
and [EDGAR data API documentation](https://www.sec.gov/search-filings/edgar-application-programming-interfaces).

XBRL quality rules are versioned content. The XBRL US Data Quality Committee had 196
approved rules and plugin version 30.0.0 in June 2026; rule effective dates matter, so a
finding cannot be judged against a rule that was not effective for the filing. See the
[approved DQC rules](https://xbrl.us/home/priorities/data-quality/rules-guidance/).

## Current platform baseline

The implementation is not greenfield. The live warehouse measured on 2026-08-14 contains:

- 217 tables, 39 views, 130 internal dataset catalog entries, and migrations through 0266;
- 14.3 million SEC company facts across 2,720 securities;
- 5.5 million canonical statement points across 1,598 securities;
- 1.59 million constructed TTM points across 1,396 securities;
- 933,564 point-in-time ratio observations from a 66-formula registry;
- 31.17 million daily bars and 68,269 historical identifier intervals;
- 177 governed factor definitions plus ownership, corporate-action, short-interest,
  insider, listing, macro, estimates, segments, footnotes, quality, and lake surfaces.

The major pre-v1 gaps are productization, broader normalized-item coverage, full historical
issuer breadth, licensed estimate content, customer operations, and multi-node serving.

## Logical data model

```text
issuer/entity
  -> security/share class
       -> listing interval
       -> identifier interval (ticker, CIK, CUSIP, FIGI, LEI, vendor IDs)
       -> universe membership interval
       -> prices / shares / corporate actions / delisting returns
  -> filing/submission
       -> document + acceptance timestamp
       -> taxonomy concept + context + unit + dimensions
       -> immutable raw fact revision
            -> canonical statement point
                 -> standardized industry item
                 -> quarterly / annual / TTM / calendarized vintage
                      -> formula input -> ratio / metric -> factor -> panel
       -> presentation, calculation, definition, label and reference relationships
       -> segment, footnote, guidance, insider and ownership evidence
```

### Required grains

| Domain | Canonical grain | Point-in-time requirement |
|---|---|---|
| issuer | `entity_id` | name/status history, not current-name overwrite |
| security | `security_id` | share-class lifecycle and issuer relationship |
| identifier | `(security_id,id_type,id_value,valid_from)` | mapping must be visible and valid on the queried date |
| listing | `(security_id,venue,valid_from)` | additions, deletions, ticker changes, and primary-listing history |
| filing | `accession_number` | SEC acceptance timestamp is the first possible availability |
| raw fact | `(accession,concept,context,unit,dimensions)` | immutable; corrections are new filing revisions |
| canonical fact | `(security,item,basis,period_end,scope)` | revision chain selected at query `as_of` |
| ratio | `(security,formula,basis,period_end)` | maximum availability of every visible input and formula version |
| price | `(source,security,session)` | source event time, load time, and adjustment vintage |
| action | `(source,security,action_type,effective_date,source_event)` | announcement and effective times both retained |
| estimate | `(provider,security,measure,period,contributor,snapshot)` | contributor observation time and vendor delivery time |
| ownership | `(manager,security,report_period,filing_chain)` | filing/amendment availability, not report-period availability |
| factor | `(factor,security,formation_date)` | maximum visible parent availability plus build release time |

### Time semantics

Every research observation uses these meanings consistently:

- `period_start` / `period_end` or `trade_date`: economic time.
- `as_of_date`: date the observation describes.
- `available_at`: earliest timestamp the normalized observation was deliverable.
- `source_loaded_at`: ingestion time, used for operational replay and as a conservative
  fallback when the upstream publication time is unknowable.
- `valid_from` / `valid_to`: effective interval for identities, definitions, and mappings.
- `is_latest_revision`: current operational convenience only; historical queries must not
  use it to reconstruct an earlier vintage.
- `run_id`: immutable lineage edge to source files, code version, parameters, checks, and
  output manifest.

For a request cutoff `T`, candidate inputs satisfy `available_at <= T`. `latest` selects
the maximum visible revision in each natural-key chain; `first_reported` selects the
minimum. A derived row's availability is at least the maximum availability of every input.
No filter on today's `is_latest_revision` is PIT-correct for historical research.

## Public API contract

Version 1 follows the method-family style:

- `GET /v1/metadata.list_datasets`
- `GET /v1/metadata.list_schemas?dataset=...`
- `GET /v1/metadata.get_schema?dataset=...&schema=...`
- `GET /v1/metadata.get_dataset_range?dataset=...`
- `GET /v1/metadata.get_dataset_condition?dataset=...&schema=...`
- `GET /v1/metadata.get_schema_coverage?dataset=...&schema=...`
- `GET /v1/metadata.list_unit_prices`
- `POST /v1/metadata.get_record_count`
- `POST /v1/metadata.get_billable_size`
- `POST /v1/metadata.get_cost`
- `POST /v1/symbology.resolve`
- `POST /v1/timeseries.get_range`
- `POST /v1/batch.submit_job`, `GET /v1/batch.list_jobs`,
  `GET /v1/batch.get_job`, `POST /v1/batch.cancel_job`, and
  `GET /v1/batch.download`.

Initial public dataset codes are stable product identifiers:

| Dataset | Schema | Grain / purpose |
|---|---|---|
| `ATX.US.FUNDAMENTALS` | `reported` | populated canonical statement points with raw SEC evidence and revisions |
| `ATX.US.FUNDAMENTALS` | `standardized` v2.0 | revision-complete annual, quarterly, instant, and TTM comparable items |
| `ATX.US.FUNDAMENTALS` | `industry-standardized` v1.0 | PIT dynamic statements across general, bank, insurer, REIT, utility, and broker-dealer templates |
| `ATX.US.FUNDAMENTALS` | `reconciliation` v1.0 | governed accounting-identity results, tolerances, status history, and exact input lineage |
| `ATX.US.FUNDAMENTALS` | `ratios` | formula-governed ratios and composite scores |
| `ATX.US.EQUITIES` | `ohlcv-1d` | daily prices, volume, adjustments, shares, and market cap |

Range intervals are start-inclusive and end-exclusive. Sync results are bounded to 50,000
records; larger work belongs in batch. `as_of` defaults to request time but should be pinned
by research systems. `fields`, `items`, and basis filters are allow-listed against a
versioned field catalog. Callers cannot submit table names, expressions, or SQL.

All responses carry `X-ATX-Request-ID`, `X-ATX-API-Version`, record count, and response-byte
usage. Authentication accepts bearer or `X-API-Key` credentials. Secrets are high entropy;
the durable schema stores only prefix and digest. Entitlements are per account, dataset,
schema, validity window, row ceiling, request rate, and monthly bytes.

Data usage is metered in uncompressed Arrow bytes, independent of requested output
encoding. Synchronous delivery is billed per response. Batch generation is billed once;
subsequent downloads of the same immutable artifact remain visible operational events but
have zero billable bytes. Prices are versioned by dataset/schema/mode/currency and can
remain `contract_required` until commercial terms are configured. Preflight metadata is
not billed.

`Idempotency-Key` is supported on batch submission for 24 hours. Its scope is account plus
endpoint; a replay with identical normalized parameters returns the existing job, while
reusing the key with different parameters fails with `409`. Sliding-window request limits
and monthly byte ceilings return `429`, `Retry-After`, remaining-quota headers, and a
machine-readable reason. Local limits are single-node; production uses a distributed,
atomic limiter and reservation service.

### Formats and artifacts

Synchronous JSON is the bootstrap format. Local batch delivery supports JSON Lines, CSV,
Arrow IPC, and Parquet with Zstandard/Gzip compression. Every batch package contains:

- the normalized request and API/schema versions;
- creation, start, end, and information-cutoff timestamps;
- symbology mappings or an explicit `ALL_SYMBOLS` omission marker;
- field dictionary and unit metadata;
- record count, uncompressed billed bytes, package bytes, and SHA-256;
- warehouse release ID, data-quality release status, and source lineage manifest.

## Physical architecture

```text
public sources / licensed feeds
       -> immutable object archive
       -> parser + validation queue
       -> normalization / revision / formula DAG
       -> writer warehouse + quality gates
       -> immutable release manifest
       -> partitioned Parquet/Arrow lake
       -> read replicas / query workers
                -> API gateway -> auth / entitlement / rate limit
                -> batch workers -> object store + signed URLs

separate transactional control plane
       -> accounts / keys / entitlements / usage / invoices / jobs / audit
```

The existing DuckDB file remains the research and single-node writer engine. Production
serving must use checkpointed immutable copies or lake partitions; API workers never read
the actively mutating ingestion file. The initial local server is intentionally a
single-node adapter. Multi-node production separates:

- PostgreSQL control plane for customer and job state;
- object storage for raw evidence, releases, and batch packages;
- queue for ingestion and batch work;
- partitioned Parquet plus DuckDB/ClickHouse-style stateless query workers;
- Redis-compatible distributed quotas and short-lived metadata caching;
- KMS/secret manager, signed URLs, WAF, audit log, tracing, metrics, and alerting.

## Formula and normalization governance

Formula definitions are data. Each version includes code, family, description, expression,
input item IDs, output unit/sign/scale, denominator and meaningfulness guards, periodicity,
industry applicability, currency policy, availability rule, valid interval, citation, and
test vectors. A formula change creates a new version and recomputation release; it never
rewrites the meaning of an old API schema or artifact.

The normalization sequence is:

1. Validate filing, context, unit, decimals, dimensions, calculation relationships, and
   rule version effective on the filing date.
2. Resolve standard and issuer-extension concepts through presentation/calculation/reference
   evidence and curated issuer aliases.
3. Select consolidated versus segment/co-registrant scope explicitly.
4. Normalize scale, currency, debit/credit sign, duration, fiscal period, split/share basis,
   and industry template.
5. Persist every accepted/rejected candidate and the exact rule decision.
6. Construct quarterly, annual, TTM, calendarized, first-reported, and latest-restated views.
7. Evaluate formulas only from inputs visible at the requested cutoff.

## Release and quality gates

A release is publishable only when all critical checks pass:

- schema and API contract drift is zero;
- migration checksums and source artifact hashes verify;
- natural-key uniqueness and referential integrity hold;
- `available_at` never precedes a required parent;
- units, signs, bounds, accounting identities, split factors, and return chains pass;
- freshness, issuer breadth, item breadth, history, revision, and null-rate SLOs pass;
- reconciliation samples meet declared tolerance against filings and licensed baselines;
- delisted names and historical identifier intervals survive universe queries;
- batch artifacts reproduce byte-for-byte from the release manifest.

Quality is customer-visible at dataset, schema, field, date, source, and release levels.
Known exceptions are versioned evidence, not silently suppressed checks.

## Delivery roadmap

### Phase A — serving foundation (current)

- versioned dataset/schema/field contract;
- authenticated and entitled metadata, symbology, and PIT range queries;
- account/key/entitlement/usage/batch-job schema;
- public API test vectors for restatements and historical ticker intervals;
- atomic Parquet/Arrow/CSV/JSONL batch artifacts, SHA-256 manifests, expiry, and account-scoped download;
- durable DuckDB adapters for key lookup, schema entitlements, usage, and batch state;
- operator CLI for account provisioning, one-time key issuance, grants, and revocation;
- exact record-count/billable-size/cost preflight and versioned unit-price catalog;
- account/schema request limits, monthly byte enforcement, and idempotent batch creation;
- lease-based workers with expired-lease recovery, ownership checks, and heartbeats;
- immutable serving snapshot requirement.
- set-based standardized-fundamentals builds with 450 effective-dated rules, revision
  chains, units, lineage, rule-set hashes, and build manifests;
- standardized public schema 2.0 with original/restated selection through the common
  point-in-time range API;
- revision-complete discrete Q2/Q3/Q4 derivation from cumulative YTD facts, with direct
  reported-quarter precedence.
- complete current security routing across six statement templates, with economic and
  knowledge-time revision intervals and classification-triggered product revisions;
- 19 versioned reconciliation rules covering balance-sheet identities, debt, gross profit,
  free cash flow, and bank/insurance calculations, with customer-visible evidence and a
  structural/mismatch-rate quality gate.
- Databento-style dataset/per-schema ranges and explicit available, degraded, pending, or
  missing conditions backed by append-only breadth/freshness snapshots and versioned
  institutional target SLOs.

### Phase B — institutional delivery

- cursor pagination and direct streaming content negotiation;
- queue-backed batch workers, object-store multipart output, signed downloads, and cleanup;
- PostgreSQL auth/usage adapter, key rotation, distributed atomic quota reservations, and
  concurrency limits;
- Python client with sync/async, pandas/polars/Arrow helpers, retries, and batch download;
- OpenAPI compatibility fixtures and semantic-version deprecation policy.

### Phase C — content parity

- expand upstream canonical extraction from the 90 metrics demonstrated by the raw rebuild
  across the 234-item governed dictionary and toward 300+ core reported items; complete
  industry-specific item coverage and reconciliation fixtures beyond the initial routed
  templates and derived-quarter combination rules;
- complete 2009-present US issuer history, delisted names, ADR/share classes, fiscal calendar,
  segments, footnotes, and preliminary 8-K results;
- corporate-action, float/treasury-share, enterprise-value, and terminal-return parity;
- licensed estimate adapters and contributor/broker symbology without embedding licensed
  content in tests;
- reconciliation corpus, analyst review queue, and published coverage/SLA dashboards.

### Phase D — scale and commercial operations

- isolated data/control planes, multi-region object storage, autoscaled read and batch workers;
- portal for catalog exploration, samples, cost estimates, keys, teammates, usage, jobs, and
  billing;
- contract tests for Python/R/SQL/Snowflake/Databricks delivery;
- SOC 2 controls, security review, incident response, retention/deletion, and license policy;
- release channels, canaries, customer-specific allowlists, and zero-downtime schema evolution.

## Explicit non-claims

The code and schema can reach provider-grade parity using public and customer-licensed feeds;
the repository does not grant redistribution rights to proprietary inputs. Empty injectable
estimate or baseline tables are interfaces, not content parity. "Compustat compatible" means
the ATX semantics and output cover the research use case; it must not imply S&P or FactSet
endorsement, identical proprietary values, or permission to redistribute their data.
