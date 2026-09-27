# Price wave: provisional selection evidence

This wave evaluates 36 preregistered price characteristics at horizons of 1, 3, 6 and 12 calendar months over January 2013–December 2023. It uses provisional labels and a reconstructed security universe. The holdout from January 2024 remains sealed; no result here is a qualified signal or a release acceptance.

**The required 4,500-line coverage criterion is NOT MET.** The accepted input universe peaks at 4,147 lines; no required month reaches 4,500. The wave measures available evidence without waiving that requirement.

## Frozen experiment

The production trial registry contains 432 cells: 36 features × `rank_normal`, `signed_raw`, `zscore` × four horizons. The registry recorded sequence 0 at `2026-09-27T12:18:48`, ID `3afdfc36b2d257244375fa784d7dfe062ab6a00d31280f85cb26dfa546b3920c`. Commit `251995c7` anchored that registration and the runner **before the first real label-value read**. No second registration was made on resume.

The three equal-weight market proxies `beta_ew_252d`, `ivol_ew_21d` and `ivol_ew_252d` count toward the 432 trials but are reported-only under policy C-24. The 33 remaining hypotheses form the primary gating family. `zero_trade_21d` and `zero_trade_252d` are unsupported on this source and remain unregistered under C-82. No beta-neutral return configuration was enabled.

The policy primary cell is `rank_normal`, horizon 3 months. Replications require a preregistered-direction one-sided p ≤ 0.05, gating BH q ≤ 0.10, investable one-sided p ≤ 0.05, and size-bucket sign agreement. The investable co-primary slice uses price ≥ $5 and market capitalization ≥ the French NYSE 20th percentile. Population coverage requires at least 60% of the declared population in at least 90% of history-eligible formations. First formations are fixed from each catalog row's minimum XNYS-session history since 2012-03-26. The separate project target of 4,500 lines remains unmet.

One EvaluationSpec, created `2026-09-27T12:35:49`, is shared by all workers and resumes. The reported all-cell family, the primary gating family and cumulative trial count have distinct roles; none is reduced after observing results. Selection status is capped at `selection_pass`. Final labels, final identity/survivorship work and the wave's single authorized final-label holdout opening remain prerequisites to qualification.

| Input | Pin |
|---|---|
| Catalog | `36bb1bbe472b1bed0371a1f4bb1cb5ebcdc17132669d0bf341e5f5ffaad82654` |
| Policy semantic SHA256 | `776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a` |
| Feature manifest ID | `58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de` |
| Serialized manifest SHA256 | `8c234060422e1b209bd1d3e6a7e32b1b7b5692d211ec578d2dce87ec8f62cb05` |
| Price lake | `price-wave-0ed96b2696f1-bcdf5d0f3437` |
| Price lake digest | `9eeb25fe81a6543cd46be11267a823a087028a2d7e3c726f7e7eb231a818731e` |
| Provisional label set | `eaf6c8b9450a7fa3f1f282717a44f9ae6f502a2c424d6185199cab317f08c757` |
| Benchmark lake | `bench-20260926` |
| Benchmark digest | `f2ff84035aa968dcbf7da09e12ceb907caa4a4dd3cc74c694a95cb4ebe429648` |
| Evaluation cache key | `6e2a40fc79731e25b0d31640383f881296fd4ea08092cb4734e1f82e0b7f5c73` |
| Runner source digest | `a95d8039fae846a993d5d659f32d6dd6acde3264ccc4215093e1a2c9792bb0f1` |
| Frozen EVAL source digest | `914a04f37ca90f2b7d934b8c67399eb6025c72766517a795ceeab2d36cb7161a` |
| Store adapter source digest | `385523273458890a593a5358f5efc8703d3caa99c5b2a8cfb4b14d430ede4ab7` |

## Sample and return windows

The selection context has 481,991 line-month rows and 6,566 distinct vendor lines over 132 formations. It withholds 140 context rows flagged as vendor artifacts; native windows carry their own exclusions. There are 476,833 positive finite market-cap observations and 479,116 finite prices before the context exclusions. These are a reconstructed population, not verified historical common-share identity: membership requires earnings evidence available by formation, with current-directory name-pattern exclusions. Delisted ADR/REIT/LP ambiguity and retrospective directory classification remain limitations.

Formation occurs at the last XNYS session of the calendar month with decision cutoff 22:00 UTC. The label enters at the next XNYS session after that calendar month-end and exits at the next XNYS session after the month-end h calendar months later. **These are calendar-month entry-to-entry windows, not fixed 21/63/126/252-session windows.** Legacy `horizon_sessions` fields are nominal engine metadata; actual dates in the pinned label set control this run. Every mature endpoint is strictly before 2024-01-01. December 2023 formations can exist as feature/context observations but cannot contribute a return crossing the seal.

