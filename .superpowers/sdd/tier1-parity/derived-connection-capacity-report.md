# DP1 derived connection-capacity draft (fix1)

Prepared 2026-09-20 against the current `feat/tier1-parity` source. Draft only;
archive4 retained the sole runtime slot throughout. No live `atx-db` source or
tests were edited. No Python/import/collection/test/lint/typecheck/database,
network/install/download, commit, checkout, reset, stash, restore, clean, or
subagent activity was performed. Discovery used static source reads and `rg`
because graph tools were unavailable. The live-versus-draft textual diff was
inspected; the diff command's status 1 means differences were present. The one
independent review found 0 Critical / 1 Important / 1 Minor. Fix1 addresses both
findings in the draft, as recorded in `derived-connection-capacity-fix1-report.md`.

## Exact draft paths and integration

| Draft path under `.superpowers/sdd/tier1-parity/` | Root integration destination |
| --- | --- |
| `derived-connection-draft/src/atx_db/derived_metrics.py` | `atx-db/src/atx_db/derived_metrics.py` |
| `derived-connection-draft/tests/test_derived_connection_capacity.py` | `atx-db/tests/test_derived_connection_capacity.py` (new) |

The files are complete replacement/addition artifacts, not an already-applied
live patch. Root should wait for the active writer's terminal receipt, compare
the live source with the basis below, copy the two exact draft files to their
destinations, and inspect the scoped diff before running the focused checks in
the sole guarded runtime slot. If live source changes, apply only the DP1 hunks
from the live-versus-draft diff instead of overwriting unrelated changes. The
one independent review is complete; root accepted the findings and requested
fix1 acceptance by implementer report plus focused checks, without another
review unless a Critical issue arises. This implementer did not integrate.

SHA256 at fix1 draft handoff (live basis unchanged):

- Live basis `atx-db/src/atx_db/derived_metrics.py`:
  `F953220EA651DD4F60BB1E205046CEE2966F89F1AAD9582412BD805E20978729`.
- Draft `src/atx_db/derived_metrics.py`:
  `EDCF0286D42F8A09DBAD999F02517E14E99D21EE860532124D02029D34560B10`.
- Draft `tests/test_derived_connection_capacity.py`:
  `C13E81E1C5176E2ADE0DC576CBDF3C01D8EDD49CB90BF3592DBDE1D479270CC3`.

## Implementation and preserved contracts

`select_security_batches` now enumerates the complete eligible ID union once
into an invocation-owned ID-only SQL snapshot. It contains every quarterly/
instant/annual standardized ID and every existing selected-source derived ID,
ordered and deduplicated. Fully fetched bounded keyset pages then read only
that small relation, using the last returned ID as a parameterized lower bound.
The production default page size of one no longer repeats fact-level union
enumeration. Removing early stale-only output scopes cannot affect later pages.
Explicit caller IDs retain their sorted-set behavior and bypass the snapshot;
the 1-to-500 batch-size clamp is unchanged. No result cursor or transaction is
held by the generator across publication or reopen.

Configured persistent stores use a persistent ID relation that survives reopen;
other callers use a session-local temporary relation. Names include a fresh
UUID. Creation never replaces or adopts an existing table; ownership starts
only after successful CREATE. Reads and cleanup address the exact catalog,
main schema and owned name. The generator drops only that relation in `finally`,
on exhaustion or explicit close. Refresh uses `contextlib.closing` to ensure
cleanup even when security processing raises. Direct consumers that stop early
must close their unfinished iterator while the store is open. No prefix-wide
cleanup or unrelated caller-object deletion is performed.

The local `_DERIVED_REOPEN_SECURITIES = 10` cadence counts successfully committed
complete security scopes, including zero-row stale cleanup. Only after the
existing `finally: pit.cleanup(...)` completes does a scope count toward the
cadence. At each full group, and once for a final nonempty partial group, the
store closes/checkpoints and reopens through the existing `DuckDBStore` methods.
No new generic lifecycle framework or connection helper change was added.

As explicitly confirmed by root during implementation, eligibility follows CF5:
the store must be a persistent existing file and both analytical memory and
thread settings must be recorded. The bounded connection-lifetime guarantee
therefore applies to the configured production path; activation records its
1 GB / one-thread budget. In-memory and unconfigured callers keep their existing
connection and settings. The draft does not invent defaults, snapshot unknown
settings, silently increase a budget, or reopen an in-memory store.

Eligible callers are checked for noninternal temporary tables/views before
`initialize` or PIT staging can mutate anything. This also protects caller
objects with owned PIT names. The same CF5-style check runs again immediately
before close; caller temporary state is refused rather than discarded. The
existing store reopen path replays recorded memory/thread/insertion-order
settings and base timezone/spill-directory setup.

The metric DAG closure, definitions, P1/AF1 arithmetic, original/amended/null
event history, annual fallback, frame/candidate/input/scope limits, SQL staging,
primary keys, per-security DELETE/INSERT/COMMIT/ROLLBACK, returned insert count,
and dataset diagnostics remain unchanged. A failure after earlier securities
committed still raises and leaves the failing security's old complete scope.
No full fact/event/output frame is accumulated in Python; only bounded ID pages
are introduced. No migration, registry, activation, jobs, `_bulk_publication`,
connection, or PIT/annual-helper edit is proposed.

