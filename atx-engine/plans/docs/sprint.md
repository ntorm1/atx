# Sprint style guide

**Purpose:** Tell future agents how to run a single sprint inside a module so the work survives context loss, the ledger stays canonical, and the sprint-close hands a clean baton to the next sprint.

**Read first:** [`module.md`](module.md) for the surrounding module structure. This file zooms in on one sprint within one module.

**Implementation quality:** [`implementation-quality.md`](implementation-quality.md) is mandatory for coding sprints. Subagent briefs must pass that standard through explicitly, not rely on taste or vague "clean code" wording.

**Canonical examples by quality:**
- **Best:** [`../p0/phase-4c-progress.md`](../p0/phase-4c-progress.md) — 4 units, marker + per-unit + close, residuals lifted, measured numbers in-line.
- **Also good:** [`../p0/phase-4-progress.md`](../p0/phase-4-progress.md) (longer Phase 4 v1 + 4b ledger), [`../p0/phase-3-progress.md`](../p0/phase-3-progress.md).

When in doubt, copy `phase-4c-progress.md`.

---

## What a sprint is

A **sprint** is one worktree's worth of forward motion in one module. Concretely:

- One git worktree, one branch (`worktree-phase-X-<theme>`).
- One progress ledger (`phase-X-progress.md`).
- 4–7 ledger units (`PX-N`) — realistic, not aspirational.
- Marker commit at kickoff, per-unit commits as work lands, close commit at end, sprint-merge into master.

A sprint is **not** a calendar period. It ends when the ledger closes — could be 2 days (Phase 4c was) or 4 weeks (Phase 4 v1 was). A sprint that runs past 4 weeks should split into v1 + Xb.

---

## The marker-commit pattern (mandatory)

Saved memory: *Sprint Sub-Agent Dispatch — Commit-on-Every-Phase. Marker commit + per-phase commits or partial work is lost when context dies.* This pattern is the contract.

### Why it exists

Sub-agents have shorter context than the orchestrator. If a unit's work isn't committed before the sub-agent returns, the next sub-agent picks up nothing — and if the orchestrator's context dies, the work is gone. **Every unit must commit before the sub-agent stops.**

### The three commit shapes

| Shape | When | Message format |
|-------|------|----------------|
| **Marker** | Sprint kickoff. Ledger created, scope frozen. | `docs(pX-0): open phase-X sprint ledger` |
| **Per-unit** | Each ledger unit lands. Code + tests + ledger row update. | `feat(pX-N): <one-line summary>` (or `fix`, `refactor`, etc.) |
| **Close** | Sprint end. Residuals lifted, ledger summary written, ROADMAP bumped. | `docs(pX-close): close phase-X — N units, M tests` |
| **Merge** | Worktree merges into master. | `merge: phase-X — <theme>` |

The marker SHA goes in the ledger header (`Base: master @ <SHA>`). The close SHA + merge SHA go in `ROADMAP.md`'s sprint-merge cell.

### Per-unit commit content

Each per-unit commit must include:

1. The implementation (source + headers).
2. The new test file(s) under `tests/src/` (auto-globbed).
3. The ledger row update for that unit — status, commit SHA *will be* added in a follow-up `docs(pX-N)` commit, or the row is updated in the same commit and the SHA recorded post-hoc.
4. (If applicable) `CMakeLists.txt` deltas.
5. For code units, API/comment/test quality must satisfy [`implementation-quality.md`](implementation-quality.md).

Anti-pattern: a per-unit commit that lands code but not tests. The next sub-agent has no signal that the unit is gate-passing.

---

## Progress ledger — required structure

Open the ledger as the marker commit. Use this skeleton (matches `phase-4c-progress.md`):

