# CC1 independent root review

Reviewed the initial fixed-contract operator script and its eight isolated
contract checks once. No Critical findings. It limits mutation to the verified
nonunique `idx_identifier_resolution_candidates_target`; rejects wrong index
ownership/columns/uniqueness and missing candidate primary key; preserves full
ordered logical row digests, columns, constraints and both catalog index SQLs.
Parquet backup hashes, bidirectional row equality, checkpoint/reopen and actual
indexed-versus-sequential acceptance protect data and repair completion. No
raw source ingestion, migration replay or row replacement is part of repair.

The original persisted corruption is proven by live read-only evidence, not
reproduced synthetically. Fixture checks cannot establish live repair success.
Root will require the real affected target query and unchanged complete-table
evidence after reopening the production warehouse.

Focused1: seven passed; the real same-name transactional DROP/CREATE test failed
at COMMIT with DuckDB `BoundIndex::CreateDeltaIndex is not supported for this
index type`. Native peak 0.571788788GiB under 1GiB. No live mutation occurred.
This is an Important operational compatibility repair, anticipated by the task
brief. Implementer is adding two durable phases and verified incomplete-repair
recovery. Completion must remain false throughout any absent-index phase.
Accept that repair on implementer report and focused runtime evidence, as the
user directs; no repeat independent review unless a Critical issue emerges.

Do not execute the live repair until its changed/new operational checks pass
and the implementation is committed. Retain every existing full backup and
the new durable candidate-table backup. This task does not qualify source
completion, historical-vintage semantics, downstream data or a release.

Accepted Important repairs: two durable DROP/CREATE phases with explicit unsafe
state, checkpoint/reopen after DROP, and verified artifact-based recovery of an
absent or already-created target index. Focused2's four new refusal paths pass.
Its three positive paths exposed only a query-plan issue: ORDER BY with LIMIT
selected a sequential plan on the tiny fixture. The read-only scratch diagnostic
proved equivalent rows and actual IndexScan for LIMIT alone, with unchanged file
hash. The bounded lookup now sorts at most 1001 returned tuples in Python and
still requires default IndexScan. All three repaired positive paths pass in
focused3, peak 0.575878143GiB under 1GiB; scoped Ruff passes. Earlier unaffected
passing evidence stands. Implementer report and focused evidence are accepted;
no second independent review. Live repair remains the production acceptance.