The read contains 1,758,721 usable or explicitly invalid label rows: 468,345 at h1, 455,350 at h3, 436,139 at h6 and 398,887 at h12. Missing-entry, missing-exit, terminal-pending and not-matured rows remain unavailable; they are not imputed. The exact-entry accounting adapter independently matches both actual endpoints against the formation calendar before reusing an already authorized return. All 1,758,721 matched with zero endpoint mismatches. Its source/content pin is `3ddadd4f93f11b3ed522cee4890ffbe48da867b2ee6f9cd9bae65c8ca24d94d3`.

Vendor market capitalization uses shares lagged at least 90 days and is unverified. Only April and May 2012 are wholly size-missing in the accepted input history; June is partially available. No point-in-time venue exists, so venue-dependent NYSE deciles remain unavailable. Supplied French p20/p50/p80 breakpoints support the investable/size calculations and capped weights; they do not establish venue identity.

The accepted source audit counted 136 suspect bars across 120 lines, 148 return-break bars and 909 invalid artifact-spanning label windows. These are suspect-data exclusions, not verified corporate-action corrections. Another 21,798 high-close bars on 25 lines lack a prior low-price anchor and remain explicit ambiguities. June 2020 has 3,187 formation bars for 3,454 eligible spine rows. Provisional terminal treatment can change after final labels arrive.

## Reading the evidence

IC, ICIR and EWC z refer to the 3-month primary cell unless the horizon table says otherwise. Returns are decimal cumulative returns over their actual horizon, not annualized. The signs are fixed catalog expectations, not fitted orientations. Robust t statistics use the existing engine's EWC inference; NW diagnostics are separately labeled in cache columns. JT-6 and JT-12 average overlapping cohorts using one-month labels and are reported evidence, not additional qualification votes.

The legacy decile statistics are retained for policy parity. The additional complete-book `trade_*` series fix formation weights and require every held name to have a finite valid current return for finite gross/net returns. Missing names do not cause survivor renormalization. Held/priced and drift-held/drift-priced counts remain available. Turnover, spread cost and net returns can be NULL even when conventional decile statistics exist. AR/CS inputs estimate spreads at formation; they are not observed execution spreads. Buy/hold state is separate for each offset modulo horizon.

Controls available to the legacy FM regression are size and `ret_12_1`; the tested momentum row therefore has a self-control rank issue rather than an invented independent control. Complete fundamental JKP controls, P3 announcement-return decomposition and a pinned internal atx factor run are absent. External French/q5 calendar-month factors remain window proxies because their endpoints differ from the next-session entry-to-entry labels. Their spanning results are not claimable exact-window alphas. Beta-return diagnostics stay off. Publication/OSAP comparisons use different samples and constructions; approximate MDE and empirical-Bayes estimates are descriptive, never replacement gate inputs.

## Results

**No characteristic passed the frozen v4 selection gates.** All 432 registered cells were evaluated and the complete 36-feature wave was graded: 12 `not_investable`, 8 `not_significant`, 13 `sign_reversed`, and 3 `reported_only`. There are zero `selection_pass`, `qualified_reconstructed` or `qualified_strict` results. Signs, thresholds, families and universe were not changed after observing these results.

The immutable provisional ledger is [`3ab92a7886a1cff1760db3e3de7484004b99df1dfe3849fdf257826a139c60c2`](../../data/research/eval/ledgers/w1_price/3ab92a7886a1cff1760db3e3de7484004b99df1dfe3849fdf257826a139c60c2.json); its [CSV](../../data/research/eval/ledgers/w1_price/3ab92a7886a1cff1760db3e3de7484004b99df1dfe3849fdf257826a139c60c2.csv) and [complete report JSON](../../data/research/work/wave_eval/w1_price/selection-s7/report.json) retain exact values and reasons. The original run remains `selection-s7`, using the frozen source and pins above. All 432 cells are testable; each transform contributes 144 cells and each horizon 108. The reported all-cell family has 222 BH discoveries at alpha 0.05, zero Holm discoveries and 54 HLZ passes. Those descriptive counts do not substitute for the 33-hypothesis primary directional, investability and stability gates. Sixteen features carry the prespecified EWC persistence warning.

In the following tables, `cov` is the fraction of history-eligible formations meeting the declared population coverage threshold, rather than a claim about the separate 4,500-line target. A blank is unavailable. Turnover in the primary table is the preserved conventional top/bottom one-month turnover diagnostic; complete-book accounting is reported separately below. The source-label metadata table includes 2012 for context; selection returns begin in 2013. A blank 2023 h12 share means there are no mature unsealed windows at that horizon.

