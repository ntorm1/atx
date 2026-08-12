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
- 2026-08-12 (universe): screen over 2 sessions × 1,039 candidates ($0.0000
  preflights): 1,021 had data, **616 survivors** (gates: two-sided rows ≥100
  both sessions, listed expiry ≥300d, ≥4 expiries, median rel spread ≤0.60).
  Dominant kill = no ≥300d expiry (≈380 names) — you cannot buy 1y vega where
  no 1y option trades. Top decile of 616 ≈ 62-name portfolio.
- 2026-08-12 (vega_panel): commit 14119cd — `resolve_atmf_strangle`
  (delta-targeted legs via `resolve_strike_by_delta`), `hedged_daily_pnls`
  (daily-rehedged, frictionless, no financing), `VegaPanelBuilder` (incremental
  pending-entry marking, fail-soft), `atx-vol-vega-panel` tool. 9 tests +
  umbrella tier bump 44→45. SPY smoke: 392 rows, labels bounded ±160/$1k vega.
- 2026-08-12 (SP100 pilot, 24,206 rows, 141 OOS days, commit 07f2539):
  * ridge: rank-IC **+0.139** (NW t 5.09); top-decile mean **+29.7**/$1k-vega
    per 21d vs always-all +15.2, random +9.7, bottom −11.6, best single
    factor ivrv_1y_63 +27.4, oracle +140.
  * HGB: IC +0.132 (t 5.78), top-decile **+36.2** — beats ridge here (unlike
    the SPY time-series selector: cross-section has ~100 names/day of data).
  * Decile monotonicity: signal lives in the tails (dec1 +29.7 … dec10 −12.9,
    middle flat) — exactly what a top-decile portfolio wants.
  * Univariate ICs: strongest are NEGATIVE carry/richness — ivrv_1y_63
    −0.157, ivrv_1y_21 −0.122, div_1y_21 −0.117, iv_ratio_1y_1m −0.100,
    term_slope_1m_1y −0.094 (buy vega where IV is cheap vs RV, term slope
    flat/inverted, IV recently fell — Goyal-Saretto / mean-reversion family);
    positive: rv_21 +0.075, iv_3m +0.060.
  * Caveat: whole window long-vol-positive (2026 regime); cross-sectional
    spread (top−bottom ≈ 42) is the robust object, absolute levels are not.
- 2026-08-12 (production pull): free window verified from **2025-08-11**
  (tail-free salvage of the boundary month). 616 names × 13 months resuming
  via 4 month-workers, 50-symbol chunks, per-session $0 preflight gate,
  disk-gated (soft 8G prune+pause / hard 4G abort), `_dbn` pruned per chunk.
