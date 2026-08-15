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
Freeze base SHA → Plan (vol-planner, 1; every lane mandatory)
  └─ per lane, pipelined (no barrier between lanes):
       Build (vol-builder, run-owned heartbeat lease, TDD ladder)
       → Review (vol-reviewer, fresh context, exact-SHA verdict)
       → Fix (vol-builder, same warm tree, only on BLOCK, one round)
       → Re-review (fresh reviewer, mandatory after every Fix)
  └─ Barrier: all mandatory lanes DONE + fresh APPROVE
     → release lane leases → acquire NEW isolated integration lease
     → merge/report exact reviewed SHAs → exact changed-closure gates → ledger → release integration lease
```

Hard limits: ≤4 lanes. Any incomplete/blocked/non-APPROVE lane fails before
integration. Second BLOCK after Fix is final. Gate FAIL carries pasted output;
never "mostly passed" and never integrate in `C:\atx`.

For the oracle loop, each scoped file has a closed `gate_closure`. The workflow
mechanically derives affected anchored unit tests, hypothesis OracleBench tests,
required aggregate smoke/tune scorecards, quiet pinned speed, and header-only
scoped PCH-off targets. Full regression/release suites and broad repository gates
are intentionally outside this loop.

## Invocation

```
Workflow { name: 'vol-sprint', args: { task: '<verbatim user ask + constraints>', base: 'main' } }
```

Pass the user's ask verbatim plus known constraints. The workflow derives run-unique
lane/integration branches and heartbeat IDs. Heartbeat acquisition starts an
independent continuously renewing keeper; every lane reports its exact keeper
PID/process-start receipt. On workflow death, check `lease-worktree.ps1 -Status`;
v3 status reports `run_id` and durable process/keeper state. Investigate before `-RecoverStale`; normal
release always supplies the acquiring `-RunId`. Resume the cached run where possible
rather than dispatching a duplicate.

## Memory duties (main thread, after every run)

1. Confirm the verifier appended `atx-vol/docs/LEDGER.md`; if it died before that, append the gate outcome yourself (append-only, `date | area | fact | source`).
2. Durable narrative (design decisions, measured numbers) → `atx-vol/sprints/YYYY-MM-DD-<topic>-sprint.md`.
3. Before ANY new task: grep the ledger + `sprints/` for prior art. Re-deriving a known trap is a harness failure.