| theme | feature | sign | status | cov | IC | ICIR | EWC z | inv IC | EW L/S (t) | VW L/S (t) | JT-12 (t) | turnover | EWC flag |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| investment | `ret_36_13` | -1 | sign_reversed | 1.00 | -0.0527 | -0.44 | -1.89 | -0.0021 | 0.0111 (0.35) | 0.0009 (0.03) | 0.0028 (0.29) | 0.17 | yes |
| investment | `ret_60_13` | -1 | sign_reversed | 1.00 | -0.0803 | -0.66 | -2.36 | -0.0309 | 0.0044 (0.11) | -0.0156 (-0.40) | -0.0035 (-0.24) | 0.12 | yes |
| low_risk | `beta_bab_1260d` | -1 | not_significant | 1.00 | 0.0532 | 0.28 | 1.17 | 0.0154 | -0.0060 (-0.15) | -0.0010 (-0.03) | -0.0089 (-0.68) | 0.08 | yes |
| low_risk | `beta_dimson_252d` | -1 | not_significant | 1.00 | 0.0620 | 0.33 | 1.58 | 0.0241 | 0.0097 (0.29) | 0.0078 (0.29) | 0.0019 (0.19) | 0.15 | yes |
| low_risk | `beta_down_252d` | -1 | not_significant | 0.99 | 0.0431 | 0.27 | 1.27 | 0.0162 | 0.0102 (0.41) | -0.0024 (-0.10) | 0.0014 (0.16) | 0.18 | yes |
| low_risk | `beta_ew_252d` | -1 | reported_only | 1.00 | 0.0468 | 0.25 | 1.34 | 0.0183 | 0.0075 (0.30) | -0.0108 (-0.42) | -0.0004 (-0.05) | 0.12 | yes |
| low_risk | `coskew_252d` | -1 | not_significant | 1.00 | 0.0030 | 0.03 | 0.20 | 0.0045 | 0.0061 (0.90) | 0.0038 (0.62) | -0.0037 (-1.25) | 0.26 |  |
| low_risk | `ivol_ew_21d` | -1 | reported_only | 1.00 | 0.1177 | 0.76 | 2.87 | 0.0563 | 0.0157 (0.46) | 0.0179 (0.76) | 0.0071 (0.62) | 0.59 |  |
| low_risk | `ivol_ew_252d` | -1 | reported_only | 1.00 | 0.1249 | 0.72 | 2.73 | 0.0532 | 0.0103 (0.25) | 0.0256 (0.94) | 0.0084 (0.65) | 0.09 | yes |
| low_risk | `price_delay_52w` | +1 | sign_reversed | 1.00 | -0.0401 | -0.45 | -2.71 | -0.0138 | -0.0016 (-0.15) | -0.0051 (-0.50) | -0.0029 (-0.82) | 0.30 |  |
| low_risk | `rmax1_21d` | -1 | not_investable | 1.00 | 0.1013 | 0.69 | 2.78 | 0.0456 | 0.0130 (0.43) | 0.0106 (0.61) | 0.0049 (0.47) | 0.67 |  |
| low_risk | `rmax5_21d` | -1 | not_investable | 1.00 | 0.1063 | 0.68 | 2.74 | 0.0491 | 0.0148 (0.46) | 0.0159 (0.67) | 0.0048 (0.45) | 0.62 |  |
| low_risk | `rskew_252d` | -1 | not_investable | 1.00 | 0.0426 | 0.68 | 2.85 | 0.0132 | 0.0099 (0.60) | 0.0040 (0.52) | 0.0066 (1.73) | 0.16 |  |
| low_risk | `rvol_21d` | -1 | not_investable | 1.00 | 0.1170 | 0.69 | 2.76 | 0.0536 | 0.0169 (0.48) | 0.0234 (0.93) | 0.0061 (0.52) | 0.55 |  |
| low_risk | `rvol_252d` | -1 | not_investable | 1.00 | 0.1185 | 0.63 | 2.51 | 0.0436 | 0.0080 (0.19) | 0.0149 (0.49) | 0.0064 (0.47) | 0.08 | yes |
| low_risk | `std_dvol_126d` | -1 | sign_reversed | 1.00 | -0.0471 | -0.56 | -2.40 | -0.0045 | 0.0031 (0.27) | -0.0053 (-0.55) | 0.0025 (0.46) | 0.09 | yes |
| low_risk | `std_turn_126d` | -1 | not_investable | 1.00 | 0.0797 | 0.62 | 2.39 | 0.0367 | 0.0096 (0.28) | -0.0016 (-0.05) | 0.0082 (0.79) | 0.15 |  |
| low_risk | `turnover_126d` | -1 | not_investable | 1.00 | 0.0517 | 0.49 | 1.99 | 0.0296 | 0.0161 (0.56) | -0.0287 (-1.09) | 0.0083 (0.90) | 0.10 | yes |
| low_risk | `turnover_252d` | -1 | not_investable | 1.00 | 0.0550 | 0.52 | 2.03 | 0.0278 | 0.0168 (0.58) | -0.0294 (-1.18) | 0.0089 (1.01) | 0.07 | yes |
| momentum | `chmom` | -1 | sign_reversed | 1.00 | -0.0168 | -0.19 | -1.73 | -0.0093 | -0.0258 (-2.34) | -0.0307 (-2.89) | -0.0018 (-0.89) | 0.40 |  |
| momentum | `frog_in_pan` | -1 | not_significant | 1.00 | -0.0157 | -0.19 | -0.98 | 0.0049 | -0.0029 (-0.23) | 0.0094 (2.39) | 0.0013 (0.41) | 0.38 |  |
| momentum | `prc_highprc_252d` | +1 | not_investable | 1.00 | 0.1106 | 0.67 | 2.68 | 0.0393 | 0.0227 (0.65) | 0.0250 (0.89) | 0.0054 (0.42) | 0.41 |  |
| momentum | `ret_12_1` | +1 | not_significant | 1.00 | 0.0571 | 0.42 | 1.75 | 0.0054 | 0.0176 (0.70) | 0.0228 (0.94) | 0.0013 (0.14) | 0.25 |  |
| momentum | `ret_12_7` | +1 | not_significant | 1.00 | 0.0355 | 0.28 | 1.27 | -0.0015 | -0.0047 (-0.18) | 0.0004 (0.02) | -0.0007 (-0.10) | 0.34 |  |
| momentum | `ret_6_1` | +1 | not_investable | 1.00 | 0.0612 | 0.55 | 2.56 | 0.0182 | 0.0378 (2.47) | 0.0396 (3.89) | 0.0036 (0.55) | 0.37 |  |
| momentum | `ret_9_1` | +1 | not_investable | 1.00 | 0.0697 | 0.54 | 2.28 | 0.0242 | 0.0337 (1.53) | 0.0402 (2.04) | 0.0025 (0.32) | 0.29 |  |
| seasonality | `seas_1_1an` | +1 | not_significant | 1.00 | 0.0082 | 0.11 | 0.94 | -0.0098 | -0.0089 (-0.86) | -0.0104 (-1.46) | 0.0000 (0.02) | 0.83 |  |
| seasonality | `seas_2_5an` | +1 | not_investable | 1.00 | 0.0179 | 0.26 | 2.12 | 0.0029 | -0.0005 (-0.05) | 0.0081 (0.72) | 0.0023 (0.70) | 0.82 |  |
| short_term_reversal | `ret_1_0` | -1 | sign_reversed | 1.00 | -0.0227 | -0.22 | -1.89 | 0.0005 | -0.0073 (-0.68) | -0.0100 (-1.17) | -0.0035 (-1.04) | 0.83 |  |
| size | `ami_126d` | +1 | sign_reversed | 1.00 | -0.0926 | -0.81 | -3.14 | -0.0284 | -0.0003 (-0.01) | 0.0024 (0.12) | -0.0018 (-0.19) | 0.04 | yes |
| size | `ami_252d` | +1 | sign_reversed | 1.00 | -0.0927 | -0.82 | -3.23 | -0.0277 | 0.0004 (0.01) | -0.0131 (-0.88) | -0.0035 (-0.41) | 0.03 | yes |
| size | `bidask_ar_21d` | +1 | sign_reversed | 1.00 | -0.0320 | -0.56 | -2.85 | -0.0020 | -0.0095 (-0.57) | -0.0211 (-1.55) | -0.0027 (-0.46) | 0.62 |  |
| size | `bidask_cs_21d` | +1 | sign_reversed | 1.00 | -0.1172 | -0.73 | -2.79 | -0.0508 | -0.0114 (-0.30) | -0.0352 (-1.48) | -0.0048 (-0.39) | 0.48 |  |
| size | `dolvol_126d` | -1 | sign_reversed | 1.00 | -0.0681 | -0.75 | -3.05 | -0.0134 | 0.0046 (0.26) | -0.0023 (-0.21) | 0.0008 (0.13) | 0.05 | yes |
| size | `me_line_log` | -1 | sign_reversed | 1.00 | -0.1035 | -0.83 | -3.15 | -0.0334 | -0.0090 (-0.26) | -0.0138 (-0.45) | -0.0051 (-0.45) | 0.06 | yes |
| size | `prc_log` | -1 | sign_reversed | 1.00 | -0.1088 | -0.82 | -3.06 | -0.0411 | -0.0066 (-0.17) | -0.0086 (-0.22) | -0.0045 (-0.35) | 0.07 | yes |

