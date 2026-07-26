# Visibility-aware consumer backlog telemetry

Date: 2026-07-18

## Operational gap

Atx-db consumers persist a processing cursor over a workspace-wide append-only
sequence, then apply an exact subject filter and schema-v22 local-control
suppression when selecting work. The raw arithmetic difference between the
workspace event high-watermark and the cursor is therefore not consumer lag:
the gap can contain other subjects, locally suppressed `consumer.*` controls,
or both. A broad consumer whose only newer events are its own controls can
correctly poll empty while appearing indefinitely behind to an operator that
only sees the two sequence numbers.

Primary systems expose delivery eligibility separately from physical log or
queue position:

- Kafka distinguishes the current position from the durably committed
  position, documents that offsets can contain gaps, and adjusts read-committed
  lag to the last stable offset rather than the physical log end:
  https://kafka.apache.org/41/javadoc/org/apache/kafka/clients/consumer/KafkaConsumer.html
- Google Cloud Pub/Sub defines backlog from unacknowledged messages and states
  that subscription-filter misses are automatically acknowledged and excluded
  from backlog metrics:
  https://docs.cloud.google.com/pubsub/docs/monitoring
- Amazon SQS reports messages available for retrieval separately from delayed
  and in-flight messages:
  https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-resources-required-process-messages.html

The shared principle is that operational backlog must reflect what this
consumer can actually receive, not every physical occurrence after an offset.

## Contract

1. Add an explicit, read-only consumer status API and CLI command. Do not make
   ordinary `poll` or `receive` pay for an exact backlog count.
2. Report the workspace event high-watermark as physical context, while
   separately reporting the exact count, first sequence, last sequence, and
   oldest creation time of pending events visible to this consumer.
3. Compute visibility with the same workspace, cursor, exact-subject, and
   migration-cutoff self-control predicate used by polling and leased receive.
4. When no event is visible, the count and first/last sequences are zero and
   the oldest timestamp is empty even if the raw event high-watermark is above
   the cursor.
5. Status is a single SQLite read snapshot and never advances a cursor,
   revision, lease, checkpoint chain, or event high-watermark.
6. Status remains derived state. Schema v22 and persisted backup formats do not
   change; restored databases recompute the same answer from immutable events
   and consumer configuration.

## Test obligations

- a broad consumer reports no visible backlog when only its post-cutoff local
  controls are newer than its cursor;
- a subject-filtered consumer excludes unrelated physical tail events;
- visible events produce the exact count, first/last sequences, and oldest
  timestamp in sequence order;
- status reads are repeatable and do not mutate cursor, revision, delivery
  state, or the event high-watermark;
- a later visible event is reflected immediately and remains visible after
  backup/restore;
- CLI JSON exposes the physical and visibility-aware fields without changing
  existing consumer JSON.
