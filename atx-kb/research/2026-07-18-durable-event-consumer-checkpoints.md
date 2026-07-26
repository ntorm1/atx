# Durable monotonic checkpoints for event consumers

Date: 2026-07-18

## Research basis

Primary references:

- NATS JetStream consumers:
  https://docs.nats.io/nats-concepts/jetstream/consumers
- NATS consumer details:
  https://docs.nats.io/using-nats/developer/develop_jetstream/consumers
- NATS JetStream model deep dive:
  https://docs.nats.io/using-nats/developer/develop_jetstream/model_deep_dive
- Apache Kafka consumer configuration:
  https://kafka.apache.org/42/configuration/consumer-configs/
- Apache Kafka design and delivery semantics:
  https://kafka.apache.org/41/design/design/
- SQLite transactions: https://www.sqlite.org/lang_transaction.html

NATS models a durable consumer as persisted state over a stream, optionally
filtered by subject, that survives client failure and advances through explicit
acknowledgements. Pull consumers let the application control batching and flow.
Without a timely acknowledgement, delivery is at least once and may repeat.

Kafka likewise distinguishes the consumer's current position from its committed
offset. Its design documentation emphasizes that committing after processing
gives at-least-once behavior, while exactly-once effects require the processed
output and consumer offset to share a transaction. A local cursor cannot make
an unrelated external side effect atomic.

SQLite's single-writer transaction boundary and `BEGIN IMMEDIATE` provide a
compact way to serialize competing checkpoint writers. A revision predicate
fences stale pollers; a caller token distinguishes an idempotent network retry
from another poller's intent.

## Schema-v10 consumer contract

- A consumer has a workspace-scoped durable name, immutable optional exact
  subject filter, immutable start sequence, monotonic cursor, revision, and
  timestamps.
- Registration is idempotent for the same name/filter/start configuration and
  rejects reconfiguration under an existing name. It cannot start beyond the
  current global event high watermark.
- `poll_event_consumer()` reads a bounded batch strictly after the persisted
  cursor and applies the exact subject filter. Polling never mutates consumer
  state, so a crash before acknowledgement replays the same batch.
- `checkpoint_event_consumer()` acknowledges through a visible event sequence.
  It requires the expected consumer revision and a non-empty caller token,
  advances only forward, and commits cursor, revision, and audit record in one
  immediate transaction.
- Two competing tokens from the same revision cannot both win. The first
  checkpoint advances the revision; the other receives a stale-revision error.
  Repeating the winning token with the exact request is idempotent. Reusing that
  token for another revision or sequence is an error.
- Checkpoint history is append-only and separate from `agent_events`. Publishing
  a checkpoint event into the same unfiltered stream would cause a consumer to
  consume and acknowledge its own acknowledgements forever.
- Integrity verification walks every consumer's checkpoint chain in revision
  order, proves cursor continuity, validates each token, and confirms every
  acknowledged sequence exists in the workspace and matches the immutable
  subject filter.
- `consumer-register`, `consumer-poll`, and `consumer-checkpoint` expose the
  contract in the CLI. Consumer/checkpoint workspaces are included when restored
  backup pairs enumerate all domains.

## Regression evidence

All 51 focused SHA-256, atx-kb, atx-db, migration, backup, concurrency, and CLI
tests pass. Coverage proves filtered batch order, explicit acknowledgement,
process reopen at the saved cursor, idempotent checkpoint retry, token-conflict
rejection, cursor rollback rejection, future-start rejection, and one winner
between simultaneous checkpoint writers. Online backup restores the consumer
head and checkpoint chain and resumes with an empty processed batch.

A separate-process CLI round trip registered `fact-projector` on `facts/1`,
delivered sequence 1, checkpointed it at revision 2, replayed the same token at
revision 2, returned no remaining events after restart, and passed integrity.

The deterministic retrieval gate remains green on 110 documents and 41
queries. The default ANN benchmark retains recall@10 of 1.0, 4.006848x
distance-work reduction, and 5.011848x warm-latency speedup.

## Explicit guarantee boundary and next gates

- The consumer provides at-least-once delivery. External sinks must be
  idempotent or share a transactional system with their offset to claim
  exactly-once effects.
- Add bounded in-flight leases, acknowledgement deadlines, negative
  acknowledgements, backoff, maximum delivery attempts, and dead-letter state
  before using one durable name as a horizontally distributed work queue.
- Add retention floors based on every durable consumer only after defining
  administrative deletion, abandoned-consumer policy, and restore semantics.
- Benchmark polling and integrity-chain verification at millions of events and
  checkpoints; add compact checkpoint summaries only if measurement requires
  them without erasing the audit trail.
