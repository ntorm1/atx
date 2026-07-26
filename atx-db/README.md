# atx-db

`atx-db` is the durable control plane for multi-agent work. It complements
`atx-kb` rather than replacing it: `atx-kb` stores immutable evidence and
retrieval artifacts, while `atx-db` stores who is working on what, which tasks
are eligible, lease ownership, retries, event history, evidence-linked episodes,
and time-versioned facts.

The implementation is embedded C++20 over the SQLite library already vendored
by `atx-core`; it adds no service or package dependency.

## Guarantees

- Workspace scoping is present on every durable record.
- Runs and task creation support caller idempotency keys.
- Dependencies gate task eligibility. A terminal failure or exhausted lease
  atomically cancels every queued or leased descendant in the task DAG, so
  unreachable work cannot remain silently blocked.
- `BEGIN IMMEDIATE` serializes claim selection and lease mutation, preventing
  two processes from leasing the same task.
- Lease tokens fence stale workers; completion, failure, and renewal reject an
  expired or replaced token.
- Failed work is requeued until `max_attempts`, then becomes terminal.
- Events are append-only, ordered by a durable sequence, pollable by cursor,
  optionally idempotent, and carry an indexed domain subject for routing and
  exact row resolution.
- Named pull consumers persist an exact-subject filter and monotonic cursor.
  Checkpoints use revision CAS plus caller tokens, survive restart, fence
  competing writers, and retain an append-only audit chain outside the stream.
- Leased receives atomically hide one ordered batch from competing workers,
  return a renewable receipt/fencing token, replay exact active-request retries,
  and redeliver the same range with a fresh token after expiry.
- Optional capped exponential retry backoff persists an exact eligibility time
  with each receipt, survives restart/backup, and prevents expired work from
  hot-looping while terminal attempts still enter the DLQ immediately.
- Optional full jitter samples once across the capped retry window and persists
  that exact delay, spreading independent consumers without making renewal,
  rejection, restart, or replay nondeterministic.
- A fenced, caller-token-idempotent rejection ends a failed lease immediately,
  records the worker's reason, applies the same retry schedule, and atomically
  DLQs a rejected final attempt without requiring another receive call.
- Rejection disposition distinguishes retryable failure from explicit
  application dead-lettering, so known-invalid work can be terminally isolated
  on its first attempt even under an unlimited automatic retry policy.
- Optional maximum delivery attempts isolate an expired poison batch in an
  inspectable dead-letter queue, record a distinct cursor outcome, and let the
  consumer continue without deleting the immutable source events.
- Explicit dead-letter redrive appends new event occurrences without rewinding
  the cursor, retains an exact original-to-redriven sequence map, and uses a
  caller token so ambiguous or concurrent retries publish the batch once.
- Optional redrive velocity control uses a durable per-consumer token bucket and
  append-only charge audit, preventing recovery replay from overwhelming a
  downstream consumer while exact redrive retries remain charge-free.
- Every event occurrence carries a durable root sequence and redrive generation.
  An optional per-consumer generation ceiling rejects an entire poison replay
  before budget charging or publication while retaining exact-token outcomes.
- An operator-token-idempotent quarantine makes a reviewed open DLQ terminal
  without deleting its events, delivery audit, checkpoint, or DLQ evidence;
  quarantine and redrive serialize to exactly one outcome.
- Every new dead-letter, redrive, and quarantine transition atomically appends
  one addressable `consumers/<name>` changefeed occurrence. DLQ JSON exposes the
  exact event sequence, while migrated history is explicitly reported as zero.
- Named consumers suppress their own post-cutoff `consumer.*` control events,
  preventing broad subscriptions from feeding DLQ notifications back into the
  failing consumer while separate monitors retain complete visibility.
- Episodes idempotently link an agent/run/task to an `atx-kb` source and exact
  source observation.
- Verified episodes additionally certify the immutable source SHA-256 and
  verification time at write time; imports remain explicitly `unverified`.
