# Surface DB Production — Status Update 2

**Date:** 2026-07-25 (second operator stop)
**Branch:** `feat/surface-db-prod` — worktree `C:\atx\.claude\worktrees\surface-db-prod`
**Tip:** `5d26105` · **7 commits** since the last status (`492ff24`), 63 on the branch
**Supersedes:** `docs/superpowers/2026-07-25-surface-db-prod-status.md` (read this one first)
**Ledger:** `.superpowers/sdd/surface-db-prod/progress.md` (gitignored)

**Money: still $0.0000 of the $100 budget.** No paid call this session.

---

## Bottom line

**The Critical that blocked merge at the last stop is closed, reviewed, and verified.**
Six more commits landed behind it, each reviewed. The working tree is **clean** — nothing
uncommitted this time.

Three things are **not** done and are listed under *Not done* rather than buried: the
production rerun-zero measurement, T7's Python verification, and the merge of `main` into
this worktree.

The production artifact `C:/atx-data/surface-db/prod-2026-07` is **untouched** — 17
partitions, 51 symbols, **858 surfaces**, generation 90, byte-for-byte as the last status
left it. Every experiment this session ran against copies.

---

## What landed

| commit | what |
|---|---|
| `d8eec9e` | **Closes the Critical.** A converged carry resume no longer reads as `TOTAL FIT FAILURE`. Also: `cells_carried` on every operator surface, `success_rate` no longer 0% for carried symbols, the real FNV prime, and an explicit `DbConfigAttestation` on `write_partition` (default fails closed). |
| `289ff3a` | The operator manual, which still documented the *pre-fix* exit-3 rule in the section the `--r` flag tells operators to read before every run. |
| `6ef88e5` | **FIX-E.** A present-but-*disabled* symbol is no longer deleted when its date is rewritten. Plus the shared `osi_root_matches_ticker` predicate and the BRK.B definitions-exporter fix. |
| `a74eb92` | States the carry exemption's real cost, adds an operator warning for the ambiguous shape (**exit stays 0**), and moves the attestation to the frame that actually runs the gate. |
| `b76678a` | **Pins the degraded-cell data loss as current behaviour** with an unmissable comment, plus the M-6 decision and four doc corrections. |
| `3706ee1` | **Ships the `disable`/`enable` verb the manual has always named** but no tool could perform. |
| `5d26105` | Stops `enable` recommending a remedy its own action disarms; states the *other* direction of the concurrency hazard. |

### Review record

| task | outcome |
|---|---|
| FIX-D fix 1 (`d8eec9e`) | approve with follow-ups — 0 Critical, 3 Important, 4 Minor → **all closed** |
| FIX-E (`6ef88e5`) | **ACCEPT** — 0 Critical, 2 Important, 7 Minor → both Importants closed |
| FIX-D fix 2 (`a74eb92`) | approve with follow-ups — 0 Critical, 1 Important → closed by `3706ee1` |
| FIX-G (`3706ee1`) | approve with follow-ups — 0 Critical, 2 Important → **both closed by `5d26105`** |

---

## The three findings worth reading

### 1. A wrong `--r` destroyed 95 stored surfaces, and I am the one who ran it

I invoked the production gate with the CLI's **default `--r 0`**. The database was built
with **`--r 0.043`** — recorded in the run log, which I should have read before running
rather than after.

On the copy: `cells_ok 55, cells_failed 98` of 153 scheduled, against 147/3 for the same
window previously. **The copy went 858 → 763 surfaces.** Production was never touched, which
is the only reason this is a finding and not an incident.

A control settles what it means. `step6-build.exe` — built **before** FIX-C, FIX-D, FIX-E,
FIX-G and both fix rounds — over the same window with the same wrong rate on its own fresh
copy produced counters **identical field for field**, and the same 858 → 763. So:

- **No code regression.** The rate is the entire story.
- **The data loss is pre-existing**, exactly as `b76678a` scoped it — but it now has a
  *measured* blast radius on production-shaped data instead of a source-read argument. One
  operator flag error, 95 surfaces, one run.
- Incidentally a clean determinism check across two independently-built binaries and five
  commits.

**And the tool exited 0 with no banner.** 64% of scheduled cells failed and the only signal
was per-cell lines — 32 printed, 66 elided under the cap. The `--r` diagnostic fires only on
*total* fit failure; the carry-masked warning needs carried cells, which are zero on a first
post-carry-over resume. **The exact shape that diagnostic exists for is silent unless it
wrecks everything.** That is a real gap and it is not fixed.

### 2. Seventeen test targets were never built — repo-wide, not this branch

The first full `ctest` reported `atx-tsdb-tests`, `atx-impl-tests` and fifteen
`atx-engine-*-tests` as `Not Run`. `ATX_BUILD_TESTS` is `ON` and every `tests/` subdirectory
*is* added — they are simply excluded from the `all` target. Building one by name needed only
a **link** step, so the objects were already compiled.

Consequence: **a bare `-Ctest` in this repository reports seventeen targets as `Not Run` and
exits non-zero.** Anyone reading "Errors while running CTest" as the known-failing set has
been gating on a fraction of the repo. Every gate this sprint ran before this one, mine
included, was narrower than it appeared.

With all 38 binaries built: **5651 tests, 36 failed, 99% passed.**

Attribution is by **reachability**, not by "they look unrelated": the branch diff touches
exactly `atx-vol` (46 files) and `docs` (4), and `atx-impl-core` links
`atx::core atx::engine atx::tsdb` — **`atx::vol` is not in its link closure**. The ~31
impl/engine failures are structurally unreachable from this diff. Isolation re-runs split the
rest: three pass alone (load-sensitive under `-j16`), while `RobustPipelineE2E` ×2 and the
`atx-impl` optimize/stage suites fail **consistently** — genuinely red, in modules this branch
cannot reach.

