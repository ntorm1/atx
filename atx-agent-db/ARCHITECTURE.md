# atx-db architecture

## Layer boundary

```text
atx-kb: sources -> observations -> chunks/claims/vectors/graph -> cited evidence
                                      ^
                                      | source_id + observation_id
atx-db: runs -> agents -> leased tasks -> episodes -> events
                    \-> bitemporal facts
```

Knowledge and coordination are deliberately separate databases. An episode is
the durable join: it points to the exact `atx-kb` observation used or produced
by an agent. This keeps scheduling mutations out of the immutable evidence
model and lets either database be backed up or inspected independently.

## Scheduler state machine

```text
queued --claim--> leased --complete--> completed
   ^                 |
   |                 +--fail/expiry (attempts remain)--> queued
   |                 +--fail/expiry (limit reached)----> failed
   |                                                       |
   |                         queued/leased descendants <---+
   |                                  |
   |                                  +------------------> cancelled
   +------ dependencies become completed
```

A claim holds an immediate SQLite write transaction across expired-lease
reclamation, eligibility selection, and the task update. Eligibility requires
an active run, active registered agent, remaining attempts, and every dependency
in `completed`. Priority is descending, with creation time and ID as stable
tie-breakers.

When a task becomes terminal through explicit failure or lease-expiry
reclamation, the same immediate transaction walks its dependency DAG and
cancels every queued or leased descendant. Each actual transition emits one
`task.cancelled` event and records the terminal root as its cause. Concurrent
failures of roots that share descendants serialize at SQLite's write boundary;
status-guarded updates keep cancellation events exactly once. New tasks cannot
declare an already failed or cancelled dependency. Schema-v4 migration applies
the same repair across all workspaces before exposing the database.

Each successful claim creates a random lease token and increments attempts and
revision. The token is a fencing token: renew/complete/fail predicates include
workspace, task, owner, token, status, and unexpired time. A stale process
therefore cannot overwrite a newer worker's outcome.

## Optimistic and idempotent boundaries

Agent heartbeats can include an expected revision for compare-and-swap updates.
Run creation, task creation, events, and episodes accept idempotency keys scoped
to a workspace. The first committed request wins and later retries resolve to
the original durable ID. Reusing a key with a different canonical request is an
error rather than a silent success. A worker that retries `claim_next()` recovers
its still-live lease and token without consuming another attempt; completion and
failure retries with the same transition token are also idempotent.

Events form an append-only changefeed. Consumers persist the last sequence they
have processed and call `events_after(sequence)`; re-reading a range is safe
when downstream actions use the event or domain idempotency key. Each new
domain event also carries an indexed subject such as `tasks/<id>`,
`episodes/<id>`, or `facts/<id>`. The sequence identifies the occurrence while
the subject identifies the affected object, matching the distinction used by
CloudEvents and allowing middleware to route without decoding payload data.
Workspace is the producer scope, event type and creation time are context, and
payload remains occurrence data. atx-db does not claim that its embedded API is
a complete CloudEvents transport binding.

Schema v9 backfills subjects only where old relational columns make the answer
unambiguous: `run.*`, `agent.*`, and `task.*` events. Historical episode and
fact events predate durable row IDs in the envelope and remain subjectless
rather than receiving a guessed identity. New episode/fact events store their
exact primary-row subject in the same transaction as the domain mutation.

## Durable event consumers

A schema-v10 consumer is a named, durable pull view over one workspace's event
sequence with an optional exact-subject filter. Registration fixes its filter
and start cursor. Polling reads after the persisted cursor but never advances
it; the caller checkpoints the last successfully processed event afterward.
This yields at-least-once delivery across process restart.

Checkpointing runs in `BEGIN IMMEDIATE` and requires the consumer's expected
revision, an actually visible event sequence, and a caller-generated token. The
cursor can only increase. The first competing token advances the revision;
other writers using the old revision are fenced. Retrying the winning token
with the exact request is idempotent, while token reuse with different intent
is rejected. Each successful transition appends a linked checkpoint record
containing the previous cursor, new cursor, request revision, result revision,
and token. Checkpoints are deliberately audited in a separate table instead of
publishing checkpoint events into the same stream and creating endless
self-consumption.

Schema v11 adds an explicit leased receive path for competing workers:

```text
persisted cursor --receive--> active batch lease --settle--> advanced cursor
                         |             |
                         |             +--renew by current owner/token
                         +--expiry--> same range, new token, attempt + 1
```

