# Migration-safe self-control-event suppression

Date: 2026-07-18

## Reliability gap

Schema v21 makes DLQ transitions observable as ordinary `consumer.*` events. That exposes a feedback path for an unfiltered consumer: it may receive its own `consumer.dead_lettered` notification, fail it, create another DLQ and notification, and repeat indefinitely. The loop can continue without any new external work.

Amazon EventBridge explicitly warns that a rule whose action produces another matching event can create an infinite loop, and recommends precise metadata patterns that prevent the action from re-firing the rule. MQTT 5 defines a normative `No Local` subscription option under which an application message must not be forwarded to a connection with the publishing client ID. NATS similarly provides per-connection `NoEcho` because a participant in a bus pattern commonly already knows the state change it published and should not process its own update.

Primary sources:

- Amazon EventBridge event-pattern best practices: https://docs.aws.amazon.com/eventbridge/latest/userguide/eb-patterns-best-practices.html
- MQTT 5.0 OASIS Standard, subscription No Local option: https://docs.oasis-open.org/mqtt/mqtt/v5.0/mqtt-v5.0.html
- NATS no-echo client behavior: https://docs.nats.io/using-nats/developer/connecting/noecho

Atx-db has no transport connection identity: lifecycle publication happens inside the database transaction on behalf of a named consumer. The v22 contract therefore treats `consumer.*` events addressed to that same consumer as local control-plane echo. Other consumers and direct event readers continue to observe them.

## v22 contract

1. A named consumer does not poll, receive, or manually checkpoint a `consumer.*` event whose subject is `consumers/<its own name>` and whose sequence is after its immutable self-control cutoff.
2. A separate monitor with subject filter `consumers/<target name>` continues to observe every target lifecycle event. Public `events` queries are unchanged; suppression applies only to the target consumer's durable cursor APIs.
3. New consumers use cutoff zero, so their registration event and every later self-addressed control event are suppressed from their own stream. Ordinary events and another consumer's control events remain visible to an unfiltered consumer.
4. Schema-v21 migration sets each existing consumer's cutoff to that workspace's current event high-watermark. Every historical checkpoint, delivery, DLQ, and cursor therefore retains its original visibility interpretation; only future local control events are suppressed.
5. The cursor remains monotonic across suppressed gaps. If only a suppressed local event follows the cursor, poll/receive returns empty without mutation. A later visible event may advance over that gap and historical reconstruction applies the same cutoff predicate.
6. A redrive may retain and republish a historical local control occurrence, but a target consumer does not receive the post-cutoff copy. The immutable redrive mapping and separate monitors still observe it.
7. Consumer API and CLI JSON expose `self_control_event_cutoff_sequence`, making compatibility behavior inspectable rather than implicit.
8. Integrity verification replays checkpoint and delivery visibility with the cutoff, bounds the cutoff against the event high-watermark, and rejects any post-cutoff self-control event referenced as a checkpoint head, delivery member, or DLQ member.

## Visibility model

```text
event is visible under subject filter
  AND NOT (
    sequence > self_control_event_cutoff_sequence
    AND type starts with "consumer."
    AND subject == "consumers/<this consumer>"
  )

existing consumer migration: cutoff = pre-v22 workspace high-watermark
new consumer:                cutoff = 0
```

## Test obligations

- a new unfiltered consumer never receives its own registration or lifecycle events;
- it still receives ordinary events and another consumer's control events in sequence order;
- repeatedly dead-lettering ordinary work cannot create a self-sustaining delivery chain;
- poll, leased receive, manual checkpoint, delivery reconstruction, DLQ reconstruction, and integrity use the identical predicate;
- v21 migration preserves historical self-control checkpoints and sets an exact cutoff without appending events;
- a post-migration self-control event is suppressed while a later ordinary event advances over it;
- backup/restore preserves the cutoff and exact cursor behavior;
- API and CLI expose the cutoff and separate monitoring remains complete.
