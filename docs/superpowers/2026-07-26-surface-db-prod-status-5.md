# Surface DB Production — Status 5 (external review response, complete)

**Date:** 2026-07-26
**Branch:** `feat/surface-db-prod` — worktree `C:\atx\.claude\worktrees\surface-db-prod`
**Tip:** `8693af5` · **23 commits** responding to the external review · **87** ahead of `main`
**Supersedes:** `2026-07-25-surface-db-prod-status-4.md`
**Companion:** `2026-07-25-surface-db-prod-review-response.md` (per-finding disposition)

**Money: $0.0000 of the $100 budget.** No paid call in this session or the three before it.

**Merge verdict: merge — as a constrained pilot foundation, not a production system.** An
independent whole-branch review reached that verdict and agreed with the branch's own concession.
0 Critical findings survive.

---

## What this sprint was

An external reviewer filed **23 findings, 4 Blockers, verdict "request changes"** (`342557b`),
with an explicit recommendation not to call the branch production-complete. Everything since is
the response.

**The label is conceded and stays conceded.** What exists is a correct, deterministic, resumable
surface database that has built and served a real 51-symbol × 17-date artifact. That is a pilot.
P-01 and P-02 are why, and both were verified against source rather than accepted on the
reviewer's word.

`C:/atx-data/surface-db/prod-2026-07` was **never written to** at any point across the sprint. It
remains 17 partitions, 51 symbols, 858 surfaces, generation 90 — re-verified read-only at three
separate points, most recently at `867/858/9/0 verdict ok`.

---

## What landed

| finding | what |
|---|---|
| **C-06** High→Blocker | The per-date drain slept on a counter only fit tasks decremented, while the scheduler had two pre-task error returns. Either one hung the build forever, silently, without reading `fit_status`. Its trigger is the `std::bad_alloc` path this project's own production run hit twice. |
| **C-01** Blocker | Migration rewrote any non-superset destination from the v1 sources alone, deleting destination-only underlyings — including anything a **paid** Databento pull had put there. Now a validated union, destination-wins, and an unreadable destination is refused rather than overwritten. |
| **C-02** mitigation | A partition rewrite that would **lose stored coverage** is now refused, leaving the existing partition untouched, unless an operator explicitly overrides. This is the guard for the 95-surface incident below. |
| **C-04** High | An entirely corrupt input window exited 0 with an empty database. Now exits 3. |
| **C-05** High | `--strict`, opt-in: a run that scheduled work and fitted nothing exits nonzero even if it carried. |
| **C-07** Medium | `parse_civil` accepted `2026-02-31` and silently enumerated a different date. Now round-trip validated, plus a follow-up rejecting negative years — a residual of the same bug class the first fix missed. |
| **F-01** High | `py_build_surface_db` never set `spec.hive.r`, so **every Python build ran at `r = 0`** while production required `0.043` — the exact configuration that destroyed 95 surfaces. Now wired, with five other CLI knobs and the CLI's finiteness guard rail. |
| **F-06** Medium | `ACTUAL SPEND` was a sampled estimate. Relabelled with the counts that make it interpretable. |
| **P-04** Medium | The frozen design spec contradicted itself on row-group pruning. Corrected as a marked erratum. |
| review Minors | pyarrow Hive date-column injection in the **paid-data** merge path; `migrate --dry-run` validating only the destination's schema. |

### The incident the guard exists to prevent

Earlier in this sprint an operator ran a build at the wrong `--r` and **destroyed 95 stored
surfaces in one run** on a production-shaped copy. The database reported success. A partition
rewrite is whole-file, so a cell whose refit fails is simply not re-emitted.

The real fix is an archive format change and is still deferred. The guard is not that fix — it is
the refusal to commit a rewrite that loses coverage, which is what would have stopped the incident
without any format change.

---

## Verification

**Whole-repo gate:** `build all` succeeded. **5703 tests run, 14 failed** — all 14 attributed
pre-existing, and the attribution was *verified*: 10 are exact-name baseline matches, and the gate
flagged loudly that **4 were not**, re-ran each serially, and confirmed each as the known `-j16`
temp-directory race in `atx-impl` binaries this branch cannot reach (the diff touches only
`atx-vol/`, `docs/`, `.superpowers/`; zero `atx::vol` references exist in `atx-impl/` or
`atx-engine/`). 64 tests self-skipped for want of paid data rather than fetching it.

**Focused:** 244/244 across `SurfaceDb*`/`BuildSurfaceDb*`/`SurfaceArchive*`; 184/184 after the
last round; 28/28 Opra; **4/4 determinism gates**; 99 Python tests with `_core` rebuilt from HEAD.

**A gap nobody had noticed:** `atx-vol/python/` is a **separate CMake project**, so `_core` is not
built by `build all` and was never covered by any gate — while three commits changed its bindings.
Now verified independently: compiles clean, and the three behaviours those commits added were
confirmed **live at runtime**, not by inspection.

**Test standard, stated accurately.** Status-4 said "every new regression test in R1, R2 and
R2-minors was verified by neutralising the fix and watching it fail" — true, but phrased as though
it covered the branch. It did not. In substance it under-claimed: R3, R4 and the later rounds were
held to a *stronger* standard, with explicit arming assertions that skip loudly rather than pass
when the scenario fails to arm.

---

## The finding of this sprint about this sprint

One defect class appeared **sixteen times**: *a comment asserting a guarantee the code does not
deliver.*

