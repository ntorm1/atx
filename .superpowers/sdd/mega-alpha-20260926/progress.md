# Active task: recent-data DSL ensemble

## T31 re-review 1 — ADDRESSED — Task T31: complete
task-T31-rereview-1.md: DSR matches §4.E term by term; inverse normal 1e-12; fixture independent (recomputed, 7e-7); no new
breakage; 2 minors (fallback label with mixed-defined dirs; one bad dir stops output for earlier dirs) deferred to T41.
- Cherry-picked into pool-2: 81e9977d, 22c30c38, c8358931. Root pytest test_nav_summ + test_fit_composition_weights: 81 passed.
- Task T31: fix round 1/5 (1 addressed, 0 open; commits 90926148..e33405da). Task T31: complete (d4ec515d..e33405da in pool-4).
- Ruling: T38 steps 1-2 (aim fit; weighted IC pass with the unchanged IC binary 647c71a7) run BEFORE T37 — they need no v5 C++
  build and the prereg (56e5b148) is committed; the NAV grid (step 3) waits for the v5-1 build — cost if wrong: none (same
  inputs either way; receipts record order).


## T31 review — spec ❌ (1 Important) — fix round 1 dispatched
task-T31-review.md: 0C/1I/8m. Important 1: R6' DSR at N = 10 has no producer (nav_summ.py lacks it; T40 is controller-only).
Named risks checked: ew-theme-v1 path cannot change bytes; masking covers exactly the 3 SHA-derived fields; Memmel SE correct;
v5_train.sh binds TRAIN only. ⚠️ ratified: -L$LEV suffix; coverage-effective weight definition; extra provenance.aim keys.
⚠️ root checks at T38: per-name flags + --cadence 1 with aim-partial-v5; mean_gross_leverage CSV vs summary; EW-REFIT vs 9a9c949a.
- Ruling: DSR producer belongs in nav_summ.py multi-dir mode (V[SR_n] = variance of daily SR across the cells given; single-cell
  fallback Lo-2002 (1+SR²/2)/T; N via --dsr-n default 10) — cost if wrong: none (T40 consumes it).
- Task T31: fix round 1/5 (0 addressed, 1 open — DSR producer; resumed t31-aimfit).
- T31 fix 1 landed: pool-4 e33405da (nav_summ DSR for every dir, --dsr-n 10, V[SR_n] across dirs / Lo fallback; 81 tests pass:
  12 nav_summ incl. 6 new, 69 fitter). review-T31-fix1.diff packaged; t31-rereview-1 (Opus) dispatched.


## D2 byte-stability on real data (T30 binary, pre-T37) — PASS — 2026-09-27
Bounded run mega-nav-v5-baseline-check-run (exit 0, 22.4 s; exe b6b21d88 from v5-0): frozen v4.1 TRAIN cell (combined 24a6cc76,
role 210fff96, fields-v6 32565c32, baseline-v1 c1 f.25 band 2 price-risk-v1) -> recipe.json af058239 and
daily_modeled-1bn-stale5-v1+swap-fin-v1.csv 3f846525 BOTH identical to build-equity/mega-nav-v4-train-b2-f.25. Not a trial
(reproduction of an existing cell). T37 repeats the check with the v5-1 binary after T36.


## v5-0 pre-build (T30 only) — PASS — 2026-09-27
mega-v5-0-receipt.json: source 7d1c82c0, Jobs 4, 20.7 s, 5 TUs, 3 links, exit 0. atx-impl-strategy-target-tests.exe
--gtest_filter=TargetReplayV5.*:NavV5* -> 10/10 PASSED (hand-derived recipe SHA pins held). Full exe: 49/49 PASSED (6 suites).


## T30 review — Approved — Task T30: complete
task-T30-review.md: spec PASS, rulings R-a..R-f applied; 0C/0I/6m; desk-check found no compile errors. ⚠️ compile/SHA pins/D2 -> T37
(if a hand-derived SHA pin fails, confirm against the d4ec515d binary; never re-pin from post-change output); ⚠️ mean-gross
definition -> ruled above (exposure gross_leverage mean over all sessions).
- Minors: (1) v5 does not force cadence 1 (default 5) -> v5_train.sh must pass --cadence 1 (checked: it does); (2) release builds
  silently fall back to fixed theta on a wrong-size rate span -> T36 must validate the span (carried into T36 dispatch); (3) extras
  recipe key aim_partial, construction.v5.decisions, construction.v5 also in target-replay summary (v5-only; deferred); (4) "recipe
  minus rule name" not asserted directly; (5) NavReplayDay +16 B; (6) dust entry check only at L = 1. Deferred to T41.
- Cherry-picked into pool-2: 34d021dd, 98277c45. Task T30: complete (commits d4ec515d..98277c45 in pool-3, review clean).
- Dispatching T36 (pool-3, on 98277c45).
- Ruling: root pre-builds T30 alone as tag v5-0 (targets atx-equity-strategy-targets, atx-impl-strategy-target-tests) while T36/T31
  review run, to surface compile errors one lane-cycle earlier; T37's v5-1 build stays the qualifying build — cost if wrong: one
  extra target-scoped ccache-warm build (minutes).


