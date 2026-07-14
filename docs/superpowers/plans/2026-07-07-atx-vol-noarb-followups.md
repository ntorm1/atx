# atx-vol No-Arb Integrity — Sprint A Close-out & Follow-up Backlog

> **For agentic workers:** pick up with **superpowers:subagent-driven-development**. Tasks below are execution-ready; each names its brief, base commit, and acceptance bar. Do the tasks in order (R1 first — it closes Sprint A on the already-committed state).

**Goal:** finish the served-surface no-arb integrity work (Sprint A of the SOTA work module) and capture the follow-ups that surfaced during implementation, so a later session resumes with full context.

**Status at handoff (2026-07-07):** Sprint A calendar-no-arb enforcement is **landed and green**. What remains is (a) the Sprint A end-to-end verification task, and (b) a set of follow-ups that are correctly scoped into later sprints rather than crammed into Sprint A.

---

## Current State

- **Branch:** `feat/atx-vol-carry-deam`  **HEAD:** `5b46b74`
- **Operational ledger (full detail):** `.superpowers/sdd/progress.md`
- **Parent roadmap:** `docs/superpowers/plans/2026-07-07-atx-vol-sota-engine-workmodule.md` (Sprints A–H)
- **Original Sprint A plan:** `docs/superpowers/plans/2026-07-07-atx-vol-surface-noarb-integrity.md`
- **Per-task briefs:** `.superpowers/sdd/task-{1..8}-brief.md`

### Sprint A task status
| Task | What | Status | Commit(s) |
|---|---|---|---|
| A1 | Calendar checker on `CurveSurface` (`arb_check_calendar`) | ✅ done | `527d728` |
| A2 | Honest calendar reporting in `session.cpp` | ✅ done | `2379606` |
| — | Test-suite overhaul (715s→111s parallel; benches env-gated) | ✅ done | `bd269f8` |
| A3 | Dense QP generalized `Gx≥0` → `Gx≥h` (+ slope-below bound) | ✅ done | `4cab210` |
| A4 | Per-node calendar floor in `fit_convex_slice` (`w_prev`) | ✅ done | `6a80c3f` |
| A7 | Band/interval loss behind `ConvexFitOpts.loss` flag | ✅ done (opt-in, **off by default**) | `62d7597` + `918f125` |
| A5 | Sequential calendar-enforcing driver — **calendar-arb-free by construction** on served dense surface | ✅ **landed** | `5b46b74` |
| A6 | Lee wing extrapolation | ⛔ **deferred → Sprint E** (see R2) | patch saved, uncommitted work reverted |
| A8 | End-to-end verification | ⬜ **pending → R1** | — |

**A5 landing result:** SPY dense calendar violations **372 → 2**. Served ConvexDense in-band px_clean 99.5% → **94.65%** (strict-floor enforcement trades ~4.8pp — a deliberate MM product choice). Gates rebaselined (`kPxCleanFloor` 99→94 in `spy_bidask_regression_test.cpp` + `spy_archive_roundtrip_test.cpp`); round-trip fidelity unchanged (bit-identical). Full fast gate: **685/685, 0 failures, ~140s**.

---

## Key Decisions & Findings (read before resuming)

1. **The QP solver is naive about problem size.** `qp_active_set` (`atx-vol/src/dense_slice.cpp`) rebuilds and *densely factorizes* a `(n+nw)×(n+nw)` KKT matrix **every iteration** — cost ≈ O((n+nw)³) × iterations. Adding arb constraints as **rows** is cheap (n unchanged). Adding them as **variables** is not: A7's interval loss introduced `2M` slack variables (`z=[g(N);s⁺(M);s⁻(M)]`), blowing `n` from ~40 nodes to ~440 on a dense SPY slice → per-iteration solve ~1000× costlier + many more iterations → **minutes-slow** full-board fits. **Rule going forward: express arb constraints as rows, not variables** — or, if a slack/band formulation is truly needed, eliminate the slacks via Schur complement so the KKT stays N-dimensional.

2. **Calendar enforcement is NOT free on SPY.** The front slices carry *genuine* calendar structure, so any floor that removes crossings pulls marks off-mid. Measured (fast QP): no-enforce 99.49%/372 · strict-floor 94.65%/2 · capped-floor 97.51%/156. The plan's premise ("enforcement is mostly slack/free") is empirically false for SPY. **Product decision (user):** enforce the strict floor by default; never mark outside the tradeable band to chase tightness; report the residual honestly.

