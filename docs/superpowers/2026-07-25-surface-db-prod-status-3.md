# Surface DB Production — Status Update 3 (sprint complete)

**Date:** 2026-07-25
**Branch:** `feat/surface-db-prod` — worktree `C:\atx\.claude\worktrees\surface-db-prod`
**Tip:** `9c7be16` · **11 commits** this session, **62** on the branch versus `main`
**Supersedes:** `2026-07-25-surface-db-prod-status-2.md`, which carried a false claim
(corrected in `c3bcc39`) and a *Not done* list that is now closed
**Ledger:** `.superpowers/sdd/surface-db-prod/progress.md` (gitignored)

**Money: $0.0000 of the $100 budget.** No paid call this session or the last.

---

## Bottom line

**The sprint is finished and the final whole-branch review says SHIP.**

All three items the last status listed as *not done* are done: the production rerun-zero
measurement, Task 7's Python binding verification, and the merge of `main` into this
worktree. Two defects found along the way were fixed and reviewed; one build break was
fixed; the final review's fix wave landed and its scoped re-review found no new
Critical or Important breakage.

The production artifact `C:/atx-data/surface-db/prod-2026-07` is **untouched** — 17
partitions, 51 symbols, **858 surfaces**, generation 90, exactly as the last two statuses
left it. Every experiment ran against copies.

---

## The measurement the sprint existed to make

Design spec §5 claims: *"re-running an unchanged build fits zero and spends $0."* That had
never been measured on production-shaped data. The previous attempt was my own error — I
ran it at the CLI's default `--r 0` against a database built at `--r 0.043`.

Two passes over 6 real sessions × 51 symbols on a copy, at the correct rate:

| | pass 1 | pass 2 |
|---|---|---|
| `cells_refit` | 150 | **0** ← the gate |
| `cells_carried` | 0 | 150 |
| `cells_ok` | 150 | 0 |
| `cells_failed` | 3 | 3 |
| `cells_to_fit` | 3 | 3 |
| surfaces | 858 → 858 | 858 → 858 |
| exit | 0 | 0 |

**`cells_refit 0`.** I wrote that prediction into the ledger *before* running pass 2 and it
matched field for field. Nothing was destroyed at the correct rate — the earlier
95-surface loss was entirely the wrong `--r`.

Two things worth noting beyond the headline:

- **The gate is `cells_refit == 0`, not `cells_to_fit == 0`.** The three permanently-failing
  cells are *absent*, absence is what schedules a date for rewrite, so those three dates
  rewrite forever. That is by design and it is exactly why carry-over exists. The manual and
  the design spec both said "fits zero" unqualified; both now say what is actually true.
- Pass 2 printed the `is_carry_masked_fit_failure` warning with the exact three-cell list
  and **exited 0** — the first end-to-end exercise of that warning on real data.

---

## What landed this session

| commit | what |
|---|---|
| `ac5d21c` `2d40535` `7e7579a` | **Task 7 verified.** The binding was already complete; what was missing was proof it links, imports and passes. It does — against a `_core` built from this worktree, proven by printing `atxvol.__file__`. Surfaced a real product limitation, now documented in git-tracked `atx-vol/python/README.md`. |
| `6e98553` `aafb76a` | **FIX-H.** `verify` no longer reports a healthy converged database as `FAILED`. |
| `36a6b3d` | **FIX-I.** `FitPreset::Populate` was not handled in `universe_autofit`'s switch, so `-Werror,-Wswitch` broke `build all`. |
| `63829b1` | A `--report` write failure no longer eats the verdict — a dead build now exits 3 with its `--r` advice instead of 1 with nothing. Plus the missing-value guard the build CLI lacked. |
| `e294017` | `verify` cross-checks every manifest partition record against its file — the "opens but is wrong" case. |
| `3d5dbc0` | What the carry fingerprint does **not** cover, and how to recover. See finding 2. |
| `c3bcc39` | Corrects status-2's false claim. |
| `9c7be16` | Pins worker-count byte identity **through** the carry path. |

### Review record

