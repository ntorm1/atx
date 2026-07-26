# Pipeline SOTA Sprint — final status, 2026-07-25

Plan: `atx-vol/sprints/2026-07-21-atx-vol-pipeline-sota-sprint.md`
Integration trunk: **`feat/pipeline-m` @ `b056538`**.
Local `main` is **untouched at `2858cab`** and was verified so after every commit.

Supersedes `2026-07-25-pipeline-sota-sprint-status-update.md`, which was written at the second stop
with two agents still running and is stale in its §1, §4, §7 and §8.

**Gate at the trunk tip `b056538`: 2279 enumerated / 2272 counted / 2225 passed / 0 failed /
47 skipped / 7 disabled**, `-L atx_vol -j 1`, full label, no `-R` filter, with the `atx-vol-python`
lane **live and Passed** rather than skipped.

Since the fork point `d4ade5b`: **265 commits, 36 merges.**

---

## 1. What closed since the second stop

The second stop left eight things open. **All eight are now closed.** Every one of them was closed
through the same loop — implementer, then an independent reviewer who was told to treat the
implementer's report as claims rather than findings — and that loop found a further **five Importants
and twenty-eight Minors** that nobody was looking for.

| Item at the second stop | Outcome |
|---|---|
| Nothing had gated `48d15ea`, `783ac13` or the four BENCH commits | **Gated, green**, and gated again at every tip since |
| A5, A6, A7 never implemented | **A6 and A7 landed and reviewed. A5 refused with four blockers, ruled 3-for-4 independently** (§4) |
| REV-TAIL I-1, I-2, I-4 and six Minors open | **All closed.** I-2's premise was refuted by the implementer and the dispute adjudicated (§3.2) |
| The `atx-vol-python` lane never observed live | **Closed — 173 passed / 0 failed**, and it now runs inside the gate (§3.1) |
| WSAPERF's worktree possibly holding a partial revert | **False alarm, disproved by mtime** (§6) |
| A6's gate named a bench row that does not exist | **Resolved by identification**, not invention (§4) |
| The tail unreviewed | Four independent reviews: 0 Critical, 3 Important, 23 Minor across them |
| `main` not re-verified against the tip | **Verified: `main` is fully contained**, 0 commits ahead (§7) |

## 2. The exit criteria

Criterion 7 remains as recorded at the second stop: **first half met** (the fitting bench really is
re-pointed at `PricerFitter::fit`, verified in the code); **second half missed on the number and
carried by the plan's own written-explanation clause** — measured 13.30/16 against a 14/16 target,
with `DATE_BATCH=1` reproducing the pre-sprint ~10/16 so the instrument is anchored and the shipped
batching's +3.0 cores is demonstrated. The residual is per-board scaling loss (7.78/8 = 97% of budget
at `fit_workers=8` versus 82% at 16), which is why the answer is an explanation and not a code change.

**B7's baseline JSON is still not produced, and it remains the one deliverable missing.** That is
deliberate: `compare_baseline.py` gates on ratio > 1.10 **and** CV ≤ 5%, so a baseline captured on a
contended box does not false-alarm — it *permanently weakens the gate*. It needs a reserved quiet
window, not an observed-quiet one.

## 3. What the closing round found

**3.1 — the Python lane had never been observed at a commit that could show its state.**
Every gate quoted anywhere in this sprint ran with `atx-vol-python` **Skipped**, because no `.pyd`
exists in the monorepo build. PY-FIX had run it at 152 passed, but on a branch *before* the C++
RunArchive cutover landed. Nobody had ever run that lane at a commit carrying both the cutover and a
built `_core` — and the tail review predicted that doing so would go red. It did. Building `_core`
and fixing what fell out took it to **173 passed / 0 failed**, and it now executes inside the gate.

**3.2 — a review's evidence was wrong while its finding was right, and both agents were partly
correct.** REV-TAIL's I-2 argued `347ad44` dropped a capability, citing `read_run_spec` and two
hardcodes. The implementer showed all three already existed at `b0080fa`, so nothing was lost and
`347ad44` actually *added* PIT resolution. The adjudicating reviewer confirmed that and found the
finding still stood for a different reason: the dispatch-table comment claiming X1/X4 for
`run-projected-var` was false. Equivalence was then proved from artifacts — two files SHA256-identical,
the third differing in exactly one column, `projections_per_second`, a timing field.

**3.3 — A7's own gate quantity moved backwards on a legal input.** A7's entire deterministic proof is
a solve-ledger count *drop*. It solved a put boundary for every (unique, vol column) pair with the
`is_exact` filter *inside* the loop, so a wholly-Taylor vol column paid a wasted cold solve per put
unique. Measured RED: **35 solves against a 20 ceiling — 75% above the pre-A7 per-cell cost**. Nothing
covered it because every A7 test used `all_exact_spec()` with both radii `0.0`, forcing every cell
Exact and hiding the case by construction. Fixed to 20, with the hoisted predicate independently
ruled **equivalent, not merely sufficient**, so no false skip is reachable.

