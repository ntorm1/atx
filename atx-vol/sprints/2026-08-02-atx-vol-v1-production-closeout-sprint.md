# atx-vol v1 Perf Closeout Sprint Plan

Successor to `2026-07-26-atx-vol-production-v1-release-sprint.md`. That sprint was cut in two after
S6-T29: Sprints 1–5 closed with gates PASS (S1 `a4567b0`, S2 `bb6d6a4`, S3 `a02bd5f`, S4 `fcfa3eb`,
S5 `2175c39`), and Sprint 6 closed early at T29 (`8e6f27a`) so the branch could merge to main. This
plan carries everything that remained: the perf items 6.3–6.7, the Sprint-6 aggregated review, the
release gate (which owns the v1.0.0 tag — **the tag was NOT created at merge time**), and the final
whole-branch review.

Process: superpowers:subagent-driven-development, same as the parent sprint. Ledger and per-task
briefs for the parent live under `.superpowers/sdd/2026-07-26-atx-vol-production-v1-release-sprint/`
(untracked, in the `C:\atx-wt\pool-3` worktree); Sprint-6 named checks 1–8 (T29) are already
ledgered there and carry into this sprint's aggregated review.

## State inherited from the parent sprint

- **T29 (items 6.1 + 6.2) is DONE and reviewed-in-part** (accepted, checks 1–8):
  - 6.2 shipped as `kPrefetchLookahead = 2` constant in `backtest.cpp` (private snapshot-cache
    capacity = W+2). NOT a config field — the plan's "existing `prefetch_depth` config" premise was
    false (no such field ever existed). Config-field promotion is a post-v1 roster item.
  - 6.1 is an evidenced negative result: whole-mapping `Mapping::prefetch()` at `open_copied`
    measured 17/24 pairs SLOWER warm; cold regime unmeasurable on the dev host. Decision comments
    sit at the seam in `surface_archive.cpp`. `Mapping::prefetch()` deliberately keeps zero
    callers. Post-v1: `Mapping::prefetch_range` (atx-tsdb) for the open_mapped/Sealed path.
- **Pre-merge NAV anchors** (at `8e6f27a`, before main was merged in): rel
  `123243.11724603444`, rel-avx2 `123243.11724602008`, corpus
  `C:\atx-data\spy-dispersion\runs\parity-full` (4 sha256 pins, 25 entries). Recipes verbatim in
  the parent's `task-s4-gate-report.md` legs 3–4. **The merge with main (SPX Wilmott / European
  semantics work) may legitimately move NAV — see Task 0.**
- Test counts pre-merge: `-Ctest -L atx_vol` 2629 registered / 2622 executed, exactly 5 known
  failures (RunDir ×3, SurfaceDbAdmin.VerifyDbFlagsNonFiniteAtmProbe,
  ListedDispersionPipeline.MarkDivergenceObserverRidesTheEngineStepHook), 1 flake
  (PricingExecutor.NestedDispatch under -j16, rerun clean). Merge may change these — Task 0
  re-establishes the baseline.
- API frozen: version 1.0.0 single-sourced from `project(VERSION)`; Tier-A umbrella = 56 entries;
  RunConfig arity pin = 16; install/export live with the out-of-tree smoke consumer
  (`scripts/atx-vol-test-package.ps1` — standing packaging gate).

## Task 0 — Post-merge re-baseline (NEW, mandatory first)

The parent sprint's determinism evidence predates the merge with main. Before any perf work:
1. Full matrix dev preset (`-Ctest -L atx_vol` and monorepo `-L`-less run): record new
   registered/executed counts and the failure list; diff against the pre-merge known-5 and the
   monorepo known-4. New failures are merge regressions — fix before proceeding.
