# Module style guide

**Purpose:** Tell future agents how to design and document a multi-sprint module under `ats-core/docs/pN/` so it matches the shape established by `p0` (kdb+/Databricks trajectory) and `p1` (Python first-class client).

**Read first:** [`p0/ROADMAP.md`](../p0/ROADMAP.md). It is the canonical example. This file abstracts the pattern out of it; when in doubt copy `p0`.

---

## What a module is

A **module** is a top-level strategic effort owned by one ROADMAP and broken into multiple sprints (phases). Examples:

- `p0` — kdb+/Databricks trajectory. 4 phases shipped (Phase 1 SIMD/UX, Phase 2 push+bitemporal+RBAC, Phase 3 replication+dist-query, Phase 4 proprietary storage), Phase 5 (validation) proposed.
- `p1` — Python first-class client. PY-0..PY-10 single-sprint module; may grow follow-up sprints.

A module is **not** a single feature. If your work fits in one sprint with one progress ledger, it belongs as a phase inside an existing module — not a new module.

A module **is** appropriate when:

- The scope spans >3 sprints' worth of work, OR
- The strategic identity is distinct from every existing module (e.g. `p0` server engine vs `p1` client surface), AND
- It has its own anti-roadmap — things you are deliberately *not* doing.

When in doubt, open it as a phase inside the closest existing module first. Promote to its own module only when the phase ledger starts pulling in unrelated strategic decisions.

---

## Directory layout

```
ats-core/docs/pN/
├── ROADMAP.md                                     # Module master. Required.
├── <market-or-source-audit>.md                    # Optional. Read-this-first source for the strategic positioning.
├── phase-X-<theme>-implementation-plan.md         # Per-phase plan. One per major phase.
├── phase-X-progress.md                            # Per-sprint ledger. One per sprint.
├── phase-Xb-progress.md                           # Follow-up sprints reuse the same shape.
├── phaseN.md                                      # User-facing reference — what shipped, public APIs, examples.
```

`p0` is the canonical example with all five doc types present:

| File | Type |
|------|------|
| `ROADMAP.md` | module master |
| `kdb-databricks-alignment-review.md` | source audit |
| `phase-2-push-bitemporal-hardening-implementation-plan.md` | implementation plan |
| `phase-2-progress.md` (and `phase-3-progress.md`, `phase-4-progress.md`, `phase-4c-progress.md`) | sprint ledger |
| `phase2.md` (and `phase3.md`) | user reference |

`p1` is in mid-flight: ROADMAP + implementation plan exist; ledger and user reference are pending until the first sprint opens.

---

## Required `ROADMAP.md` sections

Copy the section ordering from [`p0/ROADMAP.md`](../p0/ROADMAP.md). Every module ROADMAP must have:

### 1. Header

```markdown
# Module N — <Identity> (`pN`)

**Last reviewed:** <YYYY-MM-DD>
**Started:** <YYYY-MM-DD>   (or "planned; no sprint open yet")
**Source:** <one-line provenance — audit doc, research, user request>
**Goal:** <one sentence — what does the module exist to do?>
```

The `Last reviewed` date is bumped every time the ROADMAP is touched. The `Started` date is the marker commit of the first sprint, or `"planned; no sprint open yet"` until then.

### 2. Companion docs index

A table of every other doc in the module with a one-line "what it covers." If a doc is pending (created at sprint kickoff or close), list it under a "Pending" subsection so future agents know what's missing.

### 3. Sibling module cross-reference

One line linking to sibling modules. Example: `Sibling modules: **p1** — Python first-class client. See [../p1/](../p1/).`

### 4. Strategic positioning

A short prose section + one comparison table. The prose says **what identity the module is claiming**; the table contrasts the target against the closest comparables. Example from `p0`:

| Dimension | kdb+ | Databricks | ats-core target |
|-----------|------|-----------|-----------------|

This is the section you go back to when scope creep argues with you. If the proposed work doesn't fit the identity, it doesn't ship in this module.

### 5. Phase 0 — Foundation (if applicable)

If the module builds on capabilities that existed before the module was opened, list them under "Phase 0 — Foundation (pre-roadmap baseline)" with a `Solid` and `Critical gaps` split. The gaps must explicitly map to the phases that close them. See `p0/ROADMAP.md:42` for the canonical example.

### 6. Per-phase status tables

One section per phase with:

- **Theme** — one sentence.
- **Sprint merge** — commit SHA(s) of the merge commit(s).
- **Plan + ledger** — links to the implementation plan and progress ledger.
- **Status table** — one row per ROADMAP item with columns: `# / Item / Effort / Status / Ledger unit / Files (or Notes)`. Status is `✅` (done with commit SHA) / `⚠️` (partial — explain) / `⏳ pending`.
- **Exit criteria** — one paragraph; what does "done" mean? Mark explicitly if the criteria are unvalidated.

### 7. ROADMAP-item ↔ ledger-unit mapping (when they diverge)

Phase 4 grouped 10 ROADMAP items into 13 ledger units across 3 sprints. The mapping table at `p0/ROADMAP.md:153` is load-bearing — write it as you ship, not at the end. Without it, "what shipped in P4-7?" becomes a forensic exercise across three progress files.

