FP1 independent static review, 2026-09-20

Reviewed the stable six-path `forward-publication-draft/forward-publication.patch`
and its full source copies against live `delisting.py`, `_bulk_publication.py`,
`connection.py`, migrations 0186/0187, the existing SQL arithmetic tests, and the
existing publication tests. The brief and implementer report were also read.
AP1 and DP1 were not reviewed. Graph tools were unavailable in this reviewer
session, so discovery used static file reads, rg, and git diff.

**Critical: none found. Important: none found. Minor: none found.**

No corrective patch is requested from this static pass. This is acceptance of
the inspected design and source, conditional on root's pending integration and
focused runtime checks; it is not a test pass or a production capacity result.

The following observations support the result. Paths and line numbers below
refer to the full source copies under `forward-publication-draft/` unless marked
live.

- `atx-db/src/atx_db/delisting.py:2170` retains option validation, UTC cutoff
  normalization, initialization, the configured `IC_HORIZONS`, and the existing
  calendar identifiers. Its only executable change is delegation to the new
  helper. The existing public signature and other transforms are unchanged.
- `_forward_return_publication.py:251` and the subsequent SQL constants retain
  the existing price revision ordering, positive/finite filtering, cutoff-before-
  revision selection, exact calendar endpoints, earliest selected terminal,
  invalid-terminal boundary behavior, strict preterminal ASOF price, stitched
  arithmetic, availability clocks, deterministic IDs, and final finite-value
  filter. The new formation number is applied after the same price selection.
  Endpoint bars are restricted by security range, not by formation-date range,
  so a security split across batches retains its full endpoint history.
- `_forward_return_publication.py:200` caps each result INSERT at one horizon
  over a bounded formation range. The selected bars, calendar dates, and
  terminal rows are unique on their join keys, so these joins do not expand one
  formation into multiple result rows. Each statement commits independently
  under the documented no-outer-transaction/sole-writer contract.
- `_forward_return_publication.py:101` clones the live catalog declaration for
  the constrained shadow, including CHECK clauses as well as the primary key,
  NOT NULL clauses, types, and defaults. The keyset sub-batches and ordered
  prefixes bound successful indexed inserts. The open first and last ranges
  retain arbitrary foreign-source IDs, including the empty string. Foreign
  rows copy every physical column; target rows receive one shared refresh
  timestamp for the two ingestion fields. Primary-key collisions still fail.
- `_forward_return_publication.py:236` checks total and target-source counts
  before calling the existing single-table `publish_validated_shadow` API.
  Source replacement occurs only at the final swap. Empty target output still
  copies foreign rows and publishes their complete constrained table. The live
  helper validates columns/defaults/keys and performs both renames and the old
  table drop within `store.transaction()`; FP1 neither replaces that API nor
  depends on the separate AP1 proposal.
- `_forward_return_publication.py:34` limits recycling to persistent files with
  both recorded analytical settings. It rejects the listed incompatible
  temporary objects/functions and attachments before allocating stages.
  `checkpoint()` reacquires `store.con` through `store.reopen()` and restores
  the prior insertion-order setting. The inspected live `reopen()` replays
  recorded memory/thread settings and base UTC/spill configuration. In-memory
  and unconfigured file callers keep their connection; no connection-lifetime
  capacity claim is made for them.
- `_forward_return_publication.py:72` records ownership only after successful
  creation, uses an attempt-specific UUID namespace, and cleans only recorded
  relations. The published shadow name is removed from ownership after swap.
  Cleanup errors are logged without replacing the original outcome. Retry
  namespaces do not adopt or sweep orphan stages from earlier attempts.
- `bodies_0317.py:10` drops exactly the two optional indexes introduced by 0187.
  The patch's registry and facade hunks add only 0317 wiring. No historical
  migration, job, activation, connection, or shared publication helper is edited.
  Root must still merge the narrow shared-file hunks and update its owned module
  snapshot pins.

The focused test source exercises the reported control paths:

- `tests/test_forward_return_publication.py:115` uses a real file for both price
  bases and forces three-row formation batches, two-row shadow batches, and
  six-row recycle thresholds. Its prefix assertions establish multiple prefixes
  and at least one prefix requiring keyset continuation. It compares every
  logical output column, exact foreign metadata, all five horizons, physical
  contract, ingestion timestamp consistency, settings after recycling, and
  public-view visibility during repeated reopens.
- `tests/test_forward_return_publication.py:154` injects a failure after an
  indexed-shadow insert and separately after both swap renames and the old-table
  drop, before transaction commit. Both paths assert prior data and view results,
  attempt cleanup, preservation of an unrelated similarly named table, and a
  subsequent successful retry. The swap injection reaches the real helper and
  transaction context; it does not replace publication with a no-op.
- The remaining cases cover empty replacement, in-memory connection retention,
  configured temporary table/view/macro refusal, unconfigured file-session
  retention, and an idempotent populated 0317 migration preserving rows, views,
  primary-key enforcement, NOT NULL enforcement, and the described defaults.
  The existing `test_survivorship_forward_sql.py` CHECK-failure fixture also
  exercises copying a custom CHECK into the shadow.

The main differential fixture obtains its expected logical rows from the same
new implementation with normal batch sizes. It therefore proves batch/reopen
invariance, not independent equivalence to the old SQL. The retained arithmetic,
calendar, revision, cutoff, invalid-terminal, clock, and source-isolation tests
are necessary companions, as the implementer report already specifies. The
failure test is a pre-COMMIT rollback test, not a simulated engine failure inside
COMMIT. Neither distinction contradicts the stated claims.

No runtime command, Python execution/import, test collection, test run, lint,
type check, database connection/probe, network operation, installation, patch
application, live-source edit, commit, or git state mutation was performed.
Only this review file was written. Archive4 retains the sole runtime slot.

Root still owns patch integration/clean-HEAD checks and the listed focused
validation after the writer is terminal. Whole-relation unindexed input CTAS
and the ordered-stage CTAS remain deliberately outside the INSERT row caps.
Their spill behavior, disk headroom, total duration, and the fixed batch/recycle
sizes require guarded production evidence at the existing limits. The source
and tiny fixtures cannot establish production memory capacity, and this review
makes no such claim. No rereview is requested absent a Critical finding.