| feature | IC 1m (t) | IC 3m (t) | IC 6m (t) | IC 12m (t) |
|---|---:|---:|---:|---:|
| `ret_36_13` | -0.0385 (-2.62) | -0.0527 (-2.21) | -0.0564 (-2.18) | -0.0608 (-1.59) |
| `ret_60_13` | -0.0593 (-3.79) | -0.0803 (-3.06) | -0.0948 (-2.42) | -0.1063 (-3.63) |
| `beta_bab_1260d` | 0.0382 (1.69) | 0.0532 (1.26) | 0.0555 (0.97) | 0.0486 (0.78) |
| `beta_dimson_252d` | 0.0445 (2.01) | 0.0620 (1.73) | 0.0709 (1.48) | 0.0765 (1.57) |
| `beta_down_252d` | 0.0275 (1.44) | 0.0431 (1.36) | 0.0542 (1.19) | 0.0604 (1.48) |
| `beta_ew_252d` | 0.0335 (1.68) | 0.0468 (1.44) | 0.0531 (1.19) | 0.0547 (1.15) |
| `coskew_252d` | 0.0052 (0.51) | 0.0030 (0.21) | -0.0062 (-0.32) | -0.0216 (-0.58) |
| `ivol_ew_21d` | 0.0817 (4.31) | 0.1177 (3.71) | 0.1460 (3.43) | 0.1720 (4.20) |
| `ivol_ew_252d` | 0.0884 (4.01) | 0.1249 (3.45) | 0.1588 (3.34) | 0.1932 (4.08) |
| `price_delay_52w` | -0.0273 (-3.82) | -0.0401 (-3.40) | -0.0541 (-2.60) | -0.0659 (-5.15) |
| `rmax1_21d` | 0.0740 (4.37) | 0.1013 (3.53) | 0.1189 (2.98) | 0.1380 (3.61) |
| `rmax5_21d` | 0.0787 (4.41) | 0.1063 (3.46) | 0.1241 (2.91) | 0.1446 (3.59) |
| `rskew_252d` | 0.0309 (4.67) | 0.0426 (3.66) | 0.0487 (3.10) | 0.0597 (2.67) |
| `rvol_21d` | 0.0809 (4.08) | 0.1170 (3.49) | 0.1400 (3.02) | 0.1634 (3.66) |
| `rvol_252d` | 0.0846 (3.61) | 0.1185 (3.06) | 0.1487 (2.90) | 0.1785 (3.42) |
| `std_dvol_126d` | -0.0385 (-3.89) | -0.0471 (-2.88) | -0.0547 (-2.42) | -0.0612 (-2.29) |
| `std_turn_126d` | 0.0514 (2.97) | 0.0797 (2.87) | 0.1039 (2.86) | 0.1321 (3.34) |
| `turnover_126d` | 0.0304 (2.13) | 0.0517 (2.28) | 0.0698 (2.32) | 0.0924 (2.98) |
| `turnover_252d` | 0.0328 (2.18) | 0.0550 (2.33) | 0.0747 (2.41) | 0.0961 (2.75) |
| `chmom` | -0.0077 (-1.49) | -0.0168 (-1.92) | -0.0297 (-2.92) | -0.0090 (-1.20) |
| `frog_in_pan` | -0.0138 (-1.33) | -0.0157 (-1.03) | -0.0116 (-0.69) | 0.0026 (0.12) |
| `prc_highprc_252d` | 0.0759 (3.70) | 0.1106 (3.34) | 0.1328 (2.89) | 0.1470 (3.36) |
| `ret_12_1` | 0.0483 (2.65) | 0.0571 (1.94) | 0.0619 (1.59) | 0.0582 (1.50) |
| `ret_12_7` | 0.0316 (1.93) | 0.0355 (1.36) | 0.0282 (0.87) | 0.0397 (1.26) |
| `ret_6_1` | 0.0461 (3.82) | 0.0612 (3.14) | 0.0751 (2.51) | 0.0593 (2.15) |
| `ret_9_1` | 0.0517 (3.33) | 0.0697 (2.69) | 0.0704 (1.88) | 0.0599 (1.82) |
| `seas_1_1an` | 0.0035 (0.39) | 0.0082 (0.99) | 0.0195 (2.28) | 0.0218 (1.99) |
| `seas_2_5an` | 0.0107 (2.08) | 0.0179 (2.56) | 0.0211 (2.78) | 0.0293 (3.69) |
| `ret_1_0` | -0.0029 (-0.38) | -0.0227 (-2.14) | -0.0392 (-2.90) | -0.0416 (-2.60) |
| `ami_126d` | -0.0693 (-5.27) | -0.0926 (-4.25) | -0.1122 (-3.86) | -0.1325 (-4.74) |
| `ami_252d` | -0.0690 (-5.41) | -0.0927 (-4.44) | -0.1132 (-4.09) | -0.1358 (-5.23) |
| `bidask_ar_21d` | -0.0245 (-3.81) | -0.0320 (-3.66) | -0.0329 (-2.88) | -0.0395 (-3.72) |
| `bidask_cs_21d` | -0.0828 (-4.10) | -0.1172 (-3.55) | -0.1418 (-3.23) | -0.1669 (-3.87) |
| `dolvol_126d` | -0.0530 (-5.23) | -0.0681 (-4.06) | -0.0809 (-3.55) | -0.0942 (-3.95) |
| `me_line_log` | -0.0751 (-5.05) | -0.1035 (-4.27) | -0.1273 (-3.93) | -0.1497 (-4.61) |
| `prc_log` | -0.0778 (-4.73) | -0.1088 (-4.08) | -0.1337 (-3.75) | -0.1550 (-4.26) |

