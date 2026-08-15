# atx-vol DAG development harness — design

Date: 2026-08-15. Status: v1 installed. Goal: faster dev loops, durable long-term memory, real parallelism for atx-vol work, built on what the repo already has (worktree pool, `atx-build.ps1`, `.agents/` role docs, `.superpowers/sdd/` brief convention) instead of replacing it.

## Research grounding (why this shape)

- Anthropic taxonomy: predefined-path **workflows** beat free-form agents for structured dev work; add agency only where decomposition is genuinely dynamic (the Plan stage). [anthropic.com/engineering/building-effective-agents]
- Multi-agent ≈ 15× token cost; pays only for genuinely parallelizable slices, and "most coding tasks involve fewer truly parallelizable tasks than research" — hence the ≤4-lane cap, disjointness rule, and single-lane default. [anthropic.com/engineering/built-multi-agent-research-system]
- Duplicate work comes from vague delegation → lane briefs carry explicit files-in-scope **and files-forbidden**, output contracts, falsifiable done criteria.
- Fresh-context adversarial review (reviewer sees diff + criteria, not writer's reasoning) beats self-grading; reviewers report everything, filtering happens downstream. [code.claude.com/docs/en/best-practices]
- Memory: small auto-loaded CLAUDE.md spine + grep-retrieved curated files beats embedding stores for code work; aggressive summarization loses late-relevant context, so the ledger is append-only one-liners with pointers. [anthropic.com/engineering/effective-context-engineering-for-ai-agents]
- File collisions: per-agent worktrees. Repo already solved this better than stock `isolation: worktree` — the warm pool (5 s no-op vs 132 s cold, submodule-safe) — so builders lease pool trees instead.

## Components

| Piece | Path | Role |
|---|---|---|
| Memory spine | `CLAUDE.md` (root), `atx-vol/CLAUDE.md` | Auto-loaded facts: build wrapper, pool, ladder, labels, gates, traps, memory protocol |
| Fact ledger | `atx-vol/docs/LEDGER.md` | Append-only `date \| area \| fact \| source` lines; grep before re-deriving; verifier appends on gate |
| Contracts | `.agents/harness/TEMPLATES.md` | Brief / report / review formats (formalizes `.superpowers/sdd/` convention) |
| Agents | `.claude/agents/vol-{planner,builder,reviewer,verifier}.md` | DAG stage roles; builder/reviewer read `.agents/cpp/agent.md` at start (single source of truth, not duplicated into prompts) |
| DAG | `.claude/workflows/vol-sprint.js` | Plan → pipelined Build/Review/Fix per lane → barrier Gate; JSON-schema outputs at every edge |
| Playbook | `.claude/skills/vol-dag/SKILL.md` | Shape decision (inline vs single-lane vs DAG), invocation, memory duties |
| Permissions | `.claude/settings.json` | Allowlist for wrapper/lease/git-read commands — fewer prompts, faster lanes |

## DAG semantics

- **Pipeline, not barriers**, between Build→Review→Fix: lane A reviews while lane B builds; wall-clock = slowest lane chain.
- **Barrier only at Gate** (needs all lanes): merge APPROVED lane branches → `atx_vol_fast` full → `hygiene` preset (when headers moved) → `atx-vol/ci/run_all_gates.ps1` (when semantics moved) → release ALL pool leases → ledger append.
- **Concurrency = pool size (4)**. Builder without a lease reports BLOCKED rather than touching the main checkout. Leases held through Fix; released by verifier (warm-tree reuse for fix round).
- **One fix round.** Second BLOCK surfaces to the user — no unbounded loops.
- **Evidence discipline end-to-end**: builder claims void without pasted output; reviewer verifies rather than trusts; verifier is the only authoritative pass/fail; golden-replay reported SKIPPED (not passed) when the licensed corpus is absent.

## Speed levers

1. Warm pool trees (5 s/27 s vs 132 s cold) as the parallel substrate.
2. Ladder discipline in every builder prompt: `check <TU>` → targeted `build` → `-Ctest -R` — full label only at gate.
3. Permission allowlist removes per-command prompting in lanes.
4. Pipelined review overlaps QA with implementation.
5. Ledger + CLAUDE.md kill re-derivation (build traps, measured numbers, past decisions available at session start / one grep away).

## Error handling

Planner returns 0 lanes → workflow throws. Lane BLOCKED/died → excluded from integration, reported. Merge conflict → verifier stops, names conflicting lanes/files (planner partition failure). Gate FAIL → reported with evidence, branches left for inspection. Workflow death mid-run → `lease-worktree.ps1 -Status` for orphaned leases; `resumeFromRunId` to resume cached stages.

## Not in v1 (deliberate)

Stop-hook enforcement gates (documented convention first; hooks if drift shows), CI runner integration (repo has none for C++), auto ledger compaction (revisit when file is large), headless `claude -p` pipeline (Workflow tool covers it interactively), atx-engine generalization (copy the pattern once proven on atx-vol).
