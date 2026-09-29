# Task R2 review: spo-v2 (b63829f0 on 934bb4cd, pool-3), read-only
Verdict: the code can merge. Do NOT run the cell as committed: 2 I, 4 M, 6 m.
Read: the diff, strategy_spo.{hpp,cpp}, strategy_nav_v7.cpp, strategy_risk_model.{hpp,cpp} (F2 leaves the specific path
untouched), the spo-v1 cell's summary/extras/diagnostics head, and the TRAIN risk pin v7-w1-risk-all (read-only numpy).

## Findings (severity, file:line, one line each)
- I-1 strategy_spo.cpp:1123 (`p.gross = aim_leverage - gross_fixed`). spo-v2 makes the gross cap bind on purpose,
  so planned gross equals L. Realized mean gross followed planned in v1 (.6453 vs .6448), so it would be ~1.2 at
  L 1.247. R6' is "all-rows mean gross in [.90, 1.05]" (v4-prereg.md:84,131), so the cell fails mechanics by
  construction. The 1.247 was C4's L = 1/mean_gross for a partial-trading aim. A hard cap needs L = 1.0.
- I-2 The risk model is corrupt at the source, and F2 does not touch it, so pin 17f9328f has the same defect:
  - The input: one asset_growth outlier (instr. 4550, at/at_lag4-1) from 2020-05-12.
  - strategy_risk_model.cpp:47-75: `standardize` winsorizes at mu +-5 SD, where the SD is computed with the outlier
    in. The outlier's z clips to 3. Every other z collapses (cross-sectional SD .63 -> .047).
  - :133: the style-inclusion threshold (mass > 1e-12 wsum) keeps the near-degenerate column.
  - :605-617: the structural ln-sigma WLS extrapolates exp(3 f_ag) to 4550, whose history is < 252 (blend :626).
    Its D goes 1.1e-3 -> 3.9e12, then back to 8e-4 on 2020-06-10, when its history reaches 252.
  - :632-657: the cap-weighted size-decile shrinkage target spreads the blow-up to the whole decile. On 05-12, 176
    names sit at an identical 1.48e4 (53.5 by 06-09). These are the "177-179 names".
  - The ag style is a one-name dummy on 259 dates in 5 episodes, 2019-11..2022-11 (~226 TRAIN sessions, including
    the calibration date 2020-01-02). Profitability is one for 19 dates (2021-11/12).
  - F2 bias statistics are affected: the style_asset_growth and profitability series are not factor tests in those
    windows, and the random family is understated in the 20-session window.
  - Fix it and pin a new model before trial #2.
- M-1 strategy_spo.cpp:953,983: `--specific-ceiling 1.0` hides I-2 rather than fixing it.
  - The clamp feeds the alpha (a ~ sqrt(D)): the 177 names get ~24x alpha at D ~600x the median.
  - The decile is still dumped for 20 sessions and re-bought on 06-10.
  - The decile is dumped because its risk grows faster than its alpha (weights ~25x smaller).
  - The beta market variance is still distorted.
  - The shadow alpha_capture is inflated in the window.
- M-2 strategy_spo.cpp:1044 (`alpha_h = H`) ties the signal's IC horizon to the trade rate (H = 1/theta). A theta
  change would silently rescale alpha. The literature's ICs of .02-.04 are 21-session ICs (literature-v7.md:67).
  Declare h on its own.
- M-3 The "5% vol target" is nominal once gross binds.
  - The cost-free aim at gamma_vol has gross ~6.
  - A synthetic diagonal check (N 1,797, GK alpha, h 20) gives the book at L 1.247: ~850 names, selected by
    sigma|z| > mu/c (a tilt to high specific vol), and ex-ante vol ~2.0-2.3%.
  - w_max never binds: max w <= c^2 z_max^2 / (4 gamma mu) ~ .004.
  - Expect the "cost/$ not higher" test to be at risk: the tilt is to high-sigma, lower-ADV names.
  - Ledger correction: `exante_vol` is annualised (strategy_spo.cpp:1165). v1's .0066 is .66%/yr, not "10% ann".
    Against realized 1.49%, the risk model under-forecasts the v1 book 2.3x (strategy risk, Qian-Hua 2004).
- M-4 Test gaps:
  - No digest pins spo-v1's planned weights; the "bit for bit" claim is by reading only.
  - No v2 replay test asserts gross_binding across decisions or planned gross equal to L. SpoCalibration is one
    solve from flat, and SpoHook.SpoV2 uses w_max .5.
  - No test that the clamp changes alpha.
  - The corrupt-D test models one name, not the decile shrinkage.
  - No risk-model test covers standardize with one extreme outlier, a near-degenerate style column in
    regress_cross_section, or decile-target robustness.
- m-1 spo-v1 is bit for bit in planned weights and the daily/events CSVs only. recipe_sha, v7_extras (new keys,
  declaration text) and spo_diagnostics.csv (6 new columns) change. The identity check must compare the right files.
- m-2 strategy_nav_v7.cpp:180,289: spo-v2's recipe and extras blocks are still keyed "spo_v1".
- m-3 strategy_spo.cpp:813-814: gamma_bind is still solved under the Vol rule, and a failure of that report-only
  root aborts the run.
- m-4 strategy_spo.cpp:825: the refusal is unreachable on TRAIN.
  - The cost-free aim depends on a/gamma only, and v1 reached vol .05 on 2020-01-02.
  - So gamma_v2 = 220.27/sqrt(h): 49.3 at h 20, 48.1 at h 21.
  - A box-corner book (w_max .01 x 1,797 names) has vol >> 5%.