Receive selection and lease publication run in `BEGIN IMMEDIATE`, so one
connection creates the active receipt and rivals observe it as unavailable.
The caller supplies a receive request token for ambiguous-network retry; an
exact retry returns the same range, receipt, and original expiry without
silently extending ownership. Changed intent under that token is rejected.
After expiry, the next receiver gets the same ordered range with a fresh random
delivery token. The old token cannot renew or settle, even if its process later
wakes up. Settlement atomically advances the cursor, appends the checkpoint,
marks the receipt settled, and clears the active lease. Online backups retain
both active state and every delivery attempt.

`consumer-poll` deliberately remains a non-mutating diagnostic/read API. Manual
revision checkpoints support simple single-reader integrations but are rejected
while a leased batch exists. Delivery attempts and acknowledgements remain in
separate audit tables to avoid self-consumption. Integrity verification proves
active-head/audit agreement, batch/filter visibility, batch size, expiry
redelivery succession, and delivery-to-checkpoint linkage.

Schema v12 optionally bounds delivery attempts. `max_delivery_attempts=0` is
the migration-compatible unlimited policy; a positive immutable value caps the
number of receipt leases for one batch. When the final receipt expires, the
next receive transaction performs this transition before selecting later work:

```text
expired final receipt -> append reasoned DLQ batch -> checkpoint outcome=dead_lettered
                      -> advance cursor -> clear old fence -> optionally lease next batch
```

The DLQ stores the exact previous/through sequence range, event count, final
delivery token and attempt, reason, and time. Listing a DLQ reconstructs its
events from the immutable changefeed under the consumer's fixed filter.
Integrity verification distinguishes processed and dead-lettered cursor
outcomes, requires every expired receipt to have exactly one redelivery
successor or DLQ terminal, and proves DLQ range/count/reason consistency.

Consumer batches use ack-all semantics: terminal failure isolates every event
in that received range. A handler needing per-event isolation should request
one event at a time. Source events are never deleted. Automatic redrive is
excluded because it can recreate a poison loop; inspection followed by an
explicit decision preserves operator intent.

Schema v13 makes that decision durable and retry-safe. An operator supplies the
consumer, DLQ batch ID, and a redrive token. One immediate transaction appends
each original envelope as a new occurrence, records an exact one-to-one mapping,
and changes the retained DLQ record from `open` to `redriven`:

```text
open DLQ batch --redrive(token)--> append new occurrences -> persist sequence map
       |                                      -> status=redriven, retain originals
       +--same token retry--------------------> return exact existing map
       +--different token---------------------> reject conflict
```

The cursor remains monotonic; the new occurrences naturally appear after its
current high-water mark. `BEGIN IMMEDIATE`, a unique mapping for every original
sequence, and immutable token state make concurrent identical calls converge on
one publication. A subsequently failing copy creates a separate DLQ batch and
therefore requires a separate decision. Redrive preserves the original payload;
payload correction is a normal explicit event publish so the audit trail never
claims that modified content was the failed occurrence.

Schema v14 prevents retryable receipt expiry from becoming a hot loop. Consumer
registration may fix a base and maximum backoff in whole seconds; both zero
retain immediate redelivery. For expired attempt N the ordered head becomes
eligible at `expiry + min(base * 2^(N-1), maximum)`:

```text
active receipt --lease expiry--> durable cooldown --retry_not_before--> new fenced receipt
                                     |                                      attempt + 1
                                     +--receive before eligibility--> no mutation
terminal receipt --lease expiry-------------------------------------> DLQ immediately
```

The active consumer head and its delivery audit row store the same canonical
retry time. Renewal atomically moves both expiry and retry eligibility, so a
crash or online backup cannot shorten the policy. Cooldown calls do not consume
request tokens or create audit rows. At eligibility, `BEGIN IMMEDIATE` still
allows exactly one competing receiver to create the successor. Integrity checks
recompute the capped exponential delay for every historical attempt and prove
active-head/audit equality. The policy is for transient recovery, not scheduled
delivery; strict ordering means later matching events do not bypass a cooling
head batch.

Schema v15 adds an explicit negative-acknowledgment path for a worker that knows
processing failed before its lease deadline. Rejection requires the same
owner/receipt fence as renewal and settlement plus a caller-generated rejection
token and bounded reason:

```text
active receipt --reject(reason, token)--> lease ends now -> durable retry cooldown
       |                                       |             -> successor attempt
       |                                       +--final attempt--> DLQ + checkpoint
       +--same rejection token retry-------------------------> exact prior outcome
       +--stale owner/token or changed intent----------------> reject
```

