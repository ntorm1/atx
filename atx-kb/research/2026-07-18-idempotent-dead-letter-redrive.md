# Idempotent dead-letter redrive

Date: 2026-07-18

## Question

How should an operator reprocess an inspected atx-db dead letter without moving
the consumer cursor backward, losing the original failure record, or publishing
duplicates after an ambiguous retry?

## Primary-source findings

- Amazon SQS redrive is an explicit operator task. Redriven messages are new
  messages with new message IDs and enqueue times, and operators can limit
  redrive velocity to avoid overwhelming the destination.
  <https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-configure-dead-letter-queue-redrive.html>
- Azure Service Bus describes correction followed by resubmission. Its Explorer
  resend operation sends a copy and does not delete the original DLQ message,
  preserving the failure record for inspection.
  <https://learn.microsoft.com/en-us/azure/service-bus-messaging/service-bus-dead-letter-queues>
  <https://learn.microsoft.com/en-us/azure/service-bus-messaging/explorer>
- Azure's current sample similarly retrieves, corrects, and resubmits a message
  rather than rewinding queue state.
  <https://learn.microsoft.com/en-us/samples/azure/azure-sdk-for-net/servicebus-dead-letter-queue/>

## Decision for atx-db schema v13

1. `redrive_event_consumer_dead_letter` requires consumer name, DLQ batch ID,
   and a caller redrive token. It runs in `BEGIN IMMEDIATE`.
2. For each original event in the dead-letter range, append a new event
   occurrence with the same type, domain IDs, subject, and payload. The new
   durable sequence and occurrence time intentionally differ.
3. Store a one-to-one original-sequence to redriven-sequence mapping. This makes
   causation exact even when the original filtered batch was sparse in the
   global sequence or unrelated writers later append events.
4. Mark the DLQ batch `redriven` with token and time but retain the original
   events and reason. An exact token retry returns the existing mapping; a
   changed token is rejected. SQLite atomicity prevents a partial batch publish.
5. Do not rewind the monotonic consumer cursor. Redriven occurrences append
   after the current high watermark and become ordinary future consumer work.
6. Allow one redrive per DLQ batch. If the new occurrences poison again, they
   create a new DLQ record with a new independent audit/redrive decision.

This API replays exact event envelopes. Correcting payload content remains an
explicit new publish because silently editing evidence during redrive would
destroy the connection to the original failure.

## Validation

- 57 focused SHA-256, atx-kb, atx-db, and CLI tests pass. The schema-v13 cases
  cover original-envelope equality, exact token retry, conflicting-token
  rejection, consumer processing of the new occurrence, verified online-backup
  restoration of the mapping, and eight concurrent connections converging on
  exactly one appended occurrence.
- A CLI lifecycle created a bounded consumer, exhausted an event into its DLQ,
  redrove it, retried the same token, rejected a changed token, consumed and
  settled the new occurrence, then passed domain integrity verification.
- Every atx-kb quality threshold remains green on 110 documents and 41 queries.
  Citation validity, context-ledger validity, retrieval recall, hybrid NDCG@10,
  evidence precision, source recall, abstention, and ranking determinism pass.
- The 1,024-vector ANN gate retains recall@10 of 1.0, exact-distance work
  reduction of 4.006848x, and warm-latency speedup of 4.811037x.
