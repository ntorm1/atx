# v4 pre-registration (declared 2026-09-27, before ANY v4 TRAIN read) — validation trial #2 candidate

Why (v3 post-mortem, ledger "POST-MORTEM v3"): data-estimated signs + MV weights on 3 TRAIN years had no
persistence (FIT->HOLD Spearman .04; null protocol yields in-sample SR ~2.2 from noise; walk-forward OOS ~0);
book concentrated in 2020/2022 crisis regimes; cost hurdle ~1.3 gross SR at tau 3.7%/day. No leak (T17).
Design rule for v4: the literature carries the selection, TRAIN only vetoes and measures.

## R1 Library v4 (T24) — prior-signed, one variant per hypothesis, frozen before any TRAIN IC read
- Sign embedded in the DSL (prior direction => higher value = long); every candidate carries `prior_sign`
  (+1), `theme`, `tier`, `citation` in the recipe. Canonical literature parameterisation, s21 smoothing
  default; NO choice of variant by v3 TRAIN performance (v3 per-candidate TRAIN stats were seen for PV
  families — disclosed; variants are chosen by the canonical definition, not by v3 results).
- Themes (composition unit) and members (ids from T18 report §6 unless noted):
  1 value: bm, ep, cfp, fcfp, ebit_ev, net_payout, sp, rd_me
  2 profitability_quality: gpa, opbe, cfoa, roe_q, roa, accruals, fscore, qmj_lite
  3 investment_issuance: asset_growth, noa, issuance_xbrl, issuance_vendor, mgmt_sy
  4 earnings_momentum: sue, droe, chtax, ear (announcement return, canonical 3-day CAR, Chan-Jegadeesh-Lakonishok 1996)
  5 price_momentum: mom_12_1 (Jegadeesh-Titman 1993), ind_mom_12_1, within_ind_mom, high_52w (George-Hwang 2004)
  6 low_risk: low_beta (Frazzini-Pedersen 2014), low_ivol (Ang-Hodrick-Xing-Zhang 2006), low_max (Bali-Cakici-
    Whitelaw 2011), lowvol_ind
  7 short_interest: si_ratio (-SI/shares; Asquith-Pathak-Ritter 2005), dtc (-days to cover; Hong-Li-Ni-Scheinkman-
    Yan 2015), si_change (-change in SI; Rapach-Ringgenberg-Zhou 2016 direction)
  8 reversal_seasonality: ind_adj_rev_5, seasonality_same_month (Heston-Sadka 2008)
  9 options_implied: iv_rv_spread (Bali-Hovakimian 2009 direction), iv_change (An-Ang-Bali-Cakici 2014 direction)
- Firm-level accounting ratios in themes 1-3 are ranked WITHIN industry (group rank, `grp_ff12`) except ids that
  are explicitly industry-level; sole exception list must be written in the recipe. Reason: v3 failure was regime
  concentration; industry-adjusted value/profitability are the stronger published forms (Novy-Marx 2013;
  Asness-Porter-Stevens 2000). Cost if wrong: loses the between-industry value premium.

## R2 Data (T19-T21, T25)
- Identity: pinned copy of r4 dated links (rehearsal, PIT), labelled `rehearsal_identity=true`.
- Fundamentals clock: FSDS `accepted_utc` (fallback filed+46h, labelled FC1); usable from the FIRST session
  whose close is after accepted_utc, PLUS 1 declared lag session (`--fund-lag-sessions 1`). Latest-clock-wins
  restatements (value as known at the time). Staleness 200 d (quarterly/instant), 400 d (annual-only).
- Values labelled modeled/unaccepted (not F.1-accepted). Seal: nothing >= 2025-01-01 read.
- Same producer blob for TRAIN role v2 and VAL role v1 (fields-v5).

## R3 Admission `v4-prior-v1` (T23) — TRAIN 2020-2022, no sign estimation
- s_k = +1 for all (prior embedded). Reject: < 250 finite TRAIN days; tau_k > 0.70; VETO if full-TRAIN oriented
  mean f_k has HAC t < -2.0 (clear contradiction of the prior). Redundancy greedy |rho| <= 0.90 ordered by
  (tier, roster order) — never by TRAIN Sharpe. (0.90 not 0.70: within-theme overlap is handled by theme weights.)
## R4 Composition `ew-theme-v1` (T23)
- Weight = 1/9 per theme with >= 1 admitted member, split equally among admitted members; renormalise over
  themes present. No mean/covariance estimation.
