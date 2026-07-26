# Bounded redelivery and dead letters

Date: 2026-07-18

## Question

How should atx-db prevent a permanently failing event batch from pinning a
durable consumer forever without silently discarding the original evidence?

## Primary-source findings

- Amazon SQS redrive policy uses `maxReceiveCount` to move repeatedly received,
  unprocessed messages into a DLQ. AWS emphasizes choosing a limit large enough
  for transient failures, inspecting DLQ contents, and performing redrive as an
  explicit operator action.
  <https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-dead-letter-queues.html>
  <https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-configure-dead-letter-queue-redrive.html>
- Azure Service Bus increments delivery count when a peek-lock is abandoned or
  expires, moves messages exceeding `MaxDeliveryCount` to a built-in DLQ, and
  annotates them with `MaxDeliveryCountExceeded`. The DLQ is inspectable and
  applications may explicitly dead-letter malformed or unauthenticated work.
  <https://learn.microsoft.com/en-us/azure/service-bus-messaging/service-bus-dead-letter-queues>
- NATS JetStream bounds redelivery with `MaxDeliver` and emits a maximum-
  deliveries advisory while retaining the original message in the stream.
  <https://docs.nats.io/nats-concepts/jetstream/consumers>
  <https://docs.nats.io/using-nats/developer/develop_jetstream/consumers>

## Decision for atx-db schema v12

1. Add immutable `max_delivery_attempts` to each consumer. Zero preserves v11's
   unlimited redelivery; positive values cap leased delivery attempts.
2. When the final attempt expires, atomically mark that receipt expired, append
   a reason-annotated dead-letter batch, advance the consumer cursor through an
   explicit `dead_lettered` transition, clear the old fence, and optionally
   lease the next visible batch in the same receive transaction.
3. Retain the immutable source events in `agent_events`. The DLQ stores the exact
   previous/through sequence range, filter-derived event count, final receipt,
   attempt count, reason, and time; listing reconstructs the exact events for
   inspection.
4. Extend the checkpoint chain with an outcome (`processed` or
   `dead_lettered`) instead of pretending poison work completed successfully.
5. Treat a leased batch as an ack-all unit. If one handler fails the batch until
   the cap, every event in that exact batch is isolated together. Consumers that
   need per-event poison isolation should receive with limit one.
6. Keep redrive explicit and out of this cycle. Automatic replay could recreate
   the poison loop and would blur whether a corrected event is a new occurrence.
   Operators can inspect the immutable originals and deliberately append a
   corrected/new event; a future redrive API can add its own idempotent audit.

The DLQ guarantees durable isolation and forward progress, not successful
processing. Monitoring must treat every open dead letter as actionable state.

## Validation

- 56 focused SHA-256, atx-kb, atx-db, and CLI tests pass. New cases cover a
  capped two-attempt poison batch, exact metadata replay for the next lease,
  stale receipt fencing, continued healthy work, restored DLQ inspection after
  online backup, and a six-connection terminal-expiry race that commits exactly
  one dead letter.
- The public CLI isolated sequence 3 after its first expired attempt, leased and
  settled sequence 4, reconstructed the dead-letter event and
  `max_delivery_attempts_exceeded` reason, replayed the same healthy receipt and
  DLQ counters on exact request retry, and passed integrity at revision 3.
- Every atx-kb quality threshold remains green on 110 documents and 41 queries.
- The 1,024-vector ANN gate retains recall@10 of 1.0, exact-distance work
  reduction of 4.006848x, and warm-latency speedup of 4.734803x.