The delivery audit retains the rejection token, reason, and canonical time.
Below the attempt limit, the receipt remains the ordered cooling head until a
receive after `retry_not_before` creates its single successor. Renewal and
settlement are fenced as soon as rejection commits. At the limit, rejection
atomically marks the receipt expired, writes
`max_delivery_attempts_rejected`, advances the cursor, and clears the head;
timeout exhaustion remains separately classified. The worker's detailed reason
stays on the delivery audit while the DLQ reason remains a stable machine code.
This makes ambiguous response retry safe without converting application failure
into a false acknowledgment.

Schema v16 makes rejection classification explicit. `retry` retains the v15
cooldown/attempt-limit behavior. `dead_letter` is a terminal application
settlement on any attempt, including when automatic delivery attempts are
unlimited:

```text
reject(retry, reason)       -> cooldown or max-attempt DLQ
reject(dead_letter, reason) -> immediate DLQ + checkpoint + cursor advance
```

Disposition joins delivery, owner, rejection token, and reason as immutable
idempotency intent; an ambiguous retry cannot change a transient decision into
a terminal one. Automatic timeout exhaustion, rejection exhaustion, and direct
application isolation retain distinct machine reasons. DLQ inspection also
returns the delivery's disposition and bounded worker reason, while the source
event and full delivery audit remain immutable. Explicitly dead-lettered work
uses the same v13 redrive path after operator inspection.

Schema v17 prevents a shared failure from synchronizing otherwise independent
consumers at the same deterministic retry cap. Registration fixes an immutable
`none` or `full` jitter policy. For attempt N, the exponential window is still
`min(base * 2^(N-1), maximum)`; `none` chooses the window and `full` samples a
whole-second delay uniformly from its inclusive zero-to-window range:

```text
create receipt -> compute capped window -> sample once -> persist delay + retry time
                                          |              |
                                          |              +-- backup/restart preserves both
                                          +-- renew/reject/exact retry reuse; never reroll
```

SQLite `randomblob(8)` supplies the sample inside the receipt transaction and
rejection sampling removes modulo bias. The selected delay is durable protocol
state, not a value reconstructed during replay. Both the active consumer head
and append-only delivery audit store it. Renewal shifts expiry and eligibility
by that same value, while a retry rejection moves the cooldown origin to the
rejection time without changing the delay. Integrity verification recomputes
every delivery's cap, proves deterministic equality for `none`, proves bounds
for `full`, checks the exact timestamp equation, and matches an active head to
its audit. Migration assigns v16 consumers `none` and backfills their historical
and active deterministic delays before publishing schema v17.

Schema v18 bounds aggregate DLQ recovery traffic. Registration can fix an
events-per-second refill rate and burst capacity; both zero retain unlimited
v17 behavior. The durable token bucket represents one event as 1,000 units, so
each elapsed millisecond earns exactly `rate` units without floating-point
drift:

```text
open DLQ batch -> refill durable bucket -> atomically admit whole batch
       |                    |                       |
       |                    | insufficient --------+--> no mutation + retry timing
       |                    + batch > burst ----------> configuration error
       + exact committed token retry ----------------> prior map, no new charge
```

A successful limited redrive commits the new event occurrences, mappings, DLQ
status, resulting bucket head, and one append-only budget charge in the same
immediate transaction. The charge records the DLQ/token identity, event count,
refilled balance, result balance, and effective refill watermark. Wall-clock
rollback earns no tokens and cannot move the watermark backward. Integrity
verification replays each consumer's charge chain from a full initial bucket,
recomputes every refill and deduction, proves the linked DLQ/map, and matches
the final durable head. SQLite's single-writer boundary makes competing
different-batch redrives share the same budget, while exact concurrent retries
of one committed token remain idempotent and charge-free. Because DLQ batches
are ack-all and redrive is atomic, burst capacity must cover the batch; limit-one
receives remain the fine-grained recovery option.

Schema v19 makes repeated replay identity explicit and bounds poison loops.
Every original event is its own `root_sequence` at `redrive_count=0`; each new
redrive occurrence retains that root and increments the source generation. A
unique inbound target mapping gives every positive-generation occurrence one
parent while allowing intentional branches from one source:

```text
root seq 41 (g0) --redrive--> seq 88 (root 41, g1)
       |                         |
       +--independent branch--> seq 93 (root 41, g1)
                                 +--redrive--> seq 121 (root 41, g2)
```