**3.4 — a counter that had been under-reporting library-wide.** `AmericanAvxPackDispatches` bumped at
one of two AVX2 dispatch sites. Anything that ever read it as "is the pack path taken" got a partial
answer. Both sites now bump, and the counter's definition states what it excludes: complete 4-lane
packs only, patched-out lanes still count, other AVX2 routes invisible — **non-zero proves dispatch,
zero does not prove the path is dead.**

**3.5 — a guard that made an unguarded write impossible, except for the write it argued about.**
A6's `constexpr` sweep was extended to make an over-sized scheme a *build* failure rather than silent
corruption. A comment then explicitly justified omitting one table — "which this same bound covers
since nb >= 1" — bounding by `kGeoBarySize` (3168) arrays that hold `kGeoBaryPairs` (264). Five
schemes passed the sweep and wrote out of bounds; `(10,32)` writes `geo_bary_den[264..287]`, and in
Release `geo_bary_hit` is the last `AlWorkspace` member, so that write leaves the object. Unreachable
today. The fix's enumeration then established something the review had not: **den/hit was the entire
remaining exposure** — no `geo_zc` overflow or row overlap survived — and after the fix, zero unsafe
schemes are admitted and zero safe ones rejected.

**3.6 — the fourth knob that parsed and did nothing, and the first to publish the lie.**
`quote_min_bid`, `quote_max_age_ns` and `quote_reject_locked` were bound, written into
`run_config.tsv` — an artifact whose header says it records *"the EFFECTIVE value of every execution
knob the run actually used"* — and consumed only inside an entry point with no shipped caller. The
other three instances of this class were silently ignored; this one published the ignored value as
fact. RED: a spec setting `quote_min_bid 1000000000` produced a schedule and a NAV while
`run_config.tsv` recorded a billion-dollar bid floor as effective. It was **wired**, not silenced, and
the compile RED was `no member named 'quality' in 'atx::vol::ListedScheduleSpec'` — verbatim the
sentence the header itself gave as the reason the gap could not be closed. The blocker was documented,
believed, and removable.

**3.7 — the fix that closed it was itself ungated.** Deleting the single line that made the three
knobs reach the shipped route broke nothing: the new tests sat one layer below it and the only ctest
entry that executes the example drives defaults only. This sprint found five tests that disarm
themselves; this is the same disease one level up — a *fix* with no gate rather than a *test* with no
teeth. Now `listed_schedule_spec_from` is called by both routes and removing the assignment fails a
named test on all three knobs, and fails `/WX` compile first.

**3.8 — twelve tests fail under counters-ON, and no CI configuration has ever run it.**
`Counter::BoundarySolves` and the always-on `AlBoundarySolves` ledger have diverged (29 versus 4).
Attributed pre-existing on blob identity across the whole branch plus three independent structural
checks. Ruled **bookkeeping, not real**, with the direction settled: `bundle_solves = 1 + 2·vega +
2·rho` is the literal solve count, so **29 is faithful and 4 is the miss** — the gated counter is
blind to the laned kernel, and `Counter::BoundarySolves` has no reader in `atx-vol/src` at all.
Left unfixed deliberately. **The real deliverable is a counters-ON CI lane**; without one this rots
again, silently, exactly as it did.

## 4. A5 — not landed, and why that is a finding rather than a gap

A5's four blockers were ruled independently: **holds / holds / partially holds / holds.** Two of them
are findings about the plan, not the code:

- **The plan specified against superseded code.** A5's second half targets a row sampler that
  `c84ecfc` (2026-07-11) had already moved onto `andersen_lake_put_slice`, one solve per row — **ten
  days before `d4ade5b` authored the plan.** Routing it through the pack path would take
  `sl_al_boundary_solves` from 96 to 1536 and fail both assertions in an existing test.
- **A5's own deterministic gate is unfalsifiable.** `AmericanAvxPackDispatches` fires at exactly one
  site that neither A5 call site reaches, so the "counter 0 → non-zero" proof A5 is gated on cannot be
  produced. This is the **second** unfalsifiable gate in WS-A's specification — A6's named bench row
  was the first, and it was resolved by identifying two existing rows already pinned by a
  name-coverage test, so the re-spec cannot rot the way the original did.

The third blocker is the honest one and is recorded as such: it proves A5 is **not shippable ON**,
not that it is unlandable. A default-off export with `isa_golden_tol` parity would move nothing, and
`kShipAvx2Greeks` is the tree's own precedent. Stopping was the right call — but the reason is cost
and golden movement, not impossibility.

## 5. Open — what is NOT verified

