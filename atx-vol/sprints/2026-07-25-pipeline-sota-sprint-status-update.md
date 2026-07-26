# Pipeline SOTA Sprint — status update, 2026-07-25 (second stop)

Plan: `atx-vol/sprints/2026-07-21-atx-vol-pipeline-sota-sprint.md`
Integration trunk: **`feat/pipeline-m` @ `48d15ea`**.
Local `main` is **untouched at `2858cab`** and was verified so after every commit.

Supersedes `2026-07-25-pipeline-sota-sprint-status.md`, which was written at the previous stop and
is stale in its §1, §4 and §7.

Since the fork point `d4ade5b`: **233 commits, 34 merges, 248 files, +47967 / −1975.**

This is a checkpoint at a deliberate stop, not a completion report. **Two agents were still running
when work stopped** (see §7) — the tree may move after this document is written, and it was not
killed mid-edit to prevent that.

---

## 1. What happened since the last status document

The previous stop left five things open. Four are now closed, one is closed-with-a-caveat, and the
work turned up a further nine findings that were not on anyone's list.

| Item | Outcome |
|---|---|
| Whole-repo gate at the trunk tip | **Run, green** — then re-scoped to `atx_vol` at the user's direction (§4.1) |
| The `atx-vol-python` lane had never run | **Closed** — `_core` built from the tree, lane at 140→**152 passed / 5 skipped / 0 failed** |
| M9, M10, M11-part-3 | **M9 and M10 closed**; M10 turned out not to be a Minor (§3.1). M11-part-3 deliberately deferred |
| Criterion 7 benches | **Measured** — first half met, second half missed on the number and carried by the plan's own written-explanation clause (§2) |
| Merge local `main` into the trunk | **Done, `b48ec35`, gated green** — and it caught a 100× defect (§3.2) |

New work that followed from it: the post-merge reconciliation (`347ad44`, `fec64ed`, `03a747f`,
`0be71e7`), an independent review of the entire tail, and the first fix off that review (`48d15ea`).

## 2. Criterion 7, the last unmet scoreboard row

**First half — met.** The fitting bench really is re-pointed at `PricerFitter::fit` (`:377`),
verified in the code rather than taken on the workstream's word.

**Second half — missed on the number, carried by the clause.** The criterion reads "corpus fan-out
worker utilization ≥ 14/16 … **or a written explanation**". Measured: **13.30/16** mean busy cores
over 220 boards, spread 13.17–13.56 across five repetitions. Short by 0.70.

The explanation is substantive rather than an excuse, and the instrument is anchored:

- Forcing `DATE_BATCH=1` reproduces **10.06–10.29**, which is the plan's original ~9/16. So the
  measurement is calibrated against the pre-sprint condition, and **the shipped batching's +3.0
  cores is demonstrated** — the two spreads are disjoint.
- The residual 2.7 cores are **not** drain: removing 90% of drains buys +0.20 inside a 0.64 spread,
  a spread that swallows its own effect. That lever is spent.
- It is **per-board scaling loss**: 7.78/8 = 97% of budget at `fit_workers=8` versus 82% at 16 on
  the same box. 14/16 = 87.5% is a scaling problem, not a scheduling one — which is why the answer
  is an explanation and not a code change.

The quantity measured is **realized occupancy** (CPU/wall), not scheduler accounting. That choice is
load-bearing: `corpus.hpp:417-419` says `inner_slots` is "NOT a per-board mean", and offered width is
approximately constant at budget by construction (`corpus.cpp:670-681`), so scheduler accounting
would report 16/16 on a run using a single core.

**B7's baseline JSON was not produced, and that is the one deliverable still missing here.**

## 3. What was found that nobody was looking for

**3.1 — M10 was not a Minor.** It had been deferred twice on the premise that the affected paths sit
behind data-gated fixtures nobody could run. Both halves of that premise were false: a static sweep
of all 30 reconciler inputs found zero missing columns, and the path was driven end to end on a copy
of a real run directory. What the RED showed is a silent wrong number — at the pre-fix tip, a
schedule with no `roll_date` **reconciles clean and publishes a record carrying `date=""`**, and
contract marks with no `status` reconcile clean at **quote NAV 0 and coverage 0 against a model NAV
of 200**.

