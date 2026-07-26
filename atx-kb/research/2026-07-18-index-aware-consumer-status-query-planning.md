# Index-aware consumer status query planning

Date: 2026-07-18

## Measured review finding

Snapshot-consistent fleet status is correct and bounded, but its point-status
visibility predicate combines broad and subject-filtered consumers with an
`OR`. SQLite therefore chooses `agent_events_poll_idx(workspace, sequence)` for
both cases. On the Release build, an adversarial fixture with 100 subject
filters that match none of 100,000 workspace events took 18,177 ms to return an
otherwise empty fleet. `EXPLAIN QUERY PLAN` confirmed that every filtered
consumer scanned the workspace event tail instead of seeking the subject
index.

The status query also references its `visible_events` CTE five times and its
dead-letter CTE repeatedly. When SQLite flattens or inlines those ordinary
CTEs, it can repeat the same candidate-range work for count, extrema, oldest
timestamp, delivery-head count, and DLQ partitions.

SQLite provides explicit mechanisms for making the intended access contract
falsifiable:

- A partial index is usable when the query predicate implies the index
  predicate. The filtered branch must state `e.subject<>''` explicitly so it
  entails the predicate on `agent_events_subject_idx`:
  https://www.sqlite.org/partialindex.html
- `AS MATERIALIZED` evaluates a CTE into an ephemeral table and acts as an
  optimization fence, preventing flattening when one exact candidate set is
  intentionally reused:
  https://www.sqlite.org/lang_with.html#materialization_hints
- `INDEXED BY` is a plan requirement rather than a hint; statement preparation
  fails if the named index cannot satisfy the query. SQLite recommends it as a
  late regression guard for a deliberately fixed plan:
  https://www.sqlite.org/lang_indexedby.html
- `EXPLAIN QUERY PLAN` reports whether a table is scanned or searched and which
  index implements the search:
  https://www.sqlite.org/eqp.html

## Production contract

1. Preserve the exact point/fleet status result: subject and self-control echo
   visibility, one canonical fleet time/HWM, delivery-head ordering, and DLQ
   partitions do not change.
2. Replace the mixed `OR` with mutually exclusive `UNION ALL` branches. Blank
   filters use `agent_events_poll_idx`; nonblank filters explicitly require a
   nonempty equal subject and use `agent_events_subject_idx`. Mutual exclusion
   proves that no event can be duplicated.
3. Point status materializes the exact visible-event and DLQ candidate sets
   once before deriving their several metrics. Fleet status performs one
   ordered set aggregation after its metadata read: it groups each branch's
   candidate relation and the consumer-scoped DLQ relation once, then decodes
   all rows through the same partition validator as point status.
4. Require the two schema-owned event indexes with `INDEXED BY`, turning an
   accidental migration or predicate change into a preparation error instead
   of a silent large-log regression.
5. Keep exact status as an explicit control-plane operation. Poll and receive
   hot paths, schema v22, persisted state, backup format, status JSON, and the
   capability-token secrecy boundary remain unchanged.
6. Evaluate with result equivalence and query-plan evidence, then rerun the
   same 100-by-100,000 unmatched-filter fixture. Wall time is supporting
   evidence, not the sole correctness gate.

## Test obligations

- broad and filtered consumers return identical exact metrics across matching,
  unmatched, self-control-suppressed, cursor-gap, active-head, and DLQ cases;
- query-plan inspection contains searches through both
  `agent_events_poll_idx` and `agent_events_subject_idx`, with no mixed
  workspace-tail plan for the filtered branch;
- a large unmatched filtered fleet completes with zero pending work and a
  substantial deterministic improvement over the recorded 18,177 ms baseline;
- fleet snapshot atomicity, stable order, HWM/time agreement, non-mutation,
  token secrecy, backup/restore, integrity, and the 1,000-item completeness
  bound stay green.

## Implementation evidence

The production query now makes broad and filtered visibility disjoint and
requires `agent_events_poll_idx` and `agent_events_subject_idx` respectively.
Point status materializes and reuses one candidate set. Fleet status replaces
up to 1,000 point statement executions with one set aggregation after the
bounded snapshot metadata read; oldest pending time is joined from the event
at the aggregate's minimum sequence rather than approximated with a timestamp
minimum. DLQ partitions are grouped once with the quarantine overlay.

The unchanged 100-consumer/100,000-event no-match fixture fell from 18,177 ms
to 62 ms, a 293x speedup, while returning the same 100 zero-backlog statuses.
A checked regression fixture exercises 50 nonmatching filters over 50,000
events. It requires successful preparation of both named index paths, exact
zero-work results, and a generous two-second ceiling; the Release run completes
the entire test, including fixture construction, in under 0.7 seconds. Mixed
delivery and DLQ states are differentially compared against independently
executed point status, and all prior snapshot, concurrency, bound, CLI secrecy,
and restore tests remain required.

## Validation result

- All 93 atx-kb, atx-db, and CLI tests pass, including the new selective-scale
  and differential fleet cases.
- All 15 deterministic retrieval-quality thresholds pass on 110 documents and
  41 queries.
- The 1,024-vector ANN gate retains recall@10 of 1.0, exact-distance work
  reduction of 4.006848x, and warm-latency speedup of 5.131324x.
- Clang-format validation, coordination integrity, knowledge integrity, and
  paired backup/restore remain required before the live seal.
- Schema version 22 and all durable formats are unchanged.