terminal_pending share of matured windows (count); invalid (withheld, C-79) count

| formation year | h1 | h3 | h6 | h12 | invalid h1/h3/h6/h12 |
|---|---:|---:|---:|---:|---|
| 2012 | 0.84% (240) | 1.64% (471) | 2.93% (842) | 5.61% (1613) | 1/3/6/7 |
| 2013 | 0.95% (382) | 1.87% (755) | 3.21% (1296) | 5.65% (2280) | 1/3/9/21 |
| 2014 | 0.81% (340) | 1.69% (709) | 2.97% (1249) | 5.96% (2504) | 2/6/11/26 |
| 2015 | 1.06% (456) | 2.12% (914) | 3.91% (1682) | 7.69% (3308) | 2/7/17/43 |
| 2016 | 1.29% (561) | 2.54% (1109) | 4.33% (1886) | 7.59% (3308) | 5/14/23/29 |
| 2017 | 1.16% (501) | 2.26% (973) | 3.99% (1718) | 7.41% (3190) | 0/0/2/8 |
| 2018 | 1.23% (525) | 2.50% (1068) | 4.30% (1835) | 7.71% (3289) | 1/3/4/4 |
| 2019 | 1.13% (471) | 2.19% (915) | 3.74% (1565) | 6.86% (2871) | 0/0/0/24 |
| 2020 | 1.03% (428) | 2.02% (841) | 3.52% (1467) | 6.18% (2577) | 33/97/183/314 |
| 2021 | 0.89% (408) | 1.80% (822) | 3.13% (1428) | 6.07% (2767) | 0/0/0/0 |
| 2022 | 1.00% (496) | 2.06% (1018) | 3.74% (1848) | 7.30% (3310) | 0/0/0/0 |
| 2023 | 1.34% (547) | 2.64% (863) | 4.56% (936) |  | 0/0/0/0 |

