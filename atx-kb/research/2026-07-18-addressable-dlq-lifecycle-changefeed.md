# Addressable, exact-once DLQ lifecycle changefeed

Date: 2026-07-18

## Observability gap

Schema v20 makes dead-letter quarantine durable and auditable, but none of the three DLQ lifecycle transitions (`dead_lettered`, `redriven`, or `quarantined`) is itself an addressable changefeed occurrence. A monitor must repeatedly rescan consumer-specific DLQ tables and reconstruct state changes. That is inefficient, makes notification latency dependent on polling, and gives downstream processors no stable occurrence identity for deduplication.

CloudEvents defines an event as a record of an occurrence and requires the producer's `source` plus `id` to distinguish distinct occurrences and identify duplicate retransmission. Its `subject` attribute exists specifically so generic publish/subscribe middleware can filter an entity below the source scope. Apache Kafka's exactly-once design commits produced records and consumer position atomically; its guidance for external storage is to store the position in the same place as the output. Debezium's outbox documentation similarly describes an outbox table as the boundary that avoids divergence between persisted service state and events, assigns every outbox event a unique ID for deduplication, and uses an aggregate ID as the message key for ordering.

Primary sources:

- CloudEvents 1.0 specification: https://github.com/cloudevents/spec/blob/main/cloudevents/spec.md
- Apache Kafka design, exactly-once processing and transactions: https://kafka.apache.org/41/design/design/
- Debezium outbox event router: https://debezium.io/documentation/reference/stable/transformations/outbox-event-router.html

The v21 contract below is an atx-db inference from those specifications. Atx-db already owns both durable consumer state and its SQLite changefeed, so it can provide a stronger single-database transaction than an asynchronously relayed outbox.

## v21 contract

1. Every newly committed DLQ transition appends exactly one `agent_events` occurrence in the same immediate transaction as the state transition and records an immutable mapping from `(workspace, consumer, dead-letter, transition)` to that event sequence.
2. Event types are `consumer.dead_lettered`, `consumer.dead_letter_redriven`, and `consumer.dead_letter_quarantined`. Every event uses subject `consumers/<name>` and decimal dead-letter ID payload, making the occurrence filterable without parsing internal tables.
3. `agent_events.sequence` is the durable occurrence identity. The lifecycle mapping has a unique transition key and unique non-null event sequence, so exact API-token retries return the original state without appending duplicates.
4. Lifecycle events are ordinary generation-zero occurrences: each self-roots, has redrive count zero, and can be observed with the existing subject-filtered consumer APIs.
5. Schema-v20 migration appends no `agent_events`. It creates one nullable legacy mapping for each historical transition and a per-workspace activation epoch. Existing high-watermarks and consumer cursors therefore do not move merely because the database was opened by v21 code.
6. New transitions, including redrive or quarantine of a DLQ that predates migration, always receive a real event sequence. Legacy markers are migration compatibility state only; normal runtime writes cannot create them.
7. Dead-letter list/API/CLI output exposes the event sequence for each lifecycle transition. A historical transition reports sequence zero; a v21 transition reports its addressable positive sequence.
8. Integrity verification proves complete state-to-lifecycle coverage, exact workspace/consumer/DLQ ownership, valid transition combinations, unique event attachment, matching type/subject/payload, generation-zero lineage, and a legacy transition time no later than its workspace activation epoch.

## State and event model

```text
delivery exhausted/rejected
  +-- DLQ row ------------------+-- consumer.dead_lettered
                                |
open DLQ -- redrive(token) -----+-- consumer.dead_letter_redriven
                                |
open DLQ -- quarantine(audit) --+-- consumer.dead_letter_quarantined

Each horizontal state change and its event mapping commit or roll back together.
```

## Test obligations

- automatic expiry and explicit rejection each emit one addressable dead-letter event;
- redrive and quarantine emit their respective events with exact consumer subject and payload;
- exact retry never appends a second lifecycle event;
- competing terminal writers still commit exactly one outcome and one terminal event;
- failure or rollback cannot leave lifecycle state without its event mapping;
- v20 migration backfills legacy mappings without increasing the event high-watermark;
- a post-migration transition on a historical open DLQ emits a real event;
- backup/restore preserves event sequences and exact retries;
- integrity rejects missing, cross-attached, malformed, or mismatched lifecycle events;
- API and CLI expose all lifecycle event identities.
