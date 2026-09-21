# FP1 draft: bounded forward-return publication

Prepared 2026-09-20. DRAFT ONLY; no live atx-db file was changed, no runtime
command was run, and no tests, imports, collection, lint, type checks, database
connections, probes, installs, network calls, or commits were performed.
Archive4 retains the sole runtime slot and its existing limits.

## Deliverables and integration

`forward-publication-draft/forward-publication.patch` is the six-path patch.
The draft directory also contains complete review copies at the same six relative
paths:

- `atx-db/src/atx_db/delisting.py`: delegates only the SQL publication refresh to
  the new helper; retains the API, option validation, cutoff normalization,
  arithmetic transforms, diagnostics and all other functions.
- `atx-db/src/atx_db/_forward_return_publication.py`: persistent staging,
  bounded result/shadow batches, connection recycling and atomic publication.
- `atx-db/src/atx_db/migrations/bodies_0317.py`: removes only
  `idx_forward_returns_ss_key` and `idx_forward_returns_ss_delisted`.
- `atx-db/src/atx_db/migrations/registry.py`: only the 0317 import and expansion.
- `atx-db/src/atx_db/migrations/__init__.py`: only the 0317 private-module removal.
- `atx-db/tests/test_forward_return_publication.py`: eleven focused cases.

After the active writer is terminal, root should integrate these six paths
together, using the patch rather than overwriting registry/facade files with
their review copies. Serialize reserved 0317 with other migration work and update
the exact public module snapshot pin for `_forward_return_publication` as root
owns those shared files. The helper is imported eagerly under a private alias
so its module presence does not depend on test order. Govern the production
migration through the existing backup/startup flow before running publication.
Do not amend historical migrations or change activation/jobs/connection code.

## Implementation and preserved contracts

The original five-horizon SQL predicates and arithmetic are retained. Selected
positive bars gain a deterministic security/date row number. Each stage insert
has at most 100,000 formation rows for one configured horizon. Endpoint bars are
scoped by that batch's minimum/maximum security identifier, keeping their entire
date history, so splitting one security across batches never truncates its
forward endpoint. Calendar endpoints, revision tie-breaks, availability clocks,
invalid-terminal boundaries, adjusted/raw behavior and finite-value filtering
are unchanged. All five `IC_HORIZONS` are passed through, with no population,
date, precision or metric-definition reduction.

After the input snapshots are dropped, a spillable unindexed CTAS sorts the
expanded stage by deterministic ID. The constrained shadow uses 256 ordered
two-character key ranges, with keyset sub-batches capped at 100,000 rows even
when a range is unusually large. First/last bounds are open so unrelated source
IDs outside the SHA range, including an empty string, are retained. Foreign
source rows copy every physical column, including original ingestion timestamps.
New rows share one refresh timestamp in both ingestion fields, matching the
former transaction-wide `now()` behavior.

The shadow's DDL is cloned from the actual live table's catalog SQL; it retains
types, defaults, NOT NULL, PK and CHECK constraints, including the existing
arithmetic test fixture's custom CHECK. Primary-key collisions still fail.
Staging, sorted-stage and complete-shadow counts are checked; target-source and
foreign-source shadow counts must both match. The established
`publish_validated_shadow` verifies the physical column/default/key contract
and performs the short transactional rename/drop. Existing dependent views
keep their public relation references. The live table is untouched before the
swap; an empty result still removes only the requested source.

Persistent stores with recorded `analytical_memory_limit` and
`analytical_threads` recycle after 400,000 committed inserted rows. Existing
`DuckDBStore.close()` checkpoints, and `reopen()` replays those recorded settings;
the helper also restores the prior `preserve_insertion_order` setting. No budget
is raised. Configured stores reject caller-owned temporary tables/views/functions
and attached databases before creating stages. In-memory and unconfigured
persistent callers retain their connection and complete session state; they use
bounded insert transactions but have no connection-lifetime guarantee. This is
the root-directed CF5/DP1 eligibility rule, not a generic session snapshot.

## Failure and retry ownership

Each attempt allocates a UUID-specific `_ss_forward_<uuid>_*` namespace. A name
enters the cleanup list only after its CREATE succeeds; there is no CREATE OR
REPLACE, shared-stage DROP, or wildcard cleanup. Success and ordinary failure
drop only that attempt's remaining artifacts. After a successful swap, the
shadow name is removed from ownership before cleanup. Cleanup failure is logged
without masking the original error or turning a successful publication into an
ambiguous failure.

A dead connection or hard process exit can leave persistent orphan stages.
Retries use a fresh namespace and neither adopt nor delete those artifacts.
Any orphan cleanup must be an explicit root/operator action after proving the
specific attempt is no longer active. Unrelated artifacts and caller-owned
temporary objects are never silently removed.

## Focused validation for root

Run these only after integration, in the sole guarded runtime slot, with `-n 0`
and the existing production/resource limits:

- `tests/test_forward_return_publication.py` (11 cases): real file-backed
  adjusted/raw builds crossing forced formation, prefix, keyset and reopen
  boundaries; identical logical output and exact retained foreign metadata;
  configured memory/thread/order replay; prior-table/view visibility during
  every reopen; shadow-build failure; failure after both swap renames and DROP
  but before commit; retry cleanup; empty refresh; in-memory preservation;
  configured caller temporary table/view/macro refusal; unconfigured file-session
  preservation; populated 0317 index/PK/NOT NULL/default/view preservation.
- `tests/test_survivorship_forward_sql.py`: reuse the existing arithmetic,
  terminal, cutoff, revision, source-isolation and CHECK-failure assertions.
- `tests/test_bulk_publication.py`: unchanged shared-helper contracts.
- `tests/test_delisting_returns.py::test_refresh_survivorship_safe_forward_returns_stitches_and_is_idempotent`
  with `--run-slow` for this single marked integration case.
- Existing migration governance/schema contracts and
  `tests/test_module_boundaries.py` after root updates the shared pins, followed
  by scoped Ruff and strict mypy for the new helper under the same guard.

Then use one fresh independent review. Important fixes are accepted on the
implementer report plus root validation; rereview only Critical findings.

## Static limits and remaining decisions

Only source inspection and construction of the draft/patch were performed.
The tests have not been executed and the patch has not been applied or checked
against the live branch. No production capacity or current-stage OOM is claimed.
The source evidence is the original giant indexed publication transaction;
the analogous price COMMIT failures are measured in the brief, not this stage.

Initial input snapshots and the final ordered-stage CTAS remain whole-relation
unindexed, spillable statements, following the reviewed price-stage approach.
The explicit row caps apply to expanded-result INSERTs and every indexed-shadow
INSERT, not to these unindexed CTAS statements. The ordered stage avoids thousands
of unpruned scans of a stage assembled in security batches. Root/review must
validate this disk/time tradeoff at the unchanged 1GB/one-thread budget within
the 3GiB process guard; the small fixture demonstrates control flow and rollback,
not production memory capacity. The sort temporarily needs both unsorted and
sorted stages, and later both ordered stage and shadow, beside the old live
table. Measure disk headroom before production rather than narrowing output.

The workflow assumes the existing sole-writer orchestration; its input snapshots
are separate committed CTAS statements and it must not run inside an outer
transaction. It intentionally does not implement concurrent-writer reconciliation,
automatic orphan recovery, generic session replay, or any metric/PIT redesign.
No unresolved label-semantic decision was introduced. The only remaining
acceptance questions are the unmeasured sort/storage cost and production capacity
of the selected fixed batch/recycle sizes; resolve from guarded evidence without
raising budgets or weakening coverage.