## Focused tests prepared; all execution pending

Run from `atx-db` after integration, with `-n 0` under the root's existing process
guard. Do not use the repository's default four test workers while production
runtime policy is in effect. The new file has eight test functions / thirteen cases:

- `tests/test_derived_connection_capacity.py::test_keyset_reopens_preserve_full_history_stale_cleanup_and_budget`
  (batch sizes 1 and 3): seven scopes with two early stale-only scopes and one
  later stale-only scope, quarterly/annual/instant-only input issuers, overlapping
  standardized/derived IDs and a foreign-source-only excluded ID. Actual closes
  and reopens occur after scopes 2, 4, 6 and 7, including within a fetched batch.
  The test compares all modeled state columns and return counts with a baseline,
  reuses P1's amendment history and AF1's fact/current helpers, checks quarterly
  and annual original/amended/null values, and checks actual SQL memory/thread/
  insertion-order/timezone/spill settings after every reopen. Expected settings
  are captured before the baseline refresh's first reopen and asserted after it.
  An execute observer checks exactly one fact/output ID union creation across
  all bounded pages/reopens; page SQL references only the owned ID relation.
  It asserts snapshot cleanup after both successful builds. Before checkpoint
  it checks that no temporary table/view or active transaction remains.
- `tests/test_derived_connection_capacity.py::test_explicit_scopes_deduplicate_sort_and_keep_dependency_closure`:
  duplicate/unsorted explicit IDs, a reopen inside a three-ID batch, final partial
  flush, dependent growth rebuild, untouched unselected security, scoped stale
  cleanup preserving an independent metric family, and scoped return count.
- `tests/test_derived_connection_capacity.py::test_caller_temporary_state_is_refused_before_any_mutation`
  (table, view, reserved PIT-name table): initialized canonical rows and caller
  state survive refusal; an initialize hook proves validation precedes mutation.
- `tests/test_derived_connection_capacity.py::test_failure_after_reopen_keeps_earlier_commit_and_failing_scope`:
  after S1 commits/reopens, inject duplicate staging rows for S2 to make final
  publication fail its actual PK constraint after DELETE. S1 remains updated,
  S2's complete prior scope survives, and PIT temps plus the persistent ID
  snapshot are removed.
- `tests/test_derived_connection_capacity.py::test_early_iterator_close_removes_only_its_snapshot`
  (configured persistent and unconfigured temporary): verifies snapshot kind,
  continuation after real reopen where applicable, deterministic early-close
  cleanup and preservation of an unrelated caller table with the same prefix.
- `tests/test_derived_connection_capacity.py::test_snapshot_name_collision_preserves_caller_table`
  (persistent and temporary): forces an exact UUID-name collision, verifies
  creation fails, and proves the pre-existing caller table is not dropped.
- `tests/test_derived_connection_capacity.py::test_unconfigured_persistent_store_keeps_its_session`:
  manually applied unrecorded settings, caller temp data and connection survive.
- `tests/test_derived_connection_capacity.py::test_configured_memory_store_retains_database_and_caller_temp`:
  a real in-memory database with the current two required table DDLs retains its
  connection, outputs and caller temp data despite cadence one. It avoids an
  additional full-schema bootstrap.

Recommended existing semantic/failure selectors alongside that complete new file:

```text
tests/test_derived_metrics.py::test_batches_are_deterministic_and_bounded
tests/test_derived_metrics.py::test_batch_size_does_not_change_the_result
tests/test_derived_metrics.py::test_full_catalog_runs_without_error
tests/test_derived_metrics.py::test_requesting_a_metric_auto_expands_its_transitive_dependencies
tests/test_derived_pit_revisions.py::test_exact_amendment_dependency_export_daily_and_projection
tests/test_derived_pit_revisions.py::test_bounded_frames_scope_preservation_and_prepublication_failure
tests/test_derived_pit_revisions.py::test_scoped_atomic_rollback_on_publish_error
tests/test_derived_annual.py::test_precedence_switches_and_null_never_resurrects
tests/test_derived_annual.py::test_annual_limits_and_atomic_failure_preserve_previous_scope
```

After focused pytest, run Ruff on the two integrated Python paths and the usual
strict mypy check for `src/atx_db/derived_metrics.py` in the same exclusive runtime
policy. Tests and static-tool execution are pending; no pass result is claimed.

## Limits

These fixtures establish lifecycle/control flow and output preservation only.
They do not measure production peak memory, scan cost, checkpoint cost or
full-universe completion. The eligible SQL union is executed once; subsequent
pages scan only its small ID-only result. Identifier pages remain bounded in
Python, while SQL retains its existing spill/budget behavior. The default
identifier page size is unchanged. Production memory/query budgets, complete
issuer universe and metric output scope are unchanged.

CF5 eligibility and its table/view temporary-state check are intentionally
retained. No new support for caller-owned active publication transactions or
arbitrary unrecorded session configuration is introduced. Reopen/checkpoint
errors propagate; successful prior security commits remain committed and no
partial return count is reported as success. Full production capacity evidence
remains a root-owned post-integration gate. Process termination or an unusable
connection can prevent SQL cleanup and leave the uniquely named persistent ID
relation behind; a later run never adopts or deletes unrelated/orphan tables.
Ordinary success, publication failure and explicit early iterator close have
focused cleanup coverage prepared, pending execution.