Registration may fix `max_redrive_count`; zero is unlimited. Before the v18
budget path, redrive checks the whole DLQ batch and rejects if publishing any
event would exceed the ceiling. Thus a generation failure cannot consume
tokens or partially publish an ack-all batch. A committed token is checked
first and always returns its original map. Integrity proves same-workspace
generation-zero roots, exactly one parent for every positive generation,
one-step generation increments, exact envelope preservation, and conformance
to the publishing consumer's immutable ceiling. Migration initializes every
v18 event as an original, replays mappings in target-sequence order, and leaves
existing consumers unlimited.

Schema v20 closes the operator lifecycle after isolation. Cloud queue systems
require an explicit completion, acknowledgment, or deletion to remove reviewed
DLQ work; atx-db needs the decision but must preserve its evidence. It therefore
adds one append-only quarantine overlay to an open schema-v19 DLQ rather than
updating or deleting the referenced row:

```text
open DLQ -- redrive(token) --------------------------> redriven
    |
    +-- quarantine(operator, token, bounded reason) -> quarantined
```

`quarantined` is a derived API state. The base DLQ remains `open`, its immutable
event range and delivery/checkpoint audit remain addressable, and the overlay
records who made the terminal decision, the idempotency token, reason, and UTC
time. Exact retries return that record. A different token or changed intent is
rejected, and neither terminal state can transition to the other. Both paths
start with `BEGIN IMMEDIATE`: if quarantine wins, redrive rejects before lineage
or velocity mutation; if redrive wins, quarantine rejects the committed replay.
Integrity proves overlay ownership, base-open state, absence of mappings and
charges, bounded metadata, canonical time, and cross-consumer isolation.
Migration from v19 creates only the overlay and its unique indexes.

Schema v21 turns that durable state machine into an addressable changefeed. Each
new transition writes its state and one lifecycle occurrence in the same
immediate transaction:

```text
DLQ create  + consumer.dead_lettered          --subject consumers/<name>--+
redrive     + consumer.dead_letter_redriven   --payload  decimal DLQ ID---+--> agent_events
quarantine  + consumer.dead_letter_quarantined                              |
       \-- immutable (consumer, DLQ, transition) -> event sequence mapping -/
```

The mapping is both the transactional outbox record and the deduplication
identity. A transition key and every non-null event sequence are unique; the
event is a self-rooted generation-zero occurrence. API and CLI dead-letter
records expose the three sequences, allowing a monitor to persist an ordinary
event cursor and resolve the exact DLQ without table rescans.

Migration does not synthesize occurrences for old state. It records a
per-workspace activation time and event high-watermark, then creates nullable
legacy mappings for every historical transition. A legacy mapping is returned
as sequence zero. A post-migration terminal decision on an old open DLQ still
gets a positive event sequence above the activation watermark. Integrity proves
state-to-mapping completeness, exact type/subject/payload and lineage for new
events, historical transition time at or before activation, cross-consumer
ownership, and the inverse absence of orphan lifecycle occurrences.

Schema v22 closes the control-event feedback loop for broad consumers. A
consumer applies the following additional predicate after its ordinary subject
filter:

```text
visible = NOT (
  event.sequence > consumer.self_control_event_cutoff_sequence
  AND event.type starts with "consumer."
  AND event.subject == "consumers/" + consumer.name
)
```

New consumers store cutoff zero. Existing consumers migrating from v21 store
their workspace's pre-migration event high-watermark, so historical deliveries,
checkpoints, and DLQs remain valid under the semantics that created them. The
migration appends no events. Poll and receive can return empty while a local
control event is the only newer occurrence; the cursor is not mutated. Once a
later visible event arrives, its checkpoint may jump monotonically over the
suppressed sequence.

The same predicate is used for polling, leased batch selection, exact delivery
and DLQ reconstruction, manual checkpoint validation, and integrity replay.
Public event reads remain complete, and a different monitor subscribing to
`consumers/<target>` sees every target control event. Thus suppression is local
to the state-changing consumer, not deletion or global filtering.

Operational lag uses that same visibility boundary. `consumer-status` takes one
read snapshot and reports the physical workspace event high-watermark alongside
the exact count, first sequence, last sequence, and oldest creation time of
events that this consumer could receive after its cursor. Subject-filter misses
and post-cutoff local controls do not inflate the visible backlog. Status is
derived and read-only; it creates no scan cursor, checkpoint, lease, or event,
and its exact count is deliberately absent from poll/receive hot paths.

The same snapshot exposes the durable ordered head without its fencing token:

