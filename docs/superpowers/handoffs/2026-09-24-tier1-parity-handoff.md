# Tier-1 parity handoff — 2026-09-24 evening UTC

**Goal active: measured efficiency work supersedes the22:10host-capacity block.**
The user explicitly permits lower memory requirements following efficiency and
incrementality changes. Snapshot remains2026-09-20 and the production outcome
is incomplete. Continue on feat/tier1-parity. Preserve unrelated risk-test and
alpha-swarm files, every backup and **stash@{0}**; never apply/drop the stash.
All implementation/review work uses Codex; no external model spending or merge.

Current writer: activation-migrate0326 started under1.5GiB cap after all source
changes were committed; root session95160, guard child4216. Inspect its fresh
terminal receipt/process state before another warehouse workload. A running
process or partial log is not migration acceptance.

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

## Runtime position before the new migration

The source-index-catalog1 read-only receipt confirms schema0322 and all three
expected optional indexes. Warehouse12,883,341,312bytes and mtime unchanged.
The earlier full inventory measured47,941,000facts/points and31,959,271bars;
canonical downstream surfaces remain empty. No source write has occurred yet
in this efficiency continuation. Archive16 recovery remains complete.

Actual terminal source predecessors:

- CompanyFacts:513cfbbc-096a-4186-9666-b6cc5170c4ad.
- Submissions:04cf947d-53bb-49b7-a276-b3c74a2a52c8; all forms/all CIKs/history,
  same retained archive and batch50 required.

source-migration-lowmemory-window1 passed120.031seconds at4GiB physical/6GiB
commit floors. The new guarded experiment uses1.5GiB process-tree cap and
unchanged1.5GiB physical/3GiB commit emergency stops. It does not waive data or
release thresholds. See low-memory-resume-profile-2026-09-24.md for scope.
10backup files remain; C: free38,685,995,008bytes before migration. Preserve all
backups and use backup-keep100. Do not infer a process from a running ledger.

## Next action

1. Finish the running governed migrate0323..0326 under1.5GiB with the
   existing1GB/one-thread migration budget. Inspect terminal receipt, backup
   evidence and actual applied schema/index catalog before another workload.
2. Full retained CompanyFacts archive17 at512MB/one thread,1.5GiB process cap,
   force replacement/archive_members and actual predecessor above. Its full
   proof and new commits are the production capacity measurement; do not repeat
   an extra full SHA scan merely as a read-only benchmark. No CIK filter.
3. Full submissions resume, scoped CVX earnings source, full-universe run5
   from statement_points, force and16sequential reconciliation shards. Other
   stage capacity profiles still need evidence before being lowered.
4. Execute actual qualified EPS/desk SQL, coverage/provider/all-quality checks,
   PIT/survivorship and incremental recovery evidence. Publish only after the
   unchanged release thresholds and full non-slow gate; verify manifests/hashes.

Use production-resume-sequence-2026-09-21.md for exact commands/fresh artifacts.
The original head6 had110passes/1defaultslow skip and two now-repaired test
integration failures. Its passing evidence stands for unchanged components.
Existing whole-branch review is already resolved; review new tasks once and
re-review only Critical repairs. Ask before merging to main.

ED1,44fb6392, delivered read_quarterly_eps_growth.py over IQ2 exact selected-leaf
qualification. Live cvx-eps-desk1 correctly stopped at missing323columns, exit2,
peak0.643GiB, with no numeric rows. After migration it still needs actual
materialization. No numeric CVX acceptance, alpha result or release is claimed.
Preserve and remind the user about stash@{0}.
