# Retry-safe idempotency for temporal fact writes

Date: 2026-07-18

## Research basis

Primary references:

- AWS Builders' Library, Making retries safe with idempotent APIs:
  https://aws.amazon.com/builders-library/making-retries-safe-with-idempotent-APIs/
- Stripe API, Idempotent requests:
  https://docs.stripe.com/api/idempotent_requests
- IETF HTTPAPI Idempotency-Key draft 07:
  https://datatracker.ietf.org/doc/draft-ietf-httpapi-idempotency-key-header/07/
- SQLite transactions: https://www.sqlite.org/lang_transaction.html
- SQLite CREATE TABLE constraints: https://www.sqlite.org/lang_createtable.html

AWS's production guidance models retry intent with a caller-provided request
identifier. It requires recording the identifier and every side effect in one
ACID operation, returns a semantically equivalent response for retries, and
rejects reuse of the identifier with different parameters. Stripe documents
the same parameter-mismatch rule and retains the first executed result so a
client can safely retry after an indeterminate network outcome. The current
IETF draft likewise defines a unique client key that must not be reused with a
different payload; it is a draft, not relied upon here as a finalized standard.

SQLite permits only one simultaneous writer and `BEGIN IMMEDIATE` acquires that
write transaction before mutation. A partial unique index can enforce one
non-empty key per workspace. Together these provide the atomic serialization
boundary needed for local multi-process agents.

## Schema-v8 contract

- `FactInput.idempotency_key` is optional and scoped to the atx-db workspace.
- A keyed request checks for a prior primary fact version inside the same
  `BEGIN IMMEDIATE` transaction, before advancing the temporal sequence,
  closing an interval, inserting split segments, or appending events.
- An exact replay compares subject, predicate, object, the originally requested
  valid interval, evidence source/observation/hash/status, and confidence. It
  returns the original fact ID, transaction time, and temporal sequence without
  new mutations.
- A changed request with the same key returns `InvalidArgument`. Concurrent
  callers serialize at the write boundary, so the first committed request wins
  and every identical waiter resolves to that same result.
- The key remains on the client-created primary version even after it becomes
  historical. A retry therefore remains stable after later corrections.
- Valid-time fragments created internally while splitting an older assertion
  carry the older evidence tuple but never copy a caller key. This prevents one
  logical request from appearing to own multiple rows.
- For an omitted `valid_from`, schema v8 stores an empty requested value beside
  the resolved effective time. This preserves the original `valid now` intent
  and makes retries independent of their later wall-clock arrival.
- A workspace-unique partial index enforces non-empty keys. Fresh schema checks
  and runtime integrity checks enforce key/request-time consistency; the v7 to
  v8 migration gives legacy rows empty keys without rewriting fact history.
- Both `fact-put` and `fact-put-verified` expose `--key` in the CLI.

## Regression evidence

All 48 focused SHA-256, atx-kb, atx-db, migration, backup, concurrency, and CLI
tests pass. Eight simultaneous connections issuing the same keyed fact request
all receive one fact ID, transaction timestamp, and temporal sequence, with one
`fact.put` event. Coverage also proves parameter-conflict rejection, `valid
now` replay, verified replay, replay after interval splitting, internal fragment
key isolation, and schema-v2 through schema-v8 migration.

A CLI smoke test returned fact ID 1 for both the original request and its retry,
recorded one transition, rejected a changed object with the same key, and
passed database integrity verification.

The deterministic retrieval gate remains green on 110 documents and 41
queries. The default ANN benchmark retains recall@10 of 1.0, 4.006848x
distance-work reduction, and 4.870742x warm-latency speedup.

## Remaining gates

- Add bounded retention only if product semantics permit key expiry; permanent
  temporal history currently makes permanent keys the safer contract.
- Expose an idempotency replay indicator and original request timestamp in
  higher-level service responses and observability metrics.
- Add crash fault injection at each split/clock/event boundary to prove that a
  killed process leaves either the entire first request or none of it.
- Extend the same caller-intent contract to any future batch fact API while
  preventing partial batch replay.