- Strict task completion certifies the result source, exact observation, and
  immutable source SHA-256 while retaining a source-only unverified import path.
- Facts carry valid-time segments, descriptive UTC transaction times, and a
  strictly monotonic workspace sequence so callers can recover what was true
  and exactly what the system knew after any committed fact transition.
- Verified facts bind each assertion to an exact knowledge observation and
  source hash; interval splits preserve the original assertion's certification.
- Caller-keyed fact writes are retry-safe: identical retries return the original
  fact version, conflicting intent is rejected, and concurrent retries commit
  one temporal transition and one event pair.
- Integrity verification checks SQLite/FK health, task and consumer lease state,
  delivery/checkpoint audit chains, attempt limits, cross-run dependency
  invariants, and live tasks blocked by terminal parents.
- Schema v23 migrates every workspace in a database. v4 repairs descendants left
  blocked by terminal dependencies; v5 certifies episode evidence, v6 certifies
  task results, v7 certifies temporal facts, and v8 adds durable fact-request
  idempotency. v9 adds addressable event subjects and conservatively backfills
  only unambiguous historical run, agent, and task events. v10 adds durable
  consumer cursors and checkpoint history; v11 adds fenced delivery leases and
  expiry-based redelivery; v12 adds bounded attempts and dead-letter isolation;
  v13 adds durable, idempotent dead-letter redrive mappings; v14 adds persisted
  capped exponential retry schedules; v15 adds durable negative acknowledgments;
  v16 adds explicit retry/dead-letter dispositions and inspectable failure reasons;
  v17 adds persisted full-jitter retry delays; v18 adds durable DLQ redrive
  velocity budgets and charge history; v19 reconstructs durable replay lineage
  and adds immutable redrive-generation limits; v20 adds append-only terminal
  DLQ quarantine audits; v21 adds exact-once, consumer-addressable DLQ lifecycle
  occurrences without replaying historical transitions during migration; v22
  adds migration-safe self-control-event suppression; and v23 adds a durable
  consumer-state revision plus an explicit clock-driven transition boundary.
- Online backups include every workspace, retry transient SQLite contention,
  verify a restored copy, and atomically publish only to a new destination.
- Paired backups snapshot coordination before append-only knowledge, verify
  exact episode observations across every restored workspace, and publish a
  digest/watermark manifest as the pair's recovery commit point.

## CLI workflow

```powershell
$db = "data/agents.sqlite"
$workspace = "my-project"

$run = build/bin/atx-db run-create $db "Ship the retrieval evaluation gate" `
  --workspace $workspace --key release-eval | ConvertFrom-Json

build/bin/atx-db agent-register $db $run.id evaluator evaluation `
  --capabilities "retrieval,metrics" --workspace $workspace

$task = build/bin/atx-db task-add $db $run.id "Run golden queries" `
  --priority 100 --key golden-run --workspace $workspace | ConvertFrom-Json

$lease = build/bin/atx-db task-claim $db evaluator --lease-seconds 900 `
  --workspace $workspace | ConvertFrom-Json

# The lease token is a fencing token. Normal task results use strict completion
# against an exact observation; only the current token holder may finish.
build/bin/atx-db task-complete-verified $db data/research.sqlite $lease.id `
  evaluator $lease.lease_token src_verified_report 42 --workspace $workspace

build/bin/atx-db events $db --after 0 --workspace $workspace
build/bin/atx-db events $db --after 0 --subject "tasks/$($lease.id)" `
  --workspace $workspace

$consumer = build/bin/atx-db consumer-register $db task-projector `
  --subject "tasks/$($lease.id)" --max-deliveries 5 `
  --retry-backoff-seconds 2 --retry-backoff-max-seconds 60 `
  --retry-jitter full `
  --redrive-rate-per-second 10 --redrive-burst-events 100 --max-redrives 3 `
  --workspace $workspace | ConvertFrom-Json