3. **A7 interval loss stays committed but unused on the hot path** (opt-in `ConvexFitOpts.loss`, default `Mid`). If a *soft*-band served surface is ever wanted, reimplement it N-dimensionally (band penalty as a piecewise-quadratic the active-set handles; no `2M` slack variables) — see finding #1.

4. **A6 exposed a cross-task gap:** A5 enforces the floor at fit **nodes** (in-range). Pre-A6 the wings were NaN, so `arb_check_calendar` skipped them → residual 2. A6's finite Lee wings make the pure-extrapolation region (beyond the outermost market strike) visible → ~50 wing calendar crossings that node-only enforcement doesn't couple. Lee wings therefore belong **with** wing-aware calendar enforcement (Sprint E, deep-wing no-arb), not alone in Sprint A.

---

## Remaining Tasks

### R1 — A8: Sprint A end-to-end verification (DO FIRST; closes Sprint A)

**Brief:** `.superpowers/sdd/task-8-brief.md`  **Base:** `5b46b74` (verify the committed state; no new feature code).

Scope (per the A8 brief): corpus rebuild + backtest sanity + full suite, confirming the committed A1–A5 enforcement holds end-to-end and nothing downstream regressed.

**Environment notes (learned this session — hand these to the implementer):**
- Fast gate: `ctest --test-dir build -L atx_vol -j16 --output-on-failure --timeout 900` — ~140s, expect 685/685 (2 `ATX_VOL_BENCH` benches Skipped, ~2 Disabled). Run **foreground**, be patient; do not background-and-yield.
- Data dirs (`data/**`, `data/spy_ytd/**`) stay gitignored/untracked. **No paid Databento pulls** — SPY-dependent tests `GTEST_SKIP` cleanly when the parquet fixture is absent.
- Corpus rebuild is pinned to `ConvexDense`; expect it to now reflect calendar-enforced slices. If a corpus/backtest metric shifts because of enforcement, that is expected — record it, do not mask.
- If a link fails with `permission denied` on `bin\atx-vol-tests.exe`, a leftover test process holds it: `taskkill //F //IM atx-vol-tests.exe`, then relink.

**Acceptance:** full suite green (or only pre-existing/SKIP), corpus rebuilds deterministically, backtest runs sane. Record any enforcement-driven metric shift in the commit/report. **Explicit-path staging only** (never `git add -A`); commit messages end with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer.

---

### R2 — Lee wings + wing-aware calendar enforcement (Sprint E: deep-wing strict no-arb)

Fold A6 into Sprint E and land the two together so the served surface stays calendar-arb-free *including* the wings.

