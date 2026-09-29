# v7 pre-registration (platform-20260928) -- declared 2026-09-28 before any L4 measurement

Baseline: v6.1 final cell mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 (S2 net +1.239, TRAIN 2020-2022,
cross-cell N = 29). Statistics protocol: v4-prereg.md unchanged (paired dSR with Memmel SE, CBB, LW; cross-cell DSR and
Lo single-cell; R6' mechanics gate gross in [.90, 1.05], |net| <= .02, tau mean <= .20 / p95 <= .30; freeze needs S2 net
>= 1.0 AND mechanics AND DSR >= .95).

## Descriptive (no trial)
- Risk model atx-risk-v1 exposures/covariances and the bias harness on random, factor-mimicking and the v6.1 book
  portfolios. Reported, not used for acceptance.
- Cost scenarios S2-KO and S2-FIM on the v6.1 cell's planned trades; capacity curve at NAV multiples {.5, 1, 2, 4, 8}x.
  S2 stays primary. No cell is chosen on these.
- Release-build identity re-runs and research_cycle.py reproduction runs: identity checks, not trials.

## Construction trials: aim-partial-v6 (R2.2 + R2.3), library v6.1, ew-theme-v1, L 1.247 fixed, same flags as baseline
Grid of exactly three cells (cross-cell N 29 -> 32), parameters from the literature, none tuned on TRAIN:
| cell | kappa (cost shrink) | band b | rate clip |
|---|---|---|---|
| C1 | 0.5 | dust-equivalent (median band = dust .1) | [.5, 1.5] |
| C2 | 1.0 | dust-equivalent | [.5, 1.5] |
| C3 | 1.0 | dust-equivalent | [1, 1] (no regime rate) |
Acceptance of a cell over the baseline: paired S2 net dSR > 0 (sign-only inside one SE, as in v6) AND mechanics AND
cost per dollar not higher; PBO over {baseline, C1, C2, C3} reported (CSCV, 16 blocks) and a winner is accepted only if
PBO <= .2. Effective-N DSR (ONC clusters over the trial ledger) reported beside the cell-count DSR from v7 on; the v6
gate is not re-scored.

## Identity requirement
aim-partial-v6 with kappa 0, band = dust, clip [1, 1] must reproduce aim-partial-v5 bit-for-bit on the v6.1 cell before
any C1-C3 cell is run.

## Trial accounting (Appendix A) after this grid
TRAIN construction cells: 29 + 3 = 32. Validation trials: 2 spent (unchanged). 2025+: reserved.

## Addendum A1 (2026-09-28, after the C1-C3 read) -- disclosed deviation, R1 finding M-4
The implemented aim-partial-v6 rule rescales each side (long, short) back to its entry gross after the cost shrink
target_i = aim_i / (1 + kappa c_i/c_bar) (strategy_cost_v2.cpp:236-250). This step was not in the L4 brief or in the
pre-registration above, and the C1-C3 cells were run and read with it. Disclosure: the results (all three rejected, dSR
-0.08 to -0.13) were therefore obtained under a rule that redistributes weight from expensive to cheap names at constant
gross rather than shrinking gross. No cell was accepted, so no acceptance rests on the undisclosed step. Any future
aim-partial-v6 cell must state whether the side rescale is on, and a cell without it counts as a new construction trial.

## Construction trial: spo-v1 (declared 2026-09-28 before any run; cross-cell N 32 -> 33)
One cell, `mega-nav-v61u-spo-v1-L1.247`: library v6.1, ew-theme-v1 weights, role lo1, fields v7, same flags as the v6.1
baseline (cadence 1, order basis delta, exit rate .05, locate-in-aim, neutralize price-risk-v1, L 1.247) with
`--rule spo-v1` and the literature defaults from task-W1-brief.md / task-W1-report.md, none tuned on returns:
risk model = atx-risk-v1 exposures/covariances per date (build-equity/v7-w1-risk-all, pinned by sha); ic-book .02
(Grinold-Kahn scaling constant); w-max .01; adv-cap-q .05; adv-trade-p .01; spo-iters 500; spo-tol 1e-8; target-vol
.05; gamma = max(gamma for the 5% ex-ante vol aim, gamma for an aim at gross 1.247) with the gross constraint expected
to bind (ex-ante vol then ~1-2%); trade costs amortised over 1/theta = 20 sessions; books = primary (S1 + S2; S2 is
identical with all five books). Diagnostics per date: a'w, trade cost, TC, ex-ante vol, iterations.
Acceptance vs the v6.1 baseline: paired S2 net dSR > 0 (sign-only inside one SE) AND R6' mechanics AND cost per traded
dollar not higher; PBO over {baseline, C1, C2, C3, spo-v1} reported; effective-N DSR beside cell-count DSR (N 33).
Identity requirement before the cell: flag-off (aim-partial-v5) outputs byte-identical to the v6.1 cell on the v7-w1 exe.
Any further spo-v1 cell (gamma override, other books, other ic-book) is a new construction trial.

