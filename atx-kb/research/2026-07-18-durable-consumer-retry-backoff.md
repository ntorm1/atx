# Durable consumer retry backoff

Date: 2026-07-18

## Question

How should atx-db prevent an expired event-consumer receipt from entering a hot
redelivery loop while preserving ordered delivery, durable fencing, exact audit
history, and bounded dead-letter behavior?

## Primary-source findings

- Google Pub/Sub warns that immediate redelivery can repeatedly deliver work
  while the failure condition is unchanged. Its alternative applies exponential
  backoff after acknowledgment-deadline expiry, with a configurable minimum and
  maximum delay. The policy is per message rather than a global subscription
  pause, and unrelated work may continue when ordering does not constrain it.
  <https://docs.cloud.google.com/pubsub/docs/subscription-retry-policy>
- AWS Step Functions retry policies combine an initial interval, exponential
  multiplier, maximum delay, maximum attempts, and optional jitter. The maximum
  delay prevents exponential growth from becoming operationally unbounded.
  <https://docs.aws.amazon.com/step-functions/latest/dg/concepts-error-handling.html>
- AWS Lambda's SQS integration backs off failed invocations and reduces
  concurrency rather than immediately amplifying a persistent failure. It also
  recommends partial-batch responses when independently retrying only failed
  messages is safe.
  <https://docs.aws.amazon.com/lambda/latest/dg/services-sqs-errorhandling.html>
- Azure Service Bus exposes exponential or fixed client retry modes with base
  delay, maximum delay, and maximum attempts; its guidance calls out the
  interaction between backoff and message-lock duration.
  <https://learn.microsoft.com/en-us/azure/service-bus-messaging/service-bus-timeouts-retries>

## Decision for atx-db schema v14

1. Consumer registration gains immutable `retry_backoff_seconds` and
   `retry_backoff_max_seconds`. Both zero preserves schema-v13 immediate
   redelivery. Otherwise the base is positive and the cap is at least the base.
2. The delay after expired attempt `N` is
   `min(base * 2^(N-1), maximum)`, computed with saturating integer arithmetic.
   A fixed multiplier of two keeps the durable contract small and matches the
   common exponential default. Jitter is deferred: one ordered consumer has one
   fenced head batch, so randomized wake-up does not improve correctness and
   would make the durable schedule harder to reproduce.
3. Every delivery stores `retry_not_before`; the active consumer head stores the
   same value. Renewal atomically moves both lease expiry and retry eligibility.
   This makes the cooldown recoverable after process restart and online backup.
4. Before `retry_not_before`, new receive tokens fail without expiring the old
   receipt, creating a successor, incrementing attempts, or changing the cursor.
   At eligibility, competing receivers still serialize in `BEGIN IMMEDIATE` and
   exactly one publishes the next fenced receipt.
5. A terminal attempt is dead-lettered as soon as its lease expires; no retry is
   scheduled, so backoff cannot delay poison isolation. Ack-all batch semantics
   and the recommendation to receive one event for per-event isolation remain.
6. Integrity verification proves policy configuration, canonical retry times,
   active-head/audit equality, and the exact capped exponential interval for
   every historical receipt.

The retry policy controls failure recovery, not general event scheduling. It
does not make external effects exactly once and it does not allow later events
to bypass the ordered head batch.

## Validation

- 61 focused SHA-256, atx-kb, atx-db, and CLI tests pass. New cases prove exact
  1s/2s capped intervals, non-mutating cooldown rejection, restoration of an
  expired cooling receipt from verified online backup, terminal DLQ transition
  without cooldown, lease/eligibility renewal, and one winner among six
  receivers contending when cooldown ends.
- The CLI lifecycle configured base/max delays, observed a rejected early
  receive, acquired attempt two only after eligibility, dead-lettered that final
  attempt immediately on expiry, and passed domain integrity verification. The
  smoke test exposed and drove a fix for the outer CLI option allowlist; a
  permanent command test now covers those flags.
- Every atx-kb quality threshold remains green on 110 documents and 41 queries.
  The deterministic 1,024-vector ANN gate retains recall@10 of 1.0,
  exact-distance work reduction of 4.006848x, and warm-latency speedup of
  4.739577x.