$status = build/bin/atx-db consumer-status $db task-projector `
  --workspace $workspace | ConvertFrom-Json
$fleet = build/bin/atx-db consumer-statuses $db `
  --workspace $workspace | ConvertFrom-Json
$delivery = build/bin/atx-db consumer-receive $db task-projector evaluator `
  receive-0001 --lease-seconds 300 --limit 100 `
  --workspace $workspace | ConvertFrom-Json
if ($delivery.events.Count -gt 0) {
  # Long-running handlers can renew with the same owner and delivery token.
  build/bin/atx-db consumer-renew $db task-projector evaluator `
    $delivery.delivery_token --lease-seconds 300 --workspace $workspace
  build/bin/atx-db consumer-settle $db task-projector evaluator `
    $delivery.delivery_token checkpoint-0001 `
    --workspace $workspace

  # On processing failure, reject instead of waiting for lease expiry. This is
  # mutually exclusive with settlement for the fenced receipt.
  # build/bin/atx-db consumer-reject $db task-projector evaluator `
  #   $delivery.delivery_token rejection-0001 retry "dependency unavailable" `
  #   --workspace $workspace
}

# Dead letters retain exact event ranges and the terminal delivery reason.
$deadLetters = build/bin/atx-db consumer-dead-letters $db task-projector `
  --limit 100 --workspace $workspace | ConvertFrom-Json

# After inspection, redrive is explicit and caller-token idempotent. It appends
# new occurrences and leaves both the original events and DLQ record intact.
if ($deadLetters.Count -gt 0) {
  build/bin/atx-db consumer-redrive $db task-projector `
    $deadLetters[0].id redrive-0001 --workspace $workspace
}

# Reviewed poison work can instead become terminal without deleting evidence.
if ($deadLetters.Count -gt 1) {
  build/bin/atx-db consumer-quarantine $db task-projector `
    $deadLetters[1].id evaluator quarantine-0001 "reviewed invalid input" `
    --workspace $workspace
}

# Strict episode recording checks the exact source and observation in atx-kb.
build/bin/atx-db episode-record-verified $db data/research.sqlite episode-key `
  $run.id evaluator src_verified_report 42 --workspace $workspace

# Temporal assertions can be certified at the same evidence boundary.
build/bin/atx-db fact-put-verified $db data/research.sqlite atx-kb release_state `
  production-candidate src_verified_report 42 `
  --valid-from 2026-07-18T00:00:00.000Z --key release-state-2026-07-18 `
  --workspace $workspace

build/bin/atx-db verify $db --workspace $workspace
build/bin/atx-db backup $db data/agents-2026-07-18.sqlite --workspace $workspace

$pair = build/bin/atx-db backup-pair $db data/research.sqlite `
  data/recovery/atx-2026-07-18 --workspace $workspace | ConvertFrom-Json
build/bin/atx-db backup-pair-verify $pair.manifest_path `
  --sha256 $pair.manifest_sha256
