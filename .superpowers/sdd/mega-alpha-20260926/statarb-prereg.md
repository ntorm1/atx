# Stat-arb cluster study — pre-registration (declared 2026-09-27, before any forward-return read)

Owner ask: cluster tradeable US stocks into N groups of K on a rolling window (unsupervised), build stock-vs-group and
group-vs-group divergence features, test whether they (and a model on them) are significant predictors of forward returns.
Separate research family from the mega-alpha v3/v4 trials. TRAIN role 2020-2022 only (warmup 2018-06..2019 used as
lookback input only). Validation 2023-2024 and 2025+ are NOT read. Script: `studies/statarb_cluster_study.py`.

## Universe, clock, targets
- Universe: role `member` (research-prior63-usd-adv-top3000, lag 1). Returns r[d] = close[d]/close[d-1]-1 (repaired
  adjusted close), both sessions present; features use r clipped to +-50%.
- Features at decision d use closes <= d. Targets (raw returns): y1 = r[d+1] (reference only: not tradable at the 23h
  decision clock), **y2 = r[d+2] (primary; NAV convention)**, y5 = sum r[d+2..d+6] (>= 4 finite).

## Groupings (re-formed every 21 sessions at t_p, serve d in [t_p, t_p+21), inputs closes <= t_p)
- Embedding: trailing 252 sessions, stocks with >= 200 finite returns; daily cross-sectional demean (market out),
  per-stock standardise, clip +-6, SVD, top 15 PCs scaled by singular values, rows L2-normalised (cosine geometry).
- `c30` (primary): balanced k-means (k-means++ init, <= 20 Lloyd rounds, greedy max-regret capacity assignment,
  capacity ceil(n/N), N = round(n/30)). Sensitivity `c10`, `c100`. Placebo `rnd30`: random balanced groups of 30.
  SIC control `ff49`: Fama-French 49 industry of the stock at t_p.
- Neighbour group h(g) = argmax corr of group-mean (demeaned) returns over the same 252-session window.

## Features (sign NOT fixed in advance; IC sign reported)
c30 full set: dev_1, dev_5, dev_21 (sum of r_i - leave-one-out group mean), dev_5z (dev_5 / (60d sd of daily dev x sqrt5)),
sscore (Avellaneda-Lee 2010 OU s-score of 60d residual of r_i on LOO group mean, centred m), kappa (OU speed), beta_g,
cohesion (60d corr with LOO group mean), grp_dev_5, grp_dev_21 (group minus neighbour group), grp_sscore (OU s-score of
group-neighbour spread), grp_mkt_5, grp_mkt_21 (group minus market), grp_mom_12_1 (group minus market, d-251..d-21),
nbr_mkt_1, nbr_mkt_5 (neighbour group minus market: lead-lag), grp_disp_5 (within-group sd of 5d returns), grp_tight
(mean within-group correlation over the clustering window). Core set for c10 / c100 / rnd30 / ff49: dev_5, dev_21,
sscore, grp_dev_5, grp_mkt_5. Controls: rev_1, rev_5, rev_21, mom_12_1, vol63, beta252, ladv63, size (log me_company).

## Tests (TRAIN 2020-2022)
- Univariate: daily normal-score rank IC vs y1/y2/y5, Newey-West t (lags 5; 10 for y5), by-year, paper book (gauss-rank
  weights, dollar neutral, L1 = 1) gross SR at y2, one-way turnover tau, breakeven bps = mean ret / tau.
  **Significance = |NW t| > Bonferroni over all univariate feature x target tests (two-sided 5%) AND same IC sign in all
  three years.**
- Incremental: daily Fama-MacBeth OLS of gauss(y) on gauss(features), nested models: M0 controls; M1 +ff49 core;
  M2 +ff49 core +c30 full; M3 +c30 full; M4 +rnd30 core; M5 +c30 core; M6 +c10 core; M7 +c100 core. NW t per coefficient.
  H1: c30 dev/sscore significant beyond controls. H2: c30 group-vs-neighbour features significant beyond controls.
  H3: clustering beats SIC and random (c30 core t with ff49 present in M2; M5 vs M4).
- Model: FIT 2020-01..2021-12 (last 7 FIT sessions embargoed), HOLD 2022 (pseudo-out-of-sample inside TRAIN). Target
  gauss(y5). Feature sets CTRL, CTRL+FF49, FULL (= CTRL + ff49 core + c30 full + c10 core + c100 core). Models: pooled
  OLS; HistGradientBoosting (lr .05, 300 iters, 31 leaves, min leaf 5000, l2 1, 64 bins, 1M-row FIT subsample, seed 0).
  HOLD metrics: IC vs y2/y5 (NW t), price-risk-neutral book (residual on [1, beta252, vol63, ladv63]) gross SR, tau,
  breakeven bps, EMA-smoothed books (half-life 2/5/10). H4: FULL HGB HOLD IC NW t > 2 and > CTRL+FF49 (paired NW t).
- Declared honest caveat: short-horizon residual reversal is well known (Lehmann 1990, Lo-MacKinlay 1990, Avellaneda-Lee
  2010); the economic question is the breakeven cost vs the $1bn S2 cost model, not only the t-stat.
