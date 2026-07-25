# Pipeline SOTA Sprint — status as of 2026-07-24

Plan: `atx-vol/sprints/2026-07-21-atx-vol-pipeline-sota-sprint.md`
Integration trunk: **`feat/pipeline-m` @ `ed8c751`**. Local `main` has not been touched
and will not be — the user owns that decision.

This is a checkpoint written at a deliberate stop, not a completion report. Nothing below
is claimed as finished unless it says so.

---

## 1. Where the work stands

| Workstream | Branch | Tip | Implementation | Review | Merged to trunk |
|---|---|---|---|---|---|
| M (keystone) | `feat/pipeline-m` | — | done | approved | gated @ `5e2c31a` |
| A pricer | `feat/pipeline-a` | `bf6968c` | done | approved-with-minors (0C/0I) | ✅ `e35cddf` |
| B fitting | `feat/pipeline-b` | `eed7131` | done | approved-with-minors (0C/2I process) | ✅ `264b2fe` |
| C storage | `feat/pipeline-c` | `07dd317` | done | approved-with-minors (0C/1I) | ✅ `9390a15` |
| G greeks | `feat/pipeline-g` | `de0101b` | done | **needs-work → fixed** (0C/3I) | ✅ `96172e5` |
| FIX-1 | `feat/pipeline-fix1` | `36bf7e3` | done | folded into final review | ✅ `09060a2` |
| FIX-2 | `feat/pipeline-fix2` | `c601504` | done | folded into final review | ✅ `63daffe`, `ed8c751` |
| E analytics | `feat/pipeline-e` | `81cfc33` | E1–E6 committed | **not reviewed** | ✗ |
| F backtest | `feat/pipeline-f` | `4e7e04c` | F1–F6 + 3 review follow-ups | approve-with-follow-ups, follow-ups closed but **not re-reviewed** | ✗ |
| T corpus | `feat/pipeline-t` | `817959a` | T1–T2 + 4 review follow-ups | needs-work → **fixes not re-reviewed** | ✗ |
| Y python | `feat/pipeline-y` | `cfbcd97` | Y1–Y4 committed | **not reviewed** | ✗ |
| FIX-3 | `feat/pipeline-fix3` | `ed8c751` | **in flight, no commits yet** | — | ✗ |

All four wave-2 branches have already merged the trunk into themselves, so they carry
FIX-1 and FIX-2 and their post-merge test counts are the quotable ones.

## 2. What is genuinely done and verified

- **Wave 1 merged with zero conflicts.** The only shared file between any two wave-1
  branches was `tests/CMakeLists.txt` (A and C), append-only on both sides.
- **WS-T's byte gate passed on the real corpus**: golden 82-date rebuild, real OPRA,
  serial baseline vs production fan-out — 82/82 archives `cmp`- and sha256-identical,
  both arms admitted=902 / source_failed=407. An independent reviewer re-ran it and
  reproduced the result including the digest-of-digests.
- **The pinned 135-session replay does not abort** under WS-F's changed defaults. Verified
  by running it read-only; nothing was written under `C:\atx-data`.
- **The suite is now safe to run in two worktrees at once** (FIX-2 F2-A), demonstrated
  with two concurrent processes of the same binary, distinct temp roots, no leak.

## 3. Open items, by severity

### Must be resolved before the sprint can close

1. **No clean trunk serial gate exists yet.** The only whole-suite run against the merged
   trunk was cut off at 1467/2104 (zero failures at that point), and it was taken while
   several agents were building — see §4. Must be re-run end to end at low concurrency.
2. **Wave 2 is unreviewed.** E and Y have never been reviewed at all. F's and T's review
   follow-ups were implemented but not re-reviewed. Four reviews are outstanding.
3. **The golden re-pin has not been run.** Two independent, separable causes will move the
   82- and 135-session pins:
   - **WS-E's E1** — an exact ×100 projected-route book rescale (unit correction: dollar
     vega per vol point). Analytic, not measured.
   - **Wave 1's pricing/greeks drift** — measured at 1e-7..1e-13 relative, concentrated in
     greek-derived columns. Final NAV moved `125026.05919122705 → 125026.05919131593`,
     i.e. 7.1e-13 relative, which is ten-plus orders below a tick.
   These do not resemble each other, so contributions can be attributed at re-pin without
   a fork-base A/B rebuild. Old pins: 82s `5e7ca065…`, 135s `141173fd…`.
4. **A reconciliation blocker will be hit during that re-pin.** `run-backtest` writes
   `backtest.tsv` and then fails in the reconciliation stage with
   `NotFound: listed OPRA join: contract definition missing`. The diff over
   `listed_opra.cpp`, `opra_panel.cpp` and `opra_batch.cpp` is empty, so no workstream
   caused it. Either the five-file input copy used for the test was incomplete, or it is a
   pre-existing condition — the run dir holds a `definitions-orig.tsv` of identical size,
   so the definitions input has been swapped at least once. Running against the full run
   dir rather than a copy distinguishes these immediately.
