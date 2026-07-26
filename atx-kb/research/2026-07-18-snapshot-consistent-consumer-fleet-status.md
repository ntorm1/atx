# Snapshot-consistent consumer fleet status

Date: 2026-07-18

## Review finding

Atx-db now provides a detailed exact status for one named consumer, but it has
no authoritative collection operation. A supervisor must carry an out-of-band
name list, so it can silently omit a newly registered consumer, an actionable
DLQ, or an expired delivery head. Repeating single-consumer reads also permits
cross-consumer snapshot skew while writers continue committing.

Primary control-plane stores make collection semantics explicit:

- Kubernetes defines list as a resource collection, returns a collection
  resource version, and provides consistent list operations so clients can
  cache and synchronize complete state:
  https://kubernetes.io/docs/reference/using-api/api-concepts
- Kubernetes paginated continuation preserves the exact resource version of
  the initial list snapshot rather than mixing pages from different moments:
  https://kubernetes.io/docs/reference/using-api/api-concepts/#collections
- etcd range reads are linearizable by default and return revision metadata
  used to reason about the state observed by a collection read:
  https://etcd.io/docs/v3.7/learning/api/
- SQLite deferred transactions establish a read transaction at the first read,
  and WAL readers retain their historical snapshot while concurrent writers
  commit:
  https://www.sqlite.org/lang_transaction.html
  https://www.sqlite.org/isolation.html
- SQLite's `now` value is stable for one `sqlite3_step`, so a collection must
  capture and bind one observation time rather than recompute it per consumer:
  https://www.sqlite.org/lang_datefunc.html

For an embedded SQLite control plane, the simplest strong contract is a bounded
complete collection under one read transaction rather than opaque paginated
snapshot tokens.

## Proposed contract

1. Add `consumer-statuses` and a corresponding API that discovers every
   consumer in the selected workspace; callers need no external registry.
2. Return statuses in stable name order from one SQLite read transaction. All
   per-consumer event, lease-time, backlog, and DLQ calculations observe the
   same database snapshot.
3. Include collection metadata: workspace event high-watermark, canonical
   observation time, and consumer count. The high-watermark reconciles event
   feed coverage, but is not a complete fleet revision: lease acquisition,
   renewal, rejection, and settlement can change status without appending an
   event. It must not be used alone as a cache validator for the collection.
4. Bound the complete collection at 1,000 consumers. If the workspace exceeds
   the bound, fail explicitly instead of returning a partial fleet that looks
   complete. A later scalable pagination design must carry a durable snapshot
   identity across pages.
5. Reuse the exact single-consumer status calculation and secrecy boundary;
   collection JSON must not expose delivery or request tokens.
6. The list is read-only and stays off poll/receive hot paths. Schema v22 and
   backup formats remain unchanged.

The implementation bounds discovery work with a 1,001-row sentinel, reuses one
compiled point-status statement, and binds the collection HWM into each item.
This avoids unbounded count work, repeated SQL compilation, and redundant HWM
scans while preserving one immutable database snapshot. Exact per-consumer
backlog and DLQ aggregation remains an explicit control-plane operation rather
than a delivery hot-path cost.

## Test obligations

- consumers with idle, in-flight, cooling, actionable-DLQ, redriven, and
  quarantined states appear once in stable name order;
- a concurrent registration commits entirely before or after the collection
  snapshot and cannot produce a partial consumer record;
- collection count and workspace high-watermark agree with every returned
  item, with one canonical observation time for dynamic state classification;
- the hard bound fails rather than truncating;
- repeated API/CLI reads are non-mutating and capability-token free;
- backup/restore returns the same fleet at the sealed high-watermark.
