# Surface DB Production — Status Update 4 (external review response, paused mid-wave)

**Date:** 2026-07-25
**Branch:** `feat/surface-db-prod` — worktree `C:\atx\.claude\worktrees\surface-db-prod`
**Tip:** `536f1e8` · **9 fix commits** this session · **73** on the branch versus `main`
**Supersedes the merge posture of:** `2026-07-25-surface-db-prod-status-3.md`
**Companion:** `2026-07-25-surface-db-prod-review-response.md` (per-finding disposition)
**Ledger:** `.superpowers/sdd/surface-db-prod/progress.md` (gitignored)

**Money: $0.0000 of the $100 budget.** No paid call this session or the last two.

**State: paused deliberately, mid-programme.** Nothing is half-written — every commit below is
complete, tested and reviewed or awaiting a review that is named. One wave (R4) is briefed and
not dispatched.

---

## What happened

An external reviewer reviewed the whole branch and committed the review as `342557b`:
**23 findings, 4 Blockers, verdict "request changes"**, and an explicit recommendation not to
label the branch production-complete.

Status-3 had said the sprint was finished and the internal final review said **SHIP**. That
disagreement is the first thing that needed resolving, and it resolves against me.

**Both reviews are right about different questions.** The internal one asked whether the
branch's diff was sound against its own known-issues list — it was, and that verdict stands.
The external one asked whether the word **production** is earned at the design's stated scale
of ~4,000 symbols × 250 sessions. It is not. Findings P-01 and P-02 are architectural and I
verified both against the source myself.

**I concede the label.** What exists is a correct, deterministic, resumable surface database
that has built and served a real 51-symbol × 17-date artifact. That is a pilot. Status-3
overclaimed by borrowing the design spec's word instead of measuring against it.

The artifact itself is unaffected: `C:/atx-data/surface-db/prod-2026-07` has not been written
to at any point and remains 17 partitions, 51 symbols, 858 surfaces, generation 90.

---

## Method: every finding verified before anything was dispatched

A severity on a review is a claim like any other. I read the code behind each finding I acted
on. Two moved:

- **C-06 was filed High. It is worse.** It is a silent, unkillable hang, and its trigger is the
  `std::bad_alloc` path — which this project's own production run hit **twice** under memory
  pressure. Moved to the front of the queue.
- **C-01 had a second half the review missed.** Beyond partial overlap, an *unreadable*
  destination also fell through to the destructive rewrite. Fixed alongside it.

No finding was dismissed without reading the code it names.

---

## Fixed and landed

| commit | finding | what |
|---|---|---|
| `d7a4f21` | **C-06** High→Blocker | The per-date drain slept on a counter that only fit tasks decremented, while the scheduler had two *pre-task* error returns. On either, nothing decremented, nothing notified, and the build hung forever without ever reading `fit_status`. |
| `4d37052` | **C-04** High | An entirely corrupt input window exited 0 with an empty database — indistinguishable to a scheduler from an intentional no-op. Now exits 3; partial corruption warns; an empty window stays quiet. |
| `015cc78` `e60fcc1` | **C-07** Medium | `parse_civil` accepted `2026-02-31` and silently enumerated a *different* date. Now round-trip validated — and a follow-up rejects negative years, a residual of the same bug class that the first fix did not catch. |
| `89d29ab` | **C-01** Blocker | Migration rewrote any non-superset destination from the v1 sources alone, deleting destination-only underlyings — including anything a **paid** Databento pull had put there. Now a validated union with destination-wins precedence, and an unreadable destination is refused rather than overwritten. |
| `fc5efde` | **F-06** Medium | `ACTUAL SPEND` was a sampled estimate. Relabelled `REALIZED ESTIMATE` with the counts that make it interpretable. |
| `32ff9cd` | review Minor | `pq.read_table` under a `date=YYYY-MM-DD/` path makes pyarrow inject a spurious `date` column. Harmless today only because the next line hard-selects columns — one refactor from live, in the **paid-data** merge path. |
| `1a55e27` | review Minor | `migrate --dry-run` validated only the destination's schema, so a schema-drifted source was predicted as a clean merge that the real run would fail. |
| `536f1e8` | **C-02** mitigation | A partition rewrite that would **lose stored coverage** is now refused, leaving the existing partition untouched, unless the operator passes an explicit override. This is the guard for the incident below. |

### The incident `536f1e8` exists to prevent

Earlier in this sprint an operator ran a build at the wrong `--r` and **destroyed 95 stored
surfaces in one run** on a production-shaped copy. The database reported success. A partition
rewrite is whole-file, so a cell whose refit fails is simply not re-emitted.

The real fix is an archive format change and is still deferred. The guard is not that fix — it
is the refusal to commit a rewrite that loses coverage, which is what would have stopped the
incident without any format change.

---

## Review record this session

| wave | outcome |
|---|---|
| R2 — migration union + spend label | spec **PASS**, quality **PASS** — 0 Critical, 0 Important, 2 Minor. Reviewer independently reproduced both scenarios and empirically confirmed the pyarrow claim rather than trusting the report. Both Minors then fixed. |
| R1 — deadlock, exit code, dates | spec **PASS**, quality **PASS** — 0 Critical, 0 Important, 6 Minor (parked). |
| R3 — coverage-regression guard | **Review not yet run.** Committed and self-tested; this is the one named gap in the record below. |