| task | outcome |
|---|---|
| T7 | spec ✅, quality approved — 1 Important, 1 Minor → both closed, re-review clean |
| FIX-H | spec ✅, quality needs fixes — 2 Important → both closed, re-review clean |
| Final whole-branch | **SHIP WITH FIXES** — 1 Critical, 8 Important, 9 Minor |
| Final fix wave | scoped re-review: every finding ADDRESSED, **no new Critical or Important**, verdict **SHIP** |

---

## The three findings worth reading

### 1. `verify` called the finished production database FAILED — forever

Running the sprint's own verification tool against the sprint's own deliverable:

```
verdict FAILED
exit 1
9 × fail <date> <SYM> kind=unmappable detail=NotFound: symbol not present
```

Identical on a fresh copy, so it predates the branch. Those nine are the nine
permanently-failing cells. The database is healthy — pass 2 proved it re-fits nothing.

The tool could not distinguish a cell that was **never stored** from one that was stored
and is now **unreadable**. The partition's own directory answers that, and it was never
consulted. **This is the same disease as the Critical this branch already fixed for exit 3**
— a converged database reading as broken — on a code path the earlier fix does not reach.

It matters more than a cosmetic verdict: destroyed surfaces show up as exactly this kind of
absence, and a permanently-red signal is one operators stop reading.

Absence now has its own counter, its own capped list, and a distinct exit 4 — **gated behind
an operator-supplied `--max-absent N`**. That gating was a deviation from my instruction. The
implementer argued that firing unconditionally would make production exit non-zero forever
— the same permanently-red signal with a different digit — and that a ceiling is the only
thing that can express *the absent set changed*, which is the real discriminator. Two
reviewers reached that conclusion independently. **I was wrong and the deviation stands.**

Its cost is real and stated in the manual: with no flag, a run that destroyed surfaces now
exits 0 instead of 1. The defence — which the re-reviewer verified rather than accepted — is
that the old exit 1 fired identically on a healthy day and so carried no information, that
stderr is strictly louder now, and that a deleted or truncated partition still exits 1.

### 2. A wrong `--r` cannot be repaired by re-running at the right one

The final review found this and neither I nor any earlier reviewer had seen it. It is the
wrong-rate incident's second act.

The carry fingerprint folds **fit configs only — not `--r`, not market inputs**. So after a
build at a wrong rate, re-running at the *corrected* rate does not re-fit. It **carries the
wrong-rate surfaces forward as though sound**, and reports a clean `cells_refit 0`.

I had measured that same carry behaviour in pass 2 and read it as pure good news. It is the
same mechanism, and on a poisoned database it is the failure mode.

The **disclosure** shipped, at the fingerprint's declaration and in the manual's `--r`
section, with a concrete recovery procedure. The **code affordance was deliberately
declined**: folding `r` into the fingerprint requires a new parameter on the public API that
was just restructured, and would silently re-fit every rewritten date of the existing
production database on its next run. `--force-refit` is worse — a new operator switch on the
destructive path, added after the last full gate. Both are next-sprint work with tests.

The re-reviewer **ran the documented recovery end to end** on a scratch database and
confirmed it works as written.

### 3. The build was never fully built

`build all` failed on `universe_autofit.cpp`: a switch over `FitPreset` missing the
`Populate` enumerator, under `-Werror,-Wswitch`.

I first recorded this as the branch's fault. **It is not, and the final review corrected me.**
`main` already contains `FitPreset::Populate` and `universe_autofit.cpp` is byte-identical
between `main` and this branch — `main` has the same break today. This branch only made it
*reachable*, by putting both shipped CLIs behind the same `ATX_BUILD_EXAMPLES` gate.

The process lesson stands even though the blame does not: **every build this sprint was
target-scoped**, per the one-build-slot discipline, so `all` was never built until the merge.
The constraint that kept the build slot honest hid a whole-target break for the entire
sprint.

---

## Corrections to status-2

- **"`main` and this branch have zero file overlap" was false.** Both touch
  `atx-vol/CMakeLists.txt` and `atx-vol/tests/CMakeLists.txt`. Corrected in `c3bcc39`. (The
  merge auto-resolved both anyway — I also predicted a conflict, and was wrong about that.)
- **The `-Wswitch` break is not this branch's fault.** See finding 3.

---

## Merge state