**3.2 — the merge caught a 100× error with no conflict marker.** Main multiplied by
`kVegaVolPointToUnitVol` at two call sites; WS-E's E1 had already flipped the library to
per-vol-point. Composed, every book came out **100× oversized**. Both multiplies were dropped, and
this was correctly refused as a re-pin candidate — a unit error is not immaterial drift. This is
precisely the semantic-not-textual risk the merge existed to surface, and it arrived in a file that
merged cleanly.

**3.3 — a friction regime that parsed and was then thrown away.** A spec declaring
`friction_preset retail_listed_options` produced `final_nav = 24740.62412` — **frictionless** —
while silently dropping **$23,459.79 of cost, 95% of gross**. After the fix: `final_nav =
1280.834458`, cost applied, regime named. Reachability was broken at *both* links: `build-corpus`
erased the typed keys and `run-backtest` built a bare `RunConfig`.

**3.4 — three plan tasks were never implemented.** A5, A6 and A7 were tracked as
deferred-with-confirmed-blockers. They are simply absent: `avx_pack_dispatches` reads 0, no
`geo_bary` symbol exists, and the A7 solve ledger is unchanged at 896 with zero spread across three
repetitions. **WS-A's independent review returned approved-with-minors, 0 Critical / 0 Important**,
and missed all three.

**3.5 — F4's CLI surface never existed.** `git log --all -S"--half-spread-bps"` and
`-S"--frictions=off"` each return exactly one commit: `d4ade5b`, the commit that *authored the plan*.
F4 landed through typed spec keys instead, and the round-trip test the plan names already exists four
times over. The plan's §4 row is corrected. One clause is reported **unmet**: "frictionless requires
explicit `--frictions=off`" contradicts X1's pinned frictionless defaults and would move a golden.

**3.6 — a probe that could delete published reference data.** `corpus_occupancy_probe.ps1` ran an
unguarded recursive forced delete on a caller-supplied `-OutDir`. Closed in `48d15ea` with two
independent guards. Testing the fix exposed a second defect: an absolute `-OutDir` was being joined
onto the working directory, producing a doubled root that `GetFullPath` rejects — so absolute paths
never worked, **and the new guard would never have been reached for exactly the paths most worth
guarding**. Only the relative traversal form actually reached the data root, and that is the form
that still worked.

## 4. Open — what is NOT verified

1. **No gate has been run at the current tip `48d15ea`.** The last full gate was at `0be71e7`
   (2261 enumerated / 2254 counted / 2206 passed / **0 failed** / 48 skipped / 7 disabled). Four
   BENCH commits and `48d15ea` landed after it. A gate covering them was in flight when work stopped.
2. **The gates are `atx_vol`-scoped, at the user's direction, for speed.** That label is roughly
   2200 of the repo's 5700 tests. An unfiltered whole-repo run was started at the trunk tip and
   reached **713/5709 with 0 failures** before being aborted on the scope change. So **~3500 tests
   are unobserved at this tip.**
3. **The `atx-vol-python` lane is unobserved by every gate quoted here.** It reports Skipped in
   `wt-pipe-m` because no `.pyd` exists there — the standalone scikit-build-core project is not part
   of the monorepo build, so a monorepo gate can only ever see that lane green if someone builds
   `_core` first. It was verified separately at 152 passed / 5 skipped in `wt-pipe-fix1`.
4. **B7's baseline JSON** (§2). Zero baselines were committed deliberately: `compare_baseline.py`
   gates on ratio > 1.10 **and** CV ≤ 5%, so a contended baseline would not false-alarm — it would
   *permanently weaken the gate*.
5. **No throughput figure from this sprint is citable except the T1 measurement**, and even that was
   taken in a single lull. The box was shared with another Claude session running builds and sweeps
   throughout; a ten-minute poll for an idle box never fired.
6. **Carried open with reasons:** WS-F's M9, M10 and M11-part-3 from the earlier review round —
   M11-part-3 was deliberately deferred here because it edits a file the main merge rewrote.

## 5. The review layer, again

An independent review of the tail returned **approve-with-follow-ups: 0 Critical, 5 Important,
6 Minor**, and confirmed both load-bearing claims independently — the nine-path blob-SHA equality
(it found a ninth the brief had missed), and that the `kRaMinor` drift test really is load-bearing,
proved by mutation. Zero files were deleted and zero goldens changed across the whole tail.