**Honest limit:** because those targets were never built in this sprint, there is **no
branch-point baseline** for them. Reachability is the argument, not a before/after diff.

### 3. A fix shipped the same defect class it was fixing, one layer up

`3706ee1` exists because the manual documented a remedy — disable a failing symbol — that
**no shipped tool could perform**. The admin CLI's six subcommands were all read-only,
`upsert_symbol` appeared nowhere under `tools/` or `python/src/`, and the build CLI's only
flag *re*-enables.

Its review then found that `enable`'s new stderr note told operators to "re-run the build
with `--retry-disabled`" — a flag that only re-selects symbols whose config is **still
disabled**. The note prints *after* the enable has flipped that bit. Following it verbatim
gives a silent no-op. Fixed in `5d26105`.

---

## Corrections to the previous status

- **"Five copies of the ticker→OSI-root rule, and they disagree" was wrong**, and it was
  mine. The trailing-digit divergence is deliberate, argued and load-bearing — the C++
  predicates are narrower than the Python producers *on purpose*, because a producer can
  file an adjusted `AAPL1` row under `underlying="AAPL"` and a guard exists to fail loud on
  that. Unifying onto the looser rule would reintroduce the silent mispricing this branch
  closed. Also: 16 sites, not 5; three of my five paths were wrong, two of them example-gated
  and not in the default build.
- **The carry-over exemption's cost was understated** (by the implementer, and I repeated
  it). It is not only a wrong `--r`: the exemption keys on `cells_carried == 0`, so *any* run
  that carried a cell is exempt — including one whose every *scheduled* cell failed
  systematically. Now stated in general form in the manual and the header.
- **My framing of the degraded-cell fix was incomplete.** I posed it as a choice between two
  attestation options. Both were beside the point: **presence**, not the fingerprint, keeps a
  date in the rewrite set, so preserving the bytes retires the cell from the retry loop on
  *any* attestation. Verified at source myself.

---

## Not done

1. **Production rerun-zero measurement.** The correct two-pass run at `--r 0.043` was
   **stopped mid-flight** by the operator stop. `gate-2026-07` is a partially-processed copy
   — delete it and start fresh. Expected: pass 1 still re-fits (stored fingerprint 0) and
   stamps; pass 2 must show `cells_refit 0`. **The gate is `cells_refit == 0`, not
   `cells_to_fit == 0`** — the latter is unachievable by design.
2. **T7 Python binding.** Still unverified. The compiled `_core` resolves to `C:\atx`, a
   *different checkout on another session's branch*, and lacks `build_surface_db` entirely.
   `pip install -e` from this worktree would **repoint that session's live environment** —
   use a scratch `--target` with `PYTHONPATH` instead. The binding TU compiles standalone;
   link and import are unverified.
3. **Merge `main` into this worktree.** Not started. `main` was 6 commits ahead at
   `2858cab`, all in `backtest_driver`/examples/tests — **zero file overlap**. Re-check the
   tip at merge time; a neighbouring session is committing actively.

## Open items carried forward

- 🔴 **Degraded-cell data loss** (`b76678a` pins it). A present-and-*enabled* cell whose
  re-fit fails loses its stored surface. **Measured: 95 surfaces in one run.** The fix needs
  a format change — one reserved bit in `ArchiveSurfaceProvenanceRecord` meaning *"preserved
  failure: re-attempt, never carry"*, with two named hazards. **Deliberately not shipped at
  branch end**, after the last full gate.
- 🟠 **No loud signal for a mostly-failed run.** See finding 1.
- 🟠 **Manifest rewrite has no generation compare-and-swap.** Both `upsert_symbol` and
  `write_partition` rewrite the whole manifest from their own snapshot, so an interleaved
  mutation can **drop a partition record** and regress the generation, after which `refresh()`
  never picks the newer manifest up. Documented in both places; a real fix is a design change.
- 🟠 **The ambiguity warning cannot be cleared on `prod-2026-07`.** Failures are per-cell;
  the switch is per-symbol. `3706ee1` makes the action performable and honest about its
  price — it does not make the warning clearable.
- 🟠 **`enable` cannot tell an operator disable from a selector disable** — the manifest
  does not record which. One flag in `DbSymbolRecord` would fix it.
- 🟠 Producer-side normalisation (`E2-d`) deliberately deferred: `atx-core` cannot depend on
  `atx-vol`, so it is a layering decision, not a cleanup. Current state is **safe but noisy**.
- 🟠 A hard `CHECK` crash under memory pressure, not root-caused; a full-month full-universe
  build does not fit in 16 GB.

---

## Test state

- **Whole repo, all 38 binaries: 5651 tests, 36 failed (99%).** See finding 2 for
  attribution and its limit.
- Surface-db suites green throughout; each commit's targeted suites verified by me off a
  **copied** binary so a later relink could not invalidate the result.
- `atx-vol/src/pricer_fitter.cpp` remains **byte-identical to `main`** — re-verified by three
  independent reviewers across this session.

## Process notes worth carrying

- **Run destructive experiments against a copy with a hash baseline.** It cost a 6.5 MB copy
  and it is the only reason finding 1 is a finding.
- The `PowerShell` tool's cwd is `C:\atx` — the wrong checkout — and the Bash cwd resets
  there intermittently. Use an explicit `cd` in every call.
- **Piping a command whose exit code matters reads the pipe's status, not the tool's.**
- A poisoned **ccache** entry can present as a compile error in untouched code
  (`#include of 'cmake_pch.hxx' not seen…`). Remedy: `CCACHE_DISABLE=1` for one build. It is
  not a source defect; do not go hunting one.