**Test evidence:** Python 33 passed (31 pre-existing + 2 new). C++ 13 new tests from R1;
focused runs 42/42 `SurfaceDbPopulate`, 53/53 build+predicate, 27/27 OPRA, 227 across
`SurfaceDb|Opra|GenerateSymbolConfigs`, with the one known `-j16` `OpraPanel` temp-dir race
that passes 25/25 serially.

**Test standard applied:** every new regression test in R1, R2 and R2-minors was verified by
**neutralising the fix and watching the test fail** — not merely by passing. That is the
standard the briefs demanded and the reports evidenced.

---

## Two places I was wrong, and the record of them

- **On C-06 I was wrong twice, on the one finding whose failure mode is a silent hang.** My
  brief specified a separate scheduler-finished flag plus notifies. The implementer refused it
  and folded the abort into the *same* atomic counter the drain waits on. The reviewer then
  independently verified that **my** design is broken: a notify on an unchanged value is spent
  before the waiter re-enters `wait`, so the drain can still sleep forever. The implementer was
  right and said so rather than quietly complying. I wrote a concurrency ruling with more
  confidence than I had earned.
- **Status-3's "production" framing was mine and it was an overclaim.** Conceded above rather
  than defended.

The reviewer also proved a property stronger than the one I asked for: a partially-fitted date
cannot exist when the abort sentinel is stored, because `run_next` never aborts early — so a
nonzero counter implies *zero* boards of that date ran.

---

## Not done — the honest list

**Briefed, not dispatched (this is where the pause landed):**

- **R4-a / F-01 (High)** — `py_build_surface_db` never sets `spec.hive.r`, so **every Python
  build runs at `r = 0`** while production required `0.043`. A Python caller pointed at a real
  database today is running the exact configuration that destroyed 95 surfaces, with no way to
  correct it. Brief written at `.superpowers/sdd/surface-db-prod/revR4-brief.md`.
- **R4-b / C-05 (High)** — an opt-in `--strict` mode so an unattended scheduler fails when
  every newly scheduled cell dies beside healthy carried cells. Ruled **opt-in, not default**:
  the production database has three permanently-failing cells and a strict default would exit
  nonzero forever — the same permanently-red signal the carry exemption was added to remove.

**Missing from the record:**

- **R3 has not been reviewed.** It is the largest and most safety-critical change of the
  session and it has the implementer's own testing only. It needs a review before merge.

**Deferred with reasons, per-finding detail in the companion response document:**

- 🔴 **P-01 / P-02 / P-03 (2 Blockers + 1 High) — architectural, and they are why the
  "production" label is not earned.** The loader retains the whole requested window before
  population starts, and the per-date split calls the table seam once per symbol with each call
  rescanning every row (`O(S×N)`, verified at `opra_hive.cpp:302-313`). At 4,000 symbols this
  is not a plausible million-surface implementation. Next sprint.
- 🔴 **C-02 proper** — the archive format change. The guard mitigates; it does not fix.
- 🟠 **C-03 / F-02** — the carry fingerprint covers fit configs only, not `--r` or market
  inputs. Disclosed at the fingerprint's declaration and in the manual with a recovery
  procedure that a reviewer ran end to end. Folding `r` in would silently re-fit every
  rewritten date of the existing production database on its next run.
- 🟠 **F-03** — `--max-absent`, exit 4 and `verdict ABSENT` still have no automated test; the
  repo has no CLI test harness for either surface-db tool. Still the named first task of the
  next sprint.
- 🟠 **C-08, C-09, F-04, F-05, F-07** — diagnostics, durability (pre-existing, inherited from
  `main`), failed-cell persistence, packaging, and the `atxvol`/`pyarrow` Arrow DLL collision.
- ✅ **P-04 fixed as a doc erratum** — the *frozen* design spec contradicted itself: §2 said
  row-group pruning was "a documented future optimization, not built now"; §3 promised subset
  loads used it. §2 is what shipped. Corrected the spec to match the code, marked as an
  erratum. **This edit is still uncommitted** (see below).

---

## Repository state at the pause

- Local `main` **untouched**, not merged into. `main` was merged *into* this worktree earlier
  (`cc0f327`), clean.
- **Uncommitted and intentional:** the P-04 spec erratum
  (`docs/superpowers/specs/2026-07-22-surface-db-production-design.md`) and the two new
  documents — this file and the review-response companion. They are held rather than committed
  because a subagent was still committing to the same shared git index; three parties staging
  into one index cross-contaminate commits. **They should be committed as the next action.**
- **Dirty and NOT ours:** 13 `.superpowers/sdd/task-*.md` files from a different session, plus
  an untracked `X/`. Never staged at any point.
- No whole-repo gate has been run since these 9 commits. The last full gate (previous session,
  pre-review-fixes) was 5661 tests run, 14 failed, 99%, all failures attributed as unreachable
  from this branch.

## Recommended next actions, in order

1. Commit the three held documents.
2. Review R3 — the coverage guard is the session's most safety-critical change and is
   unreviewed.
3. Dispatch R4 (Python `r`, then strict mode) — the brief is written.
4. Run the whole-repo gate.
5. Decide the merge posture explicitly, as a **constrained beta foundation** rather than a
   production system, and stop using the word "production" in the branch's own documents until
   P-01/P-02 are done.
