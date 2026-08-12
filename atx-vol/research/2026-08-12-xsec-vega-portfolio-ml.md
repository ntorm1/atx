# Cross-sectional single-name long-vol portfolio — ML vega ranking

**Goal.** Of the ~1,000 US stocks with liquid options, predict which are the best
10% to BUY vega on via 1-year ATMF strangles. Build the fitting pipeline,
surface databases, feature analytics and cross-sectional ML needed to construct
the optimal long single-name vol portfolio.

## Data constraints that shape everything

- OPRA entitlement is a **rolling ~12-month free window** (verified again
  2026-08-12: 1,039-symbol preflight over 2026-08-06..07 quoted exactly
  $0.0000). History outside the window is metered ($0.0025–0.009/sym-day →
  hundreds of dollars for a 1,000-name multi-year backfill). Policy carried
  over from the SP100 sprint: **pull only on an exact $0.0000 preflight.**
- On disk today: SPY-only hive 2019-01-02..2026-07-31 + **SP100 (102 names)
  2025-08-01..2026-07-24**. Everything beyond SP100 breadth must be pulled from
  the free window, oldest-month-first (the window rolls forward one day per
  day, so 2025-08 history decays first).
- Consequence: the cross-sectional panel spans **~12 months**. Cross-sectional
  breadth (~1,000 names/day) is the statistical substance; the time axis is one
  regime and will be documented as such.

## Label definition

Per symbol per entry day: resolve a **1-year ATMF strangle** (Δ-symmetric OTM
call + put around F(1y); degenerate config = ATMF straddle), vega-normalized to
a constant structure $vega. Label = forward **h-day delta-hedged PnL per unit
vega** (h = 21 sessions), marked daily on the fitted surface with legs pinned
to absolute expiry, hedge rebalanced daily at close (reuses the
`bev_replay_pnl` daily-rehedge convention, not the 1-day entry-fixed hedge of
`structure_panel`). Hold-to-expiry labels are impossible on a 12-month corpus
and are not the portfolio question anyway — a monthly-rebalanced vega book is.

Portfolio game: each rebalance date, model ranks names; buy top decile
equal-vega; baselines = long-all-names equal-vega, random decile, bottom
decile, and the realized-oracle decile.

## Universe construction (data-driven, no hand-picking)

1. Candidates = S&P 500 (502) ∪ S&P 400 (399) ∪ lqbench (240) ∪ SP100 file
   (103) = **1,039 unique tickers** (`C:/atx-data/universe/xsec_candidates_2026-08.txt`).
2. Screen pull: 2 recent sessions (2026-08-06..07) of OPRA cbbo-1m for all
   candidates into `C:/atx-data/opra-hive-screen` ($0.0000 preflight, chunks
   of 50 symbols — >100-symbol get_range requests 504 at the gateway).
3. Keep names with (a) enough quoted rows/strikes at the snapshot minute,
   (b) a listed expiry ≥ ~0.8y (the 1y leg must exist), (c) spot/forward
   resolvable. Threshold set after inspecting the screen distribution;
   target ≥ ~700 survivors.

## Pipeline (reuse-first)

| Stage | Tool | Status |
|---|---|---|
| Pull | `atx-vol/tools/pull_opra_hive.py` (ET-anchor, resume, $0 preflight) | exists |
| Hive | `C:/atx-data/opra-hive-xsec` date=*/data.parquet | new root |
| Fit | `run_surface_db_backfill.py` → `atx-vol-surface-db-build` (Release, populate preset; thin-board autofit landed 2026-08-09) | exists |
| Surface DB | `C:/atx-data/surface-db-xsec/<year>` | new roots |
| Labels+features | new `xsec_panel` tool (generalizes `structure_panel` to strangle + h-day daily-rehedge mark + multi-symbol loop) | to build |
| ML | new `scripts/xsec_vega_train.py` (cross-sectional walk-forward ranker) | to build |

## Status log

- 2026-08-12: recon complete (SP100 run record, BEV label factory, thin-board
  autofit sprint). Candidate universe built (1,039). Screen pull launched.
