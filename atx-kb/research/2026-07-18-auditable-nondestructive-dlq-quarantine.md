# Auditable non-destructive DLQ quarantine

Date: 2026-07-18

## Reliability gap

Schema v19 can stop a poison lineage at a durable replay-generation ceiling, but the rejected DLQ batch remains `open`. That is safe for evidence retention yet operationally incomplete: an operator cannot mark the item intentionally terminal, alerts cannot distinguish “awaiting decision” from “reviewed and retained,” and repeated redrive attempts remain possible.

Azure Service Bus retains a DLQ message until a client explicitly retrieves and completes it; the DLQ otherwise supports ordinary receive/settlement operations. Amazon SQS likewise requires explicit deletion after successful processing, while Google Pub/Sub removes work from a subscription only after acknowledgment and recommends a separate subscription for dead-letter analysis. These systems make terminal settlement explicit, but their ordinary completion/delete semantics remove the queue occurrence.

Primary sources:

- Azure Service Bus dead-letter queues: https://learn.microsoft.com/en-us/azure/service-bus-messaging/service-bus-dead-letter-queues
- Amazon SQS receive and delete semantics: https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/step-receive-delete-message.html
- Google Cloud Pub/Sub dead-letter topics: https://docs.cloud.google.com/pubsub/docs/handling-failures

Atx-db needs the explicit terminal decision without destroying immutable event, delivery, or DLQ evidence. The non-destructive quarantine overlay below is an atx-db inference from those settlement models.

## v20 contract

1. An operator may quarantine an `open` DLQ batch with a required operator identity, caller token, and bounded reason. Quarantine is terminal and makes the batch permanently ineligible for redrive.
2. The original event occurrences, delivery audit, checkpoint, DLQ row, and any lineage metadata remain immutable and queryable. Quarantine adds one append-only resolution record instead of deleting or rewriting evidence.
3. Listing a quarantined batch reports derived status `quarantined` plus the operator, token, reason, and canonical timestamp. The underlying schema-v19 DLQ state remains `open`, allowing migration without rebuilding a referenced table or weakening old constraints.
4. Exact retries of the same `(consumer, DLQ, operator, token, reason)` return the original result. Reusing a token for another request or changing any intent field fails.
5. Quarantine and redrive both use an immediate transaction. If redrive commits first, quarantine rejects the already-redriven batch. If quarantine commits first, redrive rejects before budget refill, charge, mapping, or event publication.
6. A redriven batch cannot later be quarantined; its committed replay is already the terminal DLQ outcome. A quarantined batch cannot be reopened.
7. Integrity verification proves one valid quarantine at most, exact workspace/consumer/DLQ ownership, open base status, absence of replay mappings and budget charges, canonical time, bounded identity/reason fields, and no cross-consumer attachment.
8. Schema-v19 migration creates the append-only quarantine table without changing any existing DLQ status or consumer behavior.

## State model

```text
                     redrive(token)
open DLQ ---------------------------------> redriven
   |
   +-- quarantine(operator,token,reason) -> quarantined

redriven   -- quarantine --> reject
quarantined -- redrive    --> reject before any mutation
```

## Test obligations

- quarantine preserves every original event and reports complete audit metadata;
- exact quarantine-token retries are stable while changed intent and token reuse reject;
- a quarantined DLQ cannot consume velocity budget or append replay occurrences;
- a redriven DLQ cannot be quarantined;
- concurrent quarantine versus redrive commits exactly one terminal outcome;
- backup/restore preserves terminal state and exact retries;
- schema-v19 migration is non-destructive;
- integrity rejects cross-consumer or malformed quarantine attachment;
- CLI and JSON expose the explicit terminal decision.