**Saved artifacts from the A6 attempt:**
- Patch: `<scratchpad>/a6-lee-wings.patch` (correct dense-slice Lee-wing impl: linear-in-k total-variance tail, edge slope clamped to Roper/Lee `[0, kLeeCap]`, `w_of_k` helper, in-range `iv` preserved bit-for-bit, plus a **strengthened LeeWings test that also covers the LEFT wing** — the original brief's right-wing-only test passes even un-fixed).
- Report: `<scratchpad>/a6-lee-wings-report.md` (and `.superpowers/sdd/task-6-report.md`).
- `<scratchpad>` = `C:\Users\natha\AppData\Local\Temp\claude\c--atx\0c30335a-578b-4fcf-9fd1-2808e776cae9\scratchpad`. These are session-scratch — **re-derive from the report if the scratch is gone**; the impl is small.

**The missing half — cross-slice wing consistency.** Each slice's Lee wing must dominate the previous (shorter-T) slice's `w(k)` in the wing, or the surface has calendar arb in the extrapolated region. Design options (pick in Sprint E):
- Store the calendar floor (`w_prev` samples or the prior slice's tail params) *in* `ConvexSliceFit`, and floor the served wing: `w_wing(k) = max(lee_extrapolate(k), w_prev(k))`. The driver already has `w_prev` at fit time.
- Or a post-assembly wing clamp across the ascending-T slices in `fit_curve_surface`.
- Or the rigorous **φ-slope term-structure constraint** (the Sprint E headline) that bounds the wing slope consistently across expiries.

**Acceptance:** wings finite, monotone-outward, butterfly-clean (A6's tests) **AND** `CurveSurfaceNoArb.SpyDenseIsCalendarArbFree` residual stays **≤ 2** (not ~50). Do **not** land A6's finite wings without this — it regresses the integrity claim.

---

### R3 — Drive the between-node residual to 0 (grid-aligned floor)

A5's floor binds at fit **nodes**; `arb_check_calendar` scans a 64-pt k-grid, so 2 crossings survive **between** nodes (put wing, ~0.4y). The `curve_noarb_test.cpp` comment already anticipates the fix: enforce the floor on a grid aligned to (or a superset of) the check grid — e.g. add floor rows at the union of adjacent slices' node grids, or at the checker's sample points. **Acceptance:** `CurveSurfaceNoArb` residual → 0, and tighten the test to `EXPECT_TRUE(viol->empty())`. Naturally pairs with R2 (both are "make enforcement match the served/checked surface").

---

### R4 — SPY cold-Andersen-Lake test-perf pass (Sprint G)

`curve_noarb_test` (~94s) is now the fast-gate bottleneck (140s vs 111s pre-A5). The whole SPY family — `curve_noarb`, `SpyBidAskRegression`, `SpyArchiveRoundTrip`, `PnlGreeksConsistency`, `SpyRealOpra` — is dominated by **cold Andersen-Lake per strike over the full board** in `fit_curve_surface` (correctness-over-speed by design). Options: trim each to a representative expiry/strike subset that preserves its property, and/or build a **cached pre-fit surface fixture** the read-only tests can reuse (mirrors the db-suite `built_warehouse` pattern). Note: a naive `curve_noarb` subset changes the residual (it sits at ~0.4y), so subset thoughtfully. **Acceptance:** fast gate back toward ~60–90s with no loss of correctness coverage.

---

## Minor roll-ups (triage at the whole-branch review before merge)

- **A1:** `arb_test.cpp` new tests cover crossing+monotone only; the 4 guard branches (`<2 slices`, `n_grid==0`, `!(k_max>k_min)`, non-finite skip) lack direct coverage.
- **A2:** `session.cpp:284` `n_viol = cal ? cal->size() : 0` treats a *failed* check as arb-free — opposite of the sibling `session.cpp:639` conservative guard. Currently dead (`arb_check_calendar` has no `Err` path); match the sibling or comment.
- **Overhaul:** unguarded `<process.h>` in `backtest_real_test.cpp`; stale `IngestScale` "Linear" test names (linearity no longer asserted); `PnlGreeks` back-sample stops ~5/6·N.
- **A4:** `rows`/`hrows` `reserve(4*N)` can be exceeded when `bound_slope_below` **and** a full floor are both active (up to `5N−4`) → one realloc; bump to `5*N`.
- **A7:** interval branch recomputes the 3rd-difference roughness block (4-line dup of the Mid `H`); keep in sync or hoist a shared helper.
- **A5:** `curve_noarb_test.cpp` passes `64` (int) to a `std::uint32_t n_grid` param — `64u` is marginally cleaner (not a `/WX` failure).

---

## After Sprint A (R1) closes

1. **Whole-branch review:** `scripts/review-package $(git merge-base main HEAD) HEAD` → dispatch the final code-reviewer (most capable model) with the Minor roll-ups list for merge triage.
2. **Finish the branch:** superpowers:finishing-a-development-branch.
3. **Then Sprints B–H** per `2026-07-07-atx-vol-sota-engine-workmodule.md` — with R2 folded into **E**, R3 into **A/E**, R4 into **G**. Mission remains locked to American-equity MM/HFT analytics (exotics / stoch-vol / production MC / GPU explicitly OUT).

---

## Global Constraints (carry into every task)

- **Explicit-path staging only** — never `git add -A`; stage only files the task touches. Data dirs stay gitignored/untracked. No paid Databento pulls (cached slices only).
- clang-cl `/W4 /permissive- /WX` — zero warnings. C++20, namespace `atx::vol`; error vocab `Result`/`Status`/`Ok`/`Err`/`ATX_TRY`; linalg via `atx::core::linalg` (`MatX`/`VecX`).
- Commit only when the task requires it; commit messages end with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Fresh implementer per task → task review (spec + quality) → fix loop → broad final review. Hand each implementer its brief file, not this whole plan.
