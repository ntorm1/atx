# Tier-1 parity handoff — 2026-09-24 evening UTC

**Goal active: measured efficiency work supersedes the22:10host-capacity block.**
The user explicitly permits lower memory requirements following efficiency and
incrementality changes. Snapshot remains2026-09-20 and the production outcome
is incomplete. Continue on feat/tier1-parity. Preserve unrelated risk-test and
alpha-swarm files, every backup and **stash@{0}**; never apply/drop the stash.
All implementation/review work uses Codex; no external model spending or merge.

**Archive18 is LIVE at 23:53UTC**, root session55700, guard child1492, actual
warehouse Python child9152. It is the sole heavy process tree. Full scope,
replacement/force, snapshot2026-09-20, 512MB/one thread, 1.5GiB cap and 3GiB
data-disk floor. Source window2 passed 120.032seconds at 4/6GiB; initial guard
headroom was 6.266GiB physical/8.687GiB commit and disk free10.613GiB.
9,462 receipts across 15 lineage runs inventoried; retained fact fingerprints
started23:53:21UTC. Inspect existing process and fresh archive18 receipts before
any other heavy work. Do not launch archive18 again or repeat CC1/SA1 work.

**Archive17 is terminal FAILED (23:16:43UTC).**
It passed the complete retained fact and point fingerprint proof over 12,959
owner identities, 9,462 receipt targets and 14 lineage ancestors. Its peak was
1.223964691GiB under the unchanged 1.5GiB cap, using 512MB/one thread. It then
failed candidate replacement at COMMIT: `Failed to delete all rows from index`.
The latest dataset and stage ledgers are terminal failed; no recovery is due.
No new raw facts were written. Source capacity is proven for full resume proof
and candidate processing only, not the remaining full archive write workload.

Read-only `candidate-index-inspect1` proved a persisted index inconsistency:
target `SEC-COMPANYFACTS-UNRESOLVED-CIK-0001495229` returns zero rows by the
default target index, but one by forced sequential scan; PK lookup also finds
candidate `5b162a89-35e4-58eb-bfb5-ae11932bb2d4`. CC1 (313848a7) now repaired it
in the live warehouse at 23:45:47UTC: 256MB/one thread, native peak 0.829055786GiB
under 1GiB. All 12,959 actual candidate rows, logical digest, columns,
constraints and final index definitions are unchanged after checkpoint/reopen.
Default IndexScan now finds the same candidate as the sequential scan. Durable
658,019byte Parquet backup SHA256 is
e5d2872e18f38be95639634d3c964d3d3beeba6521dddb2054671c41daf81966, retained in
data/maintenance/candidate-target-index-20260924-1. All 12 full backups remain.

SA1 streaming submissions directory is committed as 9a7a3ea8, with one review.
First focused runtime: 19 passed, 3 failed. Important repairs closed Windows
fixture handles and removed stale Python read-ahead from live central-record
checks; all six affected cases now pass. Integration passed 45 cases, then two
repaired fixture/API snapshot cases passed separately. Actual retained archive
probe passed: all 991,042 directory entries, 985,667 main issuer names, 5,374
history entries, source hash/stat, ordered-name digest and four selected payloads
agree across build/reuse. Native peak 0.591762543GiB under 1GiB; sidecar is
202,481,664bytes. This qualifies directory efficiency, not full filing ingestion.
DG1 is committed as 4e0f59d8: optional disk-path/free-space floor, with 14 policy
checks and actual Windows preflight refusal verified. All upcoming warehouse
writes opt in to the data path and 3GiB floor; no backup cleanup is authorized.

CC1 uses two durable phases because this DuckDB build rejects same-transaction
DROP/CREATE. Verified interrupted-repair recovery and actual indexed lookup
passed focused checks; root accepted Important fixes without repeat review.
The live index prerequisite is closed; full source writes remain unqualified.

## Current accepted work

- UM1,4f9bfb90: real populated0314 upgrade split into checkpointed256MB sessions.
  All legacy row, constraint, lineage and PIT assertions preserved. One pass
  in154.60seconds, peak0.858974457GiB under1GiB. Root review and scoped Ruff clean.
  This closes PG1's last pending original head6 repair check; do not rerun it
  merely because earlier documents still describe it as pending.
- SM1,ca9d9a8a: full CompanyFacts proof releases connections after consumed
  aggregate phases; recorded budgets apply before reopen. Read-only close
  skips CHECKPOINT. No weakened receipt, lineage or SHA multiset checks.