1. **The gates are `atx_vol`-scoped, at the user's direction, for speed.** That label is roughly 2270
   of the repo's ~5700 tests. An unfiltered whole-repo run reached **713/5709 with 0 failures** before
   being aborted on the scope change. So **~3400 tests remain unobserved at this tip.**
2. **No throughput figure from this sprint is citable.** The box was shared with another Claude
   session running builds and sweeps throughout. Both A6 and A7 are carried on their deterministic
   halves only, and neither claims a speedup.
3. **B7's baseline JSON** (§2), and any re-measurement of T1, need a **reserved** quiet window.
4. **The counters-ON configuration is unguarded by CI** (§3.8) — 12 known failures, pre-existing,
   with a settled diagnosis and no owner.
5. **The e2e fixture is not in the repo.** It is 19 MB against a repo whose largest file is 9.5 MB, so
   the module skips loudly — naming every candidate, the remedy, and the `build-corpus` recipe —
   rather than shipping the fixture. The byte-exact out-of-repo golden it used to compare against was
   replaced with a shape-plus-precision test after the divergence was **measured** (38 of 78 cells,
   1e-16..1e-8 relative, the two largest gaps both near-zero cells).
6. **`quote_rejects.tsv` is still unwritten on the shipped `build-schedule`** — a missing report
   rather than an unhonoured knob, disclosed at the header, and deliberately not added because a new
   artifact on a shipped route is a change of a different size.
7. **WS-F's M11-part-3** remains deferred, as it edits a file the main merge rewrote.

## 6. Process findings

**Every implementer this round corrected at least one premise handed to it, and each correction was
load-bearing.** Mine were wrong about the wt-pipe-e worktree holding a partial revert — the
implementer disproved it by **mtime**, not by inspection: the test binary post-dated every source
edit, and a revert-and-restore cycle would have left it older, not newer. That is a better instrument
than the one I used, and it settled the question in one command.

**Reviews were wrong often enough that adjudication had to be a step.** REV-TAIL's I-2 evidence was
wrong while its finding was right (§3.2). REVA7FIX suggested a one-term fix where two terms were
needed. REVA7TIDY cited one of two sites where `bundle_solves` is computed. In every case the next
agent checked rather than complying, which is the only reason each was caught.

**A defect that recurs is a process defect.** Stale cross-file line citations broke **five** times in
one batch. Four passes fixed them by re-deriving the numbers by hand; each pass then invalidated more
by shifting lines elsewhere. The fifth pass found the mechanism — nobody ever re-read the whole set
after editing — and closed it structurally: citations now name the **symbol**, with the line as a
convenience allowed to rot, and a re-runnable script checks all 23.

**"Deferred with confirmed blockers" is not evidence.** A5, A6 and A7 carried that status through a
workstream review returning 0 Critical / 0 Important. Nobody had attempted any of them. Two landed
without difficulty once attempted; the third produced two findings about the plan itself.

**A tool that silently discards input costs more than one that fails.** `atx-build.ps1 configure`
drops extra `-D` arguments — it never references `$rest`, which `build` and `check` do use. That is
why counters-ON runs kept not happening: the flag was passed and discarded, and the resulting
counters-OFF build looked like a successful counters-ON one.

## 7. State

Trunk `feat/pipeline-m` @ **`b056538`**. Local `main` **untouched at `2858cab`**.

The merge of `feat/pipeline-a-perf` was verified against silent hunk loss the same way `b48ec35` was:
reconstructed with `git merge-tree --write-tree`, producing tree `4649501b…` — **identical to the
committed merge's tree.** Zero silent hunk loss. The two branches touched strictly disjoint file sets,
which rules out in-file composition but not cross-file semantic interaction, so the merged tip was
gated in full rather than inheriting either side's result.

**`main` is fully contained in the trunk**: `git rev-list --count b056538..main` = **0**, and
`git merge-base main b056538` == `main` == `2858cab`. There is nothing left to merge in that
direction; the verification the user asked for is satisfied by containment.

**No golden moved anywhere in this round**, with one deliberate, measured exception: the e2e
byte-exact comparison against an out-of-repo file built by a different agent from a different source
line (§5.5).

## 8. Next steps, in order

1. **The merge to `main` is the user's call.** This trunk contains `main` rather than diverging from
   it, which is the cheaper direction to resolve.
2. **Stand up a counters-ON CI lane** (§3.8). Twelve failures with a settled diagnosis and no owner
   will rot again without one, and the diagnosis itself was only possible because someone finally ran
   that configuration.
3. Decide whether the `atx_vol` scoping stands. A whole-repo run needs roughly an hour of a genuinely
   quiet box.
4. B7's baseline JSON and any T1 re-measurement need a **reserved** quiet window.
5. Consider A5 as a default-off export (§4) — the blocker is cost and golden movement, not
   impossibility, and the tree has its own precedent for shipping a laned path dark.