Its Importants are the interesting part, because two of them undercut work already reported as done:

- **I-1** — a header claims six entry points are covered by a test file that calls none of them.
  Three have zero callers and zero tests, which means **the M10 fix reaches no shipped binary** and
  its data-gated test self-disarms. This is the **fifth** silently-disarming test this sprint, after
  the Python discovery helper, FIX-5's `_schema.py` guard, and two of FIX-T2's parallel-for gates.
- **I-2** — `347ad44`, the commit whose purpose was *restoring* the CLI seam, orphaned
  `dispersion_book_var`.
- **I-3** — four spec keys parse and do nothing on shipped `run-surface-backtest`. **Third instance**
  of that class this sprint.
- **I-4** — a fixture root pointing at a dead session's temp UUID.
- **I-5** — closed (§3.6).

Those two classes — the test that disarms itself, and the knob that parses and does nothing —
account for most of what the reviews caught and the tests did not.

## 6. Process findings

**Briefs written from analysis are wrong often enough that agents must be told to verify them.**
Mine were wrong at least three times: the merge brief mis-described `spy_dispersion_backtest.cpp` on
both halves; a FetchContent diagnosis I recorded was refuted by the next agent (the real causes were
pip swallowing output without `-v`, vcpkg building 1.1 GB from source, first clone, and uncapped
`-j`); and an earlier claim that `ForceAvx2` stays guarded by `have_avx2()` was false. In every case
the agent checked rather than complying, which is the only reason it was caught.

**A tracker entry is not evidence.** A5, A6 and A7 sat as "deferred with confirmed blockers" through
a full workstream review that returned 0 Critical / 0 Important. Nobody had attempted them.

**Measuring a stale binary is a live hazard, not a hypothetical.** `build-rel-avx2` was not merely
four days old — it had `ATX_VOL_COUNTERS=OFF ATX_VOL_PROFILE=OFF ATX_BUILD_BENCH=OFF`, so the bench
targets did not exist in it at all.

**Exclusivity was assumed rather than reserved.** The bench task existed *because* every earlier
number was measured under contention, and it hit the same wall — a second Claude session ran
`ctest`, then 19 concurrent `clang-cl`, then 17 test processes at 87–100% CPU. It stopped and
reported rather than measuring through it, which was correct.

## 7. State at the stop

Trunk `feat/pipeline-m` @ `48d15ea`. Local `main` untouched at `2858cab`.

**Two agents were running and were not killed**, because stopping them mid-edit risks leaving a
worktree in a half-written state:

- **WSAPERF** in `C:\atx-wt\wt-pipe-e` on branch `feat/pipeline-a-perf` — landing A5, A6 and A7
  against their deterministic gates only (A7's solve-count drop, A5's pack-vs-scalar parity plus the
  counter going 0→N, A6's bit-identity), with no speedup claims and no baseline JSON.
- **FIXTAIL** in `C:\atx-wt\wt-pipe-m` — closing I-1 through I-4 and the six Minors. Its gate is
  also the first gate on the BENCH commits and `48d15ea`.

Working tree at the stop carried `M scripts/atx-build.ps1` (controller tooling, never staged),
`M atx-vol/tests/dispersion_run_test.cpp` (FIXTAIL, in progress), and four untracked `scratch-*/`
directories.

## 8. Next steps, in order

1. Collect WSAPERF and FIXTAIL, review both, and merge them into the trunk.
2. Run a gate at the resulting tip — nothing has gated `48d15ea` or the four BENCH commits.
3. Decide whether the `atx_vol` scoping stands. If the whole-repo run matters before this goes
   anywhere, it needs roughly an hour of a genuinely quiet box.
4. B7's baseline JSON, and any re-measurement of T1, need a **reserved** quiet window — not an
   observed-quiet one.
5. Clean up: `scratch-m2/`, `scratch-repin/`, `scratch-reconcile/`, `scratch-bench/`, and the
   remaining worktree build directories.
6. **The merge to `main` is the user's call.** This trunk now contains `main` rather than diverging
   from it, which is the cheaper direction to resolve later.