```markdown
# Phase X — Implementation Progress

**Worktree:** `.claude/worktrees/phase-X-<theme>`
**Branch:** `worktree-phase-X-<theme>`
**Base:** master @ `<SHA>`
**Started:** <YYYY-MM-DD>
**Source plan:** <link to phase-X-…-implementation-plan.md>
**Prior progress:** <link to predecessor ledger if any>

## Plan adjustments vs. the source plan

<one paragraph — what scope changed at kickoff and why>

Realistic scope for this sprint:

1. **PX-N** — <theme>. <one-paragraph justification>
2. **PX-N+1** — …

Defer to Phase Xd (or skip):

- **PX-M** — <one line + reason>

## Per-unit ledger

| Unit  | Status | Commit  | Notes |
|-------|--------|---------|-------|
| PX-N  | done   | `<sha>` | <one-paragraph summary including test counts> |

### PX-N measured <metric>

<tables with concrete numbers — compression ratios, throughput, etc. — when applicable>

### PX-N deferred residuals

- **<one line + plan>**

## Phase X sprint commits

| Commit  | Unit | Test counts |
|---------|------|-------------|
| `<sha>` | marker | — |
| `<sha>` | PX-N | <suite>/total/fail/skip |

**Phase X adds N new tests on top of <prior>'s M tests (total module footprint: K/0/0 across J binaries).**

## What Phase X proves / Next sprint priorities

<short prose closing — the baton handoff>
```

### Ledger row hygiene

- Each row is one paragraph, not a sentence. Include test counts (`suite N/0/0`), measured numbers when applicable, and any caveats (deferred sub-units, false-positive LSP noise, byte-format flags off by default).
- The `Notes` column is searchable — when a future agent asks "what did P4-8 actually integrate?", the ledger row answers without a code review.
- Update the row in the same commit as the implementation when possible. If the SHA isn't known until after the commit, follow up with a `docs(pX-N): record SHA` commit.

### What goes in the ledger vs the user reference

- **Ledger** (`phase-X-progress.md`) — what shipped, in what order, with what test counts and what residuals. Audience: future agents. Permanent record.
- **User reference** (`phaseN.md`) — public APIs, examples, known limitations. Audience: users of the feature. Written at sprint close.

If a number is in the ledger, don't duplicate it in the user reference. Link.

---

## Sub-agent dispatch

### Dispatch order

Sequential by default. Parallel only when units have **no** code dependencies — and even then prefer sequential because parallel sub-agents can't see each other's commits until they return.

`p0` Phase 4c was sequential (P4-4 → P4-6 → P4-8 → P4-19) because P4-6's selector consumed P4-4's compound codecs. `p1`'s PY-1 → PY-2 → PY-3 chain will also be sequential.

### What each sub-agent brief must include

A sub-agent has zero session context. Brief them with:

1. **Worktree path + branch** — they need to `cd` there.
2. **The unit's scope** — quoted from the implementation plan, not paraphrased.
3. **Acceptance criteria** — what tests must exist, what must pass, what must stay green.
4. **The marker-commit reminder** — verbatim: *"Marker-commit pattern is mandatory: commit before stopping or work is lost."*
5. **Expected commit message format** — `feat(pX-N): <summary>`.
6. **Predecessor commit SHAs** — what they're building on.
7. **What to write in the ledger row** — the structure above; "fill this in as part of your commit."

8. **Implementation quality standard** - paste the handoff block from [`implementation-quality.md`](implementation-quality.md), especially the `ats_orderbook.h` style reference and no-partial-stubs rule.

A sub-agent brief that omits any of these will produce work that doesn't fit the sprint's seam.

### Parallel dispatch — when it actually applies

Two units are parallelizable when:

- They touch **disjoint** files (no shared headers, no shared CMakeLists section).
- Neither references the other's symbols or test fixtures.
- Their tests run independently.

Phase 4c had no parallel opportunities. `p1` PY-2/PY-4/PY-5/PY-6/PY-8/PY-9 are parallelizable once PY-0+PY-1 are in. When dispatching parallel sub-agents, send all in a single tool-call message.

---

## Test surface is the gate

- **New unit ⇒ new test file under `tests/src/`.** The CMake glob picks it up automatically.
- **Existing tests must stay green per unit, not just at sprint close.** A unit that lands red regression is rejected — the sub-agent re-opens it.
- **`ATS_TEST(...)` framework, no `return 1;` in test bodies.** CI greps for it. The framework's macros do the right thing; bare `return` patterns escape the harness.
- **Test count is a load-bearing number.** Record it in the ledger row, in the sprint-commits table, and in the closing summary. Phase 4c's "153/0/0 across 13 binaries" is the kind of statement the next sprint reads.

If a test has to be skipped, mark it `skip` in the count and explain why in the row's notes. Never fudge counts.

---

## Sprint close ceremony

Run all of these *in one close commit* (or a tight series):