## R5 Construction (fixed, no grid): nav baseline-v1, cadence 1, fraction 1, neutralize price-risk-v1, band 1/N,
  limits mean .20 / p95 .30, S2 modeled-1bn-stale5-v1 x swap-fin-v1 primary; stresses as v3.
## R6 Gate before spending validation trial #2
- TRAIN S2 x swap-fin-v1 net SR >= 1.0 AND daily turnover limits met AND per-year TRAIN net SR reported.
  If the gate fails: do NOT run validation; report; any revision is a new disclosed trial family.
## R7 Trial accounting: admission trials = v4 roster size; composition 1; construction 1; validation #2.

## v4.2 revision (declared 2026-09-27 after the v4 gate failed; disclosed; last revision before reporting to owner)
Evidence so far (TRAIN): v4 prereg net .431; v4.1 construction grid best .687 (b2 f.25); T26 paper books: industry-neutral
(FF12/FF49) and liquidity scaling do NOT raise gross SR (.80/.75/.70 vs .85). Lever left: breadth + cost consistency.
- R1' additions (prior-signed, one canonical variant each; no v3/v4 per-candidate TRAIN performance consulted):
  res_mom_12_1 -> price_momentum (Blitz-Huij-Martens 2011: 12-1 cumulative market-residual return / residual vol);
  eap -> earnings_momentum (Frazzini-Lamont 2007; Barber-De George-Lehavy-Trueman 2013: long names whose expected next
    announcement, last announcement + ~63 sessions, falls within the next 21 sessions);
  low_share_turnover -> low_risk (Datar-Naik-Radcliffe 1998: low volume/shares_out, 126-session mean, long).
- R3' cost-consistency screen (structural, not performance): reject tau_k > 0.08 at $1bn (standalone TRAIN turnover),
  in addition to v4-prior-v1; themes left empty drop out of the 1/themes weights.
- R5' construction: band 2, fraction .25 (v4.1 selection) — plus band 1 fraction .25 reported (2 trials).
- R6 gate unchanged (TRAIN S2 x swap-fin net >= 1.0). If it fails: stop iterating, report to the owner.

## v5 revision (declared 2026-09-27 after the construction audit T28; disclosed; before ANY v5 TRAIN read)

Evidence: T28 — v4.1 (band 2/N, fraction .25) held mean gross 0.26 TRAIN / 0.24 VAL; band ≥ position scale blocked entry.
Design rule: the literature carries the selection; TRAIN measures second moments (autocorrelation, turnover, liquidity) only.

R1' Library: unchanged, fund_industry_ic_v4 daa9663e (37). No new candidates in v5. (v5.1, if built, adds opex_at to profitability_quality: separate disclosed family.)
R2' Data: unchanged (bridge r4-v1 ddf97164, events-v2 74ed9a50, fields-v6 32565c32 TRAIN). Nothing >= 2023-01-01 is read.
R3' Admission: unchanged, v4-prior-v1 880a0a6a (31 admitted).
R4' Composition ew-theme-aim-v1: g_k = theta * sum_{j=0..126} (1-theta)^j * rho_k(j), theta = 0.05, rho_k(j) = mean over TRAIN
    decisions d of the cross-sectional correlation of per-day standardized ranks at d and d-j over names live on both days
    (lags 0..21 exact; 28,35,...,126 exact; others linear-interpolated), clipped g in [0.05, 1].
    w_k = (g_k / (T * n_theme(k))) / sum_m (g_m / (T * n_theme(m))) over admitted non-degenerate members. No means, no covariances.
    Reference composition for pairing: ew-theme-v1 9a9c949a (unchanged bytes).
R5' Construction aim-partial-v5: every decision (cadence 1): next_i = cur_i + theta_i (L * desired_i - cur_i) unless
    |L*desired_i - cur_i| <= dust / N_d; desired = tied-rank, price-risk-v1 neutralized, gross 1; non-members forced to 0.
    Grid (10 cells, 2 combined signals x 5): for C in {C_ew, C_aim}: (theta .03, dust .1), (theta .05, dust .1) [REFERENCE when C=C_ew],
    (theta .08, dust .1), (theta .05, dust 0), (rate per-name-v1 RRA 10 clip [.01,.15], dust .1). L = 1. If the reference cell's mean gross
    < 0.90, ONE extra cell per C with L = 1 / mean_gross(reference) (deterministic, disclosed).
    Limits: daily tau mean <= .20, p95 <= .30. Primary S2 modeled-1bn-stale5-v1 x swap-fin-v1; stresses S1, S3, flat-300, engine-tiers.