```

One `AgentDatabase` object owns one SQLite connection and stays on one thread.
Concurrent workers open independent connections to the same file. WAL, foreign
keys, a busy timeout, and full synchronous durability are configured
automatically.

Backup writes a sibling `.partial` database through SQLite's WAL-aware online
backup API, reopens it through the current schema and workspace integrity gates, and
checkpoints the verifier WAL before publishing it with an atomic no-overwrite
hard link. The selected workspace is the domain-verification scope, while the
physical snapshot contains all workspaces in the source file.

`backup-pair` does not claim cross-file WAL atomicity. It first snapshots
atx-db, then the append-only atx-kb store, and verifies all workspaces' episode
links against the later knowledge snapshot. It publishes a small SQLite
manifest only after both file SHA-256 digests, domain integrity checks, and
event/episode/observation watermarks are stable. The returned manifest SHA-256
is the external trust anchor: sign it or store it separately, then pass it to
`backup-pair-verify`. Without an external anchor, digests detect accidental
corruption but cannot authenticate against an attacker who can rewrite the
entire recovery set.

SQLite foreign keys cannot cross database/schema boundaries. Use
the strict episode, task-completion, and fact APIs for normal agent work:
it validates the target source and exact observation before committing the
reference and stores the source content hash. Episode certification also safely
upgrades an idempotent unverified import. The target knowledge records are
immutable, so validation cannot be invalidated by a later deletion. Plain
source-only writes remain available for imports and disaster recovery and mark
their evidence `unverified`.

Event JSON exposes `sequence`, `type`, `subject`, occurrence time through the
API, relationship IDs, and payload. Subjects use stable paths such as
`runs/<id>`, `tasks/<id>`, `episodes/<id>`, and `facts/<id>` and are indexed with
the event cursor. This adopts CloudEvents' subject distinction for filtering;
the embedded row/API is not presented as a complete CloudEvents wire envelope.

Durable consumers provide caller-driven, at-least-once processing.
`consumer-poll` is a non-mutating diagnostic/read operation. For competing
workers, `consumer-receive` atomically leases one batch and returns a receipt
token. The same receive request token replays that exact active delivery without
extending it; other workers are fenced until settlement or expiry. Expiry
redelivers the same range with a fresh token and incremented attempt. Only the
current owner/token may renew or settle. Manual revision checkpoints remain
available for single-reader integrations but cannot bypass an active lease.
Registration can fix `--retry-backoff-seconds B` and
`--retry-backoff-max-seconds M`. The optional immutable `--retry-jitter full`
policy samples a whole-second delay uniformly from zero through
`min(B * 2^(N-1), M)` for attempt N; the default `none` uses that cap exactly.
Both backoff values default to zero for immediate schema-compatible redelivery.
The sample is chosen once when the receipt is created and returned as
`retry_delay_seconds`. Before the persisted
`retry_not_before`, receive attempts are rejected without changing the receipt,
attempt count, or cursor. Renewal moves expiry and retry eligibility together.
It reuses the stored delay rather than sampling again; rejection, exact request
retry, backup, and restore do the same.
`consumer-reject` lets the current owner end an unexpired receipt immediately,
preserving a bounded reason and caller rejection token in the delivery audit.
Exact rejection-token retries return the original result; changed intent is
rejected. Its disposition is `retry` or `dead-letter` and is also immutable
idempotency intent. A retryable rejection starts the same capped cooldown from
rejection time and fences renew/settle. Rejecting the configured final attempt
atomically creates the DLQ/checkpoint and advances the cursor, with
`max_delivery_attempts_rejected` distinguished from timeout exhaustion.
`dead-letter` performs that terminal transition on any attempt and exposes the
worker description alongside machine reason `explicit_rejection` in DLQ output.
With `--max-deliveries N`, expiry of attempt N atomically moves the ack-all
batch into the consumer's DLQ, records a `dead_lettered` cursor transition, and
allows the same receive call to lease later work without waiting for retry
backoff. Use a receive limit of one
when failures must be isolated per event. `consumer-dead-letters` reconstructs
the exact immutable events for inspection. `consumer-redrive` explicitly
appends the original envelopes as new stream occurrences, records an exact
original-to-new sequence map, and marks the retained DLQ record `redriven`.
The operation never rewinds the cursor. Exact redrive-token retries return the
same map; token conflicts are rejected, including under concurrent calls.
Registration may also set `--redrive-rate-per-second R` and
`--redrive-burst-events B`; both default to zero for unlimited v17-compatible
behavior. A limited consumer starts with B tokens, refills continuously at R
events per second using exact integer-millisecond accounting, and charges one
token per republished event. Budget refill, charge audit, new occurrences,
sequence map, and DLQ state commit atomically. Exhaustion is a non-mutating
temporary failure with retry timing, while a DLQ batch larger than B is a
permanent configuration mismatch because ack-all redrive remains atomic. Exact
retries of an already committed redrive token return its original map and charge
metadata without spending again. Backup/restore retains the balance and refill
watermark, and integrity replay proves every charge and the current bucket head.
Each event JSON record also exposes `root_sequence` and `redrive_count`: an
original occurrence is its own generation-zero root, and every redrive keeps the
root while incrementing the source generation. `--max-redrives N` fixes the
highest generation that consumer may publish; zero is unlimited. A batch with
any event already at N is rejected atomically before refill, charge, mapping, or
publication. Exact retries of previously committed tokens still return their
original occurrences. Schema-v18 migration rebuilds historical generations by
replaying the immutable sequence map in target order.
`consumer-quarantine` provides the other explicit terminal outcome for an open
DLQ. It requires an operator, caller token, and reason, and reports derived
status `quarantined` with immutable audit metadata. The underlying DLQ row and
all source events remain intact. Exact token retries return the original audit;
changed intent, later redrive, reopening, and quarantine of an already-redriven
batch are rejected. Redrive and quarantine both use an immediate transaction,
so concurrent decisions commit exactly one terminal outcome. Schema-v19
migration only adds the append-only overlay table and changes no existing DLQ.
Corrected content must be published as an explicit new event instead of being
silently substituted during redrive.

Schema v21 makes each new DLQ lifecycle transition directly observable without
rescanning internal tables. Dead-letter creation, redrive, and quarantine append
`consumer.dead_lettered`, `consumer.dead_letter_redriven`, and
`consumer.dead_letter_quarantined`, respectively, with subject
`consumers/<name>` and the decimal DLQ ID as payload. State, event, and immutable
transition-to-sequence mapping share one immediate transaction. Exact request
retries therefore return the same sequence and cannot publish twice. The three
`*_event_sequence` fields in dead-letter JSON expose these identities.

Migration from v20 appends no events and does not move any cursor or event
high-watermark. It records historical transitions as legacy mappings, reported
with sequence zero, plus a workspace activation watermark. Any later redrive or
quarantine of a historical open DLQ receives a normal positive sequence.
Integrity checks complete lifecycle coverage, the activation boundary, event
type/subject/payload, generation-zero lineage, and the inverse guarantee that no
lifecycle occurrence is orphaned.

Schema v22 prevents the newly addressable controls from becoming work for the
same failing consumer. Poll, leased receive, manual checkpoint, delivery/DLQ
reconstruction, and integrity all apply one visibility predicate: after the
consumer's immutable `self_control_event_cutoff_sequence`, an event whose type
starts with `consumer.` and whose subject is `consumers/<that name>` is local
echo and is skipped. Other consumers' controls and all ordinary events remain
visible; direct `events` queries are unchanged, so a dedicated monitor can use
the target consumer subject normally.

New consumers use cutoff zero and therefore do not receive even their own
registration event. Migration from v21 sets each existing consumer's cutoff to
its workspace event high-watermark without appending an event. Historical
checkpoints and DLQs retain their old visibility, while future local controls
are suppressed. A later visible event may advance the monotonic cursor over a
suppressed gap. Consumer JSON exposes the cutoff so this compatibility boundary
is inspectable and survives backup/restore.

`consumer-status` reports visibility-aware backlog without mutating that cursor.
Its `event_high_watermark` is the physical workspace tail, while
`pending_visible_event_count`, the first and last pending visible sequences,
and `oldest_pending_visible_event_at` use the exact same subject and local-echo
predicate as delivery. A physical sequence gap containing only unrelated or
suppressed events therefore reports zero pending work. The exact count is kept
off the poll and receive hot paths and is recomputed from durable state after
backup/restore.

Status also partitions unacknowledged work around the single ordered delivery
head. `delivery_head_state` is `idle`, `in_flight`, `retry_backoff`,
`redelivery_ready`, or `dead_letter_ready`; non-idle status includes its owner,
attempt, through-sequence, exact visible count, expiry, and retry time, but
never exposes either fencing token. `queued_visible_event_count` is the tail
behind that head, and `available_visible_event_count` applies the strict-order
rules: all pending work while idle, none while leased or cooling, the head when
redelivery-ready, and the queued tail when the terminal head will be isolated
by the next receive.

Every point status includes the workspace `consumer_state_revision` and its own
`next_dynamic_transition_at`. Schema-owned triggers advance the revision for
every committed insert, update, or delete in consumer, DLQ, or quarantine state;
failed and idempotent no-op transactions do not advance it. The transition is
the active head's lease expiry or retry boundary, and is empty when passage of
time alone cannot change that status. Event insertion intentionally advances
the event HWM, not this state revision.

Delivery health and terminal-work health share the same snapshot. Status
reports every retained DLQ batch partitioned into actionable open, redriven,
and quarantined counts. Open work additionally reports its exact source-event
count and oldest creation time. A consumer with zero pending delivery events
but a nonzero `open_dead_letter_count` is caught up, not healthy; redriven and
quarantined counts remain visible as resolved audit history.

`consumer-statuses` discovers the complete workspace fleet in stable name
order. It returns the workspace, one canonical `observed_at`, workspace event
high-watermark, consumer count, and the full point-status payload for every
consumer from one deferred SQLite read transaction. The collection fails with
`OutOfRange` above 1,000 consumers rather than returning a partial fleet that
looks complete. Every nested status repeats the collection timestamp, event
high-watermark, and consumer-state revision, making snapshot agreement directly
verifiable by clients. The collection's `next_dynamic_transition_at` is the
minimum nonempty transition across its consumers.

A cached fleet is reusable only while both durable markers are unchanged and
wall time is before a nonempty transition boundary. The HWM covers event-feed
growth; the consumer-state revision covers persisted membership, cursor,
policy, lease, DLQ, and quarantine mutations; and the time boundary covers
derived lease/backoff state changes that require no write. A v22 workspace with
consumers migrates atomically to baseline revision one, while an empty workspace
has revision zero. Revision gaps are valid because one transaction can change
several status-bearing rows. Online backup preserves the revision exactly.
Collection JSON preserves point status's capability boundary and never exposes
delivery or request tokens.

`consumer-statuses-if-current` validates a prior fleet without automatically
rerunning its event/DLQ aggregation. The validator is workspace-scoped and
contains the prior HWM, consumer-state revision, and optional next transition.
Inside one deferred read transaction, ATX enforces the fleet bound and compares
the durable markers. On a durable match it independently derives the current
minimum transition from the bounded consumer rows using the same observation
time. Only an exact three-marker match before the transition returns
`cache_valid:true` with safe metadata and no snapshot. A stale, future, or
fabricated-but-well-formed validator returns `cache_valid:false` plus a complete
current `snapshot`; negative markers, another workspace, or a malformed UTC
timestamp are rejected. This equality check does not claim historical
revision, pagination, or watch semantics.

Exact visibility evaluation is index-aware. Broad consumers use the workspace
sequence index, while nonempty subject filters use the partial subject/sequence
index through mutually exclusive branches. Point status materializes each
candidate event and DLQ set once. Fleet status performs one ordered grouped
query after its bounded metadata read, shares the point-status row validator,
and joins the minimum pending sequence back to its exact event timestamp. The
named indexes are preparation-time requirements, preventing a predicate or
migration change from silently restoring workspace-tail scans for selective
consumers.

Lifecycle event types are reserved for mapped generation-zero transition
origins. A monitor may nevertheless dead-letter and redrive one of those
controls: its positive-generation copy preserves the lifecycle envelope but is
a replay, not another DLQ transition. Integrity therefore requires the origin's
unique lifecycle mapping only at generation zero and proves every replay through
the ordinary one-parent redrive lineage. An unmapped generation-zero lifecycle
type remains invalid.

Exactly-once effects outside this SQLite database still require an idempotent
downstream operation or a shared transaction boundary.

## Build and test

```powershell
cmake --build build --target atx-db-cli atx-db-tests
ctest --test-dir build -R AgentDatabase --output-on-failure
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for the state model and current scale
boundary.
