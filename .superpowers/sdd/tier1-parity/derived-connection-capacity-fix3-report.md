# DP1 fix3: comparable quarterly fixture spans and SIM117

Root's fix2 rerun reached the lifecycle test's value/origin assertion in both
batch-size cases. Arithmetic was 460 as expected, but the origin was
`incomparable`: the reused P1 helper omits `period_start`, while AF1 requires
proven contiguous quarter spans to label a TTM `quarterly`. This is a fixture
metadata mismatch, not evidence requiring a production or assertion change.

Changed only the authorized live
`atx-db/tests/test_derived_connection_capacity.py`, plus this report. The mixed
history now uses AF1's existing `fact` helper for all seven P1 quarter values and
filing events. Explicit starts/ends describe contiguous calendar quarters from
2023-04-01 through 2024-12-31. The original cost rows are retained as well. Both
the amended Q2 revenue value 160 and its later NaN revision explicitly retain
the same 2024-04-01 through 2024-06-30 fiscal span. Removed the now-unused P1
helper imports. Annual, instant and other-issuer fixtures are unchanged.

The expected quarterly origins and values remain `(460, 'quarterly')` and
`(510, 'quarterly')`; the annual fallback 1000/1100 and both latest invalid NULL
expectations are unchanged. No NOT NULL constraint, production logic, metric
selection, memory budget, or substantive assertion was relaxed.

The forced snapshot-name-collision test now combines `closing(...)` and
`pytest.raises(...)` into one parenthesized `with` statement, addressing the
reported Ruff SIM117 while preserving context-entry/exit order and assertions.

## Comprehensive static fixture/assertion inspection

Inspected the complete current test file and its reused helpers, registry
definitions, PIT selection/origin logic, AF1 span lowering, identifier snapshot
SQL and store close/reopen replay. The inspection covered all remaining fixture
and assertion groups, rather than stopping at the first reported failure:

| Assertions/fixtures | Static check and implication |
| --- | --- |
| Catalog and helper identities | `revenue_ttm` consumes revenue, `revenue_growth_yoy` consumes that TTM, and `current_ratio` consumes current assets/liabilities. The selected three-definition catalog is closed over dependencies. AF1 IDs include security/code/basis/start/end/event/revision, so amended events do not collide. PIT event selection consumes all revisions rather than relying on the helper's `is_latest_revision` flags. |
| Original/amended quarterly TTM | Q1-Q4 2024 values are 100, 110, 120, 130, summing to 460; the later Q2 amendment adds 50, giving 510. All four quarter spans are contiguous, individually 91/91/92/92 days, and cover the 366-day leap year. These satisfy AF1's 70-120-day quarter and 330-380-day year checks with a 2024-12-31 endpoint. Initial/latest filing times precede their existing cutoff assertions. |
| Quarterly invalid state | The next revision replaces Q2 160 with a nonnull NaN at 2025-04-10 12:00. It retains the proven span. There is no annual revenue alternative for S1. PIT's finite-value publication gate therefore emits the current invalid NULL state, preserving the existing no-stale-510 expectation. |
| Annual fallback and invalid state | S2 retains its complete 2024-01-01 through 2024-12-31 span, 1000 at February 20, 1100 at March 10, and NaN at April 10. It has no quarterly alternative. Existing cutoff assertions select the first two annual values; invalid current annual evidence cannot revive an earlier value. This matches AF1's existing nonfinite revision fixture. |
| Instant-only and fourth issuer | S3's assets 250 and liabilities 125 share the same December 31 endpoint and availability; the ratio remains 2. Their missing starts are intentional instant semantics. S4's complete annual 700 preserves a distinct fourth standardized ID without expanding the tested scope. |
| Baseline/rebuild parity | Both calls use the same source, run ID, definitions, input histories and limits. `_snapshot` compares every modeled state column and excludes only load-time metadata. NaN occurs only in physical input; canonical output values are finite or NULL, so equality does not compare Python NaN values. The identifier snapshot's random name does not enter output hashes or row IDs. |
| Copied stale/foreign scopes | Clones receive new derived primary IDs and distinct security IDs. A0/B0/Z9 have no standardized input and belong to the selected source, so their staged output is empty and stale canonical rows are removed. FOREIGN belongs only to `other-source`, has no input and remains excluded. `current` lacks a source filter, but every tested S1/S2/S3 identity has only the selected source; clones use different securities. |
| Traversal, reopen and counts | The snapshot has exactly A0, B0, S1, S2, S3, S4, Z9. With cadence two, commits/reopens occur at scope counts 2/4/6/7. Zero-row stale cleanup counts as a completed scope but contributes zero inserted rows, leaving the baseline return count intact. Pages total `ceil(7 / batch_size) + 1`, including the empty terminal fetch. |
| Actual settings and query observer | Settings are captured before the baseline refresh and compared after it and each later reopen. The wrapper delegates real SQL and raw connection close; each real reopen receives a new observer. Its enumeration predicate selects the one source-union CREATE, while page SQL starts with `SELECT security_id FROM` and references only the owned ID snapshot with LIMIT. PIT preparation queries do not match that enumeration/page combination. |
| Cleanup observations | Persistent ID staging survives intermediate reopens and is removed on exhaustion before the final partial flush. The pre-close test checks PIT temp cleanup and absence of an open transaction. The final snapshot inventory is empty on success and injected publication failure. |
| Explicit scope/dependency test | The annual fixtures use complete spans. Sorted unique S1/S3/Z9 scopes preserve unselected S2; rebuilding revenue TTM includes dependent growth. The separate current-ratio family remains outside the requested metric closure, including its cloned stale Z9 row. Return-count assertions filter precisely the rebuilt family. |
| Caller-temp refusal and atomic failure tests | Existing annual 1000/1100 inputs remain physically valid. Preflight failure still occurs before initialize; the publication failure still violates the real derived PK after S1 committed and reopened. Prior passing fixture behavior is unchanged. |
| Early iterator close and name collision tests | Persistent/temporary modes, exact UUID collision, caller-table data and owned-only cleanup remain unchanged. Combining collision contexts only removes the reported lint shape. |
| Unconfigured and in-memory tests | Both continue using valid complete annual input. The unconfigured store retains its manually applied settings and caller temp; the in-memory store uses current required table DDL and must retain its connection/data. No fixture shared with these tests was modified. |

No further fixture mismatch was identified in this static inspection. This is
not an executable pass claim, and no production change is proposed.

## Root verification and limits

Rerun the substantive affected selector (two parametrized cases) under the
existing exclusive guard with `-n 0`:

```text
tests/test_derived_connection_capacity.py::test_keyset_reopens_preserve_full_history_stale_cleanup_and_budget
```

The lint-only context change is in:

```text
tests/test_derived_connection_capacity.py::test_snapshot_name_collision_preserves_caller_table
```

Root should rerun the touched-file Ruff check to establish that SIM117 is gone.
All runtime verification remains root-owned. This implementer performed only
static reads/edits: no Python/import, runtime/test/collection, DB/probe, Ruff,
mypy, Git, subagent, or extra review. Draft artifacts and the old integration
patch remain unchanged because authorization is limited to the live test and
this fix3 report. Prior 20 passing focused cases are root-reported evidence;
the two corrected lifecycle cases have not yet passed after this change.