Unsupported rows (not registered, ruling C-82): months whose valid values are all zero

| feature | reason | all-zero months | max non-zero lines | all-zero by year |
|---|---|---:|---:|---|
| `zero_trade_21d` | `vendor_zero_volume_absent` | 101/132 | 10 | 2013 12/12, 2014 12/12, 2015 12/12, 2016 1/12, 2017 0/12, 2018 5/12, 2019 12/12, 2020 12/12, 2021 11/12, 2022 12/12, 2023 12/12 |
| `zero_trade_252d` | `vendor_zero_volume_absent` | 76/132 | 33 | 2013 12/12, 2014 12/12, 2015 12/12, 2016 0/12, 2017 0/12, 2018 0/12, 2019 3/12, 2020 12/12, 2021 5/12, 2022 8/12, 2023 12/12 |

## Population and gate details

| Feature | Coverage formations | Mean names (3m) | Mean population coverage | Primary one-sided p | Gating q | Investable one-sided p |
|---|---:|---:|---:|---:|---:|---:|
| `ret_36_13` | 102 | 3078.1 | 0.679 | 0.9709 | 0.9994 | 0.5277 |
| `ret_60_13` | 78 | 2743.5 | 0.460 | 0.9908 | 0.9994 | 0.8262 |
| `beta_bab_1260d` | 78 | 3072.4 | 0.678 | 0.1212 | 0.2353 | 0.3726 |
| `beta_dimson_252d` | 126 | 3461.8 | 0.969 | 0.0576 | 0.1358 | 0.2460 |
| `beta_down_252d` | 126 | 3453.8 | 0.958 | 0.1022 | 0.2115 | 0.3251 |
| `beta_ew_252d` | 126 | 3462.3 | 0.969 | 0.0909 | — | 0.2997 |
| `coskew_252d` | 126 | 3462.3 | 0.969 | 0.4189 | 0.7275 | 0.3866 |
| `ivol_ew_21d` | 128 | 3550.1 | 0.993 | 0.0020 | — | 0.0310 |
| `ivol_ew_252d` | 126 | 3462.3 | 0.969 | 0.0031 | — | 0.0611 |
| `price_delay_52w` | 124 | 3470.6 | 0.963 | 0.9966 | 0.9994 | 0.8106 |
| `rmax1_21d` | 128 | 3550.1 | 0.993 | 0.0027 | 0.0246 | 0.0516 |
| `rmax5_21d` | 128 | 3550.1 | 0.993 | 0.0031 | 0.0246 | 0.0577 |
| `rskew_252d` | 126 | 3462.4 | 0.969 | 0.0022 | 0.0246 | 0.0980 |
| `rvol_21d` | 128 | 3550.1 | 0.993 | 0.0029 | 0.0246 | 0.0515 |
| `rvol_252d` | 126 | 3462.4 | 0.969 | 0.0060 | 0.0285 | 0.1254 |
| `std_dvol_126d` | 128 | 3519.5 | 0.985 | 0.9917 | 0.9994 | 0.6201 |
| `std_turn_126d` | 128 | 3518.7 | 0.985 | 0.0083 | 0.0343 | 0.0840 |
| `turnover_126d` | 128 | 3518.7 | 0.985 | 0.0231 | 0.0634 | 0.1167 |
| `turnover_252d` | 126 | 3463.4 | 0.969 | 0.0210 | 0.0631 | 0.1306 |
| `chmom` | 126 | 3434.0 | 0.945 | 0.9584 | 0.9994 | 0.8508 |
| `frog_in_pan` | 126 | 3432.1 | 0.944 | 0.8365 | 0.9994 | 0.3074 |
| `prc_highprc_252d` | 126 | 3464.1 | 0.970 | 0.0037 | 0.0246 | 0.0772 |
| `ret_12_1` | 126 | 3434.0 | 0.945 | 0.0404 | 0.1026 | 0.4160 |
| `ret_12_7` | 126 | 3436.0 | 0.945 | 0.1026 | 0.2115 | 0.5265 |
| `ret_6_1` | 128 | 3501.3 | 0.980 | 0.0053 | 0.0285 | 0.0869 |
| `ret_9_1` | 128 | 3469.5 | 0.971 | 0.0113 | 0.0414 | 0.1406 |
| `seas_1_1an` | 126 | 3438.3 | 0.946 | 0.1735 | 0.3181 | 0.9637 |
| `seas_2_5an` | 78 | 2924.2 | 0.568 | 0.0168 | 0.0555 | 0.3558 |
| `ret_1_0` | 128 | 3549.3 | 0.993 | 0.9708 | 0.9994 | 0.4788 |
| `ami_126d` | 128 | 3518.1 | 0.984 | 0.9992 | 0.9994 | 0.9761 |
| `ami_252d` | 126 | 3462.3 | 0.969 | 0.9994 | 0.9994 | 0.9779 |
| `bidask_ar_21d` | 128 | 3548.0 | 0.993 | 0.9978 | 0.9994 | 0.6414 |
| `bidask_cs_21d` | 128 | 3548.0 | 0.993 | 0.9974 | 0.9994 | 0.9600 |
| `dolvol_126d` | 128 | 3519.5 | 0.985 | 0.9989 | 0.9994 | 0.8561 |
| `me_line_log` | 128 | 3536.7 | 0.990 | 0.9992 | 0.9994 | 0.9758 |
| `prc_log` | 128 | 3552.6 | 0.994 | 0.9989 | 0.9994 | 0.9785 |