- m-5 The shadow is plan-level aim-partial-v5 (no drift, full fills), not the v6.1 book: alpha_capture and
  trade_cost_ratio are measured against a proxy. It also follows --aim-leverage, so it moves with I-1's fix.
- m-6 A uniform 1/sqrt(h) understates the next-session alpha of fast sleeves. R2.1's per-sleeve (1+19 phi)^-1 is
  not implemented; declare this as a known simplification.

## Answers
Q1: The three causes explain spo-v1:
- gross .645: gamma = gamma_bind 1575 >= gamma_vol 220, and costs pull the book inside the budget.
- tau 12%/day: per-session alpha was sqrt(20) = 4.47x too large against the amortised cost.
- a'w 3.6x realized: 2.68e-4/session ex-ante vs ~7.3e-5 realized; the fix predicts ~0.8x.
- net -0.94: gross ~+5.5% over 3 y, less S2 trade cost 8.8% and financing ~1%, at vol 1.49%.
- The defect also moved the v1 book. The EW-market specific variance was 1.2e6, so the beta constraint was void for
  20 sessions, and the decile was liquidated.
- The GK arithmetic is right: per-session a = IC_h sigma_1 sqrt(h) z / h. Alpha, risk (daily Sigma), amortised cost
  and financing are all per session. The only inconsistency is M-2.
Q2:
- The gross cap binds after the deployment ramp (the aim sits ~5x outside L), and the refusal is unreachable (m-4).
- The book is described in M-3.
- R6': gross FAILS at 1.247 (I-1); |net| passes (net 0 is enforced). Tau mean <= .20 is plausible, since the cost
  weight is 4.47x v1's. p95 spikes only at the decile dump and re-entry.
Q3: Clamping hides the defect (M-1). The source and the F2 impact are in I-2. The risk fix and a new pin should
precede trial #2.
Q4: Yes, by reading:
- The v1 path is unchanged: /sqrt(1.0) is exact, the inf ceiling is a no-op, the linear/impact split is exact,
  calibrate_gamma does the same operations, and the fixed_exposure reset is a no-op on zeros.
- The shadow runs after the solve and touches no planned weight.
- The flag-off path is untouched: hold() is called only with an engine, and claims_nav_args is unchanged for v5.
- Output bytes differ (m-1).
Q5: M-4.

## Proposed pre-registration: "Construction trial: spo-v2 (spo trial #2)" -- [CHG] marks a change vs the commit
One cell, `mega-nav-v61u-spo-v2-L1.0`. Base: library v6.1, ew-theme-v1, role lo1, fields v7, and the v6.1 baseline
flags (cadence 1, delta orders, exit .05, dust .1, theta .05, locate-in-aim, price-risk-v1), with `--rule spo-v2`:
- Risk model: atx-risk-v1 rebuilt with I-2 fixed (robust standardisation, no structural extrapolation outside the fit
  range, robust decile target). New pin, sha declared before the run. [CHG vs F2 pin 17f9328f, which has I-2.]
- Gross budget 1.0 [CHG vs 1.247]: spo's budget is a hard cap on planned gross, and R6' tests realized gross in
  [.90, 1.05].
  - Preferred: a budget-only flag (e.g. `--spo-gross 1.0`), so the shadow keeps L 1.247.
  - Otherwise `--aim-leverage 1.0`, disclosing that the shadow is then v5 at 1.0.
- ic-book .02: Grinold-Kahn; the low end of the 21-day ICs .02-.04. [Keep.]
- `--alpha-horizon 21`: the IC's measurement horizon, so a_i = .02 sigma_i z_i / sqrt(21). [CHG vs the implicit
  h = H = 20; alpha -2.4%.]
- spo-horizon H = 1/theta = 20: the GP rate equivalence, with Boyd's gamma_trade = 1/H. [Keep.]
- gamma = gamma_vol at target .05 on the first rebalance decision's cost-free aim (R2.1: 4-5%); expected ~48.
  Declared nominal under the cap. [Keep.]
- w-max .01, adv-cap-q .05, adv-trade-p .01, iters 500, tol 1e-8, |beta| .02, books primary: R2.1 and W1.
  [Keep.]
- specific-ceiling 1.0 as a tripwire: capped_specific_decisions must be 0 on the new pin, else the run is void. The
  root reads that column before any return. [CHG in meaning.]
- Expected mechanics (synthetic, not returns): gross binding on >= 90% of post-ramp decisions, ~45-50% of members
  held, w_max binding ~0, ex-ante vol ~1.5-2%/yr.
Identity requirement, before the cell and not counted as trials:
- (a) Flag-off aim-partial-v5 outputs are byte-identical to the v6.1 cell on the new exe.
- (b) `--rule spo-v1`, with its pre-registered flags on pin 897ffdf2, reproduces the rejected cell's daily_*.csv,
  events_*.csv and spo_diagnostics.csv columns 1-38 byte for byte.
- (c) The new pin's bias harness, max daily D and minimum style dispersion are reported.
Acceptance vs v6.1 (mirrors spo-v1): all of the following must hold.
- Paired S2 net dSR > 0 (sign-only inside one SE).
- R6' mechanics: all-rows mean gross in [.90, 1.05], |mean net| <= .02, tau mean <= .20, p95 <= .30.
- Cost per traded dollar not higher.
Also reported: PBO over {baseline, C1-C3, spo-v1, spo-v2} (CSCV, 16 blocks) and effective-N DSR beside cell-count DSR
at the ledger N.
Further trials: any completed run that changes a flag (gamma override, budget, target vol, ic-book, either horizon,
ceiling, books), the risk pin, or code that moves a planned weight is a new construction trial. Bit-identical identity
reruns are not, and neither are runs refused or voided before any return statistic is read.
