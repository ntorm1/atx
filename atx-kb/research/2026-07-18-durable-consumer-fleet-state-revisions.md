# Durable consumer fleet state revisions

Date: 2026-07-18

## Review finding

Snapshot-consistent `consumer-statuses` has an authoritative inventory, but its
workspace event high-watermark is not a revision of all returned state. Lease
acquisition, renewal, retry rejection, settlement, and manual checkpointing can
change delivery health without appending an event. A supervisor that treats the
HWM as a collection cache validator can therefore retain stale fleet state.

Production control planes attach comprehensive mutation metadata to collection
reads:

- Kubernetes list/watch clients use collection `resourceVersion`; continued
  lists retain the initial snapshot version, and exact unavailable versions
  fail rather than silently mixing state:
  https://kubernetes.io/docs/reference/using-api/api-concepts/
- etcd's revision is a logical clock for committed key-space mutations. Range
  responses carry the revision used to reason about point-in-time state:
  https://etcd.io/docs/v3.5/learning/api_guarantees/
  https://etcd.io/docs/v3.6/learning/api/
- SQLite row triggers run inside the statement's transaction and are rolled
  back with it. `AFTER INSERT`, `AFTER UPDATE`, and `AFTER DELETE` triggers can
  maintain derived metadata even when a future code path writes a status table:
  https://www.sqlite.org/lang_createtrigger.html

ATX additionally exposes wall-clock-derived head states. An `in_flight` lease
becomes ready or cooling when its expiry passes, and cooling work becomes ready
when its retry time passes, without any database mutation. A durable revision
alone therefore cannot validate the complete dynamic response.

## Selected cache-validity contract

Return three independent collection markers from the same read snapshot:

1. `event_high_watermark` covers workspace event-feed growth and therefore new
   visible backlog.
2. `consumer_state_revision` covers every persisted row mutation that can alter
   consumer membership, cursor/policy/lease state, retained DLQ partitions, or
   the quarantine overlay.
3. `next_dynamic_transition_at` is the earliest future lease expiry or retry
   boundary implied by the returned statuses. It is empty when no returned
   state can change through time passage alone.

A cached fleet remains reusable only while both durable markers are unchanged
and wall time has not reached a nonempty `next_dynamic_transition_at`. The
event HWM remains explicitly feed-only; the new state revision remains
explicitly durable-state-only. Together with the time boundary they cover every
field in the exact status response.

## Durable revision model

Add a schema-v23 `event_consumer_state_revisions` table keyed by workspace. No
row means revision zero. Existing workspaces with consumers migrate to baseline
revision one; their current state becomes the first versioned snapshot without
inventing a historical mutation count.

Schema-owned triggers advance the workspace revision after each actual row
insert, update, or delete on:

- `event_consumers`, which owns membership, policy, cursor, redrive budget, and
  active delivery head state;
- `event_consumer_dead_letters`, which owns retained/open/redriven state and
  exact source-event counts;
- `event_consumer_dead_letter_quarantines`, which overlays the quarantined
  terminal partition.

Trigger maintenance is preferable to scattered C++ calls: idempotent reads and
`INSERT OR IGNORE` no-ops do not fire, failed transactions roll back their
revision increments, and future or administrative writes cannot silently omit
the marker. One API transaction may mutate more than one status-bearing row and
therefore advance more than once; clients rely on strict monotonic change, not
on a one-call/one-increment fiction. Event insertion does not advance this
revision because its effect is already covered exactly by the HWM.

The revision row stores a canonical last-update timestamp for diagnosis. An
integer overflow must abort the originating transaction rather than wrap or
reuse a revision. Backup and restore preserve the marker exactly.

## API and integrity obligations

- Point status returns the current workspace `consumer_state_revision`; fleet
  status binds one snapshot value into every nested status and returns it at
  collection level.
- Point status derives its own next transition from the classified head. Fleet
  returns the minimum nonempty transition across all decoded consumers.
- CLI JSON names the markers explicitly and continues to omit all capability
  tokens.
- Integrity verification requires all revision triggers, validates any current
  revision row and timestamp, and rejects a workspace containing consumers
  without a revision row.
- Registration, receive, renewal, rejection, settlement, manual checkpoint,
  terminal dead-letter, redrive, quarantine, and deletion mutations are
  covered. Poll/status/list and exact idempotent retries do not advance it.
- A successful event append changes HWM but not state revision. A successful
  lease renewal changes state revision but not HWM. These orthogonal cases are
  blocking tests.
- Concurrent writers commit strictly increasing final revisions, and one fleet
  read observes the entire pre- or post-transaction state with its matching
  marker.
- Schema-v22 migration, online backup, restored fleet reads, and all existing
  delivery/DLQ invariants remain green.

## Implementation and certification

ATX now publishes this contract in schema v23. Point and fleet status share the
same decoder and reject a nonempty consumer snapshot whose durable revision is
missing. Fleet aggregation binds one metadata revision into every ordered row
and computes the top-level dynamic transition as the lexicographic minimum of
canonical UTC timestamps. The CLI exposes both fields at collection and point
level while its secret-leakage gate continues to reject delivery tokens,
request tokens, and their values.

The implementation also corrected registration's start-sequence validation:
the high-watermark query is now constrained to the registering workspace. A
higher global SQLite event sequence belonging to another workspace can no
longer authorize an impossible local start cursor.

Blocking evidence:

- All 99 atx-kb, atx-db, and CLI compatibility tests pass. Revision-specific
  cases cover events-only HWM movement, control-only revision movement,
  lease-time transitions, exact retry no-ops, DLQ redrive and quarantine,
  workspace isolation, the workspace-local registration bound, concurrent
  receive/fleet snapshots, v22 migration, backup/restore, missing-marker
  rejection, and overflow rollback of the originating lease transaction.
- The v22 migration test is repeat-safe and preserves an active lease, HWM, and
  restored revision while baselining the workspace at revision one.
- All 15 deterministic retrieval-quality thresholds pass on 110 documents and
  41 queries. The 1,024-vector ANN gate retains recall@10 of 1.0,
  exact-distance work reduction of 4.006848x, and warm-latency speedup of
  4.525571x.
- The live coordination database migrated atomically at HWM 270 with all 14
  consumers intact, consumer-state revision 1, no dynamic transition, all nine
  triggers installed, SQLite quick-check `ok`, and ATX integrity verification
  green. The paired-backup manifest format remains unchanged; schema version
  and the coordination database digest bind the new durable table and triggers.