## T31 ew-theme-aim-v1 — handed in, review dispatched — 2026-09-27
pool-4 e863cd65/90926148 (base d4ec515d): AIM_RULE_ID, standardized_ranks / rank_autocorrelation / aim_gain / ew_theme_aim_weights,
provenance.aim (theta, lags, rho, gain, gain_half, coverage_effective_theme_weight), work-record caching, --max-seconds (pre-existing,
exit 3 + partial:true), studies/v5_train.sh (fit/w/nav; COMP/COMBINED/THETA/DUST/RATE/LEV), nav_summ.py --weights/--reference
(netting ratio, paired dSR, Memmel SE, block-21 bootstrap 2000) + test_nav_summ.py. pytest 69/69 (55 + 14) and 6/6.
- Ruling (amends D3/§11 #6 and T38 step 1): the weights file embeds SCRIPT_SHA256 and SHA-derived context/admission digests, so
  literal SHA equality with 9a9c949a is impossible for ANY edited fitter; the byte-stability criterion is "ew-theme-v1 output identical
  to the pre-T31 fitter (blob fd644cba) after masking the SHA-derived fields" (fixture) and, on real data, v5_train.sh's EW-REFIT
  IDENTICAL check against 9a9c949a / admission 880a0a6a (weights, admitted set, provenance minus SHA fields) — cost if wrong: a byte
  drift hidden inside the masked fields (bounded: the fields are digests of the fitter text itself).
- Ruling: AR(1) closed-form fixture asserts exact equality on the full 0..126 lag grid and a 1e-3 bound on the AIM_LAGS-interpolated
  grid (brief's 1e-9 on interpolated lags was unattainable) — cost if wrong: none.
- Ruling: members with < 50 rankable names on every day get the floor gain .05 (letter of R4'); does not occur on real data — cost: none.
review-T31.diff packaged; t31-review (Opus) dispatched.


## T34b review — Approved — Task T34b: complete
task-T34b-review.md: spec PASS, 0C/0I/4m. Named risk checked: v5 verifies frozen v4 against pinned SHAs as v4.2 does; no safety
assertion dropped beyond ruled relaxation. ⚠️1 (--plan-only not run) -> resolved by root at T39. ⚠️2: profitability_quality family
description text changed (metadata; nothing reads it; runner reads id only).
- Ruling: "37 v4 rows byte-identical" covers candidate rows + fields, not the family description metadata; the description change
  stands (v4.2 precedent, disclosed) — cost if wrong: none (unread metadata).
- Minors deferred to T41: slot-limit attribution comment; recipe trial counts copied from v4 (1/1) vs "composition +2"; description
  change unrequested; test polish (tamper test library-only, magic number, overstated comment).
- Cherry-picked into pool-2: 6bfd9858, 6f71951e. Task T34b: complete (commits c8192c46..6f71951e in pool-8, review clean).


## T30 aim-partial-v5 — handed in, review dispatched — 2026-09-27
pool-3 34d021dd/98277c45 (base d4ec515d): TargetReplayRule::AimPartialV5, dust band, aim leverage, per_name_rate span seam, CLI
--rule aim-partial-v5 --dust-multiple --aim-leverage, recipe/summary v5 keys (v5 only), fixtures TargetReplayV5.* (6) + NavV5.* (4),
unbuilt. Four recipe SHA-256s hand-derived (t30-sha/ emulation reproduced 25 committed outputs). NavReplayDay gains
planned_held_names/decision_members (no CSV change). Root targets atx-impl-strategy-target-tests, atx-equity-strategy-targets;
filter TargetReplayV5.*:NavV5*; regression StrategyTargetReplay.*:StrategyNavReplay.*; no CMake change.
- Ruling: D1/R5'/R6' "mean gross leverage" = mean of the daily exposure gross_leverage column over ALL sessions of the run (same
  definition as the T28 audit and the §0 table; deployment ramp included, as it was for v4.1); construction.v5.mean_gross (planned
  gross over decision rows) is diagnostic only — cost if wrong: a ~2-3% ramp haircut on the reference cell's gross at theta .05.
review-T30.diff packaged; t30-review (Opus) dispatched. T36 waits for the verdict (same pool).


## T34b library v5.1 — handed in, review dispatched — 2026-09-27
pool-8 6bfd9858/6f71951e (base c8192c46): generate_fund_ic_v5.py + fund_industry_ic_v5.json sha 9e5ea08c... (38 = 37 v4 byte-identical
+ opex_at, 4 extras / 4 slots; lib max 7 slots / 5 extras) + recipe a26670b0... + test_generate_fund_ic_v5.py 10/10 (56 with v4/v4.2).
Concerns noted: opex_at may be redundancy-dropped vs gpa (|rho| <= .90); cache may miss all 38 (~86 s) if VM identity moved.
review-T34b.diff packaged; t34b-review (Opus) dispatched.


## T33a delisting feasibility — NO-GO — 2026-09-27
delisting-feasibility.md (DuckDB read-only 384 MB / 2 threads, no lock, no writes). TRAIN: 585 member lines end in 2020-22;
56 are line continuations (new securityID), 529 true terminations: mna 292 / performance 25 / unknown-with-CIK 9 / unknown-no-CIK
203 (rule R). Classifiable 59.9% of true terminations (69.0% of the 414 actually held and written off) < 80% gate; 97.2% of the
326 with a CIK. VAL counts only (35/152/29/13/94), no returns read. exchange_listings 45,820 rows, no venue -> eta -.35 would apply.
No OTC continuation source (tickerhistory = optionable listed only). No-CIK lines = ETFs/ETNs, SPACs, ADRs, preferreds, warrants.
- Ruling (R-6 applied): delisting lane PARKED — T33b and T32 are not built; eta = 0 in S1/S2 stays primary with S3 (K = 1 adverse)
  as the stress; D5 = "feasibility settled: NO-GO"; T39 step 1 does not run; owner gate U3 gains the concrete ask (bridge CIK
  coverage for the 203 no-CIK lines, or a sealed submissions export + ticker-continuity input) — cost if wrong: write-offs at
  last price remain a modelling caveat (T17 C5: material but symmetric, 5-6.5% of GMV/yr).
- Task T33a: complete (explorer). pool-5 stays untouched.


## T34a breadth check — GO (proxy) — 2026-09-27
breadth-check-v5.md: fields-v6 has no opex_ttm/xsga_ttm/cogs_ttm; has at, sale_ttm, oi_ttm, gp_ttm. Real opex item = new metric +
producer item + events-v3/fields-v7 rebuild + cold cache -> NO-GO for v5.1. Proxy opex = sale_ttm - oi_ttm: 4 extras / ~4 slots,
plan delta +512 B (1,449,071,914 B = 1,381.9 MiB, 154 MiB headroom), 37 hits / 1 miss expected. ind_lead_lag_w: documented, not built.
- Ruling: v5.1 adds opex_at with the proxy DSL decay_linear(group_rank((((sale_ttm - oi_ttm) / at) + (0 * log(at))), grp_ff12), 21)
  (prior +1, profitability_quality, tier B); deviation (includes D&A) pre-registered in v4-prereg.md '## v5.1 family' — cost if
  wrong: one weak candidate in a 38-candidate optional family; the T39 reference-cell re-run is the only read.
- Ruling: T34b relaxes the v4.2 generator's "additions are cross-section ranked" assertion for the within-industry theme
  profitability_quality (assert group_rank within grp_ff12 instead) — cost if wrong: none (v4 themes 1-3 already rank within FF12).
- Task T34a: complete (explorer). Dispatching T34b (pool-8 feat/mega-alpha-v5-lib51-20260927).


## v5 P1 dispatch (2026-09-27) — base d4ec515d
- Dispatched (Opus 5.5): t30-navrule (pool-3 feat/mega-alpha-v5-construction-20260927), t31-aimfit (pool-4
  feat/mega-alpha-v5-aimfit-20260927), t33a-delist (explorer, read-only, writes delisting-feasibility.md in pool-2),
  t34a-breadth (explorer, read-only, writes breadth-check-v5.md in pool-2). T36 waits on T30 review; T33b on T33a GO;
  T34b on T34a GO; T32 on T33b AND on T30+T36 cherry-picked into pool-2 (preflight ruling).
- Ruling (amends preflight R-a, on t30-navrule's objection): v5 keeps the uniform next = cur + theta*(L*desired - cur); with
  theta 1, L 1, dust 0 this IS baseline-v1's exact IEEE sequence (baseline computes cur + f*(desired-cur), not next = desired),
  so the ThetaOne fixture asserts bit_cast equality; no special case — cost if wrong: none (fixture proves it).


## T29 v5 pre-registration and rulings (2026-09-27) — prereg section '## v5 revision' R1'-R7' appended to v4-prereg.md
T28 evidence (construction-audit-v4.md, commit 44e6c28a): TRAIN b1f1 gross .813 net +.025 held 2,717 banded .954 tau .0347; b1f.25 .687/+.027/2,720/.931/.0267;
b2f1 .364/+.016/1,366/.994/.0205; b2f.25 (frozen v4.1) .256/+.041/1,369/.994/.0136; VAL b2f.25 .235/+.038/1,237/.994/.0133 (mean GMV $238m).
v3 VAL (trial #1, b1 f1) was a 76%-gross book (gross .763, net +.001). Entry rate at b2 bounded 0.15-1.1%/day of unheld names.
- Ruling: the T28 re-read of the two existing VAL NAV CSVs (book-level leverage diagnostics, nothing selected) is a disclosure re-read, not validation trial #3 — cost if wrong: one extra disclosed VAL read.
- Ruling: banded share denominator = members (neutralize_used + neutralize_excluded), not neutralize_used (ratio > 1 at b2 otherwise) — cost if wrong: none (shares differ < .01).
- Rulings R-1..R-7 (plan §9, verbatim, declared before any v5 TRAIN read):
  - **Ruling R-1 (disclosure):** validation trial #2 (+0.641) was produced by a book with mean gross leverage 0.24 and net +0.04; it is recorded as-is
    but is not a $1bn-deployment result. — The band `band_multiple/N_d` ≥ the position scale blocked entry. — cost if wrong: none (disclosure only).
  - **Ruling R-2 (construction):** v5 replaces band+fraction with GP partial adjustment toward a gross-1 aim (θ) plus a dust band ≤ 0.1/N_d;
    the `band_multiple` path stays for reproduction only and is refused under v5. — Handoff lever 1 grid {band 1,2} would re-test a frozen book. —
    cost if wrong: one wasted C++ lane (~1 day).
  - **Ruling R-3 (composition):** aim gain g_k = θ Σ_j (1−θ)^j ρ̄_k(j) on measured TRAIN rank-autocorrelation (lags 0..21, then 28..126 step 7,
    linear interpolation), global normalization, θ = 0.05 fixed (= reference construction θ). The τ-mapped form is **not** run (saves a trial family). —
    cost if wrong: aim weights mis-scaled for jump signals; bounded by g ∈ (0, 1].
  - **Ruling R-4 (gate):** TRAIN acceptance is paired ΔSR(net) vs the reference cell (C_ew, θ .05, dust .1) with Memmel SE and a DSR at N = 10,
    plus mechanics (gross ∈ [0.9, 1.05], |net| ≤ 0.02, τ limits). The owner's absolute "TRAIN net ≥ 1.0" rule governs only whether a freeze is proposed. —
    cost if wrong: a freeze proposed on a lucky cell; the owner still rules.
  - **Ruling R-5 (breadth):** no 10th theme in v5; v5.1 may add `opex_at` to `profitability_quality` only; industry momentum 1m, BAC, CHS are not built. —
    cost if wrong: forgone breadth, revisitable under U2.
  - **Ruling R-6 (delisting):** η by kind (M&A 0; performance/unknown −0.30/−0.55/−0.35) replaces η = 0 in S1/S2 **only after** T33a shows ≥ 80% of TRAIN
    member terminations are classifiable; S3 keeps K = 1 adverse. — cost if wrong: a biased write-off haircut; the stress run bounds it.
  - **Ruling R-7 (trial budget):** v5 TRAIN = 1 new composition + 10 construction cells (+1 delisting re-run, +v5.1 family if built). Anything else is a new
    disclosed revision in `v4-prereg.md` before it runs.
- Task T28: complete (explorer, read-only; no review needed — acceptance = numbers within ±0.01 of §0: met, max gap .005).
- prereg commit SHA: 56e5b148 (docs(mega-alpha): v5 pre-registration and rulings (T29); prereg file + plan + briefs committed before any v5 TRAIN read). Task T29: complete.


## v5 sprint start (2026-09-27) — controller: Claude Fable 5.1 (SDD); plan docs/plans/2026-09-27-mega-alpha-v5-dag-plan.md (= spec)
Base: pool-2 HEAD 41fb5e39 (branch feat/aes-codex-integration-20260925). No process running. Pools: 2 root; 3 (T30->T36),
4 (T31), 5 (T33b->T32), 8 (T34b), 9 (T35 if needed) reset via `git checkout -B <branch> <pool-2 HEAD>` (leases alive under
older run ids, reused as the plan directs); 10/11 free; 1 and 6 never touched. Briefs: task-TN-brief.md (plan task text
verbatim), plan-s3-constraints.md (§3 verbatim), plan-s4-research.md (§4), plan-s11-review-focus.md (§11).
- Ruling: this ledger (newest-first, plan §5.6) is the SDD ledger; the skill's default `.superpowers/sdd/<plan>/progress.md`
  layout is not used — the plan mandates this directory — cost if wrong: none (single ledger, git-tracked with -f).
- Ruling: T28 explorer writes its files in pool-2 but does NOT commit; root commits T28+T29 together (root-only commits in
  pool-2 keep the clean-tree guard predictable) — cost if wrong: none.
- Preflight conflict scan (plan text vs plan text; spec = plan):
  | pair / task | produces vs consumes | finding | ruling |
  | T30 -> T36 (NAVRULE+NAVSIM, pool-3 sequential) | update_weights(..., per_name_rate span) | consistent | — |
  | T30/T36 vs T32 (NAVSIM, pool-3 vs pool-5) | both edit strategy_nav_replay.cpp | cherry-pick conflict risk | Ruling: T32 branches from pool-2 HEAD only AFTER T30+T36 are cherry-picked into pool-2; if T33b finishes earlier, T32 waits — cost if wrong: T32 delay, never a hand-resolved conflict. |
  | T30 CLI <-> T31 v5_train.sh | --rule aim-partial-v5 --trade-fraction --dust-multiple --aim-leverage; --rate per-name-v1 --rate-rra --rate-min --rate-max | consistent | — |
  | T31 nav dir names <-> T38 loop | mega-nav-v5-$C-t$THETA-d$DUST-$RATE == mega-nav-v5-$C-$1-$2-$3 | consistent | — |
  | T30 fixture ThetaOne_MatchesBaseline (EXPECT_DOUBLE_EQ, §11 #1 "bit-for-bit") | next = cur + 1*(aim-cur) is not bit-equal to aim in fp | plan defect | Ruling: when theta_i == 1 the rule writes next = aim directly (identical math, exact bits) — cost if wrong: none. |
  | T30 fixture DustDoesNotBlockEntry `EXPECT_NEAR(gross, 0.05, 1e-12)` | ~5% of members (|desired| <= .1/N) are dusted on step 1, so gross < theta*1 by ~1e-4; desired gross may also != 1 after neutralization | plan defect (tolerance) | Ruling: assert gross within [0.9*theta, theta] of the un-dusted theta*gross(desired) instead; held_names >= 90% stays the load-bearing check — cost if wrong: a looser fixture. |
  | T36 fixture fields (rate_stats.min, share_at_min_count) vs T36 Interfaces (mean,p05,p50,p95,share_at_min,share_at_max) | field names disagree | plan defect (naming) | Ruling: rate_stats = {n, mean, min, max, p05, p50, p95, at_min_count, at_max_count, share_at_min, share_at_max}; summary JSON emits all — cost if wrong: none. |
  | T31 step 1 gating | require(prior == (composition in PRIOR_COMPOSITIONS)) | consistent with FIT:1170 | — |
  | T31 fixture test_ew_theme_v1_bytes_unchanged | reuses run_fitter/v4_fixture/BYTE_STABILITY_V1_SHA256 | exist per plan; implementer verifies names | — |
  | T29 R5' <-> T30 | cadence 1, non-members forced 0, dust/N_d, L | consistent | — |
  | T38 reference cell | ew t.05 d.1 fixed | == R5' REFERENCE | — |
  | T37 tag v5-1 | mega-build refuses reused tags | verify unused before build | — |
  | D1 (>= 90% members non-zero) vs dust .1/N | ~5% of members stay at 0 from dust; entry otherwise unblocked | consistent | — |
  Scan otherwise clean. Rulings R-1..R-7 (plan §9) are recorded verbatim in the T29 section below once T28 lands.


## STAT-ARB CLUSTER STUDY (owner ask, separate research family; prereg statarb-prereg.md @367e1101) — RESULT: NEGATIVE
TRAIN 2020-22 only (validation sealed). studies/statarb_cluster_study.py + statarb_run.sh (bounded; all stages <= 48 s,
<= 1266 MiB) + statarb_report.py; outputs build-equity/statarb-cluster-v1/ (analyze.json, model.json,
statarb-cluster-study.png). Clusters: rolling 252d PCA-15 embedding + balanced k-means, monthly; c30 = 96 groups x ~30,
within-group resid corr .38 (c10 .42, c100 .33, FF49 .18, random .01), FF12 purity .57, month-to-month ARI .34.
Univariate (138 tests, Bonferroni |t| 3.57): 0 significant. Strongest cluster signal c30_dev_1 vs r[d+1] IC -.0096
t -2.9, gone at r[d+2] (+.0015): the residual reversal lives in the one session our clock cannot trade. FM beyond
controls: dev/sscore/grp_dev |t| < 2 at K 10/30/100; marginal c30_grp_mom_12_1 +2.2/+2.7, c30_beta_g +2.4,
c30_nbr_mkt_5 +2.6 (y5); controls dominate (vol63 -6, mom +3). Model FIT 2020-21 -> HOLD 2022: hgb_FULL IC y2 +.010
(t 1.4), hgb_CTRL+FF49 +.008 (t 1.1), paired FULL-minus-no-cluster +.0025 (t .47); H1-H4 all fail. Best HOLD book
(hgb ctrl, hl10) gross SR 1.10, BE 33 bps — one year, not significant; cluster features add nothing OOS.
Not run (ideas): same-close / intraday execution, liquid top-1000 subset, Avellaneda-Lee threshold entry/exit rule.

## PAUSED 2026-09-27 at owner request — handoff 3
docs/plans/2026-09-27-mega-alpha-parent-handoff-3.md (done / in progress / next steps / current net SR and alphas /
fresh-parent goal prompt). No active agents or processes. Best OOS: validation #2 v4.1 net +0.641.


## VALIDATION RUN #2 — FREEZE v4.1-daily-2026-09-27 — RESULT: net +0.641 (objective >= 1 NOT met; no OOS decay)
Script studies/v4_validation_once.sh @214b383a. Runner mega-v4-VAL-1: 47 s / 696 MiB, 37 cold; combined c50829be...
NAV mega-nav-v4-VAL-b2-f.25 (14 s / 243 MiB), 2023-2024, $1bn, daily c1, f.25, band 2, neut price-risk-v1:
  PRIMARY S2 x swap-fin-v1: NET SR +0.641, gross +0.802, HAC t .95, mu 1.56%/yr, vol 2.43%, MDD 2.3%,
    years 2023 +1.7% / 2024 +1.4%; tau_gmv mean .0134 / p95 .0272 (limits MET); tc .53% (2y).
  S1 x swap-fin .706; S3 .072; S2 x flat-300 .583; S2 x engine-tiers .607. Netting ratio (TRAIN b2f.25) .0136/.0519=.26.
TRAIN in-sample same config: net .687 / gross .815 -> OOS ~= TRAIN (v3: 1.81 -> -1.27). Prior-signed, mean-free
construction generalises; the shortfall is alpha strength (gross ~.8) vs the $1bn S2 cost hurdle.

## FREEZE v4.1-daily-2026-09-27 — VALIDATION TRIAL #2 (declared before any validation run of this configuration)
- Ruling (gate override): the TRAIN gate (>=1.0) was a root resource rule to conserve validation, not a validity
  condition; v4.1 selection used no 2023-24 information, so its validation result is an honest OOS read. The owner
  objective names "fresh TRAIN freeze of fundamentals/industry via CIK -> validation trial #2" as the next step.
  Cost if wrong: validation spent on a config with TRAIN net .687 (expected VAL < 1); later trials carry the disclosure.
- library fund_industry_ic_v4 daa9663e (37); TRAIN role v2 210fff96; TRAIN fields-v6 32565c32; VAL role v1 0c757c41;
  VAL fields-v6 c034ecf3 (same producer blob, events-v2 74ed9a50, bridge r4-v1 ddf97164); orientations
  mega-v4-train-u-1 11cfd3e4; admission v4-prior-v1 880a0a6a; weights ew-theme-v1 9a9c949a (31 admitted, pinned +1).
- construction: nav baseline-v1 cadence 1 trade-fraction .25 band 2/N neutralize price-risk-v1, limits .20/.30,
  --max-bytes 1 GiB; runner --min-names 1000. Primary S2 modeled-1bn-stale5-v1 x swap-fin-v1; stresses as v3.
- binaries: atx-equity-strategy-ic.exe 647c71a7 (source 4ce2ec4e+); atx-equity-strategy-targets.exe 4642dd37.
- TRAIN (in-sample for construction only): S2 x swap-fin net .687, gross .815, tau .0136/.0401.
- Trial disclosure since v3 run #1 (TRAIN only): libraries v4 (37) + v4.2 (40); compositions 2 (v4, v4.2);
  construction 1 + 5 (v4.1 grid) + 2 (v4.2); construction study 5 paper books (T26); post-mortem analyses (T16).
  Validation: run #1 (v3) seen at book level only; this is run #2.
- Script studies/v4_validation_once.sh (single run).

## POST-MORTEM v3 (owner goal 2026-09-27 pm: verify math, diagnose, continue to objective)
- pool-2 integration ff'd to local main d63a7058 (merge of e587684b + main 9d8925ea); lanes branch from it.
- MATH (root, book-level): NAV headline stats reproduce from daily CSVs (VAL net -1.271 / gross +.161; TRAIN
  1.807/2.741); chain pre[t+1]/pre[t]-1 == net[t+1] max err 1.1e-16; net = gross - tc - borrow - long_fin
  (resid 1.8e-5 = long financing). Costs stable TRAIN tc 1.97%/yr vs VAL 1.67%/yr; gross 6.99%/yr -> .23%/yr.
- SELECTION PERSISTENCE (TRAIN-only, admission.csv): oriented FIT(2020-21) SR vs HOLD(2022) SR across 121
  candidates Spearman .040 (p .66), Pearson .077; HOLD sign kept 55.4% (null 50%). Oriented FIT SR mean .61
  -> HOLD .14. Screen admitted 23 = mostly coin-flip survivors; MV weights then fit on 2020-22 incl. HOLD.
- REGIME (book-level): TRAIN in-sample gross SR by half 2020H1 6.38 / H2 3.85 / 2021H1 .42 / H2 1.12 /
  2022H1 2.18 / H2 4.67; net 2021 negative both halves IN-SAMPLE. VAL halves gross -.07/-.39/+1.88/-.64.
  Book = crisis/bear-regime composite (2020, 2022); bull years (2021, 2023-24) fail.
- HURDLE: book vol ~1.5-2.5%/yr at GMV/NAV .77; cost+fin ~2.2%/yr -> break-even gross SR ~1.3-1.5; each
  1%/day turnover costs ~.24 SR (c~18 bps/unit, vol/GMV ~1.9%). Net SR 1 needs gross >= ~1 + .24*tau%.
- Ruling: validation 2023-24 analysed BOOK-LEVEL ONLY (no per-candidate VAL stats) so trial #2 selection
  stays uncontaminated by per-alpha VAL information — cost if wrong: coarser attribution.
- Dispatched (Opus): t16-postmortem (pool-3 feat/mega-alpha-postmortem-20260927: walk-forward, null sim,
  construction alternatives, paper-book check; root runs), t17-pit-audit (read-only PIT/leak audit),
  t18-fundamentals (read-only fundamentals/industry/CIK inventory + v4 design). pool-8 branch
  feat/mega-alpha-v4-fields-20260927 reserved for v4 fields.

- T15 re-review (t15-review-2): spec PASS, Approved 0C/0I/8m (M1 FP-flags identity read from runner TU not
  ic_screen.cpp; M2 bump comment; M6 no cross-worker-count IC-entry test). 8th artifact = summary.json (timings,
  expected diff); ids/sessions also SAME. Task T15: complete (63b34d72/74a03a82). Minors deferred.
- T16: pool-3 d18fd3ca -> root b032504b (studies/postmortem_v3.py + 9 synthetic tests). Root ran bounded
  postmortem-v3-1 (27 s/96 MiB; E train 4 s/381 MiB; E val 11 s/315 MiB). RESULTS:
  A reproduce bitwise (weights |dw| 0, admitted set equal). In-sample F SR 3.07; P&L share 2020 55% / 2021 11% / 2022 34%.
  D NULL (demeaned f_k, block-21 bootstrap, frozen protocol, 200 reps): in-sample SR null median 2.24, p95 2.97,
    observed 3.07 = 95.5th pct; admitted 23 (null 22); Spearman .04 (null p05..p95 -.35..+.34). => ~73% of the
    in-sample SR is manufactured by selection on noise.
  B WALK-FORWARD (TRAIN only) OOS SR: fit20-21->22 +.81; fit20->21 -.43; fit21->22 +1.09; fit21-22->20 -1.48;
    with HOLD test +1.08 / -1.70. Mean ~0 (factor units, gross, before ~1.3 SR cost hurdle).
  C alternatives (B1 OOS / fit2020->2021-22 OOS): EW-all FIT signs .52/.22; EW admitted .97/-.13; inv-vol .71/-.17;
    MV frozen .81/.00; family-EW .44/.12. No rule robust; library declares no prior signs (all TRAIN-estimated).
  E MATH: paper neutral book TRAIN SR 3.00 corr NAV gross .94; VAL paper .31-.38 corr NAV gross .93-.95 (NAV .16
    after caps); lag1/2/3 flat (no microstructure/leak dependence); VAL used pinned signs+weights exactly (121/121).
- T17 (read-only PIT audit): 29 inputs OK 22 / LEAK 0 / RISK 5 / UNKNOWN 2. Risks: SI pre-2021-06 settlements are
  FINRA re-publication (52% TRAIN decisions; VAL none); survivorship of vendor dead lines unknown; no delisting
  returns (write-off at last price); SI as-of producer not hash-pinned; IV earnings-adjustment vintage unknown.
  12/23 weighted alphas (60% weight) trade opposite their own TRAIN IC21 sign. TRAIN role repaired with
  factor-break-v1 (not v2). Checks C1-C8 being packaged as studies/t17_checks.py (pool-9).
- DIAGNOSIS (root): math correct; failure = (1) data-mined signs+MV weights on 3y with no persistence (null explains
  most of IS SR), (2) regime concentration (2020/2022 crisis years), (3) cost hurdle ~1.3 gross SR at tau 3.7%/day
  and vol/GMV ~1.9%, (4) no pre-validation honest gate (walk-forward predicted ~0). Not a leak.
- T18 (read-only fundamentals inventory): 6 READY / 8 PLANNED; 0 fundamentals fields on role axis. READY: CF-R
  CompanyFacts staging (53.75M facts, all occurrences), FSDS v2 SUB accepted_utc (99.9%) + SIC per filing; identity
  r4 rehearsal PIT links ~80% common (~1,750-1,800 names/day); ~1,550-1,650 fundamentals names/day expected.
  Plan T19 identity snapshot (S), T20 fundamental events producer (L), T21 fields-v5 (M-L), T22 grp_ group fields
  (S, C++), T23 prior orientation (S-M), T24 library v4 (M), T25 audit (M).

- v4 PRE-REGISTERED (v4-prereg.md @90ce38e0, before any v4 TRAIN read): prior-signed themed library (9 themes),
  within-FF12 ranking for accounting ratios, admission v4-prior-v1 (veto HAC t<-2, |rho|<=.90 by tier), composition
  ew-theme-v1, fixed construction band 1, GATE TRAIN S2xswap-fin net >= 1.0 before validation trial #2.
  Rulings: fundamentals +1 lag session after accepted_utc (conservative; cost: ~1 day staleness); identity = pinned
  r4 rehearsal links (PIT, ~80% common; cost: lower coverage than static bridge); within-industry accounting ranks
  (cost: lose between-industry value premium); TRAIN window unchanged 2020-22 (owner rule; 3y is the root
  limitation — recommend owner consider pre-2020 history for selection).
- Dispatched (Opus): t19-t23 (pool-4 v4-identity: T19 then T23), t20-fundevents (pool-8), t21-fields5 (pool-3),
  t22-grp (pool-5), t24-libv4 (pool-7); t17 packaging checks C1-C8 (pool-9 t17-checks). All branches @90ce38e0.

- T17 checks: pool-9 beb83ce5 -> root (studies/t17_checks.py). Root ran mega-t17-checks (3 bounded runs, <=12 s,
  <=344 MiB): C1 SI revisions CLEARED; C2 SI vintage (TRAIN) CLEARED (si_change_10 1.26 -> 1.05 republished ->
  vintage-safe; SI drop -.04 vs controls .59); C3 FINRA as-of alignment CLEARED (200/200); C4 survivorship CLEARED
  (vanish TRAIN 4.07% vs VAL 3.48%/yr); C5 write-off exposure CONFIRMED material but symmetric (TRAIN 5.2-6.5%,
  VAL 4.8-6.0% of mean GMV; no delisting returns = modelling caveat for v4 too); C6 union-market proxy
  INCONCLUSIVE (corr .99, +2.2 vs +1.1 bp/day); C7 SI clock CLEARED (6-8 bd); C8 IV/earn timing INCONCLUSIVE but
  symmetric (incidence ratio 1.00). => no TRAIN-specific leak; decay = selection + regime + cost (T16).
- Untracked studies/.mypy_cache (IDE churn in every pool).

- T22: pool-5 8b13d189/90f6b87f -> root 76479562/4ce2ec4e (grp_* group classifiers; tripwire re-pinned 18693b18;
  VM semantics version NOT bumped — claim: dtype only gates compilation). Build mega-t22-a 268.7 s Jobs4 (122 TUs,
  engine header); alpha 87/87, IC 67/67. Review dispatched (t22-review).
- T24: pool-7 3bb3fbf3 (library v4 generator, 40 candidates, 9 themes, within-FF12 group_rank for themes 1-3,
  tier_rank int for T23). Rulings (before any v4 TRAIN read): iv_change DROPPED (blended ATM IV mixes call(+)/put(-)
  effects of An-Ang-Bali-Cakici 2014: no unambiguous prior; cost: one diversifier) -> 39 candidates; droe canonical
  with be_lag1q_lag4 (T21 told to emit it). T24 fix round 1 sent.

- T24 fix r1: pool-7 3bb3fbf3/d7464792 -> root (39 candidates; themes == prereg names; all prior_sign 1; tiers
  A 4 / A- 4 / B+ 9 / B 9 / B- 7 / C+ 6). pytest 32/32; --check registry + grp typing OK. Review dispatched (t24-review).
- T19/T23: pool-4 83fbbf84/2d185266 -> root. pytest 66/66. Fitter SHA fbfd122b -> 69c18270 (work caches recompute;
  v3 pins reproducible from d38e7929). REAL: identity-bridge-r4-v1 (2 s/142 MiB; source r4 manifest ac9bcda7..., 6,780
  rows P 6,765/J 15); check (4 s/777 MiB): member-cell P-link coverage TRAIN ~.60, VAL .59 (2023 .596 / 2024 .588);
  static-bridge agreement TRAIN 2108 agree/52 disagree, VAL 1979/20. Ruling: T21 matches start<=d<=end_incl only
  (rows already PIT; T18 "available_at < d 22:00" would drop first days) — cost if wrong: none material (6,770/6,780
  rows available_at == start mark). Review dispatched (t19-t23-review).

- T22 review: spec PASS, Approved 0C/0I/3m (tolerance 1e-12 vs exact; duplicated panel helper; NaN-label tests
  cover 4 of 8 ops — reviewer verified all 8 by reading). VM bump not needed (confirmed). Root confirmed no existing
  manifest/library has a grp_* field (only library v4). Task T22: complete (76479562/4ce2ec4e).

- T20: pool-8 77771dd2/9de89dd0/dd8877ae -> root. pytest 17/17. REAL fundamental-events-v1 (prepare 8 s/484 MiB;
  events 4 chunks 17-24 s/<=646 MiB; finalize 2 s): 172,777 rows, manifest 519ecc1a..., SIC 235,945 rows / 6,543 CIKs,
  sub accepted null 0. SUE from first-reported quarterly NI (EPS split-contaminated); debt single-concept (understates).
- T21: pool-3 e9392d80/e31db6c6/d04c5dc7/ab3e7364 -> root. pytest 36/36. REAL fields-v5 (bridge ddf97164.., events
  519ecc1a.., --fund-lag-sessions 1): TRAIN recent-fast-train-2020-2022-v2-fields-v5 manifest 4e02b7db... (37 s/637
  MiB), VAL ...-v1-fields-v5 (33 s/564 MiB). 40 fields; legacy 8 byte-identical to v4 both roles;
  rows_used_available_after_start_mark 0 both. Member coverage TRAIN/VAL: be .589/.574, cfo_ttm .568/.568, sue
  .568/.558, me_company .604/.593, grp_ff12 .604/.591, gp_ttm .365/.355, fscore .315/.319, xrd .221/.212 (symmetric).
- T19/T23 review: both spec PASS, Approved 0C/0I/6m each. T19 caveat: r4 marks link END before knowable (stops links
  early, never mislinks). T23: real fit must pass LF library sha af5159c8. Tasks T19, T23: complete.
- T24 review: spec FAIL 1 (I1 issuance_vendor counts splits as issuance: use shares_out*raw_close/close), 6 minors
  (ear 63-session window == mean announcement gap -> fix). 
- v4 plan-only (TRAIN, fields-v5, min-names 1000): 1.76 GB @4w / 1.73 @2w / 1.70 @1w > 1.5 GB cap; driver mgmt_sy
  (8 extras) + qmj_lite (7). Without them 1.449 GB @4w (fits). Ruling (before any v4 TRAIN read): DROP mgmt_sy and
  qmj_lite — components already members of their themes (theme-EW already combines them); cost: lose rank-sum
  composite forms. Ruling: --min-names 1000 for v4 (fundamentals cover ~1,650 names/day; gates IC-date stats only).
  T24 fix round 2 sent (I1, ear window, drop composites, issuance_xbrl split check).

- T24 fix r2: pool-7 604dc555 -> root (37 candidates; issuance_vendor split-safe Daniel-Titman adj; ear = latest
  announcement 3-day CAR held <=126 sessions + s21; mgmt_sy/qmj_lite dropped; issuance_xbrl uncorrected (T20 lag-4
  from latest filing reporting the period => split-consistent; T25 to verify AAPL 2020 / NVDA 2021)). pytest 36/36;
  plan-only 1.449 GB. Library sha daa9663e. Scoped re-review dispatched (t24-rereview). v4_train.sh committed 656bbabf.
- T20/T21 review: T20 spec PASS, Needs fixes 0C/1I/6m — I1 revenue concept priority prefers ASC 606 line over
  `Revenues` total (sale_ttm/gp_ttm understated for mixed-revenue filers); no look-ahead path. T21 spec PASS,
  Approved 0C/0I/3m (Task T21: complete pending T20 re-run). gp_ttm .365 structural (financials / no COGS line).
  T20 fix round 1 -> pool-5 feat/mega-alpha-v4-fundevents-fix1-20260927 (pool-8 now T25). v4 TRAIN read HELD until
  T20 fix + events/fields-v5 re-run + T24 re-review.

- T24 re-review 1: spec PASS, Ready, all findings ADDRESSED; 3 new minors (issuance_vendor break-neutral only if
  role factor-break steps match producer -> T25 check; seasonality text; ear keeps prior CAR if new CAR NaN).
  Task T24: complete (3bb3fbf3/d7464792/604dc555; library daa9663e, 37 candidates). Checks relayed to T25.

- T20 fix r1: pool-5 6fa90efa -> root (Revenues total wins over ASC 606 line; same total-first for cash/st_debt;
  m2/m5 fixed, m1/m3 counters only). pytest events+fields 58/58. REAL fundamental-events-v2 (fresh --out; code-hash
  lock): 172,777 rows, manifest 74ed9a50...; fields-v6 TRAIN 32565c32.. (42 s/637 MiB) / VAL c034ecf3.. (30 s/564 MiB),
  coverage unchanged vs v5. v4_train.sh FD -> fields-v6. Scoped re-review dispatched (t20-rereview).

- T20 re-review 1: I1, m2-m5 ADDRESSED; m1 (concept rank beats recency; 42,175 keys counted) open by design, caveat;
  new minors N1-N3. Task T20: complete (77771dd2/9de89dd0/dd8877ae + fix 6fa90efa). Task T21: complete.
- v4 unweighted TRAIN pass mega-v4-train-u-1: 86 s / 1012 MiB, 37 cold misses, exit 0 (IC numbers not read before
  T20 re-review). Proceeding: fit -> weighted -> NAV (TRAIN; the v4 TRAIN read). T25 audit still in flight — any
  data fix it forces is correctness-driven and will be disclosed.

- v4 TRAIN (prereg config; fit 40 s/506 MiB: 31 admitted / 6 redundant / 0 veto; weights ew-theme-v1; weighted pass
  34 s; combined 24a6cc76...; NAV mega-nav-v4-train-b1 32 s/338 MiB): S2 x swap-fin NET SR +0.431, gross +1.100,
  HAC .79, mu 1.45%/yr, vol 3.37%, MDD 2.8%, years 2020 +0.3% / 2021 +2.4% / 2022 +1.6%; tau_gmv .0348/.0473 (limits
  met); tc 5.37% (3y) vs S1 1.64%; S1 net .754; S3 -.513; flat-300 .127; engine-tiers .336. Sum w tau .0519 -> NR .67.
  GATE (>= 1.0) FAILS -> validation NOT run. Redundancy per rule: bm/ep/ebit_ev/sp -> cfp (rho .91-.94); issuance_xbrl/
  vendor -> net_payout (.91/.96). Fast themes reversal_seasonality + options_implied = 22% weight, 49% of sum w tau.
  Diagnosis: honest gross ~1.1 (balanced across years) but S2 impact at $1bn (1.8%/yr, ~26 bps/unit) eats ~.6 SR.
- PRE-REGISTERED REVISION FAMILY v4.1 (construction only, same combined 24a6cc76, declared before running): NAV grid
  trade-fraction {1,.5,.25} x band {1,2}/N (6 construction trials; b1f1 = the v4 run above). Pick max TRAIN S2 x
  swap-fin net SR within daily limits; gate unchanged (>= 1.0). Disclosed as a revision after a failed gate.

- v4.1 grid (TRAIN, S2 x swap-fin net / gross / tau mean): b1f1 .431/1.100/.035; b1f.5 .589/1.135/.029; b1f.25
  .677/1.132/.027; b2f1 .643/.929/.021; b2f.5 .596/.779/.015; b2f.25 .687/.815/.014. Selected b2f.25 (.687) — GATE
  FAILS. At low turnover S1 .79 vs S2 .69: cost no longer binding; slower tracking cuts gross (fast themes decay).
- T25: pool-8 cddcf8f2 -> root (audit_fund_fields.py, 6 tests). On fields-v6/events-v2: C1 PASS (32 fields re-join
  exact; common-member coverage be .771 at .775 cfo .759 ni .759 sale .714 shrs_q .697), C2 PASS 300/300 cells vs raw
  CF + FSDS accepted_utc, C3 PASS (FC1 .03%, revised-vintage <.25%), C7 PASS; (on v5 inputs) C4 adj & shrs_q split-safe
  PASS / raw shares_out FLAG (unused), C5 PASS, C6 FLAG FF49 small groups (17% of groups <5, 1.2% members). Task T25:
  complete. Data path v4 accepted.

- T26: pool-3 a5a46fda -> root (studies/v4_construct_study.py, 15 tests). Root run mega-v4-construct-study-1 (24 s/552
  MiB), TRAIN paper books gross SR: P0 price-risk .852 (corr NAV .91) / P1 +FF12 .798 / P2 +FF49 .746 / P3 liquidity-
  scaled .703 / P4 partial .25 .862 (tau x.75). Industry-neutral and liquidity scaling do NOT help. Task T26: complete.
- v4.2 PRE-REGISTERED (v4-prereg.md "v4.2 revision"): +res_mom_12_1, +eap, +low_share_turnover (prior-signed); tau_k <=
  0.08 cost-consistency screen; construction b2f.25 (+b1f.25 reported); gate 1.0; if it fails -> report to owner.

- T27: pool-7 c9ea35b0/1dde8135 -> root (library v4.2 fund_industry_ic_v42.json 22af107c, 40 cands; fitter screen
  v4-prior-v2 tau<=.08). pytest 101/101. Review: spec PASS, Approved 0C/0I/4m; eap causal (past reaction markers
  only); eap s1 smoothing accepted (event timing); DISCLOSED: res_mom_12_1 DSL == v2/v3 resid_sharpe_12_1_s21 (its v3
  family-level TRAIN stats were seen by root). v4.2 u pass 30 s/964 MiB (37 hits, 3 misses). Task T27: complete.

- v4.2 TRAIN (fit 30 s: 27 admitted / 7 redundant / 6 reject_turnover_cost [low_max .089, si_change .091,
  ind_adj_rev_5 .181, seasonality .094, iv_rv_spread .090, eap .200]; mom_12_1 redundant; combined a5cf1c49...):
  S2 x swap-fin NET b2f.25 +.449 (gross .598, vol 2.1%, tau .010) / b1f.25 +.164 (gross .496). S1 b2f.25 .631.
  GATE FAILS. Cost screen removed fast members that carried gross alpha. STOP per prereg; report to owner.
- STATUS: best honest TRAIN config = v4.1 (v4 library, b2 f.25) net .687. Validation trial #2 NOT spent (2023-24
  touched only by v3 run #1). Recommendations to owner: (1) pre-2020 history for selection/estimation (3y TRAIN is
  the binding limit); (2) data with real OOS edge not yet available (analyst estimates/revisions, options skew,
  8-K earnings dates); (3) the $1bn S2 impact + swap financing hurdle (~.3-.6 SR) — consider target at smaller NAV or
  S1-like execution; (4) turnover-aware per-theme trading speeds (Garleanu-Pedersen aim portfolio) as a construction
  project. Integration branch ahead of local main (v4 work since d63a7058) — not merged (owner action).

## VALIDATION RUN #1 — FREEZE v3-daily-2026-09-27 — RESULT: FAIL (objective NOT met)

Script studies/v3_validation_once.sh @ee843d10. Validation-only runner (run_mode validation-only-frozen-TRAIN)
mega-v3-VAL-1: 172 s / 673 MiB, 121 cold; combined ba69712156e20a2b04de58fc6958f86e7fa8a851ed844b566e2530aa2edf25af.
NAV mega-nav-v3-VAL (27 s / 243 MiB), 2023-2024, $1bn, daily c1, neut price-risk-v1, band 1/N:
  PRIMARY S2 x swap-fin-v1: NET SR -1.271, gross +0.161, HAC t -1.91, mu -1.84%/yr, vol 1.45%, MDD 4.5%,
    years 2023 -2.4% / 2024 -1.3%; tau_gmv mean .0494 / p95 .0710 (limits .20/.30 MET), max .139;
    capacity: 48,858 of 82,511 fills capped at 1% ADV (59%), unfilled $10.3bn cumulative;
    financing $: long 3.04M, short 4.95M (gc 3.17 / warm 1.40 / special .38); short$ share gc .837 warm .154
    special .010; blocked short name-decisions 12,350; missing-predictor member decisions 10,791.
  S1 x swap-fin: net -0.653 (gross +.074); S3 x swap-fin: net -2.391; S2 x flat-300-v0: net -2.149;
  S2 x engine-tiers-v1: net -1.406.
  Netting ratio (TRAIN, band 0): 1.49.
TRAIN in-sample for the same config: net 1.807 / gross 2.741 -> gross collapses OOS (2.74 -> 0.16).
Interpretation: the TRAIN selection (screen + MV weights + band on the same 2020-22 data) did not generalize;
validation is now spent for this configuration. Per objective item 6, next lever = alpha quality (fundamentals/
industry via CIK, new library version) with a fresh TRAIN freeze; any new frozen configuration = validation
trial #2, disclosed. Per-alpha validation diagnostics must NOT inform the next selection.
- T15 (post-validation import): pool-9 4553de7c/15f79758 -> root 63b34d72/74a03a82. Build 32.9s; IC tests 66/66;
  bit-identity vs mega-v1-train-r2 (7 artifacts) SAME on IC-cold (77.4s) and IC-warm (35.5s, 48 ic hits). Review
  dispatched (t15-review). Then integration merged into local main (owner request).

## RESUMED 2026-09-27 (parent 3; owner goal prompt authorizes; SDD on Opus children)

- Docs commits 44666f09 (owner ruling revisions) + 9003f273 (T10 brief). Shared contracts:
  lane-contract.md, reviewer-contract.md, re-review-contract.md (children read these; no builds).
- Worktrees aligned to root 9003f273: pool-3 (T9) + pool-4 (T7) ff'd; pool-5 new branch
  feat/mega-alpha-nav-t4-20260927 (T2 fix -> T4 -> T10 lane). Ruling: parallel lanes in disjoint
  worktrees now, not after T2 (only root builds are serialized) — cost if wrong: rebase churn.
- Dispatched: t2-nav (T2 fix r1), t7-fields (T7), t9-weights (T9), t5-rereview (T5 fix r1 scoped
  re-review), t1-review (T1 redo), t6-review (T6).
- T6 VALIDATION fields: bounded run mega-fields-validation-v1-run exit 0, peak 452 MiB (tree 473.6MB),
  manifest a0905b0c63bf0574cf0601b0861d3a43c1a8a6a5895516c975e3dbdb29e80f0a, producer 892bd33f.
  Coverage (member, 2021-24): si 99.9%, iv 95.0%, earn 99.98%, shares_out 99.7%, mktcap/size_grp
  72.9%, mkt_ret 99.9%. Descriptive coverage only; no outcome data viewed. HAZARD: iv_atm_126d member
  max 1.19e16, IV max ~69 (garbage/units) -> v3 IV families must guard/rank (T8 brief).
- Declared BEFORE measurement: task-T8-brief.md (library v3 = fixed v2 + ~24-32 SI/DTC/dSI/IV level/
  IV slope/IV change/IV-RV/earnings-drift/size candidates, priors cited) and task-T11-brief.md
  (admission screen v3-admit-v1: orient on 2020-21 mean; tau_k<=0.70; HOLD-2022 oriented mean>0;
  greedy |rho|<=0.70 by FIT Sharpe; >=250 finite FIT days). Ruling: admission screen lives in the T9
  fitter (same f_k/tau_k data) — cost if wrong: one extra tool revision.
  Note: "FIT Sharpe > 0" is tautological under FIT-mean orientation; the binding test is the 2022 sign.
- T2 fix r1: pool-5 ca4fa80c -> root f6df5fe8 (fixture-only: v2 spent Jan budget day 0, name 1 never
  held -> forced turnover 0; fixture now exits held name 2). Build mega-t2-fix1 9.43s Jobs4 1 TU/1 link;
  target tests 25/25 0.59s. T2 review dispatched (t2-review, package review-T2.diff). T4 sent to t2-nav.
  Ruling: GMV turnover stats + ceiling flags use EXECUTED fills (planned forced turnover double-counts
  blocked absent exits) — cost if wrong: none (executed is the declared metric).
- T5 fix r1 scoped re-review: 7 addressed (I1-I4, M1, M2, M7), M3-M6 open non-blocking; new 0C/0I/6
  minor. Task T5: complete (2c92d658..f2d5fb97 -> root 6b4a21fe, library SHA b871743e). T5 minors
  (deferred): M3/M4 optional, M5 root, M6 info; N4 Amihud zero-volume blanking 252 vs 126; N5 docs; N6
  report lacks fix section. Ruling: N1/N2 (record corr notes) + N3 (abs guard in ivol_change denominators)
  fixed pre-measurement as "v2 fix round 2" first commit of T8 lane — cost if wrong: none, v2 unmeasured.
  Re-review also found: runner rejects any library field outside {close,raw_close,volume}
  (strategy_ic_runner.cpp:275-277) -> added to T7.
- T8 dispatched (t8-libv3) pool-7 branch feat/mega-alpha-library-v3-20260927 @6b4a21fe.
- T1 review (redo): spec FAIL 1C/1I/8m. C: cache key lacks engine/VM semantics identity (engine_git_sha
  recorded :557, never compared). I: pinned weights not tied to TRAIN (frozen_train :127-137).
  Ruling: T1 fix r1 routed to t7-fields lane (same file + same cache-key code as T7) — cost if wrong: T1
  fix waits for T7.
- T6 review: spec FAIL 0C/2I/6m. I1 IV no plausibility bound; I2 is_common not PIT, default-on.
  Ruling (declared before IV measurement): iv_atm_* outside [0.02, 5.0] -> NaN, counted; is_common
  opt-in + manifest point_in_time flag. Fresh implementer t6-fix pool-8 feat/mega-alpha-fields-fix1-20260927
  @71cbec8f. Fields must be regenerated (-fields-v2) for TRAIN and validation after.
- NAV TRAIN SMOKE (evaluator test, item 2): mega-nav-train-v6-c1 (run2; run1 failed: --role needs
  manifest.json path), exe @f6df5fe8, v6 blend 51740eff, baseline-v1 c1 f1, legacy flat-300 borrow.
  19.59s, peak 229 MiB, accounting max err 1.3e-13. Results (TRAIN, v6 = old equal-family blend, NOT
  neutralized, NOT MV):
    S1 linear: gross SR -0.27, net -0.38, HAC t -0.68, vol 19.2%, MDD 38.8%, trade cost 1.97%, borrow 4.49%
    S2 1bn (primary): gross -0.09, net -0.25, HAC t -0.47, vol 20.2%, MDD 37.4%, impact $21.3M
    S3 adverse: gross -0.29, net -0.45, HAC t -0.84
    executed tau ~4.0%/day mean, p95 ~6.1% (approx from daily CSV; T4 adds exact GMV column);
    held ~2979 names; participation p95 <= 0.18%; 415 write-offs; guard raw-minus-adj sensitivity $247M.
  DISCREPANCY: target-proxy said gross +0.36 and numpy prototype vol 10.4% for same blend; NAV vol ~2x.
  nav-recon investigator dispatched (writes studies/nav_recon.py; root runs it). Not a validation trial.
- T2 review: spec PASS, quality Approved, 0C/0I/8m. Task T2: complete (c4ba9c80/97e6b392/f6df5fe8, review
  clean). T2 minors (deferred): no K=5 write-off fixture (only K=1); guard branch untriggered by fixtures;
  budget-refusal fixture misses 36 B/cell volume budget; legacy 30%/mo flags unlabeled (T4 req 6);
  daily long/short dollars are post-trade (T4 adds pre-trade GMV). Others in task-T2-review.md.
- T9: pool-3 403e56fe -> root 17da002a (fit_composition_weights.py + 16 fixtures, 16/16 OK 3.1s). Review
  dispatched (t9-review). REAL TRAIN v1 fit mega-weights-v1: 98.9s, peak 523 MiB, 48 fitted, 30 nonzero,
  refused decisions 0, tau_flagged [], weighted_standalone_turnover (sum w tau) 0.0708/day, weights sha
  cdfe0dc2e95b57ee52c303432ea2401788d121cb53977c0512743bf2208ce7fd. Composition trial count +1 (v1 MV fit).
  Ruling: run limits stay 180s/1536MiB (agent proposed 300/2048); fitter made incremental (per-candidate
  cache + --max-seconds soft stop) in the T11 lane — cost if wrong: one more tool revision.
- T11 sent to t9-weights (pool-3).
  Ruling: sign conflict screen-vs-runner -> weights JSON carries `signs` (screen sign, 2020-21 FIT mean) and
  top-level `train_manifest_sha256`; runner (T7 lane) applies pinned signs over IC orientation and verifies
  the SHA — cost if wrong: blend orientation differs from runner IC sign for conflicting candidates (listed
  as sign_conflicts).
- NAV RECON (nav-recon; root ran studies/nav_recon.py bounded 8.3s/420MiB, run mega-nav-recon-v1-run):
  numpy mirror == C++ NAV S1/S2 to 1e-15 (NAV code correct; timing d->d+1->d+2 same as target replay and
  studies). Cause: DATA — TRAIN role close.f64 factor break on 2021-01-04 (105 big + 1073 small factor
  jumps with raw flat; 61 held names adj x10..x80 / x1/4..x1/10); guarded P&L -26.1% = 62% of gross var.
  Ex that day S1 gross SR +0.31 (mu 3.7%, vol 11.8%); guard=raw S2 +0.32; by year S1(guard raw) 2020
  -0.13 / 2021 +0.81 / 2022 +0.54. S2-S1 gap = 1% ADV cap leaving phantom shorts open (net lev -0.34).
  Ruling: repair the role data (task T12, new role versions, all caches/fields regenerated) and KEEP the
  declared NAV guard policy (book adjusted) — raw-on-guarded would book fake losses on genuine splits;
  cost if wrong: one more full TRAIN cold pass (~400 s in chunks).
- T9 review: spec PASS, Approved, 0C/0I/6m. Task T9: complete (17da002a, review clean). T9 minors
  (deferred): 98s/48 cands (perf -> T11 incremental); train_manifest_sha256 key (-> T11); others in
  task-T9-review.md.
- T8: pool-7 51b01e61/a20002f5 -> root d5471776 (v2' lib fd1e359b..., 4 dsl_sha changed: ivol_change_*)
  / d3016d0b (v3 pv_fields_ic121_v3 5d164ea1..., 121 = 96 v2' + 25 new). --check OK both; pytest 25/25.
  v3 refs no non-PIT field. Review dispatched (t8-review). Ruling: literature prior signs are documentation
  only (screen orients on 2020-21); si_change grid 10/21/42 accepted — cost if wrong: none for selection.
  Admission trials pending: 121 candidates.
- T6 fix r1: pool-8 a0d9deeb -> root c099cade (IV [0.02,5] -> NaN counted; point_in_time flags; is_common,
  mktcap_lagged, size_grp opt-in non-PIT). unittest OK. Re-review dispatched (t6-rereview).
  Ruling: swap-fin-v1 market-cap predictor = shares_out x raw_close only (mktcap_lagged availability
  pattern not PIT); declared before any swap-fin NAV result — cost if wrong: tier misclass for names with
  stale shares_out (bounded by stresses).
- T12 (role factor-break repair + QA scan) dispatched t12-role pool-8 feat/mega-alpha-role-repair-20260927.
- T11: pool-3 4b983cbd -> root cd2ec619 (v3-admit-v1 screen, resumable fit, signs + train_manifest_sha256,
  exit codes 0/1/3/4). 21/21 OK 3.7s. Review dispatched (t11-review).
- T6 re-review: 7/8 addressed (M3 open: shares_out restatement guard), 0 new C/I, minors N1-N3.
  Task T6: complete (cf36d83c->c615ce36, fix a0d9deeb->c099cade). Ruling: T10 treats shares_out outside
  [1e5, 5e10] as missing (-> warm tier); fields-v2 reruns use --fields si_shares,si_dtc,iv_atm_21d,
  iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret (all PIT; no mktcap_lagged/size_grp) and bind the
  REPAIRED roles (after T12) — cost if wrong: tier misclass on bad share counts, bounded by stresses.
- T7 + T1 fix r1: pool-4 aa5f06dd/2353bac8/4e018e58/d4992a32 -> root 060f440c/a9ef7175/ac0d1309/e238c94c
  (ac91e600 = v2 cherry-pick, skipped). Build mega-t7-a 171.2s Jobs4 52 TUs/4 links (vm.hpp comment
  recompiles). IC tests 50/54: 4 new fixtures throw json type_error.304 "cannot use at() with number"
  -> T7 fix round 1 sent to t7-fields. Old -fields-v1 dirs refused (no PIT flags) by design.
- T8 review: spec PASS, Approved, 0C/0I/5m. Task T8: complete (d5471776/d3016d0b, review clean).
  T8 minors deferred (task-T8-review.md).
- T4: pool-5 6a38fe39 -> root ef089af8 (neutralize price-risk-v1, --band-multiple, lockstep NAV scenarios,
  pretrade_gross_dollars + one_way_turnover_gmv, daily_turnover_gmv stats, ceiling flags). Build mega-t4-a
  19.1s Jobs4 5 TUs/3 links; target tests 32/32. Review dispatched (t4-review). T10 sent to t2-nav.
  T4 concern: exposures recomputed from scratch per decision, ~20-30 s per replay pass at N=5600.
- T7 fix r1: pool-4 c942afea -> root ccd05314 (test-only: range-for over destroyed JSON temporaries, UB).
  Build mega-t7-fix1 16.6s; IC tests 54/54. T7 fix round 1/5 (4 addressed). T7+T1-fix review dispatched
  (t7-review, package review-T7.diff).
- T11 review: spec PASS, Needs fixes, 0C/1I/7m. I1 fitter reads only cache/<train-sha>/ but runner writes
  field candidates (51/121 in v3) to ROOT/<fields-sha>/ and non-dev builds to DIR/<vm_identity>/.
  T11 fix round 1 sent (I1 + M1 fitter-script sha in per-candidate key).
- T4 review: spec PASS, Approved, 0C/0I/8m. Task T4: complete (ef089af8, review clean). Minors in task-T4-review.md.
- T12: pool-8 f822dd42 -> root 99421a5f (repair_role_factor_breaks.py, rule factor-break-v1; 10/10 OK).
  Root cause: TickerHistory3 cumulative factor not chained across 2021-01-04 (atx-db VA1/C-35; VA1 fixes
  decreases only); prepare_recent_research.py:171 builds close = raw x vendor factor unchecked.
  Scans (bounded): TRAIN MASS 2021-01-04 (1185 jump cells), max non-mass 44; VALIDATION CLEAN (max 47,
  quarter-end dividend clusters). Ruling: keep validation role v1 (no new SHA) — cost if wrong: a sub-50
  break in validation stays unrepaired (none seen >47; those are dividends).
  TRAIN REPAIR -> build-equity/recent-fast-train-2020-2022-v2 manifest
  210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de; 1,324,464 close cells rescaled;
  max |ln ret err| 2.33e-15; 2.45 s / 312 MiB. Review dispatched (t12-review).
  T12 concern -> T6 fix round 2 (shares_out restated with unchained factor, ~62 sessions post-break; plus
  shares_out [1e5,5e10] NaN) sent to t6-fix. All TRAIN artifacts must be regenerated on role v2.
- TRAIN role v2 runner: v1 48 cold, mega-v1-train-r2 122.4s / 858 MiB, combined 607f48c1, orientations
  db9b8aa6. NAV (TRAIN, c1 f1, baseline-v1 old equal-family composition, flat-300 legacy borrow) — 2 TRAIN
  construction diagnostics (trials):
    plain  (mega-nav-v1r2-c1-plain): S1 net .134/gross .322; S2 net .072/gross .331 vol 11.6% MDD 18.0%;
      S3 net -.260; tau_gmv mean .0436 p95 .0633.
    neut   (mega-nav-v1r2-c1-neut, price-risk-v1, band 0): S1 net .298/gross .593; S2 net .147/gross .612
      vol 7.8% MDD 9.4% HAC t .29; S3 net -.374/gross .080; tau_gmv mean .0506 p95 .0720 (limits met);
      S2 summed trade cost .064 (~2.1%/yr), borrow .045 (~1.5%/yr). 33-54 s / 236 MiB.
  => break repair confirmed (-26% day gone). Gap to net 1.0 = alpha quality + cost.
- T11 fix r1: pool-3 f0c223e5 -> root 1507a2d8 (runner-summary-bound cache layout; script sha in key;
  M2-M6). 33/33 OK. Report text appended by root (subagent write blocked). Re-review dispatched.
- v1 SCREEN on role v2 (v3-admit-v1, mega-weights-v1r2, 61s/482MiB): 48 candidates = 48 ADMISSION TRIALS:
  8 admitted, 19 reject_unstable (2022 sign flip), 21 reject_redundant; sign_conflicts 13 (2 weighted);
  sum w tau .0742/day; weights 9023511769f98052006560a4bcb45d7de772cffb08d9b891be2f875db454c803
  (composition trial +1). Pinned runner mega-v1mv-train-r2 65s/503MiB, combined 57901bad.
- NAV v1-MV neutralized c1 f1, band grid {0,.5,1,2}/N (4 construction trials, TRAIN in-sample, flat-300):
    band 0  : S1 net 1.044 gross 1.738 | S2 net .510 gross 1.736 HAC .90 vol 4.3% tau .092/.116 tc .112 brw .045
    band .5 : S1 net 1.092 gross 1.676 | S2 net .572 gross 1.661 HAC 1.00 vol 4.1% tau .057/.075 tc .089 brw .043
    band 1  : S1 net .990 gross 1.525 | S2 net .511 gross 1.413 HAC .90 vol 3.6% tau .042/.056 tc .061 brw .036
    band 2  : S1 net .658 gross 1.008 | S2 net .059 gross .567 vol 2.3% tau .026/.045
  All meet daily turnover limits. S2 cost ~3-3.7%/yr dominates; flat-300 ~1.4%/yr. In-sample (screen +
  weights fit on the same TRAIN) — optimistic. Not the freeze candidate (v1 library only).
- T7 review: spec PASS, Needs fixes 0C/2I/8m: I1 fields resident for whole loop (~1580 MiB for v3 at 4
  workers > cap); I2 VM semantics version hand-bumped, no guard test. T7 fix round 2 sent.
- T12 review: spec PASS, Needs fixes 0C/1I/5m: 50-cell threshold margin thin (44/47 legit). TRAIN v2 role
  usable. T12 fix r1 (factor-break-v2 detector) on NEW worktree pool-9 (lease mega-alpha-t12-20260927,
  configure failed on atx-vol install target — source-only lane) because pool-8 is busy with T6 fix 2.
- T11 re-review: 7 addressed (I1, M1-M6), M7 optional open; 0 new C/I; 4 minors (CRLF script sha, numpy
  version unbound, stray context dirs, stale report commands). Task T11: complete (cd2ec619, fix 1507a2d8;
  fix round 1/5). Fitter must run from C:/atx-wt/pool-2 (relative cache paths).
- T6 fix r2: pool-8 03a8f746 -> root b723d487 (shares_out corrected across unchained factor via
  factor-break-v1 steps from their end date; shares_out [1e5,5e10] -> NaN; role-binding check vs repair
  block). 19/19 OK; producer blob a17f83c6.
- T12 fix r1: pool-9 5093dd5c -> root 0e94be59 (factor-break-v2 unexplained-step detector, --rule v1|v2,
  v1 byte-identical). 16/16 OK. v2 scans: VALIDATION CLEAN (max 1 unexplained/session, thr ~44);
  TRAIN(v1 role) MASS-v2 only 2021-01-04 (2114 vs thr 48). Validation role v1 disposition confirmed.
- FIELDS v2 (bounded): TRAIN role v2 -> recent-fast-train-2020-2022-v2-fields-v2 manifest
  76c07277642ffa233c81a3835f43ab2eeee56e840f4aff67507981533f66bf75 (29.5s/635MiB; score cov si .9993,
  iv .936, earn .9997, shares_out .9934, mkt_ret .9997; repaired 2114 matches T12). VALIDATION role v1 ->
  recent-fast-validation-2023-2024-v1-fields-v2 manifest
  cfaaf825790e07405c55a59eeb9072bcdb20287d18a8e25b9e64fde31fb30c15 (25.2s/563MiB; mass 2021-01-04 outside
  role corrected, 2006). Descriptive coverage only for validation.
- Combined scoped re-review dispatched (t6-t12-rereview). v3 TRAIN runner waits on T7 fix 2 (memory).
- v3 --plan-only (TRAIN role v2 + fields v2): required 1,656,728,202 B @4 workers, 1,627,427,722 @2 (cap
  1,610,612,736) -> refused; @1 worker "metadata missing/over1MiB". Relayed to T7 fix 2.
- NETTING (v1-MV, band 0): tau_book .0917 (S2) vs sum w tau .0742 -> NR 1.24 (no netting; book-level re-rank +
  neutralization adds turnover). Report per blend.
- PREREGISTERED (before any v3 measurement): composition variant mv-shrink-0.9-nonneg-netcost-v1 = mu_k -
  c*tau_k, c = 18 bps/unit one-way GMV turnover (S2 cost per unit turnover measured 16.2/20.7/19.4 bps on
  v1 bands 0/.5/1). Final TRAIN selection = {netcost-v1, plain-v1} x band {0,.5,1,2}/N (8 construction/
  composition trials) by S2 net Sharpe (swap-fin-v1) subject to daily limits. Task T13 (fitter option) to
  t9-weights. Ruling cost if wrong: one extra composition trial family on TRAIN, validation untouched.
- T13: pool-3 3d5d5296 -> root d38e7929 (--composition ...-netcost-v1, c=.0018). 35/35. Review (sonnet).
  T13 review: spec PASS, Approved, 0C/0I/3m. Task T13: complete (d38e7929, review clean).
- T6 fix r2 re-review: 2/2 addressed, 0 new C/I; minors N1 (producer v1 50-cell detector copy; val 47)
  N2 (role check compares session names only) deferred. TRAIN: 130,018 cells restated in 62 sessions
  2021-01-04..2021-04-01, rest bit-identical. FINDING: ~105 lines shares_out ~1000x too small (median vol
  108x shares); 1,381 in-range member cells trade >10x shares/day; SI/shares_out >1 in 6,851 cells.
  Ruling (declared before v3 measurement / T10 freeze): shares_out INVALID -> NaN when trailing-21-session
  median volume > 1.0 x shares_out (PIT) or si_shares/shares_out > 1.5 — T6 fix round 3 sent to t6-fix.
  Cost if wrong: some real micro-caps lose size/SI-ratio (-> warm tier), bounded by stresses.
  Consequence: the v1-MV swap-fin TRAIN read above used fields-v2 (pre-guard) tiers; re-score after v3 fields.
- T12 fix r1 re-review: 6/6 addressed, 0 new C/I; minor: v1 byte-identity test skips without 99421a5f.
  Task T12: complete (99421a5f, fix 0e94be59). TRAIN v2 manifest reproducible only with blob 4f5502a2.
- T10 review: spec PASS, Approved, 0C/0I/5m. Task T10: complete (ad6d7682, review clean). Rulings on the
  review's open items: (1) planned turnover / v2 budget recorded before the locate block = diagnostic only
  (declared metric uses executed fills) — cost if wrong: none; (2) tier re-evaluated at every decision (spec
  "as of each decision"); "special until exit" read as "locate block never forces an exit", not a sticky
  tier — cost if wrong: slightly lower fees for names that leave special; (3) memory: 5-book run 338 MiB
  with --max-bytes 1 GiB. T10 minors deferred: wording "special until exit"; clamp_kept_order unfixtured;
  blocked_short_dollars mixes bases (sum of per-decision refusals); name absent at role row 0 = young IPO
  for a year; 22h/23h visibility clock hard-coded.
- T7 fix r2: pool-4 7c870053/2dfadb36 -> root b3322539/1c7827f0 (Belady field residency, capacity = max
  extras per candidate; admission cells*(8*capacity+1); VM source tripwire pinned 51bc0b2e over 29 files;
  --train-fields must be a DIRECTORY). Build mega-t7-fix2 33.1s; IC tests 60/60. Re-review dispatched.
  T7 fix r2 re-review: I1, I2, workers-1 report, M2-M5, M7 addressed; M1 root ruling, M6 accepted, M8
  partial; 0 new C/I; new minors N1 (tripwire target unlabeled), N2 (include-closure blind spots), N3
  (field-def check skipped in validation-only mode without --train-fields). Task T7: complete (060f440c..
  1c7827f0, fix rounds 2/5). Ruling M1: validation runs with pinned weights must match
  weights.provenance.orientations_sha256 to the frozen TRAIN orientations; + N3 -> follow-up T14 (t7-fields),
  must land before validation — cost if wrong: none (guard only).
- v3 TRAIN runner (fields v2, role v2) 4 bounded passes mega-v3f2-train-r2-run1..4: all time-limit 180 s
  (peak 968/970/510/509 MiB), reached 104/115/116/108 of 121. Cold misses all written (run3/4: 0 misses) —
  signal cache complete for fields-v2 keys. BLOCKER: warm pass ~1.4-1.8 s/candidate (load .5-.65, IC .56-.68,
  composition .37-.45) in Debug > 180 s. T15 (runner perf: scoped /O2 on hot TUs per 6d85ac2a, IC-result
  cache if needed, bit-identical) dispatched t15-perf on pool-9 feat/mega-alpha-runner-perf-20260927.
  Ruling: efficiency fix, not a longer cap (owner rule) — cost if wrong: one build iteration.
- T6 fix r3: pool-8 e0ae0e18 -> root be3deb3f (shares_out units rules a/b). 22/22 OK; blob f5e38b97.
  FIELDS v3 (bounded): TRAIN role v2 -> recent-fast-train-2020-2022-v2-fields-v3 manifest
  9577d80ae09f433f0d946829ce2634f2b1226fa69868cd5d5651f52bf19f3349 (32.5s/642MiB; shares_out cov .9895,
  si_ratio to_nan 2353, both-rules 4819); VALIDATION role v1 -> ...-fields-v3 manifest
  699ee8d28cc7fe81b8d07f4562b8338b8c67ac849594313135b602feff465774 (38.7s/561MiB). All non-shares_out
  field payloads byte-identical to v2. Field candidates (51) recompute under the new fields manifest sha.
  Ruling: shares_out NaN'd by rules -> warm tier in T10 (no special fallback) — cost if wrong: heavily
  shorted ETPs under-charged; bounded by engine-tiers stress. Re-review dispatched.
- v3 warm-up under fields v3 (mega-v3f3-warm-run1/2, time-limit both): run2 reached 113/121, 97 hits/16 miss.
- T14: pool-4 f38e79ad -> root 22eae712 (validation weights bound to frozen TRAIN orientations/fields;
  field definitions recorded in TRAIN orientations.json). Build mega-t14-a 43.1s; IC tests 62/62. Review
  dispatched. Ruling (provisional, pending review): weighted frozen TRAIN runs bind via their recipe's
  weights pin ("frozen-TRAIN-recipe-pins-these-weights") — cost if wrong: a looser check on one path.
  NOTE: final TRAIN artifact must be produced by the >=22eae712 binary so validation checks definitions.
- T6 fix r3 re-review: ADDRESSED (independent rebuild: 15,341 NaN cells, identical per rule), 0 new C/I; minors
  N1 restated_cells now post-rules (130,018 -> 129,588), N2 no not-evaluable count for rule b. Implementer
  decisions accepted (volume restated via close/raw factor; >=11/21 present days; implausible_to_nan totals;
  shares_out depends on si_shares; strict comparisons). FINDING: 1.0x/1.5x NaN 6,925 genuine member cells in 105
  lines (55 leveraged/inverse/vol ETPs, XRT, meme stocks) vs 3,778 defect cells in 72 lines; 5,183 genuine
  cells would drop special -> warm and escape the locate block.
  Ruling (supersedes 1.0/1.5; data-QA, TRAIN-only, no outcome data): thresholds 3.0x turnover / 5.0x SI
  (TRAIN: 3,558/3,778 defect cells caught, 568 genuine). T6 fix round 4 sent. Fields v4 + field-candidate
  recompute follow. Cost if wrong: ~220 defect cells stay (mis-sized names), bounded by stresses.
- T14 review: spec FAIL 0C/1I/3m: weighted-source exemption (runner :835) only checks hash validity. Ruling:
  strict — validation binds to the UNWEIGHTED TRAIN orientations the weights were fit on; weighted frozen
  source refused (supersedes provisional ruling). Fix round 1 sent. Review also: NAV consumer must use only
  validation-only combined outputs (joint TRAIN+VAL runs apply unprovenanced weights) — process rule adopted.
- T6 fix r4: pool-8 dae6ac4c -> root 7347153a (thresholds 3.0/5.0; restated_cells pre-rules; si not_evaluable).
  22/22 OK; blob f083ef73. FIELDS v4 (bounded): TRAIN -> recent-fast-train-2020-2022-v2-fields-v4 manifest
  389e7fb4993ca577acca6b22101a76b9f86ec903340627f410293c73060b4bd8 (28.8s/634MiB; restated 130,018; turnover
  NaN 4,545; si NaN 1,173; shares_out cov .9920); VALIDATION -> recent-fast-validation-2023-2024-v1-fields-v4
  manifest 2691dcc55e60918a71563ff9eb9e3c009e1d60f54fc52041cbcb62840eea9073 (26.2s). Other fields identical to
  v3. THESE ARE THE FROZEN-CANDIDATE FIELDS unless a re-review finds a defect. Re-review (sonnet) dispatched.
  T6 fix r4 re-review: 3/3 addressed, 0 new C/I. Task T6: complete (fix rounds 4/5; final producer 7347153a).
- T14 fix r1: pool-4 43cc33f3 -> root 914fd6f9 (strict M1; weighted TRAIN sources refused). Build 30.9s; IC 62/62.
  Re-review: 4/4 addressed, 0 new C/I. Task T14: complete (22eae712, fix 914fd6f9).
  VALIDATION FLOW (binding): unweighted TRAIN-only run (orientations O) -> fitter -> weights W (provenance O)
  -> ledger W sha -> ONE validation-only runner run with O + W -> NAV on the validation-only combined output.
- v3 TRAIN (library 5d164ea1, role v2, fields v4 389e7fb4, binary >=914fd6f9): mega-v3f4-warm-run1 time-limit
  106/121 (36 miss); run2 COMPLETE 176s; run3 COMPLETE 160s / 509 MiB, 121/121 cache hits.
  => mega-v3f4-warm-3 is the unweighted TRAIN artifact for the v3 screen/fit.
- v3 SCREEN (v3-admit-v1, TRAIN only; 121 ADMISSION TRIALS): 23 admitted, 54 reject_unstable, 44 reject_redundant,
  0 turnover; admission 2f0c12b2. Weights: plain mv-shrink-0.9-nonneg-v1 db0a8b9a1b70d6ba81f7af6d1e1a7f8774f3a98ac6e1ecf2566a1610ce764aac
  (23 nonzero, sum w tau .0676); netcost 7fbd257426664f486f43bb996d071478ec7f4107ff6bbb23244555cf1f37a723 (11 nonzero, .0385).
  +2 composition trials. Weighted TRAIN runner: plain 123s/511MiB combined 98f13b95; netcost 126s combined f0b1dc70.
- v3 TRAIN NAV GRID (8 construction trials; neut price-risk-v1, c1 f1; S2 x swap-fin-v1 net SR / HAC t / tau mean,p95):
    plain   b0 1.409/2.21/.101,.125  b.5 1.611/2.48/.067,.097  b1 1.807/2.79/.047,.067  b2 1.105/1.91/.027,.048
    netcost b0 1.570/2.57/.044,.062  b.5 1.710/2.79/.020,.031  b1 1.646/2.74/.014,.022  b2 1.396/2.33/.013,.033
  All meet daily limits. Netting ratio (band 0, S2): plain .1007/.0676 = 1.49; netcost .0436/.0385 = 1.13.
  SELECTED by preregistered rule (max S2 x swap-fin-v1 net SR within limits): PLAIN, BAND 1/N.
  TRAIN (in-sample) selected: S1 net 2.364; S2 swap-fin 1.807 (gross 2.741, mu 4.61%/yr, vol 2.55%, MDD 3.6%,
  years 2020 +10.1% / 2021 -0.7% / 2022 +4.8%); stresses S2 flat-300 1.396, S2 engine-tiers 1.702; S3 see summary.
  Capacity: 83,670 of 129,346 fills capped at 1% ADV (65%), participation max .010, unfilled $20.5bn cumulative.
  Financing (3y): long $5.14M, short $8.59M (gc 5.02 / warm 2.85 / special .72), short$ share gc .80 warm .19
  special .011; blocked short name-decisions 22,229; missing-predictor member decisions 17,146.

## FREEZE v3-daily-2026-09-27 (declared before any validation run of this configuration)
- library pv_fields_ic121_v3 5d164ea115c633677dae59de975c24c4882ee05430d03a7e1bafa6bc591cb9f8
- TRAIN role v2 210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de; TRAIN fields v4
  389e7fb4993ca577acca6b22101a76b9f86ec903340627f410293c73060b4bd8
- VALIDATION role v1 0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7; validation fields v4
  2691dcc55e60918a71563ff9eb9e3c009e1d60f54fc52041cbcb62840eea9073 (producer blob f083ef73, file sha 0d0eff53)
- frozen unweighted TRAIN artifact build-equity/mega-v3f4-warm-3: orientations.json
  33bc0f63e16869ee11716cc2e47c10b6be4638731ee74d667c7b37926d766c22, summary 9c14ad0056d17fc01d3f450458e7ad46d4666c07057bf9429ff93e3c20775c26
- admission v3-admit-v1 2f0c12b2...; weights mv-shrink-0.9-nonneg-v1 db0a8b9a1b70d6ba81f7af6d1e1a7f8774f3a98ac6e1ecf2566a1610ce764aac
  (signs pinned in file; fitter sha fbfd122b)
- construction: nav --rule baseline-v1 --cadence 1 --trade-fraction 1 --neutralize price-risk-v1 --band-multiple 1
  --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --max-bytes 1073741824, --fields validation v4
- scenarios: primary S2 modeled-1bn-stale5-v1 x swap-fin-v1; stresses S2 x flat-300-v0, S2 x engine-tiers-v1;
  S1/S3 x swap-fin reported
- binaries: atx-equity-strategy-ic.exe 25939b78320baeb5a5fdca86fffb80066554dd9563058db0c50b2326f09f1ed1 (source 914fd6f9);
  atx-equity-strategy-targets.exe 4642dd37f14fa3fd1d3946f9876a3c5ffccb5507a57c7f86b7b02f4cbab4d47a (source ad6d7682+)
- T15 (runner perf) parked unimported until after this validation run (one binary across the freeze).
- Trial counts to date (TRAIN only): admission 48 (v1) + 121 (v3); composition 4 (v1 T9 fit, v1r2 fit, v3 plain,
  v3 netcost) + 7 earlier study methods; construction 2 (v1 plain/neut) + 4 (v1 band grid) + 8 (v3 grid).
  Validation: prior disclosed use of 2023-24 target-proxy numbers for the v3 monthly blend (earlier session);
  this is validation run #1 for any daily NAV configuration.
- T10: pool-5 9c279411 -> root ad6d7682 (financing specs; swap-fin-v1 primary, flat-300-v0 bit-identical,
  engine-tiers-v1; tiers once per decision; locate block; nav --fields/--fields-sha256; 5 books with
  fields). Build mega-t10-a 22.7s; target tests 39/39. Review dispatched (t10-review).
  NAV needs --max-bytes 1073741824 with fields (5 books; default 512 MiB refused).
- NAV v1-MV neut band .5 + fields v2 (mega-nav-v1mv-c1-n-b0.5-fin2, 30s/338MiB; TRAIN in-sample; this
  is the same construction trial as band .5 above, financing re-scored):
    S2 x swap-fin-v1 (PRIMARY): net .833 gross 1.629 HAC 1.45 vol 4.06% MDD 6.8% tau_gmv .0562/.0738
      tc .081 fin .0098 (3y); blocked short name-decisions 37,312; tiers member-decisions gc 1.659M /
      warm .461M / special .125M; missing predictor 14,080; mean |net lev| .021 (max .051).
    S1 x swap-fin: net 1.309; S3 x swap-fin: net .031; S2 x flat-300-v0: .572 (== legacy);
    S2 x engine-tiers-v1: .746.

## OWNER RULING 2026-09-27 (b) — financing: swap-fin-v1 replaces flat 300 bps (handoff 2 §2b)

PB portfolio swap: long pays bench+s_L, short receives bench-s_S-fee; dollar-neutral -> bench cancels;
replay cash-0%/no-rebate = excess-return accounting (structure OK). Flat 300bps wrong both ways: GC ~30bps
holds ~83% of US SI (S3 2022-23; D'Avolio 17bps/91%), specials avg 4.3% w/ fat tail and carry anomaly
short legs (Muravyev-Pearson-Pollet JF 2025); long spread missing; /365 vs ACT/360. Declared primary
swap-fin-v1: long 40, short 20 + engine tier fee (GC 30/warm 100/special 500 bps; flags mcap<$1bn,
px<$5, SI/shares_out>10%, age<365d; missing -> warm), ACT/360, no new/increased special shorts.
Stresses flat-300-v0 (legacy, bit-identical) and engine-tiers-v1 (27.5/300/2750). New task T10 (after
T4, same owner/files). TRAIN-study net .56 used flat 300 - not comparable. No NAV result seen yet.

## OWNER RULING 2026-09-27 — daily cadence; turnover target redefined (handoff 2 §2/§2a revised)

Shipped construction is daily (cadence 1). 30%/calendar-month turnover RETIRED (reporting only; not a
constraint/flag). Combined book: mean daily one-way turnover <= 20% GMV, p95 <= 30% GMV (sum|fills| /
pre-trade long+short $, deployment excluded). Individual alphas <= 70% GMV/day standalone; netting ratio
tau_book/sum w_k tau_k reported on TRAIN. Basis: WorldQuant 4,000-alpha study (median ~30%/day, range
5-149%), BRAIN 1-70% window, practitioner 30%/day ceiling; EMN mutual funds ~1-2%/day = where 30%/mo sat.
Cancelled: validation NAV on v3 blend / monthly-budget-v2 / cadence>1. Daily construction set: band
{0,.5,1,2}/N at fraction 1 (4 trials). T4 brief (+GMV daily stats, ceiling flags) and T9 brief (+tau_k,
sum w tau) revised. Deliverable = frozen daily mega-alpha, one validation NAV run.

## PAUSED 2026-09-26 at owner request — handoff 2

Authoritative: docs/plans/2026-09-26-mega-alpha-parent-handoff-2.md; goal prompt
docs/plans/2026-09-26-mega-alpha-next-parent-goal-2.md. All child agents stopped (T2 fix round, T5 fix
round, T7, T9, T1 review). T2 imported c4ba9c80/97e6b392: build 31.62s, 24/25 (fixture
ConstantPricesNoCostReproducesTargetReplayPlanned forced_turnover==0 open). T6 imported c615ce36; TRAIN
fields run 22.03s/604MB manifest 519fc9b2 (validation fields not run). T5 fix round 1 committed pool-7
f2d5fb97, unreviewed/unimported. T7 (pool-4) and T9 (pool-3) stopped with no commits. No owned process.


## RESUMED 2026-09-26 ~20:15 ET (new parent, owner goal prompt authorizes)

Owner mid-run note: do not let RAM limits slow progress; if blocked, improve
efficiency/incremental progress to cut resource use.

- Import: pool4 8527a839 -> 050c0efc (VM arena release), 23f1541b -> bfb6b859 (fixture).
- Pool3 archive: 58c21bb0 (46/46 files verified, index 352ca7b7) -> 470eb1b6.
- Build helper recent-strategy-targets-v1: source=configured bfb6b859, Jobs2 (free2198),
  36.62s, 6 CPPs/5 links, no PCH/deps. IC tests 35/35 5.141s; target tests 7/7 0.198s.
- TRAIN export v5: guard system-memory-limit at cand 11 (peak894.6MB, others'+agents' RAM).
  Ruling: guard min-free 768->512 MiB for real runs (RSS1536/180s kept) - owner note; cost if
  wrong: host paging, no correctness impact.
- TRAIN export v6 COMPLETE 94.70s wall, peak898.7MB, exe 03607890...; stages vm30.0 ic30.7
  comp15.1 load9.4 save4.4. Orientation array canon 4a3e8004 == v2; planned targets & daily IC
  CSV byte-identical to v2; candidate diffs timing-only. Blend manifest 51740eff...
- TRAIN replay (6bps/300bps): baseline turnover mean35.32%/max80.08%, gross.901; budget-v2
  mean30.66%/max36.69% (forced-exit breaches), gross.847. Only 253/754 mature days complete
  (missing names up to232/day). All-days observed-component gross SR .36 (base)/.33 (budget);
  complete-day subset SR ~1.2 is BIASED - not claimable.
- VALIDATION frozen replay: base mean34.44%/max70.60%, gross.906; budget mean30.24%/max31.31%,
  gross.853; 180/500 complete days; obs-component gross SR .34/.38, ~1.9%/yr, vol ~5%.
  Borrow scenario ~1.25%/yr dominates -> net ~0. Objective NOT met.
- Evidence commit 5c9cbaed: saved-blend-policy archive (60 files) + report + DAG row. Audit script
  d8e5fb27 PASS: 2268 exact baseline f64 values.
- TRAIN numpy prototype (diagnostic only; C++ remains the evaluator): neutralizing desired target vs
  trailing beta252/vol63/logADV63 lifts gross ratio .36->.55, vol 10.4%->7.5%, beta -.16->-.03, but
  turnover 35->40%/mo. Cadence/fraction/EWMA variants all ~.5; borrow 300->50bps ~+.16. BINDING
  CONSTRAINT = ALPHA QUALITY, not construction.
- Sprint v2 dispatched 2026-09-26 ~21:50 (opus, no TDD, root builds):
  T1 pool-4 feat/mega-alpha-runner-cache-20260926: candidate signal cache + pinned composition weights.
  T2 pool-5 feat/mega-alpha-nav-20260926: NAV replay per nav-backtest-design.md (S1 linear, S2 $1bn
    sqrt-impact PRIMARY, S3 adverse K=1; stale-carry K=5).
  T3 pool-3 feat/mega-alpha-exposures-20260926: price exposures + neutralize_target (new files).
  Data inventory (read-only Explore) running for fundamentals/short interest/13F/industry.
  Ruling: implementer tasks branch fresh from root 5c9cbaed in pools 3/4/5 (old branches kept) -
  trivial cherry-picks; cost if wrong: none.
  Ruling: 300bps borrow and K=5 kept as registered/designed; no cost grid.
- Inventory (read-only): FINRA SI as-of (C:/atx/data/finra_short_interest/asof, securityID-native,
  ~100% members, strict available_at<session) = fastest new source; TickerHistory3 ATM IV/HV +
  earnFlag native; spine_monthly me_line/security_type native via TBLTICKERHISTORY-<id>; CIK-mapped
  fundamentals/SIC only ~65% + export seal 2020 -> next tier. GICS empty. 13F/analyst absent.
  Ruling: role extra-fields path (Python producer + runner load of referenced fields only) rather than
  new price projection; cost if wrong: one more producer iteration.
  T5 pool-7 feat/mega-alpha-library-v2-20260926: library v2 = v1 48 + 48 price/volume (8 families).
  T6 pool-8 feat/mega-alpha-fields-20260926: prepare_research_fields.py (SI, IV/HV, earn, size).
  Planned: T4 wire neutralization into target/NAV; T7 runner --extra-fields; T8 library v3 (SI/IV).
- T3 e4a869b7 -> b9cf4023; CMake 429cbe43. Build mega-t3-a 21.89s Jobs4, 3 TUs/2 links, provenance
  429cbe43. atx-impl-strategy-target-tests 15/15 pass 0.145s (7 old + 8 new). Review dispatched.
- T5 2c92d658 -> c47ffdaa: library v2 96 (v1 48 byte-identical + 8 new families). SHA 0c7f3059...;
  recipe 93cd52ab... --check ok; native --plan-only: 96 compile, max slots 8, lookback 314, TRAIN
  admitted 1,234,268,377 B. Concern: vec_avg member-masked -> mkt-based templates NaN unless member
  whole window (review checking). Review dispatched. TRAIN run waits for T1 cache (180s cap).
- T3 review: APPROVE 0C/0I/8 minor. Task T3: complete (b9cf4023..429cbe43, review clean).
  T3 minors (deferred): M1 rescale amplification up to 1e9x -> T4 must cap; M2 msg; M3 clip untested;
  M4 refusal kind untested; M5 ADV present-missing-volume=0; M6 pivot floor; M7 compute exposures once
  per decision in T4; M8 noexcept.
- T1 09a18ec3/a7fd1c02 -> af8c38ee/445e828d. Build mega-t1-b 32.29s Jobs4 5 TUs/3 links; IC 42/42 9.68s.
  First real cold-cache run TIME-LIMIT 180.27s: Debug SHA-256 ~20MB/s (cache write 2.5s/candidate).
  Fix 6d85ac2a: scoped /O2 on atx-core sha256.cpp (build sha-o2 21.89s, 3 TUs/6 links, prov 6d85ac2a).
  Rerun mega-v1-train-cache-b: COMPLETE 71.16s peak 790MB, 37 cache-hit + 11 cold, planned targets /
  daily IC / blend bytes IDENTICAL to v6. load 9.4->1.43s, save 4.35->0.33s, write .24s, hit .35s.
- T5 review: spec PASS, 4 Important (I1 raw-volume splits; I2 vec_avg member-mask blanks mkt templates;
  I3 resmom ~ v1 mom; I4 illiquidity/ivol/max reload v1 tilts). Ruling: fix pre-measurement (fix round
  1 sent); I2 via producer field mkt_ret (T6 addendum) + runner --*-fields (T7) instead of VM opcode
  (avoids engine rebuild fan-out); cost if wrong: v2 waits on T6/T7.
- T2 addendum: accept pinned-candidate-weights blend semantics. T7 dispatched pool-4
  feat/mega-alpha-runner-fields-20260926 @6d85ac2a.
- TRAIN-only studies (scripts in studies/; diagnostic, not evaluator; validation untouched):
  compose_study: 48 cached v1 signals, per-candidate neutralized (beta252/vol63/logADV63) daily factor
  returns; signs+weights fit 2020-21, holdout 2022. Gross SR hold22: equal .72, inv-vol .84,
  pos-sharpe .82, MV shrink.5 .62, MV shrink.9 1.23 (7 methods compared = 7 composition trials on the
  2022 TRAIN holdout). Mean pairwise factor corr .18. Seasonality candidates unstable.
  construct_study (MV.9 blend, neutralized target, 6bps+300bps): hold22 gross/net SR, turnover/mo:
  daily full 1.19/.56/169%; c5 full 1.07/.49/131%; c5 f.25 .60/.13/56%; c1 f.10 .70/.19/77%;
  c5 f.25 band(1/N) .69/.31/28%; c21 full .58/.10/78%. => alpha decays fast vs 30%/mo budget;
  no-trade band >> partial adjustment; flat 300bps borrow costs ~.3 SR.
  Ruling: next construction = neutralize + no-trade band (T4), composition = MV-shrink(.9) weights fit
  on full TRAIN (pinned via T1), alpha priority = slower sources (SI, IV, then fundamentals); add an
  SI-tiered borrow scenario (engine borrow_tiers GC 27.5bps/warm 300/special) next to flat 300bps.
  Cost if wrong: some construction rework; no validation data spent.
- Next: NAV backtest with declared missing-price policy (design agent), audit script review
  (agent), then construction (risk neutralization) + alpha expansion toward SR>=1.


## PAUSED at owner request — next-parent handoff

Owner requested stopping and a detailed handoff/goal prompt. Goal status is
paused; all three agents acknowledged stop; no owned build/research job remains.
Authoritative handoff: docs/plans/2026-09-26-mega-alpha-parent-handoff.md.
Paste-ready prompt: docs/plans/2026-09-26-mega-alpha-next-parent-goal.md.
Last implementation HEAD6df7cc88, root clean before documentation. Memory fix
8527a839 and fixture23f1541b are committed in pool4, source-approved by pool5,
but unimported/unbuilt. Target replay is integrated but unbuilt. Pool3 has the
ignored unfinished compact v3/v4 evidence archive; exact paths/hash in handoff.
No new build or numerical run after the v4 memory stop. Resume only on owner
instruction; older future-tense checkpoints below are superseded by this stop.

## Current: frozen validation complete; saved-blend portfolio iteration

Root imported target replay through8c7dc6a0. Root CMake registration and fixed
baseline-versus-30%-monthly-budget recipe are being committed. Pool4 is fixing
runner VM slot growth: candidate11 grows5->7 slots while old5 stay allocated,
adding248MiB transient TRAIN RAM. Release old VM before growing, retain masks,
shared workers and exact arithmetic. Pool5 reviews; root alone builds/data.

At a9b8814c: focused build82.169s Jobs2; all34 native checks10.484s.
Validation-only v3 completes48+blend110.5s, peak660586496B; no TRAIN refit.
Combined rank IC5/21/63=.0180335655/.0356902802/.0596669293;
planned mean monthly turnover.3443560. No Sharpe or capacity qualification.
Saved validation manifest build-equity/recent-fast-ic-validation-v3/validation_combined.json
SHA7407d7e7b72548e5577fdf94e2f52d238f9ff751a8ebc5640987130c1390d8e1.
All55098 comparable old daily IC values exact; one old unterminated row excluded.
Pool3 archives compact v3 build/native/actual/audit receipts plus v4 RAM stop.

v4 TRAIN export stopped at41.578s system-memory-limit, peak1101864960B,
10complete/start11; no TRAIN artifact. All processes ended. Preserve evidence.
Do not blindly retry. Once lifetime fix is qualified, one bounded TRAIN export
must reproduce original v2 signs/metrics and save the blend. Then exactly two
saved-blend TRAIN target runs (baseline and fixed.30 monthly budget) and frozen
validation construction. Cost scenario6bps one-way/300bps borrow is hypothetical.
Do not expand corporate-action catalog; no2025+ evaluation or validation tuning.

## Latest: completed TRAIN, resume validation next

Root HEAD before this checkpoint `0230d799`. No active build/process. New
pending CMake edit adds existing factory/ic_screen_test.cpp to the focused
atx-impl-strategy-ic-tests target to qualify legacy arithmetic/bookkeeping.
Next ignored build helper `build-equity/build-recent-strategy-fast-ic-resume.ps1`.

At0f618a45, optimized build31.631s Jobs3 and all13 native checks1.618s pass.
Only3privateCPP commands gained O2/Ob2/finline and local PCHexceptions;
allotherflags/CRT/FP/ISA/PCH/deps unchanged. v2 fullTRAIN48+combined completes
131.666s; wholeprocesscap180.218s leaves13VALcomplete/14started, noVALblend.
Peak896258048bytes. Samefirst10 candidates84.772s->24.090s, all62376 valid
dailyIC values identical;1truncatedbaselineCSVrow excluded. Exactv2packet
fast-ic-optimized-20260926 and adjacent qualification report preserve all.

Resume artifact `build-equity/recent-fast-ic-v2/orientations.json` SHA
5106fdc13fc5347c9c2c670714c134a1978e6b7a0d2d2b5dd79bacb7dfb782d6;
adjacentrecipe needed. Same fullrole pins/library/minnames2000/memory1536,
workers4; add --orientations PATH --orientations-sha256 SHA, newoutputdir.
No TRAIN payload/refit. 2025+ reserved.

New source imported: parallelICbfabe3a4 +fixture5330a5fd, resume34d149a2
+fixturee72f32da, caller0230d799. G0sourceapproved42337ab3 pendingimport.
Pool4 separately implements opt-in --save-combined exactf64 blend+effective
membermask and boundmanifest; include before resumedVAL if ready, to avoid
re-evaluating48DSLs for later portfolio construction. Pool5 now DESIGNONLY
for fast savedblend portfolio/turnover work; no source/data/buildthere.

TRAINcombinedIC5/21/63=.03499/.04993/.05983, training-onlyfit, noSharpe.
Plannedmeanmonthlyturnover35.32%, meangross.9012,maxabsnet.0645; targetnotmet.
Do not resume corporate-action catalog expansion. Focus runtime, composition,
then fast portfolio construction. Alloldercheckpoints supersededasneeded.

## Latest runtime checkpoint

At clean source `20bf677b`, focused build passed in27.729s Jobs3 (eight CXX
actions, warm PCH/deps); all11 native checks pass1.344s. Metadata plan confirms
maxslots7, lookback314, TRAIN1,487,603,721bytes, validation1,030,839,264bytes.
First real fast attempt `build-equity/recent-fast-ic-v1` stopped deliberately
at105.609s:10 TRAIN candidates complete,11th started;6.091–11.509s each,
peak956231680bytes. No composition/orientations/validation complete. Fixed
library/weights unchanged. Guard is process-error plus explicit operator-stop
receipt. NO active process. Root imported final review/report ef65e6d8/2147d883.

Pool4 now implements explicit1..4 sharedDetPool CS/TS execution and per-stage
timers in private runner, preserving serial default/recipe/math; fixtures then
review. Pool5 investigates narrowly scoped optimized compilation of hot CPPs
without globalDebug/PCH/dependency rebuild. Root owns CMake/build/data. Next
build/run remains bounded; don't re-run known-slow serial baseline wholesale.
Exact32-file evidence and initial qualification report under
`.superpowers/sdd/strategy/fast-ic-qualification-20260926` and adjacent report.

## Fast IC integration checkpoint

Current root before this checkpoint: `9bb58f9e`. Event expansion is stopped.
New frozen 48-alpha library/helper source `c4c8dc59` -> `0139cb15`, fixtures
`67b28ba0` -> `2a26e674`; kernel `1dba7035` -> `3048951c`, fixtures
`ade1e99d` -> `9b65981a`; runner `08940e88` -> `1e67b5c5`, fixtures
`767d8894` -> `d0c9ade6`, identifier fix `86671916` -> `9bb58f9e`.
Independent kernel/composition review `d91514f4` -> `4c4f579c`; runner source
has no substantive review blocker, final fixture review pending with G0.

Full roles are ready from cached data at source `527add1c`:
TRAIN `recent-fast-train-2020-2022-v1`: 1,155 dates × 5,627 IDs, score 399..1155,
manifest `3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493`;
validation `recent-fast-validation-2023-2024-v1`: 903 × 5,048, score 401..903,
manifest `0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7`.
Preparation 12.266s/8.016s. No IC or portfolio result has been evaluated.

Next: commit focused CMake registration, run ignored helper
`build-equity/build-recent-strategy-fast-ic.ps1` (RAM-admitted 2–4 workers;
only atx-equity-strategy-ic and atx-impl-strategy-ic-tests). Run all 11 new
native cases, then metadata-only CLI to obtain actual slots/memory before
loading data. Real runs <=180s, sampled RSS/free floor, min-names 2000.
The fixed baseline includes all TRAIN-oriented candidates irrespective of
diagnostic screen rejection. Signs fit on TRAIN 21-session mean rank IC;
undefined/zero signs are neutral. 2025+ reserved. No book/event surfaces.
Pool5 is inspecting existing DetPool APIs for a narrow runtime follow-up only
if timings justify it; no edits or numerical runs there.

Stock batch 45/45 evidence is archived in
`.superpowers/sdd/strategy/stock-qualification-20260926`; no real stock run.
Older checkpoints below are superseded where inconsistent.

## LATEST OWNER STEERING: runtime, generation and composition before realism

Owner explicitly stopped overoptimizing realism/registering every action.
All event-expansion work is stopped. In-flightstockbatchclosedall45casespass
3.028s(guard3.25s/18MiB) atb036fa32. First195.553sJobs2 compilefailure was
fixturemixedconstpointerdeduction; test-onlyb036fa32 resumes12.462sJobs4,
oneCPP+twolinks. Configuredf1c40409. NOactualv4bookrun, threeactualattempts
stillzeroresults. BothJAG/WCGstockconfigsretainedcurrentSHA
2fc4cc34103cb7cf591b7a27e4b8da58f749d76193e36fe474014c66f3d76691.

Activeplan nowprioritizesnewfastresearchphase. Pool4implements NEWlightweight
factory/ic_research.hpp opaqueoptions/cache/scratch over sharedic_screen.cpp
helpers (legacyIcScreenConfig/header/APIsunchanged); research3horizons5/21/63,
strictendpointpresence. Newprivate strategy_ic_runner.hpp/CPP +standalonemain,
no surfaces/book. Pool5owns48DSL library8families*3templates*2slowvariants,
maxlookback<=320, plusnewprivate streamingcompositionhelper. Fixed1/48weights,
TRAIN21ICsigns/undefinedzero, fixeddenominators; combinedIC/coverage andplanned
targetturnover/exposurediagnostics only. No evidenceweightoptimizerfirst.
G0 revieweddesign andisIDLEawaitingfrozenpackets; reactivatewithfollowup_task.

Rootnext: preserveconcisequalification, preparefull2020-22TRAIN fromwarmup
2018-06 and2023-24validation fromwarmup2021-06 usingexistingacceptedcache
recent-projection-v1 (no rawrescan); caprealprocesses180s/RAM/freefloor. Freeze
exactnewlibrary beforeperformance, inspectkernel/runner/composition, register
focusedCMake, one warmbuild thenboundedactualfastresearch. Preliminarysignal
diagnostics arenotcostednetportfolioSR/capacity/actualturnoverclaims. Usergoal
still$1bn,Sharpe>=1,lowturnover/thousands;2025+reserved. No activebuild/process.

Currentrootbeforethischeckpointb036fa32. NoC:/atxwrites/push/warehousewrites.
OriginalDAGupdatedsource/importSHAs. Oldercheckpointsbelowaresuperseded.

## Current checkpoint: cash qualified; stock conversion in implementation

Root3780a55d records cash/monthly qualification and49 exact evidence artifacts.
Clean compiled sourceab049a50, configuredc09df61c. Firstbuild51.129s Jobs3
failed mask span/owned-vector API; one-linefixab049a50 resumes21.906s Jobs3.
All32 native cases pass1.738s (12new), zero failures/skips; guard1.875s.
Report .superpowers/sdd/strategy/2026-09-26-cash-claim-qualification.md.

Third actualrun recent-dev-rehearsal-v3 stops4.828s/219181056bytes on first
candidate+ orientation JAG4997008 short-151822.37061726692 at2020-01-10
22UTC; priorJan9price8.2299995422363281, no guard crossing, sourceabsent.
It passes priorMDCO/WAIRendpoints; laterthreeconfiguredcashevents unreached.
Three cumulative actual attempts, zero completions; no portfolio Sharpe.
Samecashconfig257c6d645292b9f9464e6401ed1f9adb5e41a66089fdc4f7e23db0724f6da510,
same24library/costs/roles/selection. No Q2score/2023-24/2025+ evaluation.

JAGfacts775a9cad->16a8adb2: issuerJan10 08:51ET confirmscompletion;
447/1000 ParsleyEnergy CLASS A PE, CUSIP701877102 per predecessor share.
Use followingminute13:52UTC conservativeavailability. Nofixedcashleg;
actualfractionalCIL/delivery/loantransfer unresolved. Priorstockinventory
a852df6c->ffda3043 identifies onlysyntheticexistingreplaycaller.

Pool4 implements new narrow factory stocktransitioncontext/privateCPP with
signed successor addition/bridge and optionalexplicitfixedcash, retirement/
queuedorder netting, exact observed successor raw/adjustedbasis, causalclock.
Preservecashroutehash/arithmetic whenstockempty. No oldsyntheticenumwidening.
Pool5 owns strict pinned --stock-transitions runner/config v3 with sameallroles/
signs and decisionmask, no fabricatedfractionalsettlement. Pool3 extends
audit_recent_price_gap.py --tickers exactsource selector to bind PEvendorID;
rootonlyrunsrawaudit, thenG0reviewsnewengine. No activebuild orrealrun.

Need stock primary/sourcefacts artifact, preregister pinnedv3 execution before
fourthattempt; rootownsCMake,focusedbuild,guardednative/realrun. UpdateDAG
withstockimports whenready. Emptywarehousetables and commonstockcoverage
remainunresolved; optionalexternaldatapathquestion stillpending. Original
goalactive/unlimited but ownerpivotgoverns; no goalcompletion or push.

## Prior integration checkpoint: five cash claims awaiting runtime (superseded)

This checkpoint supersedes the historical sections below. Root source before
this checkpoint is d52ed04b, pool2 only. No active build or numerical process.
Owner target is $1bn NAV, net annualized Sharpe>=1, calendar-month one-way
turnover<=30%, thousands of stocks, 2020+ data. No achieved result: two real
attempts failed, zero completed trials. Recent strict diagnostic evidence is
eb2559e8; source gap is genuine MDCO archive ending January3,2020.

Cash engine core c0be402e->eb67e4d5, clock1ef2524d->8ae8294c,
capital440d538f->963adbc0, fixtures25ff0a88->a2fe3280,
3d1ef20a->9d8a718f and47696031->f8d1062f. Independent source review
eeb6dc03->88f1e2c3 approves seven fixtures/source; native runtime pending.
Runner80ebf709->1ab4a490, decision-mask5e1fa849->be2ce0ad,
three fixturesa86b274d->010cc1a8, report1b62048e->d52ed04b.
Monthlyreport779add70->494ecc75 and fixture91329304->6a05ec3f pending.
Root CMake registration65e1e50a/2d06b704 already included.

Five event records MDCO/WAIR/BOLD/ARQL/THOR are committed atba58bab2;
cash config SHA256257c6d645292b9f9464e6401ed1f9adb5e41a66089fdc4f7e23db0724f6da510.
They are reconstructed publication evidence, not verified delivery/settlement.
No invented payment: signed nonspendable claims, reserve short payables,
continue last modeled short borrow. Active plan preregisters v2 retry with all
previous candidates, trial counts, costs and role/data pins unchanged.

Source forensic batch2a41477a ran38IDs in10.5s/121MiB; output
build-equity/recent-q1-gap-batch-v1.json SHA256
037234f9c3f427dd45160a8f19ab7ecbc40dd941699cc73f56c94f717f11ed56.
Masks-only queue is data QA, not terminal proof or future universe filtering.
Read-only warehouse event, terminal and universe-type tables are empty.
Optional user data-location question remains unanswered. Stock conversions,
temporary gaps, dated common-stock/type coverage remain unresolved.

Next: finish G0 adapter source review; freeze/checkpoint; build ONLY
atx-equity-strategy and atx-impl-strategy-tests via warm equity-dev wrapper,
RAM-admitted2-4 workers. CMake regeneration updates configured provenance.
Run bounded focused native, then one fresh claim-aware v3 rehearsal with
180s/1536MiB RSS/768MiB free, internal1024MiB/min_names2000. Preserve failures.
Root alone builds/runs; pool3 reviewsadapter, pool4 inventories existing stock
conversion support, pool5 source frozen available for fixes. No C:/atx writes,
warehouse writes, push, broad build, long performance workload or goal success.

## Latest checkpoint: native qualified; first real refusal preserved

At clean source497f567e all19 focused native cases pass (0.918s native,
1.062s wrapper). Initial18/19 DSL path failure corrected by test-onlyd8cc7b65.
Initial build88.873s Jobs3; fixture-only13.521s, warm PCH/dependencies retained.
Detailed source/import/evidence index:
.superpowers/sdd/strategy/2026-09-26-native-and-first-rehearsal.md.

First real run build-equity/recent-dev-rehearsal-v1 failed in6.234s/208MiB on
momentum_12_1_s21 positive orientation: missing/guarded held return. One real
trial attempted, zero completed, no Sharpe result. Q2/2023-24/2025+ not read.
Pool4 diagnostic sourcee2b97317/fixturee53d13a3 ready for review; preserve
strict behavior and identify the exact data issue. No future filtering.

Reuse accepted cache build-equity/recent-projection-v1. TRAIN role
recent-dev-smoke-v1 manifest900839a1ea8e21edc0f5edd5e9cd8f2bc7884a9295a86d5d6f2de19a79aed36b
is461x3950 with399warmup/62scored and2872..3000 eligible names/day.
Development check recent-dev-check-v1 manifest2129ed162ca3eb1e2bce3bd74f456b83ea11187c26729d06712677bbb6848e84
is524x4101 with461warmup/63scored. Cached prep6.391s/7.359s.
No source rescan needed. Frozen24 expressions,6families,1/24 contributions,
primary cadence5/fraction.25, diagnostic fraction1; TRAIN-only signs.
Budget52 completed evaluations per stage,104 across rehearsal/full experiment;
all failed/retried attempts additionally recorded. First admission min_names2000,
workspace1024MiB, external180s/RSS1536MiB/free768MiB; fixed$1bn cost scenario.
2025+ reserved. Current stock-type/source-vintage qualification remains open.

Pool3 masks audit7a69b22a->3f9e1749; dated stock-type inventory3614788d ready.
Pool5 source review5f03b4df->e86a7bd0 and fixture fix complete. Pool4 source
report30f25f26->53163fbd and strict diagnostics ready. Root alone builds/runs.
No active compilation or numerical process at this checkpoint.

The sections below are historical checkpoints, superseded where indicated.

Owner pivot and confirmed$1bn NAV supersede fullsprint/pre2020scope. Activeplan docs/plans/2026-09-26-mega-alpha-strategy.md. RootHEADbeforethischeckpointcf252c5b. Rootonlybuilds, pool2ownedtree, noC:/atxmutation/push/warehousewrites. No compiler/runtimeactive.

Sourceintegrated: executioncadence54b4ec99->34fcac6b, libraryeeb76477->cf252c5b, boundedrunner257ffd3b. Nativequalificationpending. Runtimeguardshortsuccess andintentionaltimeoutpass; JSONreceiptcopiedhere. Initial24candidates sixfamilies, primarypartial25every5sessions, fixed50TRAINtrialbudget, $1bn NAV, lowturnoverworkingtarget30%monthlysumabsfills/NAV. No recentperformancevalues read.

Pool3implements new prepare_recent_research.py andstrategy_data.hpp/privateCPP: fivecolumnstreamingParquet?privateDuckDBcache/sort?roledata withlagged63rawUSDADVtop3000,separatepresence/membermasks, boundedmemory/disk. Source3.6GBParquetfooter2012-03-26..2026-09-18,32mrows71fields. No stocktype/listingproof, onlyresearchliquiditycohort untilresolved. No sourcepayloadrun authorizedtoagent; rootreviewsandorchestrates. Warmup2018-06-01/2021-06-01; require63+320validwarmupsessions.

Pool4implements strategy_runner.hpp/.cpp andstandalone tools/equity_strategy.cpp: actualDelayedSurfaceV2/B1markedbook, train-onlysigns, fixedfamilyweights, modelcosts/borrow+$1bncapacity, explicitRAMadmission. Handlescadencefixtures. RootownsCMake/source-localJSON. CandidateVMmustuseD12asofcrosssectionmaskinsideallhistoricalranks.

Pool5libraryfrozen; addsnewtests/strategy_dsl_test.cppactual24VMcausality/readycheck andcompanioncombinationrecipe, thenreviewsboundedrunner257ffd3b. No morecoarsescreenresearch.

OldD4 andresidualcoarsescreensourcepacketsremainunimported/uncompiled; donotaccidentallycherry-pickagentdependencyalignmentmerges. D4latest8eed1323/d5c4f393/c9ba9a60/fixturescd886b07/d37cc7fd/reportc92ddc5b; screen0fa86097/adb38ab0/79a482a2/reviewe2e15b58/addendum8e2e20fa. Notcriticalforfirstensemble.


## Integrated recent data and focused build registration
Root8bb91e82 includes adapter5f3b6631->c4a9f015, fixturese5f9630f->afcef6b6, resourcefix70dd7213->a304139f; guardreview05d2ba51->3c07ceee. Producer4Pythonchecks pass at8bb91e82, native1.493s/wrapper2.734s/sampledtreeRSS117850112bytes, receipt build-equity/recent-data-python-qualification. No actualpricepayloadread. Agentreport5fb2c6f0 copiedexactly as report/metadata only (its unrelatedcoarsescreenreviewaddendum notimported).

Combinationrecipe67a2b70e/056ea973->1ca79330/b6d8c52f, finalSHA425df0171096357b33efdf1b80d18150317e095f1d0f5cfb271ea710cc643a13. Native24DSLfixture1b3b3936/a7b52a78->46a3e5eb/0709f70f. CMake8bb91e82 registers strategy_data.cpp withsource-localJSON andone focused atx-impl-strategy-tests target usingexistingimplPCH withDSL/data/executionfixtures. NO build started. Pool4runner/cadencefixturesstillpending.

Guardfix11afd431 retains observedchildidentities afterparentexit andperprocessRSSerrors/prelaunchfloor. Finaldescendanttimeguard stoppedall3observedownedprocesses in2.203s. Initialassertionexpectedexactly2processes; correctedassert>=2againstsamereceiptpassed. Scopeissampledoperationalguard, notOSsandbox; detachedunsampledchildrenunsupported. No activeprocessleft.

Pool5independentlyreviewsadapter beforeactualprojection; rootreviewofsource/fixes isclear. Pool3nowindependentlyreviewscadence54b4ec99 andavailableforproducerfixes. Pool4newstrategy_runner.hpp/CPP +tools/equity_strategy.cpp inprogress, usesD12maskedEngineandsharedexecutiondirectlywithoutmineheader. Rootnext: finishreview, firstboundedrealprojection(2018-06..2025exclusive), codebatchreview/compile andnativefixtures. Stillno achievedSharpe/turnover/commonstockqualification.