2. NAV leg on `rel` and `rel-avx2` (parent recipes): if NAV moved vs the pre-merge anchors,
   determine WHY (expected from main's European-semantics/Wilmott work vs merge error). If
   explained and intended, re-pin anchors + section digests and record the ruling; if not, treat
   as a merge defect.
3. Re-run the packaging smoke consumer.
Task 0 output (new counts, new-or-confirmed anchors, updated corpus pins if sections changed) is
the baseline every later task in this plan cites.

## Task 1 — Thin-LTO on rel presets (parent 6.3 / S6-T30)

Brief already written: parent workspace `task-s6t30-brief.md`. Summary: enable
`CMAKE_INTERPROCEDURAL_OPTIMIZATION` (thin-LTO under clang-cl 18) on `rel`/`rel-avx2` ONLY (dev
untouched). Centerpiece is the 2×2 bit-parity table {rel, rel-avx2} × {LTO off, on} against the
Task-0 anchors. If LTO moves ANY bit: STOP, controller decides policy (re-pin vs disable
contraction vs skip LTO) — never silently re-pin. Bench evidence paired/interleaved only (see
Measurement policy). Report build/link-time delta. Full test matrix under the LTO rel build; diff
failures against the non-LTO rel matrix run FIRST. No source changes — an LTO-exposed ODR/link
issue is a finding (interacts with S5-T25's tagged inline namespaces), not an inline fix.

Note: a partial, unverified preset edit for this task existed briefly on the branch and was
reverted before merge. Start clean from the brief.

## Task 2 — AVX2 pack utilization + FullGreeks dynamic partition (parent 6.4 + 6.5 / S6-T31)

- 6.4: packs broken per `(uid, side, raw-T-bits)` run → ~25% lane fill on dispersion books;
  accumulate across T-runs before flushing (`src/laned_greek_run.hpp:213-249`,
  `src/priced_surface_view.cpp:869-876`). Kernel unchanged.
- 6.5: static contiguous `run_ranges` over sorted uniques hands one worker the long-dated tail
  (~40% parallel-region loss at 2× cost spread); `run_dynamic` is determinism-safe here (disjoint
  slot writes) (`src/portfolio_pricer.cpp:932-943`).
- Line numbers date from the parent review at `1be0668` — re-locate before editing; the merge may
  have shifted them.
- Bit-parity constraint: 6.4 changes pack composition — the existing pack-composition-invariance
  and thread-count-bit-identity tests are the gate; any golden/fingerprint movement is a STOP +
  ruling, not a re-pin. 6.5 must preserve bit-identity across thread counts (disjoint writes).
- Paired benches on the solve chain both items, plus NAV leg unchanged vs Task-0 anchors.

## Task 3 — Allocation/medium batch (parent 6.6 + 6.7 / S6-T32)

- 6.6: `StepMarkMemo` node-map clear/reinsert per step → dense generation-stamped vectors
  (`backtest.cpp:171-180` vs `:564-568`); `current_identity` ifstream open per load/prefetch on
  sealed archives (`snapshot_cache.cpp:103-119`); `resolve_universe_uids` recomputed twice per
  step with O(N²) dup scan (`dispersion_strategy.cpp:237,369`); `uid_of` linear scan →
  `lower_bound` (`backtest.cpp:1492-1505`); SVI LM scalar Black-76 loop → batch
  (`svi_calib.cpp:607-629`); serial `reduce_*_totals` duplicating kernel work
  (`portfolio_pricer.cpp:1929-1965,1011-1042`).
- 6.7: `BacktestResult` reserve sweep, O(n²) uid dedup, `cache_key` lexically_normal allocs,
  `kGreekChunk` 128→32.
- Same re-locate caveat, same NAV-unchanged gate, paired benches for anything claimed as a win.
  Items that measure as non-wins are recorded as negative results, not shipped (T29 precedent).

## Task 4 — Sprint aggregated review + fix rounds

READ-ONLY aggregated reviewers over Tasks 0–3 (named checks; T29's checks 1–8 from the parent
ledger join this set). Fix rounds capped at 5, one combined fix dispatch + scoped re-review per
round; minors deferred to the final wave unless elevated with rationale.

## Task 5 — Release gate (parent's Sprint-6 second half / S6-T33)

Brief already written: parent workspace `task-s6t33-brief.md` (carries the README perf-figure
re-measure note). Legs:
- Full test matrix: `rel`, `rel-avx2`, forcescalar leg, adversarial archive suite, python
  bindings suite.
- Bench suite vs pre-sprint-1 baseline; publish before/after for the 135-session backtest cold +
  warm (paired method).
- NAV determinism both ISAs vs Task-0 anchors.
- External-fixture decision: 134 `GTEST_SKIP` sites / 47 skipped hinge on the ~19 MB RunArchive
  e2e fixture — commit it, generate it in CI, or accept and document the skip set.
- Standing dispositions (from the parent ledger, all must be ruled): T17-F3 verify wire-in zero CI
  coverage; deep-OTM put parity gap; RunDir identity backlog; v1 framing-block condition;
  SurfaceDbAdmin cb7fe2e decision; no committed avx2 reference BYTES (digests only);
  earnings_repro* move candidate; `projection.cpp:424` `with_no_arb_check` (**BLOCKER if
  undispositioned** — implement or reject non-default at validation); README perf figures
  (re-measure).
- **Tag v1.0.0** (controller, after PASS), final CHANGELOG/README pass.

## Task 6 — Final whole-branch review

Most capable model, ONE fix wave, then superpowers:finishing-a-development-branch. Triage input:
the deferred-minors roster in the parent ledger and STATUS file (S2/S3/S4/S5 minors, T23 F-1/F-2/
F-4, T28 F-3 tier-count contract test, B-M-4 stderr-promise block).

## Measurement policy (binding, all tasks)

Sequential before/after on the dev box is NOT a valid instrument: the same binary measured 674 vs
1096 ms medians hours apart, and a sequential table showed the OPPOSITE sign of two paired
experiments (T29). All bench evidence must be paired/interleaved A/B within one session, ≥10
pairs, reporting per-pair deltas, win-counts, and medians. Keep both binaries on disk
simultaneously so pairs alternate binaries, not rebuilds. Bench configure:
`scripts\atx-build.ps1 configure -Preset rel -Bench` (flag form, never `--preset`).

## Environment cautions (every dispatch)

- `Set-Location C:\atx-wt\pool-3; ` prefix (or the worktree in use); ctest alternation requires
  `powershell -File scripts\atx-build.ps1 -Ctest -R '...'`.
- Never PowerShell Get-Content/Set-Content or `>` on sources (mojibake incident); Read/Edit tools
  only. Byte-preserving native output via bash or `cmd /c "... > f 2>&1"`.
- The IDE Grep tool has returned fabricated hit-lists in this repo; load-bearing enumeration via
  `git grep` / Select-String + Read verification.
- RTK hook filters plain `git diff`/`git log`; use `rtk proxy git ...` for raw output.
- Shared dep cache `C:/atx-cache/deps` is cross-worktree-mutable: dep-not-found ⇒ regenerate (dev
  configure + build) before diagnosing code.
- No `git stash`, no `git checkout <sha> -- <paths>`, no `git rm -r`, no `git add -A`/`-f`;
  explicit-path staging; never clang-format; 100-col; stale-exe link failure = retry once;
  `git commit -F` for long messages.
- Sprint reports/briefs stay untracked under `.superpowers/`; agents never commit them.

## Out of scope (unchanged from parent)

SpiderRock Parquet decoder (`data.cpp:558`), C8/CStar/Wing in `calibrate_pool`, SplineVol warm
refit, the 6 deferred calibration research knobs, Python `StepObserver` binding (documented
instead). Post-v1 roster: python step_observer/cancel bindings, per-(uid,expiry) risk slice,
exe-name alias, ATX_VOL_PROFILE rename, ATX_VOL_CORPUS_DATE_BATCH + prefetch-lookahead config
fields, quote_rejects reader-or-gate, tools/research link isolation, `Mapping::prefetch_range`,
per-date drain shape in populate cancellation.

## Estimate

Tasks 0–3 ≈ 3–4 days; review + gate + final review ≈ 2 days.
