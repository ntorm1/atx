# SA1 — bounded submissions archive directory

Research question: can the full retained SEC submissions history resume without
retaining almost one million ZIP-directory objects and duplicate filename sets?

The existing directory-only inventory peaked at 0.769512 GiB before the filing
loader, pandas or DuckDB. Retained source denominators are 991,042 entries,
985,667 main issuers and 5,374 history members. Root's live CompanyFacts archive17
is independent and must remain untouched while implementation proceeds.

Owned files: `_submissions_archive.py`, archive integration in `sec_submissions.py`,
`test_submissions_archive.py`, focused additions to the two submissions bulk and
resume test modules, and this brief/report. Root owns public API snapshot and
runtime/release integration. No connection, activation, registry, migration,
CompanyFacts or shared zipfile changes are authorized by this task.

Implement a versioned SQLite directory populated through bounded central-record
iteration, with disk sorting and a fixed cache. Preserve sorted unique main
traversal, duplicate-name last-wins, history lookup, standard ZIP interpretation,
ZIP64/Unicode/CRC/local-header/overlap protections, immutable archive bytes and
full forms/CIKs/history/batch50/resume contracts. Bind reuse to source SHA/stat,
parser version, completed-file digest and atomic publication. Rebuild corrupted
or interrupted caches; never infer completeness from a cached count alone.

Acceptance, after root grants the single heavy-work slot:

1. Focused archive fixtures plus affected bulk/resume checks, then scoped Ruff.
2. Guarded retained-archive directory inventory matching all existing counts and
   selected member digests, with measured native peak. Do not open the complete
   source with ordinary eager `ZipFile` in that lower-memory measurement.
3. Full submissions resume from `04cf947d-53bb-49b7-a276-b3c74a2a52c8`, unchanged
   scope and batch50, after archive17 is terminal. Require stage/dataset ledgers,
   `scope_complete=true`, all 985,667 main members, zero missing referenced
   history, and verified-prior plus new rows reconciled to covered rows.

Representative ledger query (bound to the actual new dataset UUID):

```sql
SELECT run_id, status, rows_loaded, finished_at
FROM dataset_runs
WHERE dataset_id = 'sec_submissions' AND run_id = ?;
```

The reader improvement alone does not prove lower-memory normalization/writes,
raw acceptance-clock coverage, historical listings or production eligibility.
Keep the hard memory stops, pinned snapshot, scope, source receipt and stash@{0}.
