# Actionable DLQ health telemetry

Date: 2026-07-18

## Review finding

A consumer can have no pending visible events and no active delivery head while
retained dead-letter batches still require an operator decision. Reporting only
the main delivery path makes that consumer appear caught up and healthy even
when poison work is accumulating off-path.

Primary systems treat dead-letter workload as a separate health signal:

- Amazon SQS recommends the visible-message metric for DLQ monitoring and
  alarms, because ordinary sent-message metrics do not include every automatic
  move caused by processing failure:
  https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-available-cloudwatch-metrics.html
- Amazon SNS likewise recommends alarming on the DLQ's visible message count,
  including messages moved there by failed delivery:
  https://docs.aws.amazon.com/sns/latest/dg/sns-dead-letter-queues.html
- Google Cloud Pub/Sub exposes a dedicated dead-letter message count and then
  recommends backlog count and oldest age on the dead-letter subscription:
  https://docs.cloud.google.com/pubsub/docs/monitoring

Atx-db retains richer terminal state than a separate opaque queue: every DLQ
batch has an exact event count and is either actionable open, redriven, or
quarantined through immutable audit overlays. Consumer health can therefore be
reported exactly from the same snapshot as delivery backlog.

## Contract extension

1. Add exact `open_dead_letter_count` and `open_dead_letter_event_count` for
   actionable batches that are still open and have no quarantine audit.
2. Add `oldest_open_dead_letter_at`, empty when no actionable batch exists, so
   operators can distinguish fresh from aging poison work.
3. Add retained `redriven_dead_letter_count` and
   `quarantined_dead_letter_count`. These are audit-history categories, not
   actionable workload.
4. The three batch counts must partition every retained DLQ row for the named
   workspace consumer; quarantine is derived from its immutable overlay even
   though the base row remains open.
5. Compute all values in the existing single status statement. Status stays
   read-only, exact, schema-v22 compatible, and off delivery hot paths.

## Test obligations

- a terminal delivery moves its exact batch/event count into actionable open
  telemetry while the main pending count can reach zero;
- quarantine atomically moves the batch from open to quarantined telemetry and
  clears the oldest-open timestamp without deleting any audit history;
- a redriven batch is reported separately from both open and quarantined work;
- repeated API and CLI reads do not publish lifecycle events or mutate DLQ,
  cursor, revision, or lease state;
- online backup/restore recomputes identical counts and timestamps;
- live status agrees with exact `consumer-dead-letters` inspection for existing
  historical, redriven, and quarantined batches.
