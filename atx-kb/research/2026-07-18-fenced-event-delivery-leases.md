# Fenced event delivery leases

Date: 2026-07-18

## Question

How should an embedded, durable event consumer prevent competing workers from
simultaneously receiving the same batch without weakening at-least-once
recovery?

## Primary-source findings

- NATS JetStream starts an acknowledgement timer after delivery and redelivers
  after `AckWait`; `MaxAckPending` bounds outstanding work. A late
  acknowledgement can race with redelivery, so a local design should fence
  settlement more strictly than accepting any old acknowledgement.
  <https://docs.nats.io/nats-concepts/jetstream/consumers>
- Amazon SQS makes received messages temporarily invisible and returns a receipt
  handle used for deletion. When visibility expires the message becomes
  available again, and an expired receipt handle is invalid. A retry carrying
  the same FIFO `ReceiveRequestAttemptId` can obtain the same messages and
  receipt handles while the receive remains unmodified.
  <https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-visibility-timeout.html>
  <https://docs.aws.amazon.com/AWSSimpleQueueService/latest/APIReference/API_ReceiveMessage.html>
- Azure Service Bus Peek-Lock atomically retrieves and locks work, permits the
  current holder to renew it, and returns the work for redelivery when the lock
  expires. It explicitly retains at-least-once semantics and recommends
  idempotent downstream handling.
  <https://learn.microsoft.com/en-us/azure/service-bus-messaging/message-transfers-locks-settlement>
- Kubernetes leases combine holder identity, renewal time, duration, transition
  count, and optimistic concurrency. Expired holders lose authority and only
  one competing update wins.
  <https://kubernetes.io/docs/concepts/cluster-administration/coordinated-leader-election/>

## Decision for atx-db schema v11

Keep `consumer-poll` as a non-mutating diagnostic/read API and add an explicit
leased receive protocol:

1. `receive_event_consumer` runs under `BEGIN IMMEDIATE`, returns at most one
   active batch per consumer, and records a caller request token plus a fresh
   database-generated delivery token.
2. Exact retries of an active receive request return the same token, range, and
   events without extending the lease. Reusing the request token with changed
   intent is rejected.
3. A competing receiver is rejected while the lease is live. After expiry, the
   same ordered range is redelivered with a fresh delivery token and incremented
   attempt number.
4. Only the current owner and unexpired delivery token can renew or settle.
   Settlement atomically advances the monotonic cursor, appends the existing
   checkpoint audit record, marks the delivery settled, and clears the active
   lease.
5. Manual revision-based checkpoints remain compatible but cannot bypass an
   active delivery lease.
6. Delivery attempts are audited outside `agent_events`: emitting receive/ack
   events into the consumed stream would make unfiltered consumers chase their
   own acknowledgement traffic indefinitely.

This reduces simultaneous duplicate work but does not claim exactly-once side
effects. A worker can complete an external action and then lose its lease before
settlement, so downstream mutations still need stable event-derived
idempotency keys.

## Validation

- 54 focused SHA-256, atx-kb, atx-db, and CLI tests pass. The new cases cover
  exact receive retry, changed-intent rejection, active-lease exclusion,
  renewal/owner fencing, expiry redelivery, stale receipt rejection, an
  eight-connection receive race, v8-to-v11 migration, and restoration of an
  active delivery through online backup.
- A public CLI smoke test delivered one task event, returned the identical
  receipt on exact retry, rejected a competing receiver, renewed and settled
  through the current fence, resumed with zero events, and passed integrity.
- The 110-document/41-query atx-kb quality gate passes every threshold.
- The 1,024-vector ANN gate retains recall@10 of 1.0, exact-distance work
  reduction of 4.006848x, and warm-latency speedup of 5.473627x.