- SI1,ec440e5a: reviewed migration0326 removes exactly three live-verified nonunique,
  nonprimary raw-source indexes; fresh bootstrap omits their creation.
  No source rows, columns or NOT NULL constraints are removed.
- Combined new acceptance:9isolated passes in2.38seconds, peak0.595833GiB/1cap;
  17integration passes in155.07seconds, peak0.840614GiB/1.5cap, including full
  schema/reentry, existing connection/lifecycle, damaged-proof and API checks.
  Root one independent review per task is clean. All three tasks are committed.
- MM1,06ab073f:512MB governed/restore startup budgets. Four real success/failure
  governance checks passed after an Important fixture repair, peak0.603GiB.
  Root one review accepted. Four pre-existing migration_admin Ruff findings
  are unchanged; new test clean. The actual production retry succeeded below.

## Verified production migration and source position

The first migrate0326 hit1.5GiB at final CHECKPOINT with the old1GB budget.
Governed restore returned schema322; catalog/checksum/estimated-count equality
and cleared locks were verified against its retained, hashed backup. Keep this
failure evidence. MM1's retry activation-migrate0326b SUCCEEDED512MB/one thread
under the SAME1.5GiB cap, peak1.209381104GiB. migration0326-verify1 confirms
applied323..326, zero optional source indexes, unchanged source NOT NULL
constraints, cleared lock and retained backup hash
4bdc9bce37411ecfa4f3665f3341f2c9c3dad0971304e3608519cc1317904e6b.
Backup:warehouse.duckdb.pre-migrate.20260924-225023.bak,12,883,341,312bytes.
Warehouse after migration:12,873,379,840bytes. Do not rerun the migration.
Activation's stage-only migrate receipt was skipped as already completed;
acceptance comes from actual persisted schema/backup proof, not zero-row output.

The earlier full inventory measured47,941,000facts/points and31,959,271bars;
canonical downstream surfaces remain empty. Archive17 passed retained source
proof but stopped on the index defect above. Archive16 recovery remains complete.

Actual terminal source predecessors:

- CompanyFacts:dd52e571-5786-42c5-bfaa-d7122b033912 (archive17).
- Submissions:04cf947d-53bb-49b7-a276-b3c74a2a52c8; all forms/all CIKs/history,
  same retained archive and batch50 required.

source-archive17-lowmemory-window2 passed120.016seconds at4GiB physical/6GiB
commit floors. The source experiment uses1.5GiB process-tree cap and
unchanged1.5GiB physical/3GiB commit emergency stops. It does not waive data or
release thresholds. See low-memory-resume-profile-2026-09-24.md for scope.
12backup files remain; C: free13,197,230,080bytes at22:54UTC. Preserve all
backups and use backup-keep100. Do not infer a process from a running ledger.

## Next action

1. Monitor the existing full archive18 to a terminal state and inspect actual
   dataset/stage results. It already uses the verified archive17 predecessor,
   512MB/one thread, 1.5GiB cap, full replacement scope and every proof check.
   No source completion/new facts are claimed yet. No concurrent heavy job.
2. Full submissions resume, scoped CVX earnings source, full-universe run5
   from statement_points, force and16sequential reconciliation shards. Other
   stage capacity profiles still need evidence before being lowered.
3. Execute actual qualified EPS/desk SQL, coverage/provider/all-quality checks,
   PIT/survivorship and incremental recovery evidence. Publish only after the
   unchanged release thresholds and full non-slow gate; verify manifests/hashes.

Use production-resume-sequence-2026-09-21.md for exact commands/fresh artifacts.
The original head6 had110passes/1defaultslow skip and two now-repaired test
integration failures. Its passing evidence stands for unchanged components.
Existing whole-branch review is already resolved; review new tasks once and
re-review only Critical repairs. Ask before merging to main.

ED1,44fb6392, delivered read_quarterly_eps_growth.py over IQ2 exact selected-leaf
qualification. Live cvx-eps-desk1 stopped at missing323columns. After migration,
cvx-eps-desk2 passed schema preflight but returned issuer_ownership_unresolved,
zero numeric states and zero metric definitions, exit2, peak0.642475GiB. This
is a measured remaining materialization/identity gap. No numeric CVX acceptance,
alpha result or release is claimed.
Preserve and remind the user about stash@{0}.
