---
name: vol-dag
description: Orchestration playbook for atx-vol development — decide single-lane vs DAG fan-out, run the vol-sprint workflow, keep the ledger honest. Use for any non-trivial atx-vol feature/refactor/sprint request, or when the user says "vol dag", "fan out", "parallel lanes", or "sprint".
---

# vol-dag — atx-vol orchestration playbook

## Decide the shape FIRST (token economics: multi-agent ≈ 15× single-agent)

| Situation | Shape |
|---|---|
| One file / one suite / bugfix | Inline, or one `vol-builder` agent. No workflow. |
| One coherent work stream, heavy | Single `vol-builder` (pool tree) + one `vol-reviewer` pass. No workflow. |
| ≥2 file-disjoint, compile-independent streams | `Workflow { name: 'vol-sprint', args: { task, base? } }` |
| Unknown partition | Run `vol-planner` alone first; fan out only if it returns ≥2 lanes. |

Disjoint means: no lane needs another lane's NEW interface to compile. Shared files (module CMakeLists, umbrella `include/atx/vol/api/vol.hpp`, CHANGELOG.md) belong to at most one lane or the gate stage.

## The DAG (vol-sprint workflow) — broker-only

Every lane is a v3 lane-broker capability. No stage leases a worktree, runs a
shell, or names a physical path: the workflow opens and releases each lane and
the worker only ever holds an opaque capability.

```
Freeze base SHA (vol-ref-auditor, broker ref_resolve)
  → Plan (vol-planner, broker repo_search/repo_read; every lane mandatory)
  └─ per lane, sequentially:
       lane_open (vol-lane-opener, operation_id=sprint_build, scope_paths pinned to the lane)
       → Build   (vol-builder: patch_apply → fixed gate IDs → lane_commit sprint_lane)
       → Review  (vol-reviewer, fresh context, broker commit_inspect, exact-SHA verdict)
       → Fix     (vol-builder, same capability, only on BLOCK, one round)
       → Re-review (fresh reviewer, mandatory after every Fix)
       → lane_release (vol-lane-releaser)
  └─ Barrier: all mandatory lanes DONE + fresh APPROVE
     → lane_open (operation_id=sprint_integration)
     → Gate (vol-verifier: lane_integrate the exact reviewed SHAs, then the exact
       changed-closure gate IDs against the sealed integrated SHA/tree)
     → lane_release
```

Hard limits: ≤4 lanes. Any incomplete/blocked/non-APPROVE lane fails before
integration, and its lane is released either way. Second BLOCK after Fix is
final. Gate FAIL carries the broker's pasted output; never "mostly passed".

Neither sprint operation is `finalize`-capable, so no sprint lane can move
`refs/heads/oracle/canonical`; only the oracle Ratchet can, through its own CAS.
The sprint appends nothing to the ledger — `sprint_integration` cannot commit at
all, and the oracle Ratchet is the single writer of scorecards and memory.

**Holdout is invisible to vol-sprint.** It is a tuning stage: a holdout-tainted
`task` is refused before any dispatch, its gate set is a closed registry with no
holdout or Measure gate in it, and the broker itself refuses `holdout_mode_a`,
`holdout_mode_b` and `rel_avx2_speed` outside `operation_id=ratchet`.

For the oracle loop, each scoped file has a closed `gate_closure` checked against
the workflow-owned exact path/path-pattern target/test registry. Mandatory gates
cannot be substituted; planner additions are additive. Unknown paths, unrelated
mappings, omissions, and extra command receipts fail. The workflow mechanically derives
affected anchored unit tests, hypothesis OracleBench tests,
required aggregate smoke/tune scorecards, quiet pinned speed, and header-only
scoped PCH-off targets. Full regression/release suites and broad repository gates
are intentionally outside this loop.

Unit regexes are real fully-qualified `gtest_discover_tests` names from the closed
registry. They run through the production targeted adapter and must return positive
typed executed/passed counts; plain or zero-test success text is invalid.

## Invocation

```
Workflow { name: 'vol-sprint', args: { task: '<verbatim user ask + constraints>', base: 'main' } }
```

Pass the user's ask verbatim plus known constraints, minus anything about the
holdout cohort — a task naming it is refused. The workflow derives a run identity
that is a pure function of the frozen base SHA, the task text and `run_key`, so a
resumed run reuses the same lanes instead of forking new ones. The broker starts
the keeper behind `lane_open`; every lane reports the exact keeper PID and
process-start receipt the broker returned. On workflow death, check
`lease-worktree.ps1 -Status`; v3 status reports `run_id` and durable
process/keeper state. Investigate before `-RecoverStale`; normal release always
supplies the acquiring `-RunId`.

## Memory duties (main thread, after every run)

1. vol-sprint no longer writes the ledger (its integration lane cannot commit). For an oracle iteration the Ratchet commits it; for a standalone sprint append the gate outcome yourself (append-only, `date | area | fact | source`).
2. Durable narrative (design decisions, measured numbers) → `atx-vol/sprints/YYYY-MM-DD-<topic>-sprint.md`.
3. Before ANY new task: grep the ledger + `sprints/` for prior art. Re-deriving a known trap is a harness failure.