`main` was merged **into** this worktree (`cc0f327`) — **clean, zero conflicts**. Local
`main` is untouched at `2858cab`, per instruction. It brought the `backtest_driver` spine,
three driver migrations and the Wave C/D/E plans: 16 files, +2843/−49.

**Whole-repo gate after the merge: 5661 tests run, 14 failed, 99% passed, 270 s.** Down from
36 failures last session, and this time **every target built** — the nine "Not Run" are all
`(Disabled)`, not the `_NOT_BUILT` that made the previous gate a fraction of the repo while
looking complete.

Attribution, verified two ways rather than asserted:

1. `git diff --name-only 2858cab..HEAD` outside `atx-vol/`, `docs/`, `.superpowers/` is
   **empty**.
2. **`atx::vol` appears in zero files under `atx-impl/` and zero under `atx-engine/`** — not
   in their link closure at all.

So the 3 `atx-vol` failures are the known-failing set, and the 11 `atx-impl`/`atx-engine`
failures are structurally unreachable from this diff. **Honest limit, unchanged from last
status:** those targets have no branch-point baseline. Reachability is the argument, not a
before/after diff.

---

## Known limitations shipping with this branch

- 🔴 **A wrong `--r` destroys stored surfaces** — measured, 95 in one run. The fix needs an
  archive format change and is deliberately deferred; the behaviour is pinned by a test that
  fails by design if someone "fixes" it without confronting the argument. **The final review
  judged the deferral itself correct** — the preserve-the-bytes alternative trades one-shot
  loss for permanent silent staleness, which is worse. What was missing was disclosure, and
  that shipped.
- 🔴 **A wrong-`--r` database cannot be repaired by re-running.** Finding 2. Disclosed with a
  verified recovery procedure; no code affordance.
- 🟠 **`--max-absent`, exit 4 and `verdict ABSENT` have no automated test.** They were
  measured four ways on production and independently reproduced by two reviewers, but the
  repo has **no CLI test harness for either surface-db tool** — building one is a new test
  target with a process-spawning fixture, not a test. This is now the only untested
  operator-facing contract on the branch, and it is the named first task of the next sprint.
- 🟠 **`atxvol` and `pyarrow` cannot be imported in the same process.** `_core` links vcpkg's
  `arrow.dll`/`parquet.dll` (new on this branch, via the OPRA hive loader) and pyarrow ships
  its own same-named DLLs; it fails in **both** orders. That collides with design spec §5's
  notebook use case. Documented in `atx-vol/python/README.md`. Real fixes are a
  `delvewheel`-style repair step or statically linking Arrow/Parquet — neither done.
- 🟠 **No fsync-before-rename and no rename retry** on DB write paths. **Pre-existing and
  inherited from `main`** — `surface_archive.cpp` is untouched by this branch. The final
  review flagged it explicitly as not blocking.
- 🟠 Manifest rewrite has no generation compare-and-swap; `enable` cannot tell an operator
  disable from a selector disable; producer-side normalisation (`E2-d`) deferred on a
  layering decision; a hard `CHECK` crash under memory pressure, not root-caused.
- 🟠 **Six Minor observations from the final re-review are parked, not fixed** — there is one
  fix wave by design. Two are worth a follow-up because they are doc-versus-binary
  disagreements in a manual operators read before every run: the exit-3 row's stale
  "`--report` is still written" clause, and a banner that says "`--report` writes all of
  them" on a run whose report write failed.

---

## Process notes worth carrying

- **A target-scoped build discipline can hide a whole-target break for an entire sprint.**
  Build `all` at least once before proposing a merge.
- **Write the prediction down before running the measurement.** Pass 2's expected counters
  went into the ledger first; matching them field-for-field is evidence, matching them from
  memory afterwards is not.
- **Run destructive experiments against a copy with a hash baseline.** Said last time; it
  paid again.
- **Piping a command whose exit code matters reads the pipe's status.** I did this once more
  this session — `verify | tail -5` reported 0 when the tool had exited 1 — while measuring a
  bug that was *about* an exit code.
- `import` succeeding is not evidence you imported what you think. An ambient
  editable-install `.pth` finder silently overrode `PYTHONPATH`; only printing
  `module.__file__` caught it.
