# Terminal dependency propagation in the agent task DAG

Date: 2026-07-18

## Primary workflow references

- Argo Workflows DAG: https://argo-workflows.readthedocs.io/en/latest/walk-through/dag/
- Airflow DAG trigger rules: https://airflow.apache.org/docs/apache-airflow/3.0.3/core-concepts/dags.html

Argo's default fail-fast DAG behavior stops scheduling new tasks after a node
fails and waits for already running nodes before failing the DAG. Airflow's
default `all_success` trigger rule similarly requires every direct parent to
succeed and exposes an `upstream_failed` state for dependency failure. The
shared invariant is that ordinary success-gated descendants must reach an
explicit terminal state rather than remain indefinitely pending.

atx-db deliberately keeps a smaller state model: every dependency means
`all_success`, with no alternate trigger rules yet. Therefore a failed or
cancelled prerequisite makes all of its descendants unreachable. Leaving those
tasks queued would be observably misleading, prevent clean run accounting, and
force every consumer to rediscover a transitive graph condition.

## Schema-v4 implementation

- Explicit terminal failure and retry-exhausting lease reclamation walk the
  dependency graph inside the same `BEGIN IMMEDIATE` transaction.
- Every queued or leased descendant transitions to `cancelled`, loses any lease
  token, records the terminal root and status as its cause, and emits exactly one
  ordered `task.cancelled` event.
- Status predicates make the cascade idempotent. SQLite's immediate writer lock
  serializes simultaneous terminal roots whose descendant sets overlap.
- New tasks reject dependencies that are already failed or cancelled.
- Integrity verification rejects any queued or leased task with a direct failed
  or cancelled parent.
- The v3-to-v4 semantic migration finds terminal roots across every workspace
  and repairs their live descendants before setting the global schema version.

## Regression evidence

The release build passes all 18 `AgentDatabase` and CLI tests. New coverage
includes direct and transitive cancellation, retry idempotence, an independent
schedulable branch, exhausted lease reclamation, rejection of newly impossible
tasks, two concurrent root failures sharing a diamond-DAG descendant, exactly-once
cancellation events, and migration repair spanning two workspaces. The live
self-improvement coordination database migrated to schema v4 and passed full
integrity verification while retaining its active fenced lease.

## Remaining scheduler gates

- Add explicit trigger policies before supporting cleanup, failure-handler, or
  any-success tasks; unconditional propagation is correct only for the current
  all-success dependency contract.
- Exercise kill/restart boundaries around every cascade statement and verify
  recovery from a copied WAL database.
- Measure claim and cascade latency on wide and deep DAGs, including thousands
  of overlapping descendants and hundreds of concurrent claimant processes.
- Add event retention/checkpoint policy, authorization boundaries, and encrypted
  storage integration before treating the embedded control plane as multi-tenant.
