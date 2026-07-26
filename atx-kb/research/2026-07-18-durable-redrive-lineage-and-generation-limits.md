# Durable redrive lineage and generation limits

Date: 2026-07-18

## Reliability gap

Schema v18 bounds aggregate redrive velocity, but each successful replay is a new immutable event occurrence. If that occurrence fails, enters a DLQ, and is redriven again, neither the original root nor the number of replay generations is directly queryable. A poison event can therefore circulate indefinitely at an acceptable rate while fragmented identities hide the loop.

Amazon SQS explicitly treats a redriven item as a new message with a new message ID and enqueue time. Its ordinary redrive policy separately bounds delivery attempts before DLQ isolation. Spring for Apache Kafka publishes diagnostic headers for the original topic, partition, offset, timestamp, and consumer group when creating a dead-letter record. Together these establish two useful but distinct identities: a new occurrence for delivery correctness and durable original-message context for diagnosis.

Primary sources:

- Amazon SQS dead-letter redrive identity and velocity: https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-configure-dead-letter-queue-redrive.html
- Amazon SQS maximum receive count and poison-message isolation: https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-dead-letter-queues.html
- Spring for Apache Kafka dead-letter original-record headers: https://docs.spring.io/spring-kafka/api/org/springframework/kafka/support/KafkaHeaders.html

The generation ceiling below is an atx-db design inference from those primitives; the cited systems do not claim an equivalent cross-redrive counter.

## v19 contract

1. Every event exposes immutable `root_sequence` and `redrive_count`. An original event is its own root at generation zero. A redriven occurrence keeps the source root and increments the source generation exactly once.
2. A redrive still appends a new occurrence and retains the exact original-to-redriven mapping. Lineage supplements occurrence identity; it never rewrites or aliases event sequence numbers.
3. Each consumer has an immutable optional `max_redrive_count`. Zero preserves unlimited behavior. A positive value is the highest generation that consumer may publish.
4. Redrive admission evaluates the entire ack-all DLQ batch before budget charging or publication. If any source event is already at the configured ceiling, the whole request fails permanently and leaves the DLQ, budget, mappings, and event stream unchanged.
5. An exact retry of an already committed redrive token returns the original result even at the ceiling. A committed outcome is never reinterpreted or republished.
6. Schema-v18 migration initializes all existing events as generation-zero roots, then replays redrive mappings in target-sequence order to reconstruct every historical branch. Existing consumers receive unlimited policy.
7. Integrity verification requires each root to exist in the same workspace at generation zero; each positive-generation occurrence to have exactly one inbound redrive mapping; and each mapping to preserve root identity, increment generation by one, and retain the exact event envelope.
8. A unique redrive-target index prevents one occurrence from acquiring multiple parents. One source may still legitimately branch into distinct replay occurrences.

## Atomic-batch and policy boundary

The ceiling belongs to the publishing consumer, not globally to the root. Different consumers may intentionally use different replay policies while the event's generation remains portable. A mixed-generation DLQ batch is admitted only when every new occurrence would be within the consumer ceiling. This matches the existing all-or-nothing redrive and budget transaction.

## Test obligations

- original events report self-rooted generation zero;
- first and subsequent redrives retain one root and increment generation exactly;
- a bounded consumer rejects the next generation without stream, DLQ, mapping, or budget mutation;
- an exact committed token retry remains stable;
- backup/restore preserves lineage and the policy;
- schema-v18 migration reconstructs multi-generation mappings in sequence order;
- concurrent exact redrives publish one lineage occurrence;
- integrity rejects root drift, generation drift, missing or duplicate parents, and policy violations;
- CLI and JSON expose event lineage and the immutable consumer ceiling.