The accepted source label inventory contains 2,042,924 rows: 1,869,328 valid, 909 invalid, 64,582 terminal-pending, 105,065 not matured, 1,211 missing entry bars and 1,829 missing exit bars. That broader 2012–2023 inventory is distinct from the 1,758,721 selection/accounting rows actually read. The 161,332 source-universe exclusions comprise 88,305 rows without earnings evidence and 73,027 whose evidence becomes available only later. Every exclusion remains explicit; none is presented as a verified repair.

## Economic availability across all 432 cells

All 397 schema fields are present in every cached cell. The [complete field availability audit](../../../.superpowers/sdd/tier1-v2/receipts/1.13-s8-whole-wave-audit.json) records finite/NULL counts, numeric ranges, basis labels and every claimability flag. The following counts concern finite cell means; they include the three transformations of each characteristic, which are not independent economic histories.

| Complete-book portfolio | Gross mean finite / NULL | Turnover mean finite / NULL | Spread-cost mean finite / NULL | Net mean finite / NULL |
|---|---:|---:|---:|---:|
| EW deciles | 39 / 393 | 39 / 393 | 39 / 393 | 3 / 429 |
| Capped-VW deciles | 12 / 420 | 12 / 420 | 6 / 426 | 0 / 432 |
| Buy/hold hysteresis | 15 / 417 | 15 / 417 | 15 / 417 | 0 / 432 |

