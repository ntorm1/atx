# platform-20260928 -- ledger (newest first; `git add -f` this dir)

Goal (owner, 2026-09-28): continue building atx-engine + atx-impl into a production-quality equity L/S quant platform
(high Sharpe, high capacity, low turnover, low/medium frequency). Three fronts: (1) platform + pipeline for faster research
iteration, (2) the implementation and the mega strategy, (3) the pitch to production. Start with code review + web research
-> plan -> 3-4 Opus 5.5 implementer lanes. Root (pool-2) = PM: builds, real-data runs, acceptance, ledger.

Standing rules carried from mega-alpha-20260926 (unchanged): root alone builds (mega-build.ps1) and runs real data
(run_bounded_research.py 180 s / 1536 MiB); children own pool worktrees, never build, never run real data, never spawn
subagents, trailer Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, task-TN-report.md, reply < 15 lines; TRAIN
2020-2022 only; 2023-2024 used twice (new read = validation trial #3, owner gate U1); 2025+ reserved; never read a
per-candidate VAL statistic; no pushes, warehouse writes, broker actions; tree clean before every real-data run; every
ruling declared before any measurement it could bias; Appendix A trial accounting on every TRAIN result.

Inputs: mega-alpha-20260926/{v6-code-review-exec.md, v6-code-review-signal.md, v6-literature.md, v4-prereg.md, progress.md},
docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md, docs/plans/2026-09-28-mega-alpha-data-request-atx-db.md,
docs/plans/2026-09-28-mega-alpha-v6-pitch.html (the v6.1 pitch), final cell mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247
(S2 net +1.239, DSR N29 .911 < .95: freeze gate unmet).

## 2026-09-29 spo-v2 cell REJECTED (a research result this time, not a defect); N 36; spo line closed for v7
- W1b fix-up 116c23e3 merged (digests pinned; the tripwire failure was a wrong test expectation: the middle-ranked
  member's alpha is 0 on the corrupt session, so nothing can inflate). Build mega-v7-6 exit 0; tests 132/132.
- Identities before the cell, all passed, none a trial: (a) flag-off aim-partial-v5 on the v7-6 exe -> 10/10 daily /
  events files byte-identical to the v7.0-lo3 cell (build-equity/v7-6-id-lo3, 27.4 s); (b) `--rule spo-v1` with its
  pre-registered flags on pin 897ffdf2 -> 4/4 daily / events files and spo_diagnostics.csv columns 1-38 (1,509 rows)
  byte-identical to the rejected spo-v1 cell (build-equity/v7-6-id-spo-v1, 53.8 s); (c) gtest digest pins.
- Cell mega-nav-v70-lo3-spo-v2-G1.0 (pre-registered flags, risk pin 786cb601 lo3 atx-risk-v1.1): exit 0, 47.2 s, 349
  MiB, 1,508 solves at 11.0 ms mean (max 33.5), gamma 48.16 (= gamma_vol; aim gross 8.24 at 5% vol; gamma_bind not
  reached, report-only). TRIPWIRE CLEAR (capped_specific_decisions 0), read before any return.
  S2 net SR +0.538 vs +1.332 (v7.0-lo3); gross SR 1.242 vs 1.715; cost 16.99 vs 13.38 bps/$; tau mean .0436 (p95
  .0644); held names 1,514 vs 1,930. Paired dSR -0.794, rho .663, Memmel SE .476 (t -1.67), CBB 95% [-1.680, -0.030],
  LW p .070. Mechanics PASS (gross all rows .9735, post-ramp .9993, |net| .0003, tau within limits): the I-1 fix works.
- VERDICT: REJECTED on two of three criteria (dSR < 0; cost per traded dollar higher, +27%). The optimiser now deploys
  the budget and trades at the intended rate, but it loses a quarter of the gross SR and pays more per dollar: as R2
  M-3 predicted from a synthetic check, a vol-scaled alpha a = IC sigma z under a binding gross cap tilts to high
  specific-vol, lower-ADV names. Lesson G2: with a binding gross budget the risk term does not discipline the book;
  the alpha scaling does. By ruling spo-b no further spo cell is run in v7; the optimiser stays behind its flag;
  aim-partial-v5 stays the construction.
- Integrity (N 36) for the spo-v2 cell: cell-count DSR .314, effective-N DSR .519, PSR(0) .822; PBO over 36 cells .268.
  JSON build-equity/mega-nav-v70-lo3-spo-v2-summ-n36.json, -pbo-n36.json.
- Appendix A: TRAIN construction cells 36 (29 v6 era; C1-C3, spo-v1, spo-v2 rejected; v7.0, U-lo3 accepted); ledger
  36 lines. Admission trials this sprint 5. Validation trials 2 spent. 2025+ reserved. No VAL statistic read.
  Current accepted book: library v7.0 on role lo3, S2 net +1.332. Freeze gate UNMET (cell-count DSR .798 at N 35;
  to be re-scored at the final N).

## 2026-09-29 F3 (risk model atx-risk-v1.1) ACCEPTED; W1b merged (one test fix-up pending); spo-v2 pre-registered
- Merges: W1b step 0 cd01f74e (digest pin test on the base API), W1b 4c4ce75f, F3 b3d03b38. Builds: mega-v7-4 (pin
  capture on the base, exit 0), mega-v7-5 (W1b + F3, 10 TUs, 35 s, exit 0 under /W4 /WX, no compile fix-up needed).
- Tests on v7-5: 129 of 132. SpoPin x2 fail by design until pinned: digests weights 0xda6b6871e7e267c5 and replay
  0xaabdbb72f99a6e13 are IDENTICAL on the base and on the W1b head -> spo-v1 is bit for bit (R2 M-4 closed).
  SpoTripwire.TheClampFeedsAlphaAndTheVoidStopsTheRunAtCapture fails (alpha_shadow equal with and without the clamp):
  sent back to W1b with the pin values. All Risk* / RiskRobust* tests pass.
- F3 risk verb, lo1 (build-equity/v7-f3-risk, 23.1 s, 505 MiB, manifest sha 312aff19...f174eab): max daily specific
  variance .367 (F2 pin: 9.0e12; cells > 1: 3,549 -> 0; p99.99 1.48e4 -> .126; p50 unchanged 4.5e-4); style-dates
  dropped 0; min style dispersion .46; bias factor family b 1.004 (F2 1.004), random family b .947 (F2 .931), dropped
  0. 116 names carry D > .05 on some date (genuinely extreme names; inside the bound). F3 ACCEPTED; R2 I-2 CLOSED.
  The F2 pin 17f9328f and the W1 pin 897ffdf2 are superseded for any new work (897ffdf2 stays for identity (b)).
- F3 risk verb, lo3 (build-equity/v7-f3-risk-lo3, 23.9 s, 481 MiB, manifest sha 786cb601...76e14913): max D .361,
  factor b 1.002, random b .949, 7 structural factors. This is the spo-v2 pin.
- v7-prereg.md "Construction trial: spo-v2 (spo trial #2)" declared with rulings spo-a (runs on the accepted v7.0-lo3
  book) and spo-b (no further spo cell in v7 if rejected). Nothing run yet.
- Appendix A unchanged: TRAIN construction cells 35; validation trials 2 spent; 2025+ reserved.

## 2026-09-29 universe trial U-lo3 ACCEPTED under its pre-registration; N 35; U2 and P5 merged
- Merges: U2 9105bcbc (role linked-operating-v3, shared SIC stage for role and grp_* fields, 203 pytests in
  scripts/tests + atx-engine/tools), P5 89896acc (pitch iteration 3 sections, mega_report/pitch3.py). Pre-registration
  "Universe trial U-lo3" + conditional final cell V7-F declared before any lo3 IC read (rulings U-a, U-b, U-c).
- Role lo3 built (2,037,622 kept member cells; manifest sha 40e3d832...95b78d). Spec scripts/specs/v70-lo3.json (from
  U2's template; summ cells = the 34 ledgered; ledger kind left `construction` so ledgered cells are skipped, the trial is
  a universe trial in meaning). Cycle in one invocation: fields-v7 list on lo3 (41 fields, peak 649 MiB), u 82.2 s /
  1,216 MiB (cold cache), fit 21.6 s, w 13.0 s, nav 27.8 s / 347 MiB.
- Admission refit on lo3: 34 admitted, the same 34 as on lo1; no status flip (ruling U-b: nothing confounded).
- Cell mega-nav-v70-lo3-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247: S2 net SR +1.332 vs +1.313 (v7.0 lo1), gross SR 1.715
  vs 1.688, cost 13.38 vs 13.44 bps/$, tau .0378 (p95 .0468), held names 1,930 vs 1,849, HAC t 2.31.
  Paired dSR +0.019, rho .997, Memmel SE .042 (t +0.45), CBB 95% [-.067, +.098], LW p .658.
  Mechanics: gross all rows .9645, |net| .0047, tau mean .0378, p95 .0468 -> PASS.
- VERDICT: U-lo3 ACCEPTED (paired S2 net dSR > 0, sign-only inside one SE, AND mechanics). Again a sign result inside one
  SE. Role lo3 replaces lo1 for the v7 final book. Cumulative vs the v6.1 cell: +1.239 -> +1.332 (two accepted steps,
  each inside one SE).
- Integrity (N 35): cell-count DSR .7980, effective-N DSR .8566 (N_eff 2), Lo null .5612, PSR(0) .9851, MinTRL 95% 432
  sessions; PBO over 35 cells .2435. FREEZE GATE UNMET (cell-count DSR < .95).
- JSON: build-equity/mega-nav-v70-lo3-summ-n35.json, mega-nav-v70-lo3-pbo-n35.json.
- Appendix A: TRAIN construction cells 35 (29 v6 era + C1-C3 + spo-v1 rejected + v7.0 accepted + U-lo3 accepted);
  ledger 35 lines. Admission trials this sprint 5. Validation trials 2 spent. 2025+ reserved. No VAL statistic read.
- Next: v7.1 (wave 2) on lo1 fields-v9 vs the v7.0 cell (dsr_n becomes 36); if accepted, V7-F = v7.1 on lo3 with
  fields-v9 rebuilt on lo3 (N 37). spo trial #2 after F3 + W1b + new risk pin + its pre-registration.

## 2026-09-29 library v7.0 (wave 1) ACCEPTED under its pre-registration; N 34; freeze gate still UNMET
- Cycle `research_cycle.py run scripts/specs/v70.json` (three invocations: two hard-stops on a dirty tree when lane
  reports landed in the sprint dir, resumed after a commit; no phase was re-run): u 48.4 s / 1,162 MiB (cache seeded from
  v7w2), fit 28.9 s, card 21.3 s, w 19.4 s / 505 MiB, nav 42.1 s / 346 MiB.
- Identity (ruling 7.0-d): the 39 v6.1 members' orientations and train_daily_ic rows are byte-identical to
  mega-v61-train-u-1; only the `__combined__` rows differ (the u-pass combination includes the new members).
- Admission v4-prior-v1 (5 trials): q5_eg ADMITTED (HAC t 1.09, max |rho| .78 value_composite), smax5 ADMITTED (t 1.26,
  |rho| .69 high_52w); qmj_safety rejected by the veto (HAC t -2.33 against its prior sign); nincr redundant with sue
  (|rho| .93); res_mom_ind redundant with res_mom_12_1 (|rho| .91). Library 39 -> 41 admitted-eligible members.
- Cell mega-nav-v70u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 (v6.1 final construction, L 1.247): S2 net SR +1.313 vs
  +1.239, gross SR 1.688 vs 1.594, cost 13.44 vs 13.55 bps/$, tau .0375 (p95 .0463), held names 1,849, HAC t 2.26.
  Paired dSR +0.073, rho .978, Memmel SE .123 (t +0.60), CBB 95% [-.180, +.309], LW p .566.
  Mechanics: gross all rows .9700, |net| .0048, tau mean .0375, p95 .0463 -> PASS.
- VERDICT: wave 1 ACCEPTED whole (pre-registered rule: paired S2 net dSR > 0, sign-only inside one SE, AND mechanics).
  The gain is inside one SE: it is a sign result, not a significant improvement. Rejected members are disclosed above
  and stay in the library file as non-admitted; nothing was dropped one by one. v7.0 is the parent and its cell the
  reference for wave 2 (ruling W2-a).
- Integrity (N 34): cell-count DSR .7952 (SR0 .807 ann), effective-N DSR .8512 (ONC N_eff 2), Lo null .5534, PSR(0)
  .9838, MinTRL 95% 446 sessions. PBO: all 34 cells .284; explicit grid {v6.1, C1-C3, spo-v1, v7.0} .258 (IS winner OOS
  +1.269, P[OOS < 0] .023) (ruling 7.0-c: both reported, neither gates).
  FREEZE GATE UNMET: S2 net >= 1.0 yes, mechanics yes, cell-count DSR .7952 < .95.
- CORRECTION to handoff 1 / the 2026-09-28 ledger: the baseline's "cell-count DSR .9032 (N 33)" was the N 32 figure
  (before spo-v1). With the spo-v1 defect cell (SR -0.94) inside the cross-cell variance the baseline scores .7663 at
  N 33 and .7594 at N 34 (V[SR_n] 1.75e-4 -> 5.59e-4). Ruling 7.0-e (declared after seeing it, so it is a disclosure,
  not a choice): the protocol's variance is "all ledgered cells" and stays so; no cell is removed from the variance ex
  post -- cost if wrong: a defect cell, which is not a research hypothesis, makes the gate harder; whether defect cells
  belong in the variance is an owner question (recorded for the scorecard), not something root changes mid-sprint.
- JSON: build-equity/mega-nav-v70-summ-n34.json, mega-nav-v70-pbo-n34.json, mega-nav-v70-pbo-grid6.json; cards
  build-equity/mega-cards-v70; monitor build-equity/mega-monitor-v70.
- R2 review of spo-v2 (task-R2-review.md): 2 I, 4 M, 6 m; NO-GO on b63829f0 as committed. I-1 the gross budget must be
  1.0 (hard cap vs R6'); I-2 the risk model is corrupt at the source (one asset_growth outlier -> style collapse ->
  structural specific variance 3.9e12 -> decile shrinkage spreads it to 176 names; pins 897ffdf2 and 17f9328f both
  carry it; the asset_growth style is a one-name dummy on 259 dates). Lanes F3 (pool-7, risk model atx-risk-v1.1) and
  W1b (pool-3, spo-v2 corrections) launched; spo trial #2 waits for both, a new risk pin and its pre-registration.
  Ledger correction from R2: spo `exante_vol` is annualised (v1's .0066 = .66%/yr, not 10%).
- U2 (pool-9 9105bcbc): role lo3 keeps 1,404,404 score cells (lo1 1,343,804, lo2 1,360,656); 43,811 of the 53,691
  no_visible_sic cells recovered, 9,776 reclassified non-operating; the recovered cells have no fundamentals under
  fundamental-events-v2 (be finite .000). Universe trial not yet pre-registered.
- Appendix A: TRAIN construction cells 34 (29 v6 era + C1-C3 + spo-v1, all four rejected, + v7.0 accepted); ledger
  build-equity/trials.jsonl 34 lines. Admission trials this sprint: 5 (wave 1). Validation trials 2 spent (2023-2024
  read twice). 2025+ reserved. No per-candidate VAL statistic read.

## 2026-09-29 L7 merged (library v7.0, spec v70); wave 2 pre-registered; rulings before the v7.0 run
- L7b finished the stopped lane: pool-10 e1915059 + merge 36f92885; library sha e7bae75c..., recipe 60b82300...; 327
  pytests. P4 wrote library-v7-wave2-prereg.md; v7-prereg.md "Library v7.1" appended with rulings W2-a..e (e3880d83),
  before any wave-1 read.
- Ruling 7.0-c (before the run): the cycle's summ scores 34 cells with bare `--pbo` (all cells, as n29) and root also
  writes the explicit grid {v6.1, C1, C2, C3, spo-v1, v7.0} as in n33; both PBO figures are reported, neither gates
  wave 1 (the pre-registered rule is paired S2 net dSR > 0 AND mechanics) -- keeps both earlier conventions comparable --
  cost if wrong: one more descriptive number in the scorecard.
- Ruling 7.0-d (before the run): the candidate cache v70 is seeded by hard links from mega-candidate-cache-v7w2 (39 v6.1
  entries, content-keyed by DSL sha + field payload sha); the v6.1 members' orientation / daily IC rows must come out
  byte-identical to mega-v61-train-u-1, else the run is void and no trial is counted -- cost if wrong: a recompute (~75 s).

## 2026-09-29 session 2 opened; W1 spo-v2 merged (4cd5513e), build v7-3, identity ok
- Lanes launched (Opus 5.5): L7b pool-10 (finish library v7.0), P5 pool-11 (pitch iteration 3 sections), U2 pool-9 (role
  lo3 with atx-db SIC events + fields SIC override + universe prereg draft); read-only: P4 (wave-2 prereg text), R2
  (adversarial review of spo-v2 b63829f0 + specific-variance root cause + prereg defaults). Briefs task-{L7b,P4,R2,P5,U2}-brief.md.
- Ruling: merge and build W1 before the R2 review returns -- spo-v2 is behind `--rule spo-v2`, spo-v1 and flag-off paths
  claim bit identity, and the identity run below tests that claim -- cost if wrong: one more build tag if R2 finds a defect.
- Build mega-v7-3 exit 0 (6 TUs, 42.6 s; targets atx-impl-strategy-target-tests, atx-equity-strategy-targets). Tests
  Spo*/Risk*/NavV7Hook*/AimV6*/CostV2*/StrategyLive*/StrategyNavReplay*/NavV5*/TargetReplayV5*/TransferCoefficient* 116/116.
- Identity (not a trial): v6.1 final construction, --rule aim-partial-v5, on the v7-3 exe -> build-equity/v7-3-id: 10/10
  daily/events files byte-identical to mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247.
- No spo-v2 cell run: waits for R2 and the pre-registration. Appendix A unchanged: TRAIN construction cells 33;
  validation trials 2 spent; 2025+ reserved.

## 2026-09-28 SESSION CLOSED by owner ("stop here") -- handoff written
- docs/plans/2026-09-28-platform-v7-handoff-1.md (state, lanes, evidence, open decisions, next steps, commands, pitfalls)
  and docs/plans/2026-09-28-platform-v7-next-goal-prompt.md (the /goal text for the next PM).
- In flight at close: L7 (pool-10, library v7.0), W1 (pool-3, spo root-cause), P4 (read-only wave-2 prereg). Their
  commits live in their pools; hand-backs to this session are lost. Nothing was run after the fields-v9 build.
- Not in local main yet: everything since c0dc376f. Merge command in the handoff �0.

## 2026-09-28 role linked-operating-v2 built (lo2); fields v9 building
- lo2 (identity-bridge-v2-pit, share-class letters allowed, delisting block): kept member cells 1,967,838 vs lo1
  1,940,364; min kept in the score window 1,697 vs 1,675; dropped share .3956 vs .4031. Dropped by reason (score window):
  unlinked 820,393 (lo1 899,105), no_visible_sic 53,691 (lo1 34), secondary_line 7,901, non_operating_sic 8,711.
  W5b-F1 confirmed: the new links mostly fall to no_visible_sic because pool-2's fundamental-events-v2 lacks their SIC ->
  next role rebuild must take SIC from the atx-db fundamentals/sic_events stage. W5b-F2: 820k unlinked member cells on
  the top-3000 base role = the largest remaining universe lever (P1 measured +0.22 for the v6 restriction); ask atx-db
  which base-role names those are (ETFs/ADRs/non-filers vs link gaps).
- Fields v9 built: 63 fields (= 64 limit - 1), 121 s, peak 571 MiB; all 55 v8 payloads byte-identical. The 14 W5a fields
  and sv_ratio126 were recomputed, not reused: the M-6 code-identity rule keys on the builder module's code sha, which the
  W5b hook changed (F1-F2, minor: per-module producer hashing would make reuse finer). W5b coverage on member cells:
  inst_own_share .993, inst_best_ideas .999, ftd_shares_ratio21 .985, regsho_threshold_days63 .426 (NYSE lists pending).
  Manifest: build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9.

## 2026-09-28 F2 merged (10a723a7) and ACCEPTED; W5b merged
- F2 structural forecast: build mega-v7-f2 exit 0; Risk*/Spo*/NavV7Hook*/StrategyLive* 60/60. Risk verb on lo1 with
  per-date exposures (build-equity/v7-f2-risk, 21.9 s, 455 MiB): 11 factors structural on 1,029 sessions, "forecast
  sessions with an unforecast exposed factor 0"; bias factor family ok (52 series, dropped 0), random family OK (64
  series, 53,696 obs, dropped 0). F1-F1 CLOSED. Manifest sha 17f9328f...138bbe9f = the risk model pin for any spo trial #2.
- W5b research_fields_holdings.py (+5-line hook): 8 PIT fields (13F 5, FTD 1, Reg SHO 1, off-exchange SV 1), 86 tests;
  role rule linked-operating-v2 on identity-bridge-v2-pit with delisting terminations in the manifest and optional
  --delisting-returns (off by default; v1 rule byte-identical). Kept member cells 1,940,364 -> 1,967,838 (+1.4%; W5b-F1:
  53.7k newly linked cells lack SIC in fundamental-events-v2 -> rerun with atx-db SIC events later). Reg SHO field ~.43
  coverage until the NYSE lists land (NaN, not zero). Fields total with W5a = 63 of the runner's 64 limit (W5a-F1).

## 2026-09-28 W5a merged (6b12220c) and ACCEPTED; fields v8 built
- W5a research_fields_sec.py + 15-line FIELD_MODULES hook: 14 PIT fields (earnings calendar 6, Form 4 5, 8-K 3), 66
  pytests. Two data rules pre-registered (10%-owner joint Form 4 dropped; insider ratios NaN outside [-1,1]).
- Root build lo1-fields-v8 (55 fields; --reuse fields-v7 hardlinked): 55.9 s, peak 399 MiB; every fields-v7 payload
  byte-identical (see check line above); coverage on member cells 2020-2022: ea_* .93-.96, ins_* .97, k8_* .97-.98.
  Field count 55 of the runner's 64 limit (W5b adds ~8 -> 63; W5a-F1: the limit will bind at the next wave).
- Note: L7 found the draft's "six extra q5 fields" are existing fields-v7 fields; prereg corrected, no fields v8 needed
  for wave 1 (wave 1 runs on fields-v7; wave 2 on v8/v9).

## 2026-09-28 spo-v1 cell REJECTED (implementation defect, not a research result); N 33
- mega-nav-v61u-spo-v1-L1.247 (pre-registered params): exit 0, 59.5 s, 365 MiB, 1,508 solves at 14.2 ms mean (max 35.6),
  gamma 1575. S2 net SR -0.939 vs +1.239 (paired dSR -2.179, Memmel SE .577, t -3.77, CBB [-3.62, -1.01]); gross_lev
  all rows .6465 (mechanics FAIL: < .90), |net| .0009, tau mean .1217 / p95 .1753 (mechanics FAIL: p95 > .30 not hit
  but tau 3x the baseline), cost 15.06 bps/$; DSR N33 .002. REJECTED on every criterion. Cross-cell N 32 -> 33; ledger 33.
- Diagnostics (spo_diagnostics.csv, 1,508 rows): gross_binding 0 on every date (the gross-1.247 gamma calibration never
  binds: the book deploys at .39-.83 gross), iterations 122-178, exante_vol ~.0066/day (~10% ann vs the 5% target),
  exante_vol_current has garbage values (mean 21.4 vs p95 .0073), trade_cost ~1.1e-4/day vs alpha 2.7e-4/day with the
  20-session amortisation making the effective cost penalty 5.7e-6 -> the optimiser trades 12%/day. Verdict: defects in
  gamma calibration, cost amortisation and a diagnostic column; a negative net SR from the same signals also suggests a
  sign or scaling error in alpha_i = IC sigma_i z_i or in the delta-order mapping. Sent to W1 for root cause. Any rerun is
  construction trial #2 for spo (new prereg line), disclosed.
- CSCV PBO over {baseline, C1, C2, C3, spo}: .347 (IS winner baseline; OOS +1.221). Effective-N over 33 series: N_eff 2
  (the spo series is its own cluster).
- Appendix A: TRAIN construction cells 33 (29 v6 era + C1-C3 + spo-v1, all four rejected). Validation trials 2 spent.
  2025+ reserved.

## 2026-09-28 W2, W3, F1, W1-fix ACCEPTED; spo-v1 cell launched
- W2: build mega-v7-w2 exit 0 (120 TUs, 223 s); AlphaLitOps 22/22; golden digests captured on the merged build
  (vm_audit_exact 0x971c3da60ca89aa3, vm_research_fast 0xe9e7128fd5359900, oracle 0x971c3da60ca89aa3) and pinned by W2
  (merged ce0ab3a6); real-data proof of unchanged semantics: v6.1 u pass on a fresh cache with the W2 engine ->
  orientations/daily IC/combined byte-identical to mega-v61-train-u-1, candidates identical after drops (72.6 s, 1,061 MiB).
  vec_sum allowed via an explicit POLICY_OPS entry (q5).
- W3: 173 pytests; report cards for the 39 v6.1 candidates in 22.2 s / 922 MiB, runner_check valid_date_mismatches 0 at
  h = 5/21/63 for all 39; monitor baseline written (M1 n/a: no book bias rows yet; M2 in-sample IC CUSUM: 1 alarm / 16
  warn / 15 ok of 32 sleeves -- W3-F1: the alarming sleeve is a within-TRAIN decay signal to read in the cards; M3, M4
  ok); fitter WorkStore: computed 39 (24.9 s) then computed 0 / reused 39 (0.5 s), admission byte-identical a == b and
  candidate-identical to the v6.1 admission (only the `inputs` pins differ). W3 ACCEPTED.
- F1: risk verb bias v2: factor family ok (51 series, b .995, dropped 0); random family REFUSED (dropped_factor_exposures
  15,168 of 38,528 obs = 28% > 5%): random long-short portfolios load on 11 factors with < 63 observations (empty
  forecast series) -> F1-F1: the earlier random-family b was optimistic; also the optimiser's X F X' treats those
  exposures as zero variance. Fix = structural forecast for short-history factors (R3.1 already called for it) -> lane F2.
  --reuse r7c with code identity: computed [sv_ratio126], 40 reused with reused_from code records. F1 ACCEPTED.
- W1 fix (test expectation; gross multiplier populated; gamma calibration unaffected): target tests 103/103 (build
  mega-v7-w1b). Identity flag-off on the current exe then the pre-registered spo-v1 cell launched.

## 2026-09-28 F1, W2, W3 merged (7de07b49, W2/W3 -> 94770874)
- F1 (R1 fixes I-1, M-5, M-6, M-7 + m-5/6/13/16): build mega-v7-f1 exit 0; 101/102 C++ (only the known SPO test), 26
  pytests. Real-data re-runs (risk verb counts, --reuse with code identity) in progress.
- W2 DSL ops (ids 89-104: pack2/3, ts_topk_mean, bucket -> Group, group_cross, ts_resid_on/ts_beta_on, cs_resid_on,
  ts_count_increases, *_mp min-periods variants; builtin_ops() unchanged at 74 rows; checker extended). Golden-digest
  test deliberately unpinned: root captures digests on the merged build, verifies the v6.1 IC pass byte-identical on
  real data (the stronger unchanged-semantics proof), then has W2 pin them. W2 notes: q5 DSL needs vec_sum (checker
  policy), nincr lookback 797 and FF3/q5 503 sessions vs the runner's lookback limit -> P3 must respect these.
- W3 (alpha_report_card.py, book_monitor.py M1-M4, fitter WorkStore, cycle card/monitor phases, specs/v61-ops.json):
  173 pytests pass + 1 skipped (live). Real-data acceptance (cards, monitor baseline, fitter identity) next.

## 2026-09-28 W1 merged (f6faa6cd); spo-v1 pre-registered; risk exposures built
- W1 spo-v1 (FISTA/ADMM around the GP aim with factor risk + cost v2; new strategy_spo.{cpp,hpp}). Build mega-v7-w1 exit
  0 (6 TUs, 35 s). Tests 77/78: SpoSolver.NetGrossBetaAndBoxesHoldTo1e10 fails on `multipliers.rho == 0` (gross
  multiplier) -> sent to W1; the cell waits for the fix because gamma is calibrated from the binding gross constraint.
- v7-prereg.md: spo-v1 single cell declared (defaults ic-book .02, w-max .01, adv q .05 / p .01, iters 500, tol 1e-8,
  target-vol .05, gamma = max(vol-5% gamma, gross-1.247 gamma), 20-session cost amortisation, primary books; N 32 -> 33).
- Risk model with per-date exposures (`--emit-exposures all`): build-equity/v7-w1-risk-all, 27.9 s, 495 MiB, 377 MB,
  manifest sha 897ffdf2...b46bd9d19967e (pinned for the cell).

## 2026-09-28 W4 merged (8921dc2c) and ACCEPTED on real data
- Build mega-v7-w4 exit 0 (12 TUs, 54 s), 80/80 gtests (StrategyLive 18 + identity suites). NAV v6.1 flag off 32.8 s /
  flag on with binary holdings 39.8 s (1.21x, cap 1.3x; L3-F1 CLOSED: was 140 s / 374 MB CSV, now 118 MB f64 + index);
  10/10 NAV files identical both ways. decide at the three dates from the binary holdings: exit 0, parity 0, 1.8-2.1 s
  (was 16-17 s), targets.csv/orders.csv byte-identical to the L3 runs, orders_shares.csv written ($1bn NAV, lot 1, min
  notional 500). reconcile self: 1,838 names, 0 breaks; mutated broker file: missing 1, quantity 1, extra 1 unexplained
  (exit 5) and the x2 split explained by corporate actions -> as specified.
- L3-F2 CLOSED as definitional: under the replay's TC definition (desired/sigma) the three dates read .723 / .695 / .755
  (> .5); the .09-.29 values were the signal/sigma^2 definition (R1 M-3), now both reported under explicit keys.
- R1 rows M-1, M-2, M-3, m-8, m-13, m-14 fixed in W4. Remaining review rows: I-1, M-5, M-6, M-7 in F1 (running); M-4
  disclosed (prereg addendum A1).

## 2026-09-28 atx-db delivered most of the data request -> wave 3 opened
- ALPHA_PANEL_REQUEST_V7_RESPONSE.md (atx-db, 20:44): U1 link table met (95.6-99.1% linked/yr), U2 dead-line 91-93%, U3
  security master, U4 delisting 85.8% classified (imputed returns), D1 13F 2013q2-2026q2 (124.4M rows), D2 earnings
  calendar with expected next date, D3 short volume splits + FTD + Reg SHO (NYSE landing still running), D6 all 15
  statement items, D7 history (fundamentals 2010+, prices 2012-03+), D10 corporate actions, D11 Form 4 (4.90M), D12 8-K
  metadata. Not available: consensus, borrow fee/utilisation, GICS/NAICS, options skew/OI, VWAP, index add/drop. S2
  coverage improved but most targets short (gp_ttm .753 linked-USD vs .90; xrd .895 vs .95; fscore_partial .922 MET).
  Panel v2 assembly, borrow proxy, metrics and the lo1 aligned export are pending on their side (1.5-2.5 h).
- Ruling: consume the delivered stage exports directly (not the pending panel) through new PIT field modules; role
  variant linked-operating-v2 on identity-bridge-v2-pit; library v7 pre-registered from a read-only draft before any IC
  run -- data before definitions would bias admission -- cost if wrong: rework when the panel lands. Lanes: W5a pool-8
  (earnings calendar, Form 4, 8-K fields), W5b pool-9 (13F, FTD/Reg SHO fields + role v2 + delisting returns), P3
  read-only library-v7 draft. Owner pointer: C:/atx/atx-db/docs/ALPHA_PANEL_STATUS.md.

## 2026-09-28 R1 review landed (1 I, 7 M, 16 m) -- routed
- I-1 v6 capacity pass prices c_i at base scale (v5 capacity curve unaffected) -> F1. M-1..M-3 decide (flat-book default
  when the as-of row is missing; no positions/locates SHA in decision.json; TC definition differs from the replay's
  v7_transfer_coefficient.csv: signal/sigma^2 vs desired/sigma -> the L3-F2 TC values are not comparable with L4's) -> W4.
  M-4 aim-partial-v6 side rescale undisclosed -> v7-prereg.md Addendum A1 (disclosed deviation; results already read; no
  acceptance depended on it). M-5 bias harness silently drops unforecast factors and uncovered names (b biased up ->
  the .995 reading is optimistic until fixed) -> F1. M-6 --reuse keyed on a hand-bumped revision -> F1. M-7 pitch.py
  cache-scan picks a v2 entry without DSL sha -> F1. F1 lane opened in pool-7.

## 2026-09-28 GRID RESULT: aim-partial-v6 C1-C3 REJECTED; baseline v6.1 stands (N 32)
| cell | kappa | clip | S2 net | gross SR | cost bps/$ | tau mean | dSR vs v6.1 (Memmel SE, t) | DSR N32 |
|---|---|---|---|---|---|---|---|---|
| baseline v6.1 (aim-partial-v5) | - | - | +1.239 | 1.594 | 13.55 | .0379 | - | .9032 |
| C1 | .5 | [.5,1.5] | +1.160 | 1.431 | 11.82 | .0372 | -0.080 (.078, -1.02) | .8790 |
| C2 | 1.0 | [.5,1.5] | +1.121 | 1.424 | 11.34 | .0369 | -0.118 (.103, -1.15) | .8663 |
| C3 | 1.0 | [1,1] | +1.112 | 1.425 | 11.42 | .0367 | -0.127 (.100, -1.27) | .8639 |
- Mechanics pass on all three (gross_lev_all_rows .969-.971, |net| <= .0077, tau p95 <= .047), cost per traded dollar
  falls 13-16%, but gross SR falls ~10%: the cost-scaled target shrink removes more alpha than cost (cheap names are not
  the alpha-rich names). Acceptance rule (dSR > 0) fails for every cell -> REJECTED, no freeze change. Regime rate
  (C1/C2 vs C3) adds nothing measurable. Finding G1 for the SPO lane: shrink targets by alpha-per-cost, not by cost alone
  (that is what the optimiser objective does).
- CSCV PBO over {baseline, C1, C2, C3}: .346 (IS winner = baseline in 9,790 of 12,870 splits; OOS +1.226 ann; P[OOS<0]
  .026). Effective-N (ONC) over the 32 trial series: N_eff 5; ledger 32 lines.
- Appendix A trial accounting: TRAIN construction cells 29 -> 32 (C1-C3 disclosed above, all rejected). Validation
  trials 2 spent (unchanged). 2025+ reserved. No per-candidate VAL statistic read.
- Baseline v6.1 at N 32: effective-N DSR .9512 (N_eff 5) beside cell-count DSR .9032 and Lo .5153. Reported only; the
  freeze gate remains the pre-registered cell-count DSR >= .95 (unmet at .903).
- Note: PBO .35 > .2 threshold in v7-prereg applies only to accepting a NEW winner; the baseline was not chosen on this
  grid, so nothing is accepted or gated by it. Recorded for the pitch's honesty section.

## 2026-09-28 v7-2 build green; identity PASS; grid C1-C3 running; wave 2 dispatched
- Fix-ups merged: L1 short partial-file names (Windows 259-char path limit was the "partial output" cause), L4 risk-verb
  role masks + `--band-exponent` (+ test rename). Build mega-v7-2a/2b: exit 0; StrategyIcRunner 46/46; target tests
  124/124 (L4 + identity + StrategyLive). HEAD debbb27f.
- aim-partial-v6 identity (kappa 0, band exponent 0, clip [1,1]): 10/10 CSVs byte-identical to the v5 cell -> prereg
  identity requirement met; C1-C3 launched (three cells, N 29 -> 32 as pre-registered).
- Risk verb on lo1: exit 0, 31.4 s, 521 MiB; geometry 1,155 dates x 5,627 instruments, 62 factors (market + industries +
  11 styles; descriptors book_to_price, earnings_yield, gross_profitability, asset_growth, leverage, si_ratio, days_to_cover;
  unavailable []). Bias harness factor family full-sample b mean .995 / median .997; rolling-252 b mean .987 within band
  [.911, 1.089]; pooled kurtosis 7.0. Descriptive; ACCEPTED as a working risk model v1.
- Owner (mid-turn): spin up more sub agents, speed up. Ruling: wave 2 dispatched now, before the grid result -- wave-2
  lanes do not touch the pre-registered grid parameters, so nothing they do can bias it -- cost if wrong: rebase work.
  W1 pool-3 spo-v1 optimiser (FISTA/ADMM, factor risk + cost v2), W2 pool-10 DSL ops, W3 pool-11 report card + monitor +
  fitter WorkStore, W4 pool-4 holdings writer + order file + reconcile + freshness/TC checks, R1 adversarial review of
  cdc9c2a8..HEAD (read-only).

## 2026-09-28 L4 real data: stress + capacity ACCEPTED (descriptive); aim-partial-v6 identity FAILED (fix requested)
- `--cost-v2 --capacity-curve` on the v6.1 cell (v7-l4-nav-stress): exit 0, 80.4 s, 368 MiB; S1/S2/S3/flat-300/engine-tiers
  10/10 files byte-identical to the v6.1 cell. New descriptive scenarios (TRAIN, no trial): S2-KO net SR 1.269, S2-FIM 1.347
  vs S2 1.238, S1 1.380, S3 .359 -> S2 remains the conservative primary (P2 S4 expectation confirmed).
- Capacity curve by replay (R4.2; x1 == S2 bit-for-bit): NAV multiple .5/1/2/4/8 -> net SR 1.273/1.239/1.183/1.053/.912;
  gross SR 1.590/1.594/1.578/1.467/1.312; cost 11.5/13.5/16.0/18.2/19.9 bps per traded $; capped-fill share .12%/.51%/
  3.3%/12.1%/26.6%. Read: net SR >= 1.0 holds to ~4x ($4bn at the $1bn base) with the 1% ADV cap binding from 2x.
- aim-partial-v6 identity cell (kappa 0, clip [1,1]): S2 net 1.241 vs 1.238, all 10 files differ from row 0 (banded_names)
  because the band is always cost-scaled with exponent 1/3 and no uniform option exists. Per v7-prereg.md no C1-C3 cell
  runs until identity holds -> L4 asked to add --band-exponent (default 1/3; 0 = uniform). Also pending: the risk verb
  mask fix.

## 2026-09-28 L1 merged (b5554999) -- real-data ACCEPTED, one gtest fix-up pending; L4 merged (6f7ee661) -- build green
- Build mega-v7-l1 (core tests, IC tests, IC exe, target tests): exit 0, 95 TUs, 215.6 s. Sha256.* 7/7; StrategyIcRunner.*
  44/45 (CandidateCacheReadsV1EntriesInPlaceThroughKnownManifests: "candidate cache partial output" -> sent to L1);
  StrategyLive.* 9/9 after the L3 test fix.
- L1 real data (hard-linked v6u cache -> mega-candidate-cache-v7l1, --cache-legacy-fields lo1-fields-v6b): u pass 23.9 s /
  856 MiB (was 76.1-104.9 s / 1,059 MiB), signal + IC cache 38 hit / 1 miss (sv_flow only), orientations.json,
  train_daily_ic.csv, train_combined.json byte-identical to mega-v61-train-u-1, train_candidates identical after the four
  timing/cache drops. w pass 19.3 s / 505 MiB (was 31.5 s), 39/39 hits, identical to mega-v61w-train-ew-1. --cache-report
  works with --max-memory-mib 1536 (without it the admission check refuses; L1-F1 minor). Targets were u <= 20 / w <= 15 s;
  measured under a concurrent 4-job build; accepted on identity + hit counts, timing re-measured on a quiet host later.
- L4 rebased onto L3 (fb5920f0; conflicts in the two CMakeLists and strategy_nav_replay.cpp, hook 14+/4- lines at 10
  seams inside L3's plan_weights). Build mega-v7-l4 (target tests, NAV exe, new atx-equity-strategy-risk): exit 0, 46.6 s,
  9 TUs. 119/119 gtests (L4 suites + identity suites + StrategyLive). Risk verb on the real lo1 role refused:
  "InvalidArgument: risk: role presence/membership masks" -> sent to L4 (must load the restriction masks as the replay does).
  Stress/capacity run and the aim-partial-v6 identity cell in progress.

## 2026-09-28 L2 ACCEPTED; L3 merged (31f79c0e + fix 8f66bd25) and ACCEPTED; L1 follow-up in flight
- L2 items (2)-(4): `run specs/v61.json --suffix r7` reproduced the v6.1 ladder with zero hand pins (u 104.9 s / 1059 MiB,
  fit 34.7 s, w 43.3 s, nav 40.5 s); all 10 daily/events CSVs byte-identical to the v6.1 cell (primary sha 1adf0e8f...).
  `--reuse-fields` from lo1-fields-v6b: 40 reused, computed [sv_ratio126], files map == fields-v7 (41/41). Refusal:
  max_rss_mib=64 -> "HARD-STOP [u]: ... outcome rss-limit, exit_code 15", driver exit 4. L2 ACCEPTED.
- L3: build mega-v7-l3 (exit 0, 52.5 s, 6 TUs, no warnings-as-errors fixes needed). gtests 89/90; the one failure was a test
  bug (two refusal outputs collided with the fixture's nav dir) fixed test-only in c1fa1e75 (merged 8f66bd25; re-run of the
  9 StrategyLive tests pending the next test build). Real data: flag off -> v6.1 cell 11/11 files identical (33.6 s);
  `--emit-holdings` -> 11/11 identical but 140.3 s wall and a 374 MB holdings.csv (finding L3-F1: needs a streaming/binary
  writer or per-year shards before the 180 s cap bites; holdings.csv deleted after acceptance, holdings_days.csv kept).
  decide at 2020-08-06 / 2021-08-04 / 2022-10-12 from the emitted holdings with --check-replay: exit 0, replay_parity
  mismatches 0 at all three, 16-17 s each, health warn. L3 ACCEPTED on the pre-registered criteria.
- Finding L3-F2 (for wave 2): transfer coefficient corr(alpha/sigma^2, w) = .089 / .185 / .262 (target) at the three dates;
  P2 R2.5 flags TC < .5 as constraints (locate, ADV cap, neutrality, partial aim) eating alpha -> supports the SPO lane.
- L1 (pool-10 HEAD 35861db6): SHA-NI + content-keyed cache v2 + --cache-report; flagged that fit_composition_weights.py
  and mega_report/pitch.py read only v1 sidecars -> sent back to L1 to add v2 readers with tests before root builds.

## 2026-09-28 L2 merged (21663eed) -- acceptance in progress
- L2 (pool-11, HEAD 6332d686, 104 tests): research_cycle.py (plan/lock/run/status, hard-stop on refused receipt),
  prepare_research_fields.py --reuse, studies/backtest_integrity.py + nav_summ.py (trial ledger, ONC effective-N DSR,
  CSCV PBO, PSR/MinTRL). Stretch WorkStore skipped (needs L1 sidecar). Merged before L1 (no C++ touched).
- Accepted so far: (1) `plan specs/v61.json --lines-only` == v61_train.sh DRY fixture (diff empty). (5) integrity stats on
  the 29 cells, two runs byte-identical, ledger 29 lines then +0. Final cell v61u L1.247: effective-N DSR .9491 (ONC
  N_eff 5 of 29; mean off-diagonal rho .931) beside cell-count DSR .9108 and Lo .5305; PSR(0) .978, MinTRL 95% 501 sessions
  (1.99 y); PSR(.5) .886. CSCV PBO over all 29 cells .357 (exhaustive 12,870 splits, 16 x 47 sessions; IS winner +1.350 ->
  OOS +.978, P[OOS<0] .069). Reported only; the v6 gate is NOT re-scored (R5.2). Note: the 29-cell grid mixes v5 and v6
  lineages, so this PBO is an upper-bound-ish reading for the v6 ladder; the v7 grid PBO will be over {baseline, C1-C3}.
- Pending: (2) full reproduction run --suffix r7 (S2 daily CSV SHA must equal 1adf0e8f...), (3) --reuse-fields from v6b,
  (4) refusal stop with max_rss_mib=64.

## 2026-09-28 P2 landed; plan v7; lanes L1-L4 dispatched; A5 A/B
- P2 literature-v7.md (451 lines). Top: integrity tooling (trial ledger, effective-N DSR, CSCV PBO) first; cost model v2
  (KO/FIM) + replayed capacity curve; USE4-style risk model + bias harness; GP cost-aware optimiser (L, after risk + cost);
  keep ew-theme-v1 (fitted weights lose 35-60% SR at T = 3 y). Do not build: fitted sleeve weights, stop-losses, finer grids.
- plan-v7.md = synthesis (decisions D1-D10, lanes, contracts, trials). Briefs L1-L4 committed cdc9c2a8; implementers spawned
  (Opus 5.5): L1 pool-10 IC cache/hash, L2 pool-11 research_cycle + --reuse + nav_summ integrity, L3 pool-4 decide path,
  L4 pool-3 risk model + cost v2 + aim-partial-v6. Merge order L1 -> L2 -> L3 -> L4.
- A5 result: build mega-v7-rel0 (equity-rel, exit 0, source 8cb735ab). v6.1 u pass with the Release IC exe on a fresh
  cache (mega-v7rel-train-u-1, receipt source cdc9c2a8): orientations.json, train_daily_ic.csv, train_combined.json
  byte-identical to mega-v61-train-u-1; train_candidates.jsonl identical after dropping timing/cache keys. IDENTITY PASS.
  Wall 98.1 s vs 76.1 s Debug, peak RSS 1,058 vs 1,061 MiB -- measured while four agent lanes hammer the disk; timing
  inconclusive, re-measure on a quiet host before adopting Release. Not adopted yet.
- Ruling: v7-prereg.md declares aim-partial-v6 parameters and the 3-cell grid, S2-KO/S2-FIM as descriptive scenarios, and
  effective-N DSR reporting from v7 on -- declared before any L4 measurement -- cost if wrong: 3 cells of N.

## 2026-09-28 sprint open
- Ruling: research phase first (P1 code review of platform/pipeline/production gaps, P2 literature on combination,
  construction with costs, risk models, capacity, platform design, new families) -> synthesis plan -> 3-4 implementer lanes
  -- owner asked for review + research before implementation -- cost if wrong: ~1 h of agent time before code moves.
- Ruling: research agents read-only, write to this dir; no measurements in this phase -- nothing to bias.

## 2026-09-28 P1 landed; root A5 started
- P1 code-review-v7.md (395 lines) delivered. Headline: v6.1 ladder = 234 s compute inside a ~90 min cycle; SHA-256 is
  30/76 s of u and 19/31 s of w; one new field invalidates 35/39 cache entries; research exes are Debug; no daily decide
  path; risk model + QP + GP aim exist unused in atx-engine/risk. Lanes proposed: L1 IC cache keys + fast hash, L2
  research-cycle driver + field reuse + fit store, L3 daily decide path (holdings emit, decide verb, deploy manifest),
  L4 DSL ops. Root: A5 Release A/B, C3 canary, disk GC.
- Ruling: A5 = build atx-equity-strategy-ic/targets under preset equity-rel (tag v7-rel0), then A/B the v6.1 u pass and
  the final NAV cell for byte identity; adopt Release only if identical -- identity re-run of an existing book is not a
  new trial (same book, same window) -- cost if wrong: one wasted build + two 180 s runs.
