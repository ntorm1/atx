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
