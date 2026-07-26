# Explicit dead-letter settlement

Date: 2026-07-18

## Question

Should a worker that knows a fenced event delivery is permanently unprocessable
have to burn every configured retry attempt before atx-db isolates it?

## Primary-source findings

- Azure Service Bus separates abandon-for-redelivery from application-level
  dead-lettering. A receiver can explicitly move malformed, unauthenticated, or
  otherwise unacceptable messages to the DLQ and attach a reason plus human
  description; this is distinct from system `MaxDeliveryCountExceeded`.
  <https://learn.microsoft.com/en-us/azure/service-bus-messaging/service-bus-dead-letter-queues>
  <https://learn.microsoft.com/en-us/azure/service-bus-messaging/message-transfers-locks-settlement>
- RabbitMQ/AMQP likewise separates `released` or NACK-with-requeue from
  `rejected` or NACK-without-requeue. The former returns work for another
  delivery; the latter dead-letters an invalid/unprocessable message when a DLX
  is configured.
  <https://www.rabbitmq.com/docs/amqp>
  <https://www.rabbitmq.com/docs/3.13/confirms>
- Azure's current sample tests both automatic exhaustion and explicit
  dead-lettering of messages that fail an application criterion.
  <https://learn.microsoft.com/en-us/samples/azure/azure-sdk-for-net/servicebus-dead-letter-queue/>

## Decision for atx-db schema v16

1. Delivery rejection gains an immutable disposition: `retry` or `dead_letter`.
   It is part of caller-token idempotency intent and is retained in the delivery
   audit and rejection result.
2. `retry` preserves schema-v15 behavior: below the attempt limit it starts the
   capped cooldown; at the final attempt it DLQs with machine reason
   `max_delivery_attempts_rejected`.
3. `dead_letter` immediately performs the same atomic cursor/checkpoint/DLQ
   transition on any attempt, including consumers with unlimited automatic
   attempts. Its stable machine reason is `explicit_rejection`; the bounded
   worker description remains separately available on the DLQ API.
4. Exact rejection-token retries require delivery, owner, disposition, and
   reason to match. Changing retry into dead-letter, or vice versa, is an intent
   conflict rather than a second settlement.
5. Integrity verification distinguishes timeout exhaustion, rejection
   exhaustion, and explicit application isolation and proves each is compatible
   with the stored disposition and consumer attempt policy.

Explicit dead-lettering is a terminal settlement, not deletion. The original
events, delivery failure reason, DLQ record, checkpoint, and optional later
redrive mapping remain durable and independently inspectable.

## Validation

- 64 focused SHA-256, atx-kb, atx-db, and CLI tests pass. The new case proves
  attempt-one terminal isolation under an unlimited retry policy, exact token
  retry, disposition-conflict rejection, worker-reason inspection, unchanged
  redrive compatibility, successor settlement, and full integrity verification.
- A CLI lifecycle reproduced the same path: `dead-letter` created machine reason
  `explicit_rejection` with worker reason `authentication failed`, `retry` under
  the same token was rejected, redrive appended one new occurrence, and that
  occurrence settled successfully.
- Every retrieval-quality threshold remains green on 110 documents and 41
  queries. The deterministic 1,024-vector ANN gate retains recall@10 of 1.0,
  exact-distance work reduction of 4.006848x, and warm-latency speedup of
  4.926749x.