If your phase's ROADMAP items map 1:1 to ledger units (typical for short modules like `p1`), you can skip this table.

### 8. Future-work backlog

When a sprint defers items into a successor sprint or a future module, list them in the ROADMAP under "Phase Xd / future-work backlog." Each item gets a one-liner with the unit number it carried in the ledger and why it deferred. See `p0/ROADMAP.md:185` for the Phase 4d example.

### 9. Anti-roadmap

The "we're explicitly NOT doing X" list. Numbered, each with a one-sentence reason. This is the second place scope-creep arguments go to die. See `p0/ROADMAP.md:202`.

### 10. Strategic decisions — resolved by what shipped

When the original positioning had open forks (server vs embedded? on-prem vs SaaS?), revisit them at sprint-close cadence and write down which one shipped. The act of writing the decision down removes the fork. See `p0/ROADMAP.md:216`.

### 11. Recommended sequencing

"If you can only do one slice, do X. If two, add Y. If three, add Z." This is the section a future agent reads when they have limited time and need to know what to drop. See `p0/ROADMAP.md:241`.

### 12. (Optional) Reference back to module style

If you're in a module other than `p0`, finish with: `Sprint discipline: same as p0 — see [../p0/ROADMAP.md#sprint-discipline](../p0/ROADMAP.md#sprint-discipline-lessons-from-phase-3--4--4b--4c).` Don't duplicate the discipline — link.

---

## Cross-module dependencies

When a module's work depends on another module's capability, call it out explicitly in ROADMAP positioning. Two patterns:

**Pattern A — depends-on already-shipped:** name the upstream phase + commit and mark the downstream item as "depends on Px Phase N (PX-Y) — already shipped."

`p1/ROADMAP.md:40`:
> **PY-7 push subscriptions** depends on **`p0` Phase 2.1 (P2-1)** push executor — already shipped.

**Pattern B — depends-on not yet shipped:** mark the downstream item `⏳ blocked-on PX-Y` and add a one-line note in the upstream module's residuals so the upstream agent sees the dependency.

Modules are independent; the dependency edge lives in the depending module's ROADMAP, not the upstream.

---

## Numbering convention

- **ROADMAP items** are strategy-grain: `1.1, 1.2, … 4.10`. Stable across sprints.
- **Ledger units** are sprint-grain: `P1-0, P1-1, … P4-19`. Allocated at sprint kickoff.
- **Bridge:** the mapping table in §7 (ROADMAP-item ↔ ledger-unit). Keep it current per-unit, not per-sprint.

For client/edge modules where every sprint is a self-contained surface (e.g. `p1`), the convention may collapse to `PY-0..PY-10` with no separate ROADMAP-item numbers. That's fine — keep the status table on the units directly.

Never renumber ledger units after they ship. If you skip a number, leave it skipped (P4-5 is permanent backlog).

---

## When to bump `ROADMAP.md`

- **Sprint kickoff.** Bump `Last reviewed`, link the ledger, update sprint-merge row.
- **Sprint close.** Update status markers (`⏳ → ✅`/`⚠️`), record commit SHAs, lift residuals into the future-work backlog, add new entries to "Strategic decisions resolved" if the sprint resolved a fork.
- **Strategic pivot.** When the identity in §4 changes, the ROADMAP gets rewritten (not edited). Save the prior version in `archive/` first.

Don't bump on unit close — that's the ledger's job. ROADMAP cadence is sprint-level.

---

## Starting a new module

1. Pick the module name. `pN` where N is the next free integer.
2. Create `ats-core/docs/pN/`.
3. Copy `p0/ROADMAP.md` as a template; rewrite the §1 header, §4 strategic positioning, and §9 anti-roadmap. Strip the per-phase tables — those fill in as sprints land.
4. Write the first phase's `phase-X-<theme>-implementation-plan.md` using the per-unit decomposition style from `p0`'s phase-2/3/4 plans.
5. Add the new module to `p0/ROADMAP.md` companion-doc / sibling section so the cross-link is bidirectional.
6. Open the first sprint per [`sprint.md`](sprint.md).

---

## Anti-patterns

Things past modules got wrong; don't repeat them.

- **Inventing a new module to escape the anti-roadmap of an existing one.** If `p0` says "no Q language," opening `p2-q-language` to do it anyway just hides the violation. Either argue the anti-roadmap update on `p0`, or accept the constraint.
- **One ROADMAP per phase.** The ROADMAP is the *module* master; phases are sections inside it. A separate ROADMAP per phase fragments the strategic positioning and breaks the residuals-roll-forward pattern.
- **Skipping the source audit.** Modules without a clear `<market-or-source-audit>.md` end up arguing positioning during sprint planning. Write the audit *before* opening the first sprint.
- **Status-marker debt.** Letting the ROADMAP status table drift more than one sprint behind reality is how a module loses the trust of future agents. The numbering bridge is load-bearing — keep it current per-unit.
- **Documenting hopes as commitments.** "Phase 5 will validate kdb+ parity" is fine as a *proposed* phase (`p0` does this at line 228). Promoting it to a status row before the work is scoped is a lie that other docs cite.
