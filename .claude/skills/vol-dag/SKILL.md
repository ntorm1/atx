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

## The DAG (vol-sprint workflow)

```
Plan (vol-planner, 1)
  └─ per lane, pipelined (no barrier between lanes):
       Build (vol-builder, leased pool tree, TDD ladder)
       → Review (vol-reviewer, fresh context, diff-only, verify-don't-trust)
       → Fix (vol-builder, same warm tree, only on BLOCK, one round)
  └─ Gate (vol-verifier, barrier): merge lanes → atx_vol_fast full → hygiene →
     ci/run_all_gates.ps1 → release ALL leases → append docs/LEDGER.md
```

Hard limits: ≤4 lanes (pool size). Second BLOCK after the fix round = surface to the user, don't loop. Gate FAIL = report with evidence; never "mostly passed".

## Invocation

```
Workflow { name: 'vol-sprint', args: { task: '<verbatim user ask + constraints>', base: 'main' } }
```

Pass the user's ask verbatim plus known constraints; the planner reads code itself — don't pre-chew. On workflow failure mid-run: check `lease-worktree.ps1 -Status` for orphaned leases before rerunning (resume with `resumeFromRunId` where possible).

## Memory duties (main thread, after every run)

1. Confirm the verifier appended `atx-vol/docs/LEDGER.md`; if it died before that, append the gate outcome yourself (append-only, `date | area | fact | source`).
2. Durable narrative (design decisions, measured numbers) → `atx-vol/sprints/YYYY-MM-DD-<topic>-sprint.md`.
3. Before ANY new task: grep the ledger + `sprints/` for prior art. Re-deriving a known trap is a harness failure.