1. **Lift residuals into the module ROADMAP's future-work backlog.** Each deferred unit gets a one-liner with the carrying unit number and why.
2. **Update the module ROADMAP status table.** Replace `⏳ pending` with `✅ <commit>` or `⚠️ partial — <note>`.
3. **Bump `ROADMAP.md`'s `Last reviewed`.**
4. **Write the "What Phase X proves" + "Next sprint priorities" section** in the ledger. This is the baton-handoff to whoever opens the next sprint.
5. **Write or update `phaseN.md`** user reference if the sprint shipped user-facing surface.
6. **Resolve any "Strategic decisions — resolved by what shipped"** entries on the ROADMAP if the sprint settled a fork.
7. **Merge the worktree into master** (`--no-ff`) with a `merge: phase-X — <theme>` commit. Don't push unless the user explicitly asks.

A close that skips any of (1)–(4) leaves debt for the next agent.

---

## Carry-forward and unit numbering

When a unit gets retried in a later sprint:

- **Renumber, don't reuse.** Phase 1's `1.2 SIMD aggregations` was reverted; the retry shipped as Phase 2's **`2.9`** (`p0/ROADMAP.md:105`). Reusing `1.2` would have broken the historical commit-SHA chain.
- **Reference the predecessor.** The retry row says "(carry-forward from 1.2)" so the failure context is one click away.
- **Lift the failure analysis into the ROADMAP's status notes**, not just the ledger. The ROADMAP is what the next strategist reads.

---

## Common failure modes (lessons from past sprints)

### "We forgot to commit."

The single biggest cause of lost work in the project's history. Every sub-agent brief must end with the verbatim marker-commit reminder. The orchestrator should not let a sub-agent return without confirming the commit landed.

### "The status table drifted."

Phase 4 status was bumped at sprint close, not per-unit. Result: `p0/ROADMAP.md` had to be rewritten in a single pass to reconcile three sprints' worth of ledger movement against the strategy table. Keep the bridge current per-unit.

### "We deferred without lifting."

A residual that exists only in the ledger ("we'll do this in 4d") and not in the ROADMAP's future-work backlog evaporates when the ledger goes out of date. Lift on close — the ROADMAP backlog is the canonical "what's left."

### "The plan moved during the sprint."

Implementation plans are frozen at sprint kickoff. If reality forces a scope cut mid-sprint, write the cut into the ledger's "Plan adjustments" section, not into the plan file. The plan should be a fossil of what we thought going in; the ledger is the diary of what actually happened.

### "Validation never ran."

`p0`'s Phase 1/3/4 exit criteria all carry `*Validation pending — see Phase 5*` notes (e.g. `p0/ROADMAP.md:85`). It's better to ship the criteria with explicit validation-gate language than to claim "done" and have the next sprint discover the bench was never run. When you write an exit criterion, also write where the validation evidence will live.

### "LSP false positives sucked time."

Without `compile_commands.json`, LSP regularly flags missing includes / undeclared symbols that build cleanly under clang+Ninja. Don't chase these mid-sprint — verify against the actual build (PowerShell `cmake --build`), then ignore the LSP noise.

---

## Quick checklist — sprint kickoff

- [ ] Worktree created (`.claude/worktrees/phase-X-<theme>`).
- [ ] Branch matches worktree name.
- [ ] Marker commit landed; `Base: master @ <SHA>` in ledger.
- [ ] Implementation plan exists and is linked from the ledger.
- [ ] Module ROADMAP updated to point at the new ledger.
- [ ] Realistic scope picked (4–7 units); deferrals listed up front.
- [ ] Implementation plan references [`implementation-quality.md`](implementation-quality.md) for code units.
- [ ] First sub-agent brief includes the verbatim marker-commit reminder.

## Quick checklist — sprint close

- [ ] Every unit row has a `done`/`partial`/`deferred` status and a commit SHA.
- [ ] Test counts recorded in the ledger AND the sprint-commits table.
- [ ] Residuals lifted into module ROADMAP's future-work backlog.
- [ ] Module ROADMAP status table reflects what shipped.
- [ ] `Last reviewed` bumped on `ROADMAP.md`.
- [ ] `phaseN.md` user reference written or updated.
- [ ] "What Phase X proves" + "Next sprint priorities" written.
- [ ] Sprint merge commit landed on master (`--no-ff`).
- [ ] Strategic-decisions section updated if a fork resolved.
