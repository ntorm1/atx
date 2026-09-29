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

## Library v7.1 = parent + wave 2 (declared 2026-09-29 before any IC pass of any wave-2 candidate and before the wave-1 read; by reference)
Pre-registration text: `.superpowers/sdd/platform-20260928/library-v7-wave2-prereg.md` sections 1-5 as committed with this
paragraph (P4, read-only; field coverage and distributions only, no returns). Candidates in order: ins_opp
`rank(ins_opportunistic_net)` (ownership_flow), inst_best_ideas `rank(decay_linear(inst_best_ideas, 21))`
(ownership_flow), ftd_fail `rank((-1 * ftd_shares_ratio21))` (short_interest), ea_overdue
`rank((-1 * max(sign((-1 * ea_days_to_expected)), 0)))` (earnings_momentum); 4 admission trials; fields-v9 as built
(manifest sha 8fd00e9f...d7b8769b, 63 rows); no budget exception. Two mechanical respellings versus the draft, both decided
by field semantics (ins_opportunistic_net is already per share outstanding; ea_days_to_expected is signed).
Rulings, all declared before the wave-1 result exists:
- Ruling W2-a: parent and reference = v7.0 and its cell if wave 1 is accepted, else v6.1 and the v6.1 cell -- a rejected
  wave must not ride into the next test and the two waves stay separable -- cost if wrong: if wave 1 is rejected for
  noise, its members are lost to v7 (they may be re-proposed only in a later sprint with a new pre-registration).
- Ruling W2-b: ea_overdue keeps the cut `< 0` -- it is the draft's alignment rule verbatim; moving the cut is a variant --
  cost if wrong: .67% of cells carry a one-session false flag from the 8-K acceptance lag (disclosed).
- Ruling W2-c: inst_best_ideas keeps the summed overweight and ins_opp keeps the net measure -- the registered
  hypotheses; a per-holder mean or a buy-only leg is a separate later trial -- cost if wrong: a weaker member that the
  admission rule (prior sign, HAC t veto) or the wave-level rule rejects.
- Ruling W2-d: the 64-row fields cap is not raised for wave 2 (63 rows fit); wave 3 gets a lean manifest -- no C++ change
  inside an open trial sequence -- cost if wrong: none for v7.1.
- Ruling W2-e: the reference cell is re-run on fields-v9 as an identity check (S2 daily CSV bit for bit) before the
  v7.1 cell; a mismatch aborts without a trial.
Cross-cell N for the v7.1 construction cell = ledger lines at run time + 1 (the run order of the v7.0, spo-v2 and v7.1
cells decides the number; each cell adds exactly one). Acceptance as in section 5 of the referenced text: wave whole,
P2 paired S2 net dSR > 0 vs the reference (sign-only inside one SE) AND R6' mechanics; admitted-but-losing members
disclosed, never dropped one by one. Citations to complete before the freeze (JEF 2016 authors): not a variant.

## Universe trial U-lo3 (declared 2026-09-29 before any IC pass, fit or NAV on role lo3; after the v7.0 read)
Parent: the accepted library v7.0 cell on role lo1 (`mega-nav-v70u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`, fields-v7,
ew-theme-v1, final construction, L 1.247). Change, one thing: role lo1 -> lo3 = `linked-operating-v3` (the v2 rule with
the SIC table from the atx-db fundamentals stage, manifest sha 9f9b2f85...06816b; identity-bridge-v2-pit 09aac28f;
delisting attributes only, delisting returns off), and the fields-v7 list rebuilt on lo3 with the same bridge and the
same stage SIC (`--sic-events`), so that finite(grp_ff12) == linked-P and visible SIC holds on every cell.
Held fixed: library v7.0 + recipe, admission v4-prior-v1 and ew-theme-v1 (both refit on lo3, as V6-U did), aim-partial-v5
theta .05 dust .1 fixed rate, delta orders, exit .05, locate-in-aim, liquidity cache, price-risk-v1, L 1.247 (not
re-derived), cost S2, fundamental items from fundamental-events-v2 (74ed9a50), every non-issuer source, TRAIN 2020-2022.
Known before the run (membership counts only, no returns): score-window member cells 1,343,804 -> 1,404,404 (+4.5%);
43,811 recovered cells on 162 lines; those cells carry no fundamental items, so they rank on non-fundamental themes only;
20 cells change FF12 / FF49 label. Prior (V6-U form): gross SR up slightly or flat; cost per dollar ambiguous.
Budget: 1 u pass, 1 composition, 1 construction cell; ledger kind universe; cross-cell N = ledger lines at run time + 1.
Acceptance: paired S2 net dSR > 0 vs the parent (sign-only inside one SE) AND R6' mechanics (all-rows gross in
[.90, 1.05], |mean net| <= .02, tau mean <= .20, p95 <= .30). If rejected, lo1 stays the role and lo3 is not retried
in v7 with another library.
- Ruling U-a: the parent is v7.0 on the fields-v7 list, not v7.1 on fields-v9 -- the cycle does not yet pass the W5a /
  W5b stage arguments on a new role, and the universe question is separable from wave 2 -- cost if wrong: if both U-lo3
  and v7.1 are accepted, their combination is one more cell (V7-F below).