The three finite EW net cells each have only **one** usable net observation, with mean −0.1243145 (−12.43145% over that observation's horizon), so all net-return t statistics remain NULL. EW gross and turnover means use at most six complete observations; capped-VW uses at most three; buy/hold uses at most two. These sparse summaries do not establish implementable net alpha. Missing held-name returns stay missing; portfolio weights are never redistributed to survivors.

| Portfolio | Held exposures | Priced exposures | Prior drift-held exposures | Prior drift-priced exposures |
|---|---:|---:|---:|---:|
| EW deciles | 36,755,241 | 35,042,133 | 34,885,554 | 33,322,263 |
| Capped-VW deciles | 57,568,648 | 55,145,894 | 54,165,144 | 51,982,042 |
| Buy/hold | 52,388,043 | 50,276,850 | 49,713,729 | 47,793,543 |

All four count fields are populated in every cell. Totals above sum exposure observations across feature/transform/horizon cells; they are deliberately repeated observations, not unique securities. Per-formation counts remain in each cached series.

Conventional JKP EW/VW/capped-VW spread means and approximate IC minimum detectable effects are finite in all 432 cells. Capacity estimates at 1% ADV are finite in 396 cells for decile 1, 420 for decile 2, 432 for deciles 3–9, and 408 for decile 10. These are formation-liquidity diagnostics, not executable-book capacity guarantees. Missing source inputs are not replaced with assumptions.

Every external French CAPM/FF6 and q5 claimability flag is false (432/432 per model/portfolio). Internal atx factor claimability fields are NULL (432/432); no internal factor run was pinned. Fundamental JKP/HXZ controls, announcement-return decomposition and beta-neutral return configurations remain unavailable. Descriptive estimates never overwrite original family/gate columns.

## Execution and provenance

The original first feature and next 11 features retain their session-7 evidence. The session-8 continuation evaluated only the remaining 24 features on attempt 1, with zero failures. All 36 native worker peaks are below 0.6 GiB and all cap-hit flags are false; the maximum is **0.4556 GiB**. The grade worker peaks at 0.2845 GiB. The [whole-wave audit](../../../.superpowers/sdd/tier1-v2/receipts/1.13-s8-whole-wave-audit.json) binds all 36 worker records and preserves hashes of the original caches, job receipts, prepared spec and ledger.

| Feature | Native job peak GiB | Evidence |
|---|---:|---|
| `ami_126d` | 0.4546 | session-7 job-0.receipt.json |
| `ami_252d` | 0.4527 | session-7 job-1.receipt.json |
| `beta_bab_1260d` | 0.4521 | session-7 job-2.receipt.json |
| `beta_dimson_252d` | 0.4539 | session-7 job-3.receipt.json |
| `beta_down_252d` | 0.4534 | session-7 job-4.receipt.json |
| `beta_ew_252d` | 0.4516 | session-7 job-5.receipt.json |
| `bidask_ar_21d` | 0.4512 | session-7 job-6.receipt.json |
| `bidask_cs_21d` | 0.4521 | session-7 job-7.receipt.json |
| `chmom` | 0.4542 | session-7 job-8.receipt.json |
| `coskew_252d` | 0.4553 | session-7 job-9.receipt.json |
| `dolvol_126d` | 0.4544 | session-7 job-10.receipt.json |
| `frog_in_pan` | 0.4538 | session-8 continuation summary |
| `ivol_ew_21d` | 0.4518 | session-8 continuation summary |
| `ivol_ew_252d` | 0.4515 | session-8 continuation summary |
| `me_line_log` | 0.4532 | session-8 continuation summary |
| `prc_highprc_252d` | 0.4537 | session-8 continuation summary |
| `prc_log` | 0.4535 | session-8 continuation summary |
| `price_delay_52w` | 0.4546 | session-8 continuation summary |
| `ret_1_0` | 0.4545 | session-8 continuation summary |
| `ret_12_1` | 0.4524 | session-7 first-worker summary |
| `ret_12_7` | 0.4547 | session-8 continuation summary |
| `ret_36_13` | 0.4537 | session-8 continuation summary |
| `ret_6_1` | 0.4522 | session-8 continuation summary |
| `ret_60_13` | 0.4508 | session-8 continuation summary |
| `ret_9_1` | 0.4520 | session-8 continuation summary |
| `rmax1_21d` | 0.4548 | session-8 continuation summary |
| `rmax5_21d` | 0.4515 | session-8 continuation summary |
| `rskew_252d` | 0.4556 | session-8 continuation summary |
| `rvol_21d` | 0.4540 | session-8 continuation summary |
| `rvol_252d` | 0.4520 | session-8 continuation summary |
| `seas_1_1an` | 0.4552 | session-8 continuation summary |
| `seas_2_5an` | 0.4523 | session-8 continuation summary |
| `std_dvol_126d` | 0.4519 | session-8 continuation summary |
| `std_turn_126d` | 0.4525 | session-8 continuation summary |
| `turnover_126d` | 0.4546 | session-8 continuation summary |
| `turnover_252d` | 0.4529 | session-8 continuation summary |

The accepted independent real-data engine comparison covers every original column and table of the original `ret_12_1` result against the prior engine, with exact equality and no tolerance relaxation. That existing measurement was not repeated; the whole-wave audit adds full schema/availability and native-worker coverage.

The runner's cache-hit receipt bug was corrected only after this original wave and grade were preserved. Exact current basis/spec identity and sidecar/table hashes are required before recovering the evaluated basis. Previous receipt and summary bytes are archived by content hash, with atomic publication; worker guard evidence is retained as each worker finishes. A separate bounded proof uses the same registered feature, data, policy and accounting configuration. No original source pin is rewritten, no new registration is added, and the original 432-cell result is not rerun.

The bounded final-source proof uses run `cache-resume-s8-r2`, runner digest `da13f1fe4b0b0727e0fb814ed68fd0850f25098dbc4c10382f606722d3e79176` and cache key `d547eeaac63fcf04a71706e19921cf9a8278751639e174573e87f2dfd0d09db0`. Its fresh feature peaks at 0.4521 GiB and the real all-cache-hit worker at 0.4512 GiB, with no cap hit. The cache hit performs no evaluation, recovers the exact basis and preserves every cached byte plus the old receipt/summary bytes. All six fresh feature tables equal the original `ret_12_1` tables exactly. The [proof](../../../.superpowers/sdd/tier1-v2/receipts/1.13-s8-cache-resume-proof.json) additionally refuses basis/key/file/feature drift, an attempt to reuse the original frozen run with changed source, and conflicting history. An injected interruption before the history's atomic rename resumes safely. The proof/audit peaks at 0.1482 GiB.

The source commit and full commands are recorded in the node 1.13 report. The sealed holdout, final identity/terminal-label work, coverage target, qualified-signal library and candidate release remain outstanding.