5. **FIX-3 is in flight and unfinished** — the Ok-stamp is still ISA-dependent on the
   scalar path, and the temp-isolation fix does not yet cover `atx-impl`, `atx-engine` or
   `atx-tsdb` tests.

### Known and accepted, for the record

- `ATX_SIMD_ISA` is new shipping-binary behaviour: it seeds the process-global ISA at
  static init in production, not only in tests. Defaults are safe (unset → Auto), input is
  validated, `ForceAvx2` stays guarded by `have_avx2()`, and it is documented.
- Two ctest runs sharing **one** build dir still share the relative artifact-cache
  directory (`tests/support/cached_artifacts.cpp`). Cannot arise in the per-worktree model
  used here; a single-build-dir CI matrix would hit it.
- F6's quote-staleness gate cannot fire on the current feed. The OPRA panel's `ts` column
  holds exactly one distinct value per file — a snapshot stamp, not an observation time.
  Rather than fake a measurement, the counter now reports `stale_unevaluable` separately
  and does not treat it as a rejection. The machinery goes live when a per-quote-timestamped
  source is wired.
- Deferred with confirmed blockers: A5, A6, A7's solve-count half, B4's default flip, B6's
  selector holdout, B7's baseline JSON. All timing and throughput rows across every
  workstream are deferred to a quiet-window re-run; **none of the numbers measured during
  this sprint are citable.**

## 4. Two environment defects that invalidated earlier measurements

Both were misdiagnosed for most of the sprint, and both are now fixed. They matter because
they mean earlier "green" results were not trustworthy.

**The host is RAM-bound, not core-bound.** 16 logical cores but 15.7 GB of RAM. A heavy
clang-cl TU holds 1–3 GB, so five agents at `-j 4` is up to twenty compilers against 15.7 GB.
Every `LLVM ERROR: out of memory` and clang frontend crash reported in this sprint — by four
separate workstreams, each on files they never touched, each written off as a "transient
shared-machine artifact" — was the box running out of memory. Build parallelism is now `-j 2`
while siblings run.

**The test suite corrupted its own results under concurrency.** ~35 fixtures derived scratch
paths from `fs::temp_directory_path()` with fixed names, and `test_root()` calls `remove_all`
on entry — so a second process *deleted* the first's tree mid-write. That is why the symptom
was an `IoError` rather than an assertion. Two independent agents hit it on two branches
within an hour. **Every full-serial count quoted while siblings were running is superseded**,
including the partial trunk gate. Fixed by giving each test process its own temp root before
`main()`.

A third, milder one: builds were being run with raw `cmake --build`, which does not see the
preset environment and therefore never sets `CCACHE_BASEDIR`. Cross-worktree cache sharing
was off for most of the sprint; the repo-wide hit rate had fallen to 18%. All builds now go
through `scripts/atx-build.ps1`, target-scoped. Reclaimed 27.2 GB of dead build directories
and pruned 11 stale worktree registrations along the way.

## 5. What the reviews actually caught

Worth recording, because it is the argument for keeping independent review in the loop:

- **Merge damage invisible to git.** The keystone merge unioned two AVX2 parents cleanly —
  zero conflict markers — and silently dropped a pairing: unrequested-greek zeroing survived
  on the put batch and vanished on the call batch. Live on any no-rate-shift P&L step, where
  AVX2 call lanes returned `rho = 0` and scalar-patched lanes returned a populated value in
  the same column. Found only because the WS-G reviewer was explicitly tasked to semantically
  re-review an auto-merged surface.
- **A guard that closed two of three sites.** WS-G's finite-greek sweep missed a third
  Ok-stamp on a live dispersion path, so the finding it was closing stayed open.
- **Two knobs that could not fire.** F6's staleness bound and F5's subset-deserialize both
  parsed, reported, and did nothing on the production path — the exact class WS-F's own
  parse-time rejection was built to kill.
- **A fix that solved the wrong half.** WS-T's worker reclaim was evaluated at task-claim
  time only, so the straggler symptom it targeted was untouched; the test that should have
  caught it never saturated the pool.
- **A vacuity trap caught by the author.** WS-F found that adding a fill policy alone would
  have been bit-identical, because NAV measures the first move from the entry mark rather
  than from what was paid.

## 6. Next steps, in order

1. Finish FIX-3; merge.
2. Review E and Y; re-review F's and T's follow-ups. Merge wave 2 in order **Y → E → T → F**
   — Y has zero file overlap, and F last so it absorbs the one real conflict
   (`corpus_test.cpp`, where F's and T's edits are independent regions).
3. Run the whole-repo serial gate on the merged trunk, at low concurrency, end to end.
4. Quiet-window run: the golden re-pin as a single serialized event covering both causes in
   §3.3, against the full run dir so §3.4 resolves; then the deferred timing rows (T1
   utilization, B7 baseline, G4 A/B, A5/A6/A7).
5. Final whole-branch review — FIX-1's four commits and WS-F's three follow-up commits are
   mandatory scope, since neither had a standalone review.
6. Walk the plan's §1 scoreboard line by line, append the sprint report, delete
   `scratch-m2/` (~260 MB), and trim the remaining build directories.