R6' Gate (TRAIN): mechanics — mean gross in [0.90, 1.05], |mean net| <= 0.02, tau limits met. Statistics — paired dSR(net) of each cell vs
    the reference cell with Memmel SE and a Ledoit-Wolf studentized bootstrap (2000 draws, block 21), DSR at N = 10 (skew/kurtosis from daily net).
    A freeze for validation is proposed ONLY if a cell's TRAIN net SR >= 1.0 (owner rule) or the owner says so; otherwise report.
    On failure: do NOT run validation; report; any revision is a new disclosed section here.
R7' Trial accounting: admission 37 (unchanged); composition +1 (ew-theme-aim-v1); construction +10 (+2 if L re-run; +1 delisting re-run);
    validation: none (trials #1, #2 disclosed; #3 needs U1).

## v5.1 family (declared 2026-09-27 after T34a breadth check; disclosed; before any v5.1 TRAIN read; optional lane)
R1'' Library v5.1 = frozen v4 (37, daa9663e) + ONE candidate `opex_at`, theme profitability_quality, tier B, prior_sign +1
    (Novy-Marx 2011 operating leverage = operating costs / assets), DSL
    decay_linear(group_rank((((sale_ttm - oi_ttm) / at) + (0 * log(at))), grp_ff12), 21).
    Deviation disclosed: fields-v6 has no opex_ttm; sale_ttm - oi_ttm = COGS + SG&A + D&A + other operating items (paper: COGS + SG&A).
    No producer change; data pins unchanged. Admission v4-prior-v1 unchanged (38 candidates). Composition: ew-theme-v1 and
    ew-theme-aim-v1 re-fit on the 38 (+2 composition trials). Construction: reference cell only (+1). Gate as R6'.

## v6 revision (declared 2026-09-27 ~18:50 after Phase A reviews; disclosed; BEFORE any v6 TRAIN read)
Sources: v6-code-review-signal.md, v6-code-review-exec.md, v6-literature.md (all read-only; no TRAIN run). Parent: v5 cell
`v5-ew-t.05-d.1-fixed` (REF, S2 net .742) and its deployed twin L1.279 (net .712). TRAIN 2020-2022 only; no 2023+ read.
Every step below is accepted only on a paired dSR vs its parent whose SIGN matches the prior stated here; magnitude is not a
selection criterion. Every TRAIN result carries the Appendix A block. DSR N accumulates on the 13 v5 cells.

V6-C Construction (C++; existing library v5.1 and weights W_ew51; zero new data). Prior: each lowers executed turnover
and cost/$ with gross SR ~unchanged, so net rises.
  C1 `--order-basis delta` (orders = decision-NAV delta, drift rides; default `target` keeps v5 bytes). Prior +.
  C2 `--exit-rate r` for nonmembers (next = cur*(1-r), snap to 0 inside dust band); r in {theta, 2*theta}. Prior +.
  C3 locate-in-aim: zero special-tier short aims BEFORE neutralisation. Prior: |mean net| falls; SR ~0.
  C4 L calibrated on post-ramp rows (rows after the first 63 sessions), reported by nav_summ; gate stays all-rows mean
     gross in [.90,1.05]. Prior: fewer L re-derivations.
  C5 industry neutralisation `price-risk-ind-v1` = price-risk-v1 regressors + within-FF12 demeaning (Frisch-Waugh),
     `price-risk-ind-v2` = ind-v1 with vol126 / ladv252 windows. Prior: vol down, gross SR up or flat; ind_mom_12_1 bet lost.
  Grid budget: <= 12 cells: {C1 on/off} x {C2 off, theta, 2theta} at theta .05 dust .1 (6), best of those at theta .03 (1),
  dust re-tune {0, .2} on the best (2), C5 v1 and v2 on the best (2), one spare. L re-derivation on the final cell (1).
V6-U Universe (u pass): members restricted to PIT-linked operating common stock (drop ETF/ETN/SPAC/ADR without issuer
  link). Prior: gross SR up (39.4% of member cells now ranked on price signals only); cost/$ ambiguous. Budget: 1 u pass,
  1 composition, 1 construction cell.
V6-W Composition `ew-theme-v6` (fit_composition_weights.py), applied to library v5.1 first: (a) drop theme low_risk
  (all four members negative TRAIN HAC t AND projected out by price-risk-v1; selection-bias caveat disclosed), (b) merge
  options_implied into short_interest as one theme, (c) fast-sleeve shrink: members with standalone tau >= .08 get weight
  x 1/3 within theme, mass reallocated within theme, (d) theme weights equal across the resulting 7 themes with coverage
  redistribution (member missing -> theme mass stays in theme). Prior +. Budget: 2 compositions (v5.1 lib, v6 lib) + 2 cells.
V6-L Library v6 (generate_fund_ic_v6.py; one variant per hypothesis, prior-signed, admission v4-prior-v1 on TRAIN):
  replace/add: residual momentum, cash-based operating profitability, composite value (incl. intangible-adjusted), 5-year
  composite issuance / net external financing, multi-lag (Heston-Sadka) seasonality, betting-against-correlation and scaled
  MAX (replace low_beta/low_ivol/low_max), long-window FINRA shorting flow, EAR-centred earnings momentum; smoothing moved
  after the rank (R(decay) -> decay(R) fixed: rank first only where the 21-session blackout binds) or removed for fast
  sleeves. Budget: admission trials = roster size (<= 48); 1 composition; 1 cell.
V6-F Final: best construction x V6-U x V6-W x V6-L in one cell, L re-derived (C4). Freeze proposed only if S2 net >= 1.0
  with R6' mechanics (all-rows gross in [.90,1.05], |mean net| <= .02, tau mean <= .20 / p95 <= .30) AND the cell's
  cross-cell DSR >= .95 with N = 13 + all v6 cells. Validation trial #3 still requires U1.
Not done (pre-declared exclusions): IC/HAC/MV-fitted weights on TRAIN; ML stacking; valuation timing; book-level vol
  targeting; cash-PB financing scenario as a route to 1.0; any cost-model relaxation. Borrow realism: swap-fin-v1 already
  tiers GC/warm/special (30/100/500 bps); kept as S2.

## v6.1 sub-alpha: long-horizon FINRA shorting flow (owner-directed 2026-09-28; declared BEFORE any read of the field or its returns)
Source: FINRA consolidated NMS daily short sale volume files (CNMSshvolYYYYMMDD.txt.gz; columns Date|Symbol|ShortVolume|
ShortExemptVolume|TotalVolume|Market), read-only from C:/atx/atx-db/data/raw/finra_short_volume (2018-08-01 onward).
Literature: Wang, Yan & Zheng (2020, JFE) -- long-term shorting flows predict negative returns for about a year; short-term
abnormal flows do not. FINRA short volume is contaminated by market-maker shorting, so only long-window aggregates carry
signal (v6-literature.md §3.8, §4).
Field `sv_ratio126` (fields builder): at session d, sum(ShortVolume) / sum(TotalVolume) over the sessions d-126..d-1 on
which the instrument's PIT ticker appears in the file (lag 1 session: day d's file is never used at d); NaN when fewer than
63 such sessions or the sum of TotalVolume is 0. Ticker -> instrument mapping = the builder's existing PIT FINRA symbol map.
Candidate `sv_flow` (library v6.1 = library v6 + this one member, theme short_interest, tier B-): the field demeaned within
FF12 industry (the DSL's existing group-demean op), prior sign NEGATIVE (higher shorting flow -> lower future return). One
variant; no window or sign search.
Promotion tests, all required, in order (stop at the first failure):
  P1 admission v4-prior-v1 on the restricted TRAIN role lo1: runner sign agrees with the prior, not redundant (|rho| <= .90
     with every admitted member, incl. si_ratio / dtc / si_change), tau within the screen limit.
  P2 book: library v6.1 x ew-theme-v1 refit on lo1 x the final construction (aim-partial-v5 theta .05 dust .1 fixed, delta
     orders, exit .05, locate-in-aim, liquidity cache, price-risk-v1) with L FIXED at 1.247; paired dSR(net) vs the final
     cell mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 must be > 0.
  P3 R6' mechanics on that cell (all-rows gross in [.90, 1.05], |mean net| <= .02, tau mean <= .20 / p95 <= .30) and S2 net
     >= 1.0.
Promotion = the v6.1 cell replaces the v6 final cell as the frozen candidate in the owner packet (still subject to U1; the
freeze DSR gate stays .95 and is reported with N = 29). Trial accounting: admission +1 (only sv_flow is new; the 38 v6
members are identical definitions on identical data), composition +1, construction +1.