It was found at four escalating depths, then swept for deliberately, then found again:

1. The guard read the **manifest** while its comment said it read the directory.
2. `open_partition_file`'s contract comment, while `open_file` collapsed the error codes beneath it.
3. The header restating that same false contract.
4. **A false *safety* guarantee** — two comments asserting the CLI can never emit a verdict
   alongside the carry-masked hedge "as a property of the predicates rather than of the order
   `main` tests them in". It was **true when written** and was invalidated **twice, independently**,
   by the coverage guard's exit 5 and by `--strict`. Neither change touched the sentence, so
   neither task's diff contained it. Two further Important findings were its downstream
   consequences: nobody checked the banner interactions **because the comment said the interaction
   could not exist.**
5. Seven more, found by an audit dispatched specifically to look for them — including an `Err` list
   that literally ends "This list is meant to be exhaustive — keep it so" while omitting a path
   that has its own named test, and a citation to a test (`SurfaceDbBuildExitMatrix`) that **does
   not exist**.
6. Four more beyond those, every one in the *neighbouring sentence* of something the audit had
   already named.

**The class survived the passes aimed at it.** The round whose entire job was fixing it introduced
a new instance in the same commit: a header claim that "no predicate in this header reads that
counter", added alongside a predicate 150 lines below that reads exactly those counters.

**The structural conclusion, which is next sprint's work:** another reader will not converge this.
The manual's counter table and the CLI banners each restate a header contract with nothing tying
the copies together. Until a copy can **fail a test**, "check the neighbours" is a human defence
against a mechanical problem.

Two rules were adopted along the way and are now enforced in code:
- **No banner may assert an exit code it does not decide.** The tool was printing
  `WARNING (exit 0)` on runs that exit 5 and on runs that exit 3.
- **On a state where the `--r` advice is not the right advice, do not print it.** Following that
  advice is what destroys surfaces; under `--strict` with a refusal, one stderr stream said to
  reach for `--r` and another said doing so would delete the surfaces.

---

## Not done — the honest list

- 🔴 **P-01 (Blocker) — architectural.** The loader retains the whole requested window before
  population starts. The batch return boundary has to become a bounded per-date pipeline. This is
  the main reason "production" is not earned, and it is why every end-to-end run in this sprint was
  per-date.
- 🔴 **P-02 (Blocker) — real work, but *not* architectural.** The per-date split calls the table
  seam once per symbol, each call rescanning every row (`O(S×N)`). **Correction:** the response
  document originally filed this with P-01 under "architectural"; that word was doing rhetorical
  work and let P-02 inherit P-01's excuse. It is a contained per-date change — one lambda plus one
  seam overload.
- 🔴 **C-02 proper** — the archive format change. The guard mitigates; it does not fix.
- 🟠 **P-03** — long failures commit no progress; the same boundary as P-01.
- 🟠 **C-03 / F-02** — the carry fingerprint covers fit configs only, not `--r` or market inputs.
  Folding `r` in would silently re-fit every rewritten date of the existing production database.
- 🟠 **F-03** — `--max-absent` still has no automated CLI test. **Correction:** the reason
  status-4 gave ("no CLI test harness, so this needs a new test target with a process-spawning
  fixture") was stale, and stale *because of a commit on this branch* — lifting the decision out of
  `main()` worked, and most of F-03 is now a small refactor.
- 🟠 **The admin CLI has no exit-code matrix tests** while the build CLI now has seven. That
  asymmetry is why one of the late findings could exist, and it is the most likely home for the
  next one.
- 🟠 **The exit *diagnostic* is untested; only the exit *code* is.** Three of four Important
  findings lived in stderr text no test can see. That is still true.
- 🟠 **C-08, C-09, F-04, F-05, F-07** — diagnostics, durability (pre-existing, inherited from
  `main`), failed-cell persistence, packaging, and the `atxvol`/`pyarrow` Arrow DLL collision.

**Known residuals, recorded rather than glossed:**
- `SurfaceArchiveV2::open_file` re-probes internally and still folds a failed probe into
  `NotFound`, so a filesystem that starts failing between two adjacent stats returns `NotFound`.
  Narrowed, not eliminated; `open_file` was left alone because it has other callers.
- The `IoError` branch's test is **Windows-only** (`icacls`); on a POSIX runner it is compiled out,
  not skipped, so the run says nothing about it.
- Two `ErrorCode::Internal` cross-checks in `populate_universe_streaming` are unpinned.
- `main()` still evaluates `strict && is_strict_total_fit_failure` for the diagnostic, which can
  print a contradictory banner but cannot move an exit code.

---

## Repository state

- Local `main` **untouched** at `2858cab`, not merged into.
- The other live session's checkout at `C:/atx` **untouched** at `52db5a9`, exactly as at session
  start. Verified explicitly after one agent misreported its branch — the report was wrong, the
  work was right, and `git branch --contains` confirmed every commit landed on
  `feat/surface-db-prod` alone.
- Dirty and **not ours**: files from another session, never staged at any point.

## Recommended next actions

1. Merge as a **constrained pilot foundation**. Do not use the word "production" in this branch's
   documents until P-01 and P-02 are done.
2. Next sprint, in order: **P-01**, then **P-02**, then the archive format change that fixes C-02
   properly.
3. Make a duplicated contract able to fail a test. That, not another reading pass, is what closes
   the defect class this sprint spent six rounds on.
4. Give the admin CLI the exit-code matrix tests the build CLI now has.
