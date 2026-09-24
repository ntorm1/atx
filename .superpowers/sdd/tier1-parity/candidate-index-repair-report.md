# CC1 — implementation report

Root acceptance: the three repaired positive paths passed in focused3, 4.54s,
native peak 0.575878143GiB under 1GiB. Earlier six unaffected checks and four new
recovery refusal checks remain valid, covering all 13 current cases. Scoped
Ruff passes. Root's single independent review is complete; Important fixes are
accepted on the implementer report plus focused evidence. Ready to commit and
perform the guarded, backed-up live repair. No live repair is yet claimed.

Status: Important repairs after the focused runs, ready for root-controlled
verification of the three remaining repaired paths. The implementer ran one
explicitly authorized 64 MB read-only scratch diagnosis under the memory guard;
all tests and live work remain root-controlled. Root completed one independent static review before focused1;
no Critical or Important findings were found in that review.

## Repair behavior

`rebuild_candidate_target_index.py` validates the candidate primary key and the
exact two expected named index definitions, including main-schema ownership,
nonunique/nonprimary flags, indexed expressions, and CREATE INDEX SQL. Unexpected
index ownership, uniqueness, expressions, extra indexes, or missing primary key
fail before backup or DROP.

The utility retains full table DDL, columns, constraints, and both index
definitions in a durable JSON manifest. It checkpoints and exports all candidate
rows to Parquet, fsyncs the artifact, streams its SHA256, and checks both ordered
logical row digest/count and bidirectional EXCEPT ALL equality against the live
table. It then commits only the target-index DROP, checkpoints/reopens, and
verifies unchanged rows/catalog with precisely that index absent. Exact CREATE
INDEX runs in its own transaction. It never changes the primary-key index or
the source-key index.

After COMMIT it verifies every row and catalog contract, checks the formerly
failing target through an actual default IndexScan and a forced sequential
query, checkpoints, closes, reopens read-only at the same resource budget, and
repeats verification. Before/after manifests and the retained backup are hashed.
Only this completed sequence sets `safe_to_resume_source=true`.

An inspect-only mode opens the warehouse read-only and records current catalog,
whole-table streaming row evidence, and the bounded target lookup. It performs
no backup, mutation, or source authorization. New repairs require fresh artifact
directories. Failures retain explicit phase/error state and backups. There is
no implicit retry. Explicit `--resume-artifact-dir` validates the original
manifest and Parquet hashes, unchanged full row evidence and exact catalog
before completing an absent target index. If CREATE already committed, resume
only verifies the existing index. Any other drift refuses mutation. Previous
failed state is retained in timestamped history; artifact-preflight refusals get
their own receipt without overwriting the original state.

## Focused checks supplied

- Real DuckDB two-phase same-name index rebuild, preservation of all rows,
  candidate clocks/provenance, unrelated table, primary key and NOT NULL rules;
  then actual DELETE/reinsert of the same candidate through COMMIT and reopen.
- Read-only inspection leaves the database file hash unchanged, and existing
  artifact directories are refused.
- Unique, wrong-owner, wrong-column, and missing-primary-key contracts are
  refused without deleting an index or creating a data backup.
- Failed CREATE leaves only the target index absent with every row intact and
  a complete backup/unsafe receipt. Explicit verified resume restores its exact
  definition and confirms indexed lookup after checkpoint/reopen.
- Resume refuses changed Parquet/manifest hashes, row provenance, or other
  catalog drift without creating the absent target index. An interruption after
  CREATE already committed resumes through verification alone.
- A backup missing another provider's candidate fails exact row verification.

These are small persistent DuckDB checks without full warehouse schema bootstrap.
They cannot reproduce the production index's historical corruption. The live
before/after target lookup and resumed source COMMIT remain required evidence.

## Evidence and limitations

Root's live `candidate-index-inspect1.json` establishes the persisted target
index/table discrepancy; it does not prove the original mechanism that damaged
the index. No loader update, error suppression, source skipping, constraint
removal, schema migration, or retention change is proposed.

Focused1: seven passed, one failed at same-name transactional DROP/CREATE COMMIT
with `BoundIndex::CreateDeltaIndex is not supported for this index type`; native
peak 0.571789 GiB under 1 GiB. No live write was attempted. This concrete engine
limitation prompted the two-phase Important repair described above, which was
the contingency anticipated in the original proposal. The old atomic-DDL
rollback claim is superseded.

Focused2: four recovery-refusal cases passed, three positive repair/recovery
cases failed the mandatory IndexScan acceptance, six unaffected tests were
deselected; peak 0.574188 GiB under 1 GiB. Authorized
`candidate-index-lookup-diagnostic1.json` examined the failed scratch file at
64 MB/one thread (peak 0.548080 GiB under 1 GiB) and verified its file hash was
unchanged. Default and sequential lookups returned the same correct row for all
four query variants. Only combined SQL ORDER BY plus LIMIT selected a sequential
plan; plain SQL, LIMIT alone, and ORDER BY alone used IndexScan.

The lookup repair preserves LIMIT 1001, sorts the bounded tuples in Python,
and still requires an actual default IndexScan. It does not weaken production
acceptance. Resume now clears obsolete current error fields after retaining the
original state history. Two datetime UTC lint fixes are also applied.

Root should rerun only the three repaired positive paths: `two_phase_rebuild`,
`failed_recreation_retains`, and `resume_after_create`. Other focused1/focused2
passing evidence remains applicable. Corrected focused receipt: pending. Live
repair: pending. Full archive ingestion and downstream data readiness remain
incomplete.
