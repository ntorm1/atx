# Sprint plan — Robustness foundation (Phase R + M1-lite)

**Branch:** `feat/robustness-foundation` (worktree, off `main` @ `52c2d1b`)
**Source roadmap:** `docs/superpowers/plans/2026-06-20-tradeable-mega-alpha-roadmap.md` §3, §8.
**Sequencing (human):** robustness-first — gate the accumulation sweep BEFORE scaling it.
**R2 scope (human, 2026-06-20):** build the **walk-forward holdout** (roadmap R2 as written), not the
robustness-gate seam.

## Governing principle
The mega-alpha thesis (accumulate many alphas × many seeds) is exactly the operation that defeats
per-run DSR and exhausts the fixed terminal holdout. This sprint makes the sweep trustworthy:
cross-sweep trial accounting (R1) + walk-forward holdout (R2) + PBO recorded / OOS-on for
accumulation (R3), then the minimal sweep to exercise them end-to-end (M1).

## Determinism discipline (NON-NEGOTIABLE — every task)
- F1 search digest byte-identical across substrates/worker-counts; OOS reports byte-identical across
  {seq, Process@1, @4}. A shifted golden that should stay identical = BUG.
- Every new path is **opt-in / flagged with a byte-identical default**. The legacy single-run,
  non-accumulation path (no `--library-dir`, `oos_fraction==0`) stays byte-for-byte unchanged.
- `eval/oracle.hpp` (independent BATCH reference) stays UNCHANGED.
- The ONLY sanctioned golden re-baseline this sprint is the **library manifest `version_id`** (R1
  folds the cumulative-trials field into the content-address). No other golden may shift; call out
  any that does as a BUG, not a re-baseline.
- `/W4 /WX` clean. `ATX_ASSERT` is compiled out under NDEBUG (`macro.hpp:130`) — never put
  load-bearing logic in an assert.

## Build / test
See `.superpowers/sdd/scratch/BUILD_ENV.md`. Build-rel (clang-cl + Ninja, /W4 /WX). Engine tests +
`atx-impl-tests`. Pre-existing unrelated failures NOT to chase: `atx-impl-tests`
`AtxImplPanel.BuildsPanelFromSegments`; a `-Werror` break in `eval/eval_cpcv_test.cpp`.

---

## Task order (each independently testable; byte-identical default)
1. **R1** — Library-persisted cumulative cross-sweep trial counter → DSR deflation.
2. **R2** — Walk-forward holdout window (engine `reserve_window` + `mine_into_oos`; terminal default
   byte-identical).
3. **R3** — PBO recorded on the accumulation/OOS path + OOS-on-by-default for accumulation runs.
4. **M1** — Wire `ResearchDriver` into a `sweep` subcommand carrying R1 (cumulative counter) + R2
   (per-run window advance) + R3 (OOS-on).

Briefs (the source of requirements, with exact values) live in `.superpowers/sdd/scratch/`:
`task-R1-brief.md`, `task-R2-brief.md`, `task-R3-brief.md`, `task-M1-brief.md`.

## Acceptance (whole sprint)
- A K-run sweep deflates survivors against ≈ cumulative-N (R1), validates each run on a DIFFERENT
  walk-forward window (R2), records PBO + runs OOS by default (R3), all driven by one `sweep`
  command (M1), and is **twice-run byte-identical** (ResearchReport.digest + library manifest
  version_id).
- Single-run `discover` with no `--library-dir` and no new flags = byte-identical to `main`.
