# Atomic conditional consumer fleet reads

Date: 2026-07-18

## Review finding

Schema v23 gives a fleet response a complete cache identity:
`event_high_watermark` covers event growth, `consumer_state_revision` covers
persisted control state, and `next_dynamic_transition_at` covers mutation-free
lease and retry transitions. A supervisor still has to run the full event/DLQ
aggregation to learn that its cached fleet remains reusable. On large backlogs,
validation can therefore cost almost as much as retrieval.

Established control planes separate cache validation from representation
transfer:

- HTTP `If-None-Match` lets a client replay a validator and avoid transferring
  an unchanged selected representation. RFC 9110 requires weak comparison for
  cache validation and reserves 304 for GET/HEAD conditional success:
  https://www.rfc-editor.org/rfc/rfc9110.html#section-13.1.2
- Kubernetes collection `resourceVersion` identifies the snapshot used to
  construct a list and defines explicit exact/not-older-than semantics. Those
  historical semantics depend on retained watch-cache or etcd history:
  https://kubernetes.io/docs/reference/using-api/api-concepts/#resource-versions
- etcd assigns modifying operations increasing revisions and exposes retained
  historical range/watch access until compaction:
  https://etcd.io/docs/v3.5/learning/api_guarantees/
- SQLite keeps one coherent historical view only while the read transaction is
  open; a separate probe followed by a list would introduce a TOCTOU gap:
  https://www.sqlite.org/lang_transaction.html

ATX has no server-held cache, MVCC fleet history, or watch journal. It should
therefore provide equality-based cache validation, not claim exact historical
reads, not-older-than reads, or resumable watch semantics.

## Selected contract

Add a workspace-bound `EventConsumerFleetValidator` containing the three v23
markers, plus a conditional response with:

- `cache_valid`: whether the caller's prior authoritative snapshot remains a
  valid substitute;
- `validated_at`: the database observation time used for the decision;
- the current validator; and
- an optional full snapshot, present exactly when the cache is invalid.

`cache_valid` is more precise than `not_modified`: observation metadata changes
on every read, and a clock crossing can require a fresh snapshot even though no
durable row was modified. A future HTTP adapter may map cache-valid to 304 with
a weak ETag, but the embedded API should retain its domain semantics.

The validator is scoped to one workspace. Negative markers, a workspace
mismatch, or a noncanonical transition timestamp are `InvalidArgument`.
Well-formed stale or numerically future markers simply return a new snapshot;
the operation tests equality and does not request historical state.

## Atomic fast path

Both unconditional and conditional reads share one implementation and one
deferred SQLite transaction:

1. Read one `observed_at`, workspace HWM, bounded consumer count, and durable
   state revision. Enforce the 1,000-consumer completeness bound before any
   early return.
2. Only when both durable markers match, derive the authoritative current
   minimum transition directly from `event_consumers` using the same observation
   time and exact head-classification order.
3. Return cache-valid only when the caller's transition equals that value and
   the value is empty or strictly later than `observed_at`. Equality is already
   at the transition boundary and must rebuild the snapshot.
4. Otherwise continue inside the same transaction through the existing
   set-aggregated event/DLQ query and shared row decoder. The decoded minimum
   must equal the independently derived value whenever it was computed.

Recomputing the transition is a deliberate defense. Trusting a caller-supplied
empty or fabricated far-future value could reuse an expired in-flight snapshot
while HWM and revision remain unchanged. The authoritative query scans at most
1,000 small consumer rows and avoids the expensive event and DLQ relations.

The first metadata statement pins the snapshot. A concurrent writer is observed
wholly before or after its commit; a conditional response never combines old
markers with new detail rows. A writer may commit immediately after validation,
as with any snapshot read, but it cannot create internal response skew.

## Blocking evaluation

- Exact empty, idle, in-flight, and retry-backoff validators hit the fast path.
- Event-only HWM movement, control-only revision movement, and exact clock
  boundary crossing return a fresh full snapshot.
- Fabricated empty, past, and far-future transition values cannot validate an
  active lease.
- A deterministic access-path test drops the detailed query's required poll
  index: an exact cache hit must still succeed, while a forced miss must fail
  when preparing the indexed aggregation.
- A fixed large-backlog benchmark compares normalized cache-hit and forced-full
  reads and requires a material speedup without treating latency as the semantic
  oracle.
- Concurrent registration or receive versus conditional validation may return
  only a coherent pre-commit cache-valid result or a complete post-commit
  snapshot with matching nested markers.
- Empty, 1,000-consumer, and 1,001-consumer fleets preserve the completeness
  contract. Invalid validators and partial CLI options fail explicitly.
- Conditional JSON never contains delivery or request tokens. Unconditional
  `consumer-statuses` output remains byte-compatible.

## Release evidence

The release build passed all blocking gates on 2026-07-18:

- 75 of 75 `AgentDatabase` tests passed, including the new conditional-read
  race, validator truth-table, migration/restore, missing-revision corruption,
  access-path bypass, completeness-bound, and large-backlog cases.
- 105 of 105 combined `AgentDatabase`, `AgentDatabaseCli`, `KnowledgeBase`, and
  `KnowledgeBaseCli` tests passed.
- The fixed 100,000-visible-event microbenchmark measured a 51.615 us average
  cache validation, a 141,884.3 us average forced full read, and a 2,748.8966x
  normalized speedup. The semantic gate requires only a 5x improvement; the
  measured latency is evidence, not part of the API contract.
- The KB quality gate passed all 15 thresholds. The vector benchmark passed
  with ANN recall@10 of 1.0, distance-work reduction of 4.006848x, and warm
  latency speedup of 4.920994x.
- All three shared SHA-256 conformance tests passed, `clang-format --dry-run
  --Werror` reported no drift in the changed C++ files, and `git diff --check`
  reported no whitespace errors.