## Library v7.0 = library v6.1 + wave 1 (declared 2026-09-28 before any IC pass; by reference)
Pre-registration text: `.superpowers/sdd/platform-20260928/library-v7-draft.md` §0 (conventions) and §1 (wave 1) as
committed at f43e54d9. Candidates, in the draft's order: qmj_safety (low_risk), nincr (earnings_momentum), q5_eg
(investment_issuance), smax5 (low_risk), res_mom_ind (price_momentum); 5 admission trials. Rulings: q5_eg's six extra
fields are allowed (fields v8 = fields v7 + those six; every v7 payload byte-identical via --reuse); the generator roster
cap rises 48 -> 56; theme `ownership_flow` is added to the fitter's theme list now (empty in v7.0; used by wave 2).
Admission v4-prior-v1 unchanged (prior sign, |rho| > .90 redundancy, tau .70); composition ew-theme-v1 unchanged;
the v6.1 members must be byte-identical in the generator output. One composition cell (fit) and one construction cell
(the v6.1 final construction, aim-partial-v5, L 1.247 fixed) -> cross-cell N 33 -> 34 (after spo-v1). Acceptance of
v7.0 as a whole: P2 paired S2 net dSR > 0 vs the v6.1 cell (sign-only inside one SE) AND mechanics; otherwise wave 1 is
rejected whole and the draft's ranking is not re-tuned on TRAIN. Admitted-but-losing members are disclosed, not dropped
one by one. Wave 2 (§2 of the draft minus eap_8k = 4 trials: ins_opp, inst_best_ideas, ftd_fail, ea_overdue) is
pre-registered only when the W5a/W5b fields exist and their semantics are checked against the draft; not before.
Root checks before the run: W2 op spellings in the DSL; q5 slopes against the RoF 2021 version (the draft cites the NBER
WP); if a slope differs, the RoF value is used and disclosed.
Correction (2026-09-28, before any run; from L7): the draft's "six extra fields" for q5_eg means the candidate reads six
existing fields-v7 fields (me_company, at, cfo_ttm, ni_q, be_lag1q, grp_ff12), one over the per-candidate budget of five.
There are no new fields and no fields v8; library v7.0 runs on lo1-fields-v7 unchanged. The ruling "q5_eg's six extra
fields are allowed" is read as "q5_eg's six-field read is allowed". max() and sign() are allowed in the checker via
POLICY_OPS entries for nincr. Nothing else in the pre-registration changes.

## W5a field rules (declared 2026-09-28 before any IC pass on these fields; set from field distributions only, no returns)
1. Form 4 joint filings that include a 10% owner are dropped from every `ins_*` field (sponsor block sales are not
   insider signals). 2. `ins_net_buy_ratio` and `ins_opportunistic_net` are NaN outside [-1, 1] (vendor shares_out unit
   defects; 13 cells on lo1 2020-2022). Fields v8 = fields v7 (41, byte-identical via --reuse) + 14 SEC-derived fields.
Ruling 7.0-b (2026-09-28, before any run; from L7's static check): qmj_safety needs 8 estimated peak slots (house budget 7);
allowed as a recorded exception (recipe row), cost ~+52 MB per slot per worker in the u pass (headroom: 1,061 MiB peak vs
1,536 cap). Wave 1 stays 5 candidates / 5 trials. q5_eg slopes: RoF 2021 Table I Panel D (-0.029 / 0.516 / 0.771) are
used; the NBER WP values (-0.031 / 0.530 / 0.802) are disclosed as the draft's source.
