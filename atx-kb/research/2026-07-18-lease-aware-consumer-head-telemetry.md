# Lease-aware consumer-head telemetry

Date: 2026-07-18

## Review finding

Visibility-aware backlog fixes phantom lag from subject filters and local
control suppression, but every event after the committed cursor remains
unacknowledged while a batch is leased. A single pending count cannot tell an
operator whether the ordered head is actively being processed, hidden in retry
cooldown, eligible for redelivery, or ready for terminal dead-letter handling.
Those states have very different remediation and scaling implications.

Primary systems expose the distinction explicitly:

- Amazon SQS separately reports messages available for retrieval, messages in
  flight after receipt, and delayed messages not yet available:
  https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-resources-required-process-messages.html
- SQS visibility-timeout semantics keep received messages in the queue but hide
  them until deletion or expiry, at which point they become retrievable again:
  https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-visibility-timeout.html
- NATS JetStream models a consumer as a stateful filtered stream view, tracks
  delivery and acknowledgments, bounds outstanding unacknowledged messages,
  and applies explicit redelivery backoff:
  https://docs.nats.io/nats-concepts/jetstream/consumers

Atx-db is stricter than those general queue models: one ordered leased head
blocks every later visible occurrence until it settles or becomes a DLQ
terminal. Its status should expose that durable head directly.

## Contract extension

1. Keep `pending_visible_event_count` as the exact unacknowledged backlog under
   the consumer's delivery visibility predicate.
2. Add a non-secret `delivery_head_state` with five exhaustive values:
   `idle`, `in_flight`, `retry_backoff`, `redelivery_ready`, and
   `dead_letter_ready`.
3. For a non-idle head expose owner, attempt, through-sequence, exact visible
   event count, lease expiry, and retry eligibility time. Never expose the
   delivery fencing token or receive request token through status.
4. Report `queued_visible_event_count` behind the durable head. Head plus
   queued count must equal pending visible count.
5. Report `available_visible_event_count` using strict ordering: all pending
   events when idle, zero while in flight or cooling, only the head when ready
   for redelivery, and the queued tail when the terminal head will be moved to
   the DLQ by the next receive.
6. Determine time-dependent state inside the same SQLite read statement as
   backlog and consumer state. Status remains mutation-free and schema-v22
   compatible.

## Test obligations

- a newly leased head reports `in_flight`, its non-secret audit fields, zero
  available work, and the exact queued tail;
- a retry rejection reports `retry_backoff`, then becomes
  `redelivery_ready` without a database mutation when its durable time passes;
- a final expired attempt reports `dead_letter_ready`, after which receive
  isolates it and exposes the next ordered batch;
- every state preserves the count partition and hides all fencing tokens;
- active and cooling status survives online backup/restore;
- repeated API and CLI status reads do not alter revisions, delivery audits,
  checkpoints, or the event high-watermark.
