# 2026-08-15 — vrp-ml round 1 (+ round-2 completion gate)

VRP (variance risk premium) ML pipeline across three parallel lanes on frozen
base `a75c389a`, integrated on `integrate/vrp-ml-r1`. Two TSV contracts were
frozen up front so the lanes could build against paper instead of each other:

- `vrp_panel_v1` (`src/analytics/vrp_panel.hpp`, Tier-B): 18-column
  (symbol, date) label/feature panel; label = (rv_fwd^2 − iv_fair21^2)·(21/252);
  features RAW — standardization is the trainer's, in-fold.
- `vrp_signal_v1` (`api/backtest/vol_edge.hpp`, Tier-B): 5-column OOS
  prediction file (symbol, date, pred_label, pred_edge_norm, vov_63d).

## Lanes

1. **vrp-panel** (`7b031177`, APPROVE): `bev_label_factory --vrp-panel` mode —
   SurfaceDb roots → vrp_panel_v1 panel. Traps found: UCRT prints sign-bit
   quiet NaNs as `-nan(ind)` (canonicalized before emit); spot-mirror RV
   off-by-one gate-tested at t and t+22.
2. **vrp-book** (`81077d5e`, APPROVE): `VolEdgeStrategy` + frictions +
   `atx-vol-quant-research vrp-backtest` consumer. Bumped Tier-B census 19→20.
   Accepted-open major: horizon 21d vs rebalance-21 leaves <1.1 calendar days
   of expiry margin — fix before production.
3. **vrp-model** (`646fefec`, APPROVE): schema-v2 `IFairVolModel` seam
   (elastic-net TSV + flat-array GBT scorer, byte-stable round trips) and the
   walk-forward trainer `tools/vrp_train.hpp` / `atx-vol-vrp-train` (purged +
   embargoed folds, train-fold-only standardization, QLIKE in variance levels,
   retransform + insanity clip, deterministic outputs).

## Harness desync incident

The workflow saw a stale BLOCKED marker for vrp-model and round 1 gated without
it (round-1 gate PASS at `2c0249f4`/`4a2643b1`, panel+book only, e2e smoke
SKIPPED). The lane's real completion `646fefec` was reviewed manually
(APPROVE, 30/30 + 14/14 re-run by the reviewer) and landed in round 2.

## Round-2 gate (this document's occasion)

At `integrate/vrp-ml-r1` = `4a2643b1` + merge `646fefec` (`62bd758c`,
conflict-free) + review-mandated fix (`5b46ff99`):

- **Fix (review MAJOR)**: `load_gbt_fair_vol_model_data` reserved vectors from
  file-declared trees/nodes counts before bounding them by the lines present —
  a corrupt count (`nodes\t4e11`) threw `bad_alloc` out of a Result-returning
  loader. Counts are now bounded by `content.size()` before any `reserve`;
  five new loader fail-closed tests (over-declared counts, truncated node
  section/line, trailing content). Dead if-block in `build_vrp_observations`
  deleted (minor #2).
- Targeted: theo.cpp TU check; atx-vol-tests + atx-vol-vrp-train built;
  ctest `VrpModel|VrpTrain|TheoEngineTest` 86/86.
- Fast suite: 3638 ran / 3 failed — the same 3 base-red items carried at
  `a75c389a` (VolUmbrella.NoFixturePathResolvedOutsideTheSharedResolver,
  MarkDomain.CarryRollCloseBooksAtTheCarriedMark, VarswapCompareColumns
  not-built), zero new / 32 skips (unchanged).
- Hygiene (PCH-off, scoped): atx-vol-tests + atx-vol-vrp-train clean.
- Module gates 4/4 PASS incl. REAL golden replay (82-session SPY corpus).
- **End-to-end smoke — first real crossing of both frozen contracts**:
  `bev_label_factory --vrp-panel` on spy-2024+spy-2025 → 495 panel rows
  (21 tail unlabeled) → `atx-vol-vrp-train` → 3 folds (GBT beats baseline
  QLIKE in all three), 210 OOS vrp_signal_v1 rows → `vrp-backtest` consumer →
  392 report sessions, exit 0, cost/edge accounting columns populated.
  Caveat: a single-name universe is a structural no-trade for VolEdge
  (`build_vol_edge_book` requires ≥2 usable names per day), so the real-data
  book is empty by design; multi-name trading paths are covered by the lane's
  fixtures in the fast suite. No schema mismatch anywhere.

## Deferred (logged, not fixed this sprint)

- Per-asset z-stats persistence for the live serving path.
- VolEdge horizon/rebalance expiry-margin risk — fix before production use.
- Five open review minors on vrp-model (non-tail unlabeled-row validation,
  stale seam docs, baseline-file s2 persistence, and friends — see
  `.superpowers/sdd/2026-08-15-vrp-ml/task-vrp-model-review.md`).
- A multi-symbol SurfaceDb corpus, so the e2e smoke can exercise an actual
  trading book.
