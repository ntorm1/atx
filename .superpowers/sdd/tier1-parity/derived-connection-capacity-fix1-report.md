# DP1 fix1: one-time identifier enumeration and initial-budget assertion

Prepared 2026-09-20 in response to the completed independent review:
`derived-connection-capacity-review.md` (0 Critical / 1 Important / 1 Minor).
Both requested findings are addressed in the draft only. Root requested
acceptance through this report and focused checks, with no second independent
review unless a Critical issue appears. No runtime pass is claimed.

## I1: materialize the complete ID union once

`derived-connection-draft/src/atx_db/derived_metrics.py` now creates one ID-only
snapshot before the first unscoped page. A single CREATE TABLE AS query preserves
the exact union of all quarterly/instant/annual standardized security IDs and
existing selected-source derived IDs. SQL orders and deduplicates the snapshot;
only bounded pages of at most 500 identifiers cross into Python. Later pages
read `security_id` from this snapshot with a deterministic keyset boundary.
Default `batch_size=1` therefore no longer re-enumerates fact/output tables once
per issuer. Explicit caller scopes still use their existing sorted unique IDs
without a SQL snapshot.

The snapshot is persistent only for the existing CF5-eligible configured
persistent path; it survives the ten-completed-security checkpoint/reopen.
Unconfigured and in-memory callers use a temporary ID snapshot and keep their
existing connection/settings. The snapshot contains no fact, event or output
frames and introduces no registry/migration/index/default-budget change.

The table name is `_derived_security_ids_` plus a fresh UUID hex value. A plain
CREATE (without REPLACE or IF NOT EXISTS) fails safely on a collision. A local
ownership flag is set only after CREATE succeeds. Snapshot reads and DROP are
catalog/schema/name qualified; cleanup drops exactly the owned relation rather
than searching for a prefix or deleting another caller's objects. The generator
uses `finally` for cleanup on exhaustion or close. Refresh now wraps it with
`contextlib.closing`, so an exception in staging/publication also closes it
deterministically after the existing PIT cleanup/transaction rollback. Direct
consumers that exit early must close the iterator while its store remains open.

The existing per-security atomic block is only indented under the closing
context; its arithmetic, limits, DELETE/INSERT/COMMIT/ROLLBACK and row counting
are unchanged. Cadence, final-partial flush, stale-scope inclusion, source scope,
metric closure, caller-temp refusal and recorded-budget replay are retained.

Focused test extensions observe actual executed SQL across several real
reopens: exactly one fact-level union CREATE is allowed, and every identifier
page must query only the snapshot with LIMIT. The existing seven-security case
still includes two early stale-only IDs and a later stale-only ID, both batch
sizes 1 and 3, complete output/count parity, and the foreign-source exclusion.
It now asserts no snapshot remains after success; the injected PK publication
failure also asserts snapshot removal after the earlier committed reopen.
Two additional parametrized tests cover persistent/temporary early iterator
close and forced exact-name collisions, including preservation of caller data.

## M1: preserve the original settings expectation

The lifecycle test captures actual `SETTINGS_SQL` immediately after receiving
the configured fixture, before seeding or the first baseline refresh. It asserts
the same values after that baseline's final-partial reopen, then continues to
check those original values after every later observed reopen. Initial replay
loss can no longer become the expected baseline.

## Exact files and verification selectors

Updated draft artifacts and eventual root destinations:

| Draft beneath `.superpowers/sdd/tier1-parity/` | Destination |
| --- | --- |
| `derived-connection-draft/src/atx_db/derived_metrics.py` | `atx-db/src/atx_db/derived_metrics.py` |
| `derived-connection-draft/tests/test_derived_connection_capacity.py` | `atx-db/tests/test_derived_connection_capacity.py` |

`derived-connection-capacity-report.md` is updated to describe fix1, the completed
review policy, revised test inventory and these replacement hashes:

- Unchanged live source basis SHA256:
  `F953220EA651DD4F60BB1E205046CEE2966F89F1AAD9582412BD805E20978729`.
- Fixed draft source SHA256:
  `EDCF0286D42F8A09DBAD999F02517E14E99D21EE860532124D02029D34560B10`.
- Fixed draft test SHA256:
  `C13E81E1C5176E2ADE0DC576CBDF3C01D8EDD49CB90BF3592DBDE1D479270CC3`.

Packaging follow-up: the accepted two-path, live-relative patch is
`.superpowers/sdd/tier1-parity/derived-connection-draft/derived-connection.patch`.
Its SHA256 is
`719FE0D5BC460C0C6744DE6B8427405F46DFAA09A4D72E3B137ED79296943634`.
The patch touches only `atx-db/src/atx_db/derived_metrics.py` and the new
`atx-db/tests/test_derived_connection_capacity.py`. It was assembled using raw
`git -c core.autocrlf=false diff --no-index` output and header-only path rewriting;
no patch application/check, runtime execution or commit was performed. Both
accepted artifact hashes above are unchanged. The mixed-ending live basis and
LF-only accepted source require a full-file source hunk to preserve exact bytes;
this is an EOL consequence, not an additional source change. Root should use
`git -c core.autocrlf=false apply --check` and then the same configuration for
application, from `C:/atx`, after the active writer releases its slot.

Root should integrate with this narrow patch rather than overwrite complete
live files, checking that the live basis is unchanged or merging only the DP1
changes if necessary. Run the complete new test file (eight functions,
thirteen cases) with `-n 0` under the sole existing root runtime guard. The most
direct I1/M1 selectors are:

```text
tests/test_derived_connection_capacity.py::test_keyset_reopens_preserve_full_history_stale_cleanup_and_budget
tests/test_derived_connection_capacity.py::test_failure_after_reopen_keeps_earlier_commit_and_failing_scope
tests/test_derived_connection_capacity.py::test_early_iterator_close_removes_only_its_snapshot
tests/test_derived_connection_capacity.py::test_snapshot_name_collision_preserves_caller_table
tests/test_derived_connection_capacity.py::test_unconfigured_persistent_store_keeps_its_session
tests/test_derived_connection_capacity.py::test_configured_memory_store_retains_database_and_caller_temp
```

The main report retains all exact existing P1/AF1 semantic/failure selectors.
After focused pytest, root should run Ruff on the two integrated Python paths
and the usual strict mypy check for `src/atx_db/derived_metrics.py`. All of these
checks remain pending; this implementer did not run them or collect tests.

## Static work and limitations

Only static file reads/edits, textual inspection and hashes were used. No live
files were changed; no Python/import, tests/collection, Ruff/mypy, DB/probe,
network/install/download, commits/Git mutation or subagents were run. Archive4
retained the sole runtime allocation. The independent review was treated as
authoritative; no additional review was requested.

Tiny fixtures establish one-time enumeration and lifecycle/output contracts,
not measured production capacity or elapsed time. The one source enumeration
and later small-ID scans still operate within the existing SQL memory/spill
budget. No memory/query budget increase, issuer/metric reduction, or historical
output omission is proposed. The bounded connection-lifetime guarantee remains
limited to configured persistent callers, as root previously specified.

Ordinary completion, explicit early close and recoverable publication failures
have deterministic owned-snapshot cleanup. Process termination, a closed store
before a direct consumer closes its iterator, or an unusable database connection
can prevent DROP and leave an orphan uniquely named persistent ID table. There
is deliberately no blanket orphan cleanup that could delete unrelated caller
objects; later runs use new names and do not adopt old snapshots. Production
capacity and all executable verification remain root-owned pending work.