```text
idle -> in_flight -> retry_backoff -> redelivery_ready -> in_flight
                    final expiry -----------------------> dead_letter_ready
```

Head count plus queued visible count equals the unacknowledged visible backlog.
Available count is zero while the head is leased or cooling, equals only the
head when redelivery is ready, and excludes a terminal head that the next
receive will isolate before proceeding to its queued tail. Owner, attempt,
through-sequence, expiry, and retry time make a stuck consumer diagnosable;
delivery and request tokens remain capability-bearing secrets returned only to
the owning receive workflow.

Schema v23 gives that derived status a composite cache identity. The event
high-watermark changes when the append-only workspace feed grows.
`consumer_state_revision` changes for committed consumer, DLQ, and quarantine
row mutations through schema-owned triggers. `next_dynamic_transition_at` is
the head expiry or retry boundary at which wall-clock passage can change the
classification without a write. Point reads return all three markers from one
snapshot. Existing v22 workspaces with consumers migrate to revision one;
unseen empty workspaces read as revision zero. Trigger increments participate
in the source transaction, so rollback removes them, compound mutations may
create valid gaps, and integer overflow aborts the source mutation.

The status snapshot also joins the retained DLQ and quarantine overlay. It
partitions every DLQ batch into actionable open, redriven, or quarantined
history, and reports the exact event count plus oldest time for open work. This
keeps a zero main backlog from masking poison batches that still require an
operator decision. No lifecycle event is emitted by observation; redrive and
quarantine remain explicit, idempotent state-changing commands.

Fleet discovery is an explicit collection read. `consumer-statuses` first
establishes a deferred SQLite read snapshot containing one canonical observation
time, workspace event high-watermark, and consumer count. Stable name
ordering and one set-based detailed-status aggregation then run inside that
same transaction. The complete collection is bounded at 1,000 consumers and
fails instead of truncating; scalable pagination would require a durable
snapshot identity across pages. This gives supervisors an authoritative
inventory without cross-consumer skew or an out-of-band name registry. Every
nested row repeats the fleet HWM and consumer-state revision, while the fleet
transition boundary is the minimum nonempty boundary across the decoded rows.
A client may reuse a cached fleet only if both durable markers still match and
current time precedes a nonempty boundary. This separates append-only feed
growth, persisted control-state changes, and mutation-free clock transitions
without pretending that any one marker covers all three. The collection reuses
the point-status secrecy boundary and never serializes delivery or request
tokens.

Conditional fleet validation uses that identity without trusting its time
component. `list_event_consumer_statuses_if_current` starts the same deferred
transaction and reads the same observation time, HWM, bounded count, and state
revision. When the durable markers match, a small consumer-only query derives
the authoritative minimum transition with the exact status classification
order. Cache reuse requires that value to equal the caller's value and be empty
or strictly later than the observation time. Equality is already across the
boundary. A fabricated empty or far-future value therefore falls through to
the full aggregation.

The cache-valid branch commits before the event/DLQ query is prepared. Every
miss continues through the existing set-based query and shared decoder inside
the same SQLite snapshot, and any independently computed transition must equal
the decoded minimum. The result is called `cache_valid`, rather than
`not_modified`, because observation metadata changes on each validation and
clock passage can invalidate reuse without a durable mutation. This is a
current-state equality validator, not retained MVCC history or a resumable
watch token.

Status aggregation has two disjoint visibility legs. Empty filters require
`agent_events_poll_idx(workspace,sequence)`; nonempty filters state the partial
index predicate explicitly and require
`agent_events_subject_idx(workspace,subject,sequence)`. Point status
materializes its exact candidate sets once. Fleet status aggregates both legs
and the consumer-scoped DLQ/quarantine relation in one ordered statement after
the metadata snapshot read, then decodes every row with the same invariant
validator. `INDEXED BY` turns access-path drift into a preparation failure
instead of an unbounded selective-filter latency regression.

Reserved lifecycle type validation is generation-aware. The generation-zero
`consumer.dead_lettered`, `consumer.dead_letter_redriven`, and
`consumer.dead_letter_quarantined` origins each require one lifecycle mapping.
If a monitor later redrives one of those controls, the positive-generation copy
keeps its type, target subject, and payload but represents replay transport, not
a second state transition. Its unique parent, preserved envelope, root, and
one-step generation increment are proved by the redrive lineage invariant. This
keeps replay legal without allowing callers to forge an unmapped lifecycle
origin.

