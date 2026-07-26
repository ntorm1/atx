# Durable dead-letter redrive velocity controls

Date: 2026-07-18

## Reliability gap

Schema v17 makes retry timing resistant to synchronized failure, but an operator can still redrive many dead-letter batches into a recovering consumer without a capacity boundary. Exact, idempotent redrive prevents duplicate publication; it does not prevent a legitimate replay surge.

Amazon SQS exposes a custom maximum redrive velocity and recommends starting small, monitoring the source, and ramping only after verifying that it is not overwhelmed. AWS SDK retry guidance uses a token budget to stop retries when failure pressure has depleted capacity. Both patterns separate correctness of one retry/redrive from admission control over aggregate recovery traffic.

Primary sources:

- Amazon SQS dead-letter queue redrive configuration: https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-configure-dead-letter-queue-redrive.html
- AWS SDK retry quota behavior: https://docs.aws.amazon.com/sdkref/latest/guide/feature-retry-behavior.html

## v18 contract

1. Each consumer has an immutable optional `redrive_rate_per_second` and `redrive_burst_events`. Both zero preserve unlimited schema-v17 behavior; otherwise both are positive.
2. The durable token bucket stores thousandths of an event. One elapsed millisecond earns `rate_per_second` units, one event costs 1,000 units, and capacity is `burst_events * 1,000` units. Integer arithmetic makes refill and replay exact.
3. A limited consumer starts with a full bucket. Before publishing an open DLQ batch, redrive refills from the last durable timestamp, rejects a batch larger than burst capacity, and fails without mutation when the available budget is insufficient.
4. A successful redrive deducts the entire atomic batch in the same immediate transaction as its new event occurrences, sequence mappings, DLQ state transition, and budget update.
5. Every limited redrive appends a budget-charge audit containing the DLQ, redrive token, event count, refilled balance, result balance, and effective refill time. Integrity verification replays the charge chain and matches the consumer head.
6. Exact retries of a committed redrive token return the original occurrence map without consuming budget. Conflicting tokens still reject. Concurrent redrives serialize at SQLite's write boundary and cannot overspend the shared consumer bucket.
7. Wall-clock rollback earns no credit and does not move the refill watermark backward. Backup and restore preserve the balance, watermark, and complete charge history.
8. Schema-v17 migration assigns unlimited policy and empty budget state, preserving every existing consumer and redrive.

## Atomic-batch boundary

Consumer DLQs retain ack-all batches. A velocity-limited redrive remains all-or-nothing, so its configured burst must be at least the event count of a batch being redriven. An undersized burst is a permanent configuration mismatch and is rejected distinctly from temporary budget exhaustion. Handlers that require fine-grained recovery should continue to receive with limit one.

## Test obligations

- unlimited v17 behavior remains unchanged;
- limited registration is immutable and validates rate/burst pairs;
- the first burst succeeds, the next redrive is non-mutating and reports temporary exhaustion;
- refill after elapsed time admits the next batch with exact integer accounting;
- exact token retry is charge-free;
- concurrent different-batch redrives cannot overspend one bucket;
- backup/restore retains throttling state;
- integrity rejects broken charge arithmetic, ordering, identity, or head state;
- CLI and JSON expose policy and auditable charge metadata.
