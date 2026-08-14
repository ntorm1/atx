# Commercial API controls — 2026-08-14

## Decision

ATX historical delivery now uses a provider-style commercial contract instead of treating
authentication and usage tables as passive future schema.

- Preflight methods return exact visible record count and uncompressed Arrow billable
  bytes for the normalized point-in-time request.
- A versioned unit-price catalog converts decimal gigabytes to USD when an active contract
  price exists. Unconfigured schemas report `contract_required` and a null cost rather
  than inventing a public price.
- Synchronous data is billed per delivered response. A batch job is billed once when its
  immutable artifact is generated; repeat downloads carry zero billable bytes.
- Entitlement request rates use a local sliding one-minute window. Monthly byte ceilings
  are checked before synchronous/batch work, checked again against exact synchronous
  output, and enforced while batch batches are written.
- `Idempotency-Key` records are scoped to account and endpoint for 24 hours. Identical
  retries return the original batch job; changed parameters fail with `409`; an unfinished
  concurrent request reports in-progress rather than creating a duplicate.
- Batch execution can run inline for development or through standalone lease-based
  workers. Workers reclaim expired jobs, heartbeat during output, verify ownership before
  finalization, and publish attempt-isolated artifacts so stale workers cannot overwrite a
  winning attempt.

## Research basis

Databento exposes `metadata.get_record_count`, `metadata.get_billable_size`, and
`metadata.get_cost`, meters historical data by its uncompressed binary size, bills batch
generation once, and permits repeated downloads without another data charge. It also
distinguishes billed size from compressed/package size. ATX uses the equivalent stable
internal unit `uncompressed_arrow_bytes` because Arrow is the canonical typed transport in
this implementation. Sources:

- [Databento historical batch parameters and metered pricing](https://databento.com/docs/api-reference-historical/batch/batch-submit-job/parameters)
- [Databento usage pricing and data credits](https://databento.com/docs/faqs/usage-pricing-and-data-credits)
- [Databento batch downloads](https://databento.com/docs/api-reference-historical/batch/batch-download)

Stripe's API documents the retry properties needed by customer SDKs: idempotency keys are
client supplied, account/API scoped, parameter reuse must match, POST results are retained
for at least 24 hours, and keys are limited to 255 characters. Its rate-limit guidance uses
`429`, reason headers, retry/backoff, and multiple limiter scopes. ATX adopts those transport
semantics without coupling the data plane to Stripe. Sources:

- [Stripe idempotent requests](https://docs.stripe.com/api/idempotent_requests?lang=curl)
- [Stripe rate limits](https://docs.stripe.com/rate-limits)

## Implementation evidence

- Migration 0270 creates `api_unit_price_catalog` and `saas_idempotency_records`, enriches
  usage with billable bytes/cost/mode, and adds request hashes, attempts, workers, leases,
  retry time, idempotency key, and quota snapshot fields to batch jobs.
- `atx-db-control set-price` versions a schema price without rewriting prior terms.
- `atx-db-worker` supports one-shot, drain-until-empty, and continuously polling modes.
- HTTP responses distinguish billable bytes from encoded response/package bytes.
- Metadata, auth, and repeated artifact downloads remain non-billable.

## Verification

- strict Ruff and mypy checks pass for the complete `atx_db.api` package and migration
  0270;
- 51 focused policy/API/migration/import/schema-contract tests pass;
- the repository fast lane passes 1,425 tests with 5 intentionally skipped, from 1,430
  collected, in 195.5 seconds;
- end-to-end tests prove exact estimate-to-batch billable-byte equality, persisted USD
  cost, identical idempotent replay, changed-parameter conflict, one-job durability,
  rate and byte-quota rejection, out-of-process processing, and expired-lease recovery;
- `atx-db-control --help` and `atx-db-worker --help` execute through installed entry
  points.

## Remaining production work

The DuckDB adapters are deliberately a single-node deployment. Multi-node enforcement
still requires PostgreSQL transactions for control state, a Redis-compatible distributed
rate/concurrency limiter, atomic quota reservations, a real queue with visibility timeout
and dead-letter policy, and object-store conditional publication plus signed URLs. Those
interfaces are now explicit rather than hidden inside HTTP handlers.