The checkpoint means that processing completed from atx-db's perspective. It
cannot make an unrelated external side effect atomic; those consumers still
need an idempotent sink or a shared transactional system for exactly-once
effects. Integrity verification walks each checkpoint chain, checks monotonic
revision/cursor continuity, and proves every acknowledged sequence was visible
under the consumer's immutable filter.

SQLite foreign keys cannot cross schema boundaries, so coordination-to-knowledge
integrity is an application-level boundary. The strict episode, task-completion,
and fact APIs read the immutable atx-kb source, prove the exact observation
belongs to it, then commit `evidence_status=verified` with the source SHA-256.
Episodes also record a UTC certification time. Since sources and observations
cannot be deleted or rewritten, validation-before-write remains valid after the
two independent transactions. An idempotent strict episode retry can promote a
matching unverified row but never changes its identity or payload. Source-only
writes are retained as explicit unverified import paths. Schemas v5-v7 and
integrity checks enforce the metadata states; cross-store verification also
detects missing observations and certified hash drift.

## Backup and restored-copy verification

Both agent coordination and knowledge storage share atx-core's bounded SQLite
online-backup loop. It checks `sqlite3_backup_step()` directly, retries only
transient busy/locked results, and never mistakes `backup_finish()` success for
completion of an abandoned step sequence. atx-db writes to a sibling partial
file, reopens the snapshot, runs schema migration plus SQLite, foreign-key,
lease, DAG, run, and temporal invariants for the selected workspace, then
publishes the new destination name atomically without overwriting an existing
backup. The SQLite snapshot includes all workspaces even though verification is
scoped to the `AgentDatabase` instance.

WAL does not provide power-loss atomicity across multiple attached database
files. A knowledge/coordination recovery pair therefore uses monotonic reference
ordering instead of pretending it has a distributed transaction:

```text
snapshot atx-db -> snapshot append-only atx-kb -> verify every workspace link
       -> hash both files -> commit SQLite manifest -> externally anchor manifest hash
```

Because episodes point from atx-db to immutable atx-kb observations, the later
knowledge snapshot can contain a superset without breaking the earlier
coordination snapshot. `backup_pair()` enumerates all workspaces in the copied
file, verifies their task/fact invariants and exact evidence links, records
event, episode, and observation high-water marks, and stores relative filenames
plus streaming SHA-256 digests in `atx-backup-pair-v1`. The manifest is the
recovery commit point; unmanifested files are incomplete attempts. Verification
checks an optional externally pinned manifest digest, rejects path traversal,
rehashes both databases, checks watermarks and every domain invariant, then
rehashes again to prove that verification did not mutate either snapshot.

## Bitemporal facts

For one `(workspace, subject, predicate)`, `put_fact` closes the prior
transaction-time version, preserves any non-overlapping valid-time segments,
and inserts a new active version with a supersession pointer. Valid time
represents when a fact applies in the modeled world; transaction time represents
when atx-db knew that version. Every write also advances a workspace-local
integer sequence, avoiding ambiguity when several transitions share one wall-
clock millisecond. `facts_as_of_sequence()` is the exact audit API;
`facts_as_of()` remains a descriptive UTC-time view. Both intersect half-open
valid and transaction intervals without erasing corrections.

`put_verified_fact()` additionally requires the exact atx-kb observation at
write time and stores the immutable source hash. When replacement valid time
splits an earlier assertion, every preserved segment inherits that assertion's
source, observation, hash, and certification state. This makes provenance a
property of each fact version rather than mutable metadata on the predicate.

Fact creation also accepts a workspace-scoped caller idempotency key. The
`BEGIN IMMEDIATE` transaction checks that key before advancing the temporal
clock or closing any interval, then records the key on the primary fact version
in the same commit as every split and event. A matching retry returns that
original version even after it has become historical; a changed subject,
predicate, object, requested valid interval, evidence tuple, or confidence is a
request conflict. Internal interval fragments never receive caller keys. A
partial unique index and SQLite's single-writer boundary make concurrent
identical requests converge on one transition.

## Current boundary and next gates

SQLite serializes writers, which is appropriate for an embedded agent team and
provides simple, strong claim semantics. Production qualification still needs
long-running crash/restart tests, repeated lease-expiry fault injection,
backup/restore drills, event retention policy, ACL/encryption integration, and
throughput characterization under hundreds of processes.
Horizontal distributed scheduling is not claimed; a future service layer can
preserve task tokens, revisions, events, episodes, and temporal facts as the
wire contract if one SQLite writer becomes the measured bottleneck.