- Ruling U-b: admission is refit on lo3 (members may change status); the comparison is book against book, and the
  admission differences are disclosed -- this is what V6-U did -- cost if wrong: the universe effect and an admission
  flip are confounded in one number; the admission table shows which.
- Ruling U-c: switching the fundamentals source to the atx-db export (which would give the recovered cells fundamental
  items) is a separate data trial and is not part of v7.
V7-F (declared now, run only if BOTH v7.1 and U-lo3 are accepted): one final cell = library v7.1 on role lo3 with
fields-v9 rebuilt on lo3, same construction, L 1.247; acceptance paired S2 net dSR > 0 vs the better of the two accepted
cells AND mechanics; N + 1. If only one is accepted, that cell is the v7 final cell and V7-F is not run.

## Construction trial: spo-v2 (spo trial #2) (declared 2026-09-29 before any spo-v2 run; after R2, W1b, F3)
Source of the defaults: task-R2-review.md "Proposed pre-registration" (adversarial review of b63829f0), implemented by
W1b; none tuned on returns. One cell, `mega-nav-v70-lo3-spo-v2-G1.0`:
- Signals and universe: the accepted book at declaration time = library v7.0, ew-theme-v1, role lo3, fields-v7 list on
  lo3 (combined signal build-equity/mega-v70-lo3w-train-ew-1). Baseline flags unchanged (cadence 1, delta orders, exit
  .05, dust .1, theta .05, locate-in-aim, liquidity cache, price-risk-v1, --aim-leverage 1.247 for the shadow book).
- `--rule spo-v2`; risk model atx-risk-v1.1 on lo3, build-equity/v7-f3-risk-lo3, manifest sha
  786cb601dd4295450872ee0fd726a752b2996f2c3ea886ab6f399f3676e14913 (F3: robust standardisation 3.5 x 1.4826 MAD, style
  validity >= 10 effective names, structural sigma clamped to the fit and its [p1, p99], median decile target, refusal
  outside (0, 1); measured on lo3: max daily D .361, factor b 1.002, random b .949, 0 style-dates dropped).
- `--spo-gross 1.0` (hard cap on planned gross; R6' tests realized gross in [.90, 1.05]); `--ic-book .02` (Grinold-Kahn,
  low end of 21-session ICs); `--alpha-horizon 21` (the IC's measurement horizon; a_i = .02 sigma_i z_i / sqrt(21));
  spo horizon H = 1/theta = 20 (cost amortisation; Garleanu-Pedersen rate); gamma = gamma_vol at `--target-vol .05` on
  the first rebalance decision's cost-free aim (nominal once the gross cap binds); `--w-max .01`, `--adv-cap-q .05`,
  `--adv-trade-p .01`, `--spo-iters 500`, `--spo-tol 1e-8`, |beta| .02, `--spo-books primary`;
  `--specific-ceiling 1 --specific-ceiling-void on` (tripwire: a clamp voids the run with exit 3 before any NAV or
  return file exists; a void run is not a trial).
- Expected mechanics (synthetic, not returns; R2 M-3): gross binding on >= 90% of post-ramp decisions, ~45-50% of
  members held, ex-ante vol ~1.5-2%/yr, a tilt to higher specific-vol names (cost per dollar at risk).
Identity before the cell, not trials: (a) flag-off aim-partial-v5 on the new exe reproduces the v7.0-lo3 cell's daily /
events CSVs byte for byte; (b) `--rule spo-v1` with its pre-registered flags on pin 897ffdf2 reproduces the rejected
spo-v1 cell's daily_*.csv, events_*.csv and spo_diagnostics.csv columns 1-38 byte for byte; (c) the gtest digest pins
(weights 0xda6b6871e7e267c5, replay 0xaabdbb72f99a6e13; captured identical on the pre-W1b base and on the W1b head).
Acceptance vs the v7.0-lo3 cell, all required: paired S2 net dSR > 0 (sign-only inside one SE); R6' mechanics (all-rows
mean gross in [.90, 1.05], |mean net| <= .02, tau mean <= .20, p95 <= .30); cost per traded dollar not higher.
Reported, not gating: PBO over {v6.1, C1-C3, spo-v1, v7.0, v7.0-lo3, spo-v2}; effective-N DSR beside cell-count DSR.
Cross-cell N = ledger lines at run time + 1. Further trials: any completed run that changes a flag, the risk pin, the
signals, or code that moves a planned weight. Not trials: identity reruns; runs refused or voided before a return
statistic is read.
- Ruling spo-a: the cell runs on the accepted v7.0-lo3 book, not on v6.1-lo1 as R2 drafted -- a construction rule is
  judged on the book that would ship, and the reference must be the current accepted cell -- cost if wrong: if v7.1 /
  V7-F is accepted later, spo on that book is one more cell (not run in v7 unless spo-v2 is accepted here).
- Ruling spo-b: if spo-v2 is rejected, the optimiser stays in the codebase behind its flag, aim-partial-v5 stays the
  construction, and no further spo cell is run in v7 -- two spo cells are the sprint's budget -- cost if wrong: a
  fixable second-order defect ends the line for this sprint; it can be re-opened by a new pre-registration later.
