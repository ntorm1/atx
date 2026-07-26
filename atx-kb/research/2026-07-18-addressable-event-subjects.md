# Addressable subjects for the agent event changefeed

Date: 2026-07-18

## Research basis

Primary references:

- CloudEvents 1.0 core specification:
  https://github.com/cloudevents/spec/blob/main/cloudevents/spec.md
- CloudEvents primer:
  https://github.com/cloudevents/spec/blob/main/cloudevents/primer.md
- OpenTelemetry CloudEvents attribute registry:
  https://opentelemetry.io/docs/specs/semconv/registry/attributes/cloudevents/
- OpenTelemetry event semantic conventions:
  https://opentelemetry.io/docs/specs/semconv/general/events/

CloudEvents separates an event's unique occurrence ID from its optional
`subject`, which identifies the affected object in the producer's source
context. Keeping subject in context metadata lets generic middleware filter and
route events without decoding domain payload data. OpenTelemetry preserves the
same `cloudevents.event_subject` meaning and recommends structured,
occurrence-specific event attributes.

atx-db already had a workspace producer scope, a globally durable sequence for
occurrence identity, an event type, a canonical creation time, and payload
data. The missing context was a stable address for the affected run, agent,
task, episode, or fact. Schema v9 adopts that subject distinction without
claiming that the embedded C++/SQLite API is a complete CloudEvents transport
binding or JSON wire envelope.

## Schema-v9 event contract

- `agent_events.subject` is indexed with `(workspace, subject, sequence)` when
  non-empty. New subjects use stable paths such as `runs/<id>`, `agents/<id>`,
  `tasks/<id>`, `episodes/<id>`, and `facts/<id>`.
- Run, agent, and task subjects derive from the relational IDs already supplied
  to the event insertion helper. Episode and fact events explicitly pass the
  newly inserted primary-row ID, in the same transaction as the domain write.
- Every event for one episode or fact shares its subject, so consumers can
  resolve `recorded`/`verified` or `put`/`verified` occurrences to one immutable
  domain row.
- Generic `append_event()` accepts an explicit subject. Its idempotency contract
  includes the subject, so a retry cannot silently redirect an existing event
  key to another object.
- `events_after()` accepts an optional subject filter while retaining the
  durable cursor and bounded limit. The CLI exposes `events --subject` and emits
  `subject` plus `created_at` in its JSON result.
- Local integrity validates UTF-8/size bounds and exact derived subjects for
  run, agent, and task events. It validates typed prefixes on new episode and
  fact subjects.
- The v8-to-v9 migration backfills only unambiguous historical `run.*`,
  `agent.*`, and `task.*` events. Historical episode and fact events did not
  retain their primary IDs in the envelope, so they remain subjectless rather
  than receiving a guessed identity.

## Regression evidence

All 49 focused SHA-256, atx-kb, atx-db, migration, backup, concurrency, and CLI
tests pass. Coverage proves derived run/agent/task subjects, exact
episode/fact subjects, custom subject idempotency, subject-only cursor queries,
one addressable event under eight concurrent fact retries, and conservative
schema-v8 backfill that leaves ambiguous episode history empty.

A disposable CLI round trip wrote fact 1, returned one total event and one
subject-filtered event at `facts/1`, and emitted its canonical creation time.

The deterministic retrieval gate remains green on 110 documents and 41
queries. The default ANN benchmark retains recall@10 of 1.0, 4.006848x
distance-work reduction, and 5.130942x warm-latency speedup.

## Remaining gates

- Add a documented CloudEvents JSON export adapter with explicit `specversion`,
  `id`, `source`, `type`, `subject`, `time`, `datacontenttype`, and data mapping;
  validate it against the official JSON schema before claiming compatibility.
- Add subject-prefix or event-type filters only with query-plan benchmarks that
  prove bounded polling at millions of events.
- Define retention/checkpoint semantics and consumer acknowledgements without
  weakening the append-only audit log or cursor replay contract.
- Add correlation and causation context after defining stable semantics across
  task transitions, episodes, facts, and externally appended events.
