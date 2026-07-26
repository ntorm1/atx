# Durable negative acknowledgments

Date: 2026-07-18

## Question

How should an atx-db worker explicitly report that a leased event batch failed,
without waiting for lease expiry, bypassing fencing, hot-looping, or creating an
ambiguous outcome when the rejection response is lost?

## Primary-source findings

- Google Pub/Sub treats a negative acknowledgment, or an acknowledgment deadline
  set to zero, as an explicit request to redeliver. Its retry policy applies the
  same exponential backoff to NACKs and deadline expiry, and its troubleshooting
  guide warns that repeated NACKs can cause duplicates and head-of-line delay.
  <https://docs.cloud.google.com/pubsub/docs/pubsub-basics>
  <https://docs.cloud.google.com/pubsub/docs/pull-troubleshooting>
- Azure Service Bus PeekLock settlement lets the lock owner complete successful
  work, abandon failed work for redelivery, or explicitly dead-letter work known
  to be non-retryable. Repeated abandon or lock expiry contributes to maximum
  delivery count and eventual DLQ isolation.
  <https://learn.microsoft.com/en-us/azure/service-bus-messaging/message-transfers-locks-settlement>
- RabbitMQ distinguishes negative acknowledgment with requeue from rejection
  without requeue; the latter is one of the broker's dead-letter triggers.
  <https://www.rabbitmq.com/docs/dlx>

## Decision for atx-db schema v15

1. `reject_event_consumer_delivery` requires consumer, current owner, fenced
   delivery token, caller rejection token, and a bounded reason. It runs in
   `BEGIN IMMEDIATE`; an expired or replaced receipt cannot reject work.
2. The delivery audit stores rejection token, reason, and rejection time. An
   exact caller-token retry returns the original durable outcome even if a
   successor has since run; token or intent reuse with different values fails.
3. Below the consumer's maximum-attempt policy, rejection ends the lease at the
   rejection time and schedules `retry_not_before` using the same capped
   exponential delay as timeout. It does not increment attempts until a new
   receipt is actually acquired. Settlement and renewal are fenced immediately.
4. If the rejected receipt is already the configured final attempt, rejection
   atomically expires the audit receipt, creates a reasoned DLQ batch, advances
   the monotonic cursor with a `dead_lettered` checkpoint, and clears the active
   fence. It does not wait for another receive call or a retry cooldown.
5. Schema-v14 timeout DLQs retain `max_delivery_attempts_exceeded`; terminal
   explicit rejection uses `max_delivery_attempts_rejected`, while the worker's
   detailed reason remains on the immutable delivery audit.
6. Integrity verification proves the all-or-none rejection metadata, caller
   token uniqueness, canonical time, terminal DLQ reason, and that rejected
   retryable receipts still have exactly one successor before their range can
   advance.

This API classifies a processing attempt as failed; it does not support silently
skipping ordered work or changing the immutable source event. Non-retryable work
can be isolated by registering a one-attempt consumer and rejecting its receipt.

## Validation

- 63 focused SHA-256, atx-kb, atx-db, and CLI tests pass. New cases cover
  immediate lease fencing, exact rejection retry, changed-intent rejection,
  durable cooldown, successor attempt creation, atomic terminal DLQ/checkpoint,
  online-backup restoration, and eight concurrent callers converging on one
  rejection time and retry schedule.
- A CLI lifecycle rejected a 30-second receipt immediately, replayed the exact
  result, rejected conflicting intent and an early receive, acquired attempt two
  after cooldown, terminally rejected it into a reasoned DLQ in the same call,
  replayed that terminal result, and passed integrity verification.
- Every retrieval-quality threshold remains green on 110 documents and 41
  queries. The deterministic 1,024-vector ANN gate retains recall@10 of 1.0,
  exact-distance work reduction of 4.006848x, and warm-latency speedup of
  4.927039x.
