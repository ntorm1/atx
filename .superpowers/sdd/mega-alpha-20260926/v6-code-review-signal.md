# v6 code review: signal side (data -> DSL -> admission -> composition -> desired target)

Reviewer: Opus 5.5 (read-only). Tree: `C:/atx-wt/pool-2` @ `b1887951`. Nothing was edited, built or run. The only
data read was TRAIN metadata already on disk: `build-equity/mega-weights-v5-aim/composition_weights.json`
(report-only coverage) and `build-equity/recent-fast-train-2020-2022-v2-fields-v6/manifest.json` (link counters).
No 2023+ file was opened.

Result: **0 Critical, 5 Important, 7 Minor.** The code has no look-ahead defect. The fundamentals clock, SIC clock, FINRA
as-of join, membership and fill timing are all point in time (all 40 fields-v6 entries are `point_in_time: true`). The
problems are in what the signal measures and who it trades:
- 39% of the book's member cells are unlinked instruments that only get price, short-interest and IV signals.
- The low-risk theme is largely projected out by the price-risk-v1 regressors.
- The member-masked rank combined with full-window `decay_linear` blanks every signal for 21 sessions after any
  membership gap.
- With no redistribution, the realized theme balance is set by coverage, so it is not the pre-registered 1/9.

## 1. Findings (ranked)

| # | Sev | file:line | Defect | Failure scenario | Fix sketch | Net SR |
|---|---|---|---|---|---|---|
| I1 | Important | `atx-engine/tools/prepare_recent_research.py:344-353,408-411`; `prepare_research_fields.py:1710-1720`; `build-equity/recent-fast-train-2020-2022-v2-fields-v6/manifest.json:4582-4584`; ledger `progress.md:350` | **Universe is not an operating-company universe.** Membership is the top 3000 by prior 63-day $ADV (`common_stock_verified: False`). On TRAIN, 1,269,334 of 3,220,647 member cells (39.4%) have no CIK link (ETFs/ETNs, SPACs, ADRs, preferreds, warrants, unbridged names). For those names every fundamental and every `grp_*` candidate is NaN (coverage .21-.58, `mega-weights-v5-aim/composition_weights.json:18-56`). Their composite holds at most .445 of the weight mass: SI .111, IV .111, low-risk .083 (3 of 4), mom .056, seasonality .056, ear .028. | Unlinked names (ETFs included) are ranked mostly on ETF short-interest and vol ratios. ETF short interest comes from creation/redemption mechanics, not informed shorting. Those names still use gross, borrow and cost budget, and they sit in the price-risk OLS with leveraged/inverse-ETP betas. | Pre-register a universe revision: `member &= linked-P & finite(grp_ff12)`. Both inputs are already PIT fields in fields-v6. Re-run u/w/fit/NAV on TRAIN. | unknown; + plausible (see L1) |
| I2 | Important | `atx-impl/strategies/generate_fund_ic_v4.py:560-577` (low_risk DSL); `atx-impl/tools/fit_composition_weights.py:21-28,784-801`; `atx-impl/src/strategy_price_exposures.cpp:375-376,387-414`; `strategy_target_replay.cpp:304-313`; scorecard `2026-09-27-mega-alpha-scorecard.md:79-82` | **The low_risk theme is structurally collinear with the neutralizer.** price-risk-v1 regresses the rank target on z(beta252 vs EW market) and z(vol63), then keeps the residual. low_beta, low_ivol, low_max and lowvol_ind are rank(-beta) and rank(-vol) variants, so the admission factor and the book keep only the nonlinear, estimator-difference residual (a vol-curvature bet). All 4 members have negative TRAIN HAC t (-.62, -1.90, -.46, -1.89); two sit just above the -2.0 veto (`fit_composition_weights.py:149`). | About 13.5% of the ew book's effective signal mass (derived below) goes to a residual that has no literature prior. The prior sign was justified for the unneutralized anomaly. | Pre-register one of: drop the theme; or keep low-risk exposure outside the neutralizer (e.g. drop z_vol for this theme). Justify it structurally (theme vs risk model), not by TRAIN t. | + (bounded by the theme's share; the magnitude cannot be computed without a run) |
| I3 | Important | `atx-engine/include/atx/engine/alpha/vm.hpp:425-441`; `ts_ops.hpp:25-26` (+ `ts_sliding.hpp:48-49`); `atx-impl/src/strategy_ic_runner.cpp:1605`; `generate_fund_ic_v4.py:58,613-618` | **21-session signal blackout.** Every Cs op emits NaN for non-members (mask = `decision_member`). Every candidate is `decay_linear(rank(.),21)`, and ts ops use full-window, any-NaN -> NaN. A name therefore has NO signal from any candidate until it has been a member for 21 consecutive sessions. Evidence: price-only candidates top out at coverage .959 (low_ivol/low_max), dtc .958, mom .922 (`composition_weights.json:18-56`). | About 4% of used names per day (entrants to, and names flickering at, the top-3000 ADV boundary) get composite 0, then a delayed jump 21 days later. A change to the membership rule (top_n, ADV floor, a VAL-period role) silently changes coverage. No gate reports it. | Put the smoothing outside the mask: `rank(decay_linear(x,21))` or `group_rank(decay_linear(x,21), g)`, so the ts op sees unmasked history and only the final Cs op is masked. The alternative is a min-periods decay. This is a library revision (disclosed trials). | small +; upper bound about 4% of names (≈ .04 x gross SR 1.17 ≈ .05 gross) |
| I4 | Important | `atx-impl/src/strategy_ic_composition.cpp:141-156` (no redistribution); `fit_composition_weights.py:1230-1237`; `composition_weights.json:7-17` | **The realized theme weights are coverage-weighted, not 1/9.** A missing candidate contributes 0 and nothing is redistributed. Themes with ~95% coverage (SI, IV, low_risk, momentum) therefore dominate the cross-section, and fundamentals (~50%) are diluted. Derived from the published TRAIN coverage_mean x w_ew: SI .153, options .145, low_risk .135, reversal .120, momentum .119, earnings .093, investment .090, profitability .075, value .069 (nominal .111 each). The aim fitter's own report shows the same thing for W_aim (low_risk .158, SI .153). | The two themes whose members all have negative TRAIN HAC t (low_risk; SI dtc/si_change/si_ratio -.56/-.91/-1.27, scorecard `:98-100`) carry 28.8% of the effective mass instead of the nominal 22.2%. Any coverage change (a U3 bridge expansion) changes the effective strategy without a weight change. | Fix it at the source (I1). Otherwise pre-register a coverage-balanced rule: theme weights scaled by 1/c_theme, where coverage uses no returns. Warning: TRAIN t is now known, so any rule that happens to cut low_risk/SI will look better on TRAIN than it deserves. | unknown |
| I5 | Important | `generate_fund_ic_v4.py:58,592-609,613-618`; `strategy_target_replay.cpp:192-222` | **Double smoothing of fast signals.** Every candidate is `decay_linear(.,21)` (mean lag 6.7 sessions, `generate_fund_ic_v4.py:600-604`), and aim-partial-v5 then moves at theta .05 (mean lag about 19 sessions), on top of the 1-session fill delay. ind_adj_rev_5 (tau .181), iv_rv_spread (.090), si_change (.092), seasonality (.094) and low_max (.089) are fast-alpha signals held in about 4-week-old form. GP places the smoothing in the trading rule, with the current signal in the aim. | Fast alphas decay before they are traded. The aim result (down-weighting fast signals lost 0.10-0.29 SR) says these signals do carry alpha net of cost at theta .05. They are delivered stale. | Pre-register an s1/s5 variant for the tau >= .08 group (theta keeps book turnover bounded). This is a library revision: new candidates = disclosed admission trials. | + gross plausible, - cost; net unknown |
| m1 | Minor | `fit_composition_weights.py:724-725` | Forward return NaN -> 0 (delisting, halt). Admission HAC t, redundancy rho and aim all ignore terminal returns. This is consistent with NAV eta = 0 (S3 -0.10). | Longs that delist look neutral. This biases the value/distress admission statistics upward. | Keep; add an S3-style terminal-return stress to the admission statistics. | 0 (disclosure) |
| m2 | Minor | `ts_ops.hpp:25-26,100`; DSL `high_52w`, `low_beta`, `lowvol_ind` (`generate_fund_ic_v4.py:556-577`) | 252-session full-window NaN: a single missing close/return makes high_52w (coverage .900) and low_beta (.900) NaN for 252+21 sessions. | A vendor gap or halt removes a name from 3 candidates for about a year. | Min-periods windows (engine policy variant). | small |
| m3 | Minor | DSL returns `close/delay(close,1)-1` (`generate_fund_ic_v4.py:419`) vs guarded returns in `strategy_price_exposures.cpp:130-136` | DSL returns are unguarded (no abs-log > 1.5 or adj-vs-raw check). The neutralizer's returns are guarded. | One bad adjusted print tops `low_max` (`ts_max(ret,21)`) and distorts stddev-based signals for 21+21 sessions. The role v2 factor-break repair covers the 2021-01-04 break only. | Guard in the DSL or in the role. | small |
| m4 | Minor | `prepare_research_fields.py:266,1721-1726` | Fundamentals use `--fund-lag-sessions 1`: visible only if accepted before the mark of d-1, then filled at close d+1. That is 2 closes after acceptance. L=0 is still PIT under the house definition (`POINT_IN_TIME_DEFINITION`, accepted < d 22:00 < 23:00 decision). | sue/droe/chtax/opex enter one day later than necessary, and are then blunted again by s21 decay. | Pre-register L=0 (fields rebuild, TRAIN). | small + |
| m5 | Minor | `prepare_research_fields.py:179` (iv caveats); `generate_fund_ic_v4.py:606`; scorecard `:83` | iv_rv_spread, the largest single weight (w_ew .111, one-member theme), uses the vendor "clean" IV. Its earnings component is removed with a forward-looking vendor earnings calendar (vintage unproven), and in-domain garbage cannot be detected. Clean IV minus RV21 (which includes the earnings day) is also mechanically low for 21 sessions after every announcement, which works against PEAD. | A calendar-vintage leak or a vendor methodology change moves 11% of the book, with no in-theme diversification. | Cap single-member themes, or add iv_atm_63d as a second member. Disclose the IV vintage. | unknown |
| m6 | Minor | `generate_fund_ic_v4.py:443-469` (`positive()` guards on cfp/ep/sp/ebit_ev/rd_me) | cfp excludes negative operating cash flow (-> NaN -> neutral), so cash-burning firms are not shorted by value. fcfp and net_payout keep negatives. | Weaker short leg of the value theme. | Keep negatives for cfp (rank lowest), as fcfp does. | small |
| m7 | Minor | `fit_composition_weights.py:1011-1016,1019-1037` | The aim gain uses the rank autocorrelation of the already-smoothed (s21) signal as the GP alpha-persistence proxy. This double-counts the smoothing of I5 and is not the alpha term structure. The formula itself is correct: theta * sum (1-theta)^j rho(j) = 1/(1+phi(1-theta)/theta) for AR(1). | ew-theme-aim-v1 was already worse; there is no action unless aim is revisited. | If revisited, use rho of the unsmoothed base signal. | 0 |

Verified clean (no finding):
- **Centered tied ranks:** Python `fit_composition_weights.py:618-642` equals C++ `strategy_ic_composition.cpp:23-34` and
  `strategy_target_replay.cpp:155-176`, with average ties.
- **HAC:** Newey-West Bartlett weights `1-l/(L+1)`, divisor n, demeaned (`:1155-1166`).
- **Neutralization:** equilibrated Cholesky with one refinement step, and a refusal when the target is spanned
  (`strategy_price_exposures.cpp:284-355`).
- **Signs:** all 38 prior signs match the cited literature direction.
- **Pinned weights and signs:** fully SHA- and TRAIN-bound (`strategy_ic_runner.cpp:807-840,1767-1771`).
- **Timing:** fill at d+1 and return d+1->d+2 (`strategy_nav_replay.cpp:992-995`), matching fitter fwd r[d+2].
- **Fundamentals:** FSDS acceptance clock, latest-clock-wins, restatements never backdated
  (`fundamental_events_schema.md` §2).
- **Company ME:** the multi-line rule has 0 `other_linked_line_nan` cells on TRAIN (manifest `:3970`).

## 2. Levers that could honestly raise TRAIN net SR

All levers below are TRAIN-only tests: no 2023+ read. Each one needs a v6 prereg entry and counts as a disclosed trial.
A signal-side lever of this size is unlikely to close the +0.26 net gap by itself. The cost hurdle is about 0.43 SR at
$1bn S2 (handoff 4 §0).

| # | Lever | Direction | Rough magnitude (evidence) | Cost in trials / work |
|---|---|---|---|---|
| L1 | Universe = PIT-linked operating companies (I1) | + likely gross; costs ambiguous (ETFs are cheap to trade) | Unknown. It removes 39.4% of member cells whose composite is at most 44% signal mass, dominated by the two all-negative-t themes. | Construction/universe revision: u pass + fit + w + NAV (about 2 min root); +1 admission family, +1 composition, +1 construction |
| L2 | Drop or redefine low_risk (I2) | + | Bounded by its effective share (.135 ew); every member has negative TRAIN t. The selection-bias caveat must be disclosed. | Composition-only: fitter theme exclusion (small code change) + w (26 s) + NAV (40 s); +1 composition, +1 construction |
| L3 | Coverage-balanced theme weights (I4) | unknown (shifts mass to value/profitability) | Structural (coverage has no returns), but TRAIN t is now known. It looks better on TRAIN than it will out of sample. | Fitter rule + w + NAV |
| L4 | Smoothing fix: `R(decay(x))` instead of `decay(R(x))` (I3) | small + | At most about 4% of names regain a signal; ≲ .05 gross | New library (38 admission trials) + full u pass |
| L5 | Fresh fast signals (s1/s5 for the tau >= .08 group) (I5) | + gross, - cost | unknown | New library members (disclosed) |
| L6 | fund lag L = 0 (m4) | small + (earnings_momentum) | unknown, small (s21 decay blunts it) | Fields rebuild + u/w/NAV |
| L7 | IC/HAC-weighted or MV composition on TRAIN | **Not recommended.** It raises TRAIN SR in-sample only: 30/31 v4-admitted have \|t\| < 2 (only noa 2.07), and v3 already showed TRAIN 1.81 -> VAL -1.27 | n/a | n/a |

Ordering by value per trial: L2 (cheapest, composition-only) and L1 (structural, fixes I4 as a side effect), then L6.
L3/L4/L5 cost many trials.

## 3. Critical points (most fragile), with a failure scenario

1. **Mask x full-window coupling:** `vm.hpp:425-441` + `ts_ops.hpp:25-26` + `strategy_ic_runner.cpp:1605`.
   - Failure: any membership-rule or role change (top_n, ADV floor, a new VAL/2025 role with different churn) changes
     signal coverage for every candidate at once. No coverage gate refuses it.
   - Nor would the book notice: a 4% blackout becomes 8% with no error.
2. **Issuer-link coverage drives the book mix:** `prepare_research_fields.py:1710-1745`; r4 bridge (T19 concern 3,
   `task-T19-report.md:110`: `link_end` is sometimes asserted before knowledge time).
   - Failure: a bridge scope change (U3) flips hundreds of names from price-only to full composites. Effective theme
     weights (I4) and turnover shift with no weight change. A late-asserted `link_end` is a mild presence leak at
     corporate events.
3. **iv_rv_spread is a single 11.1% candidate on a vendor field:** `prepare_research_fields.py:176-182`,
   `generate_fund_ic_v4.py:606`.
   - Failure: in-domain vendor garbage, missing IV (about half of vendor rows are null; ts_backfill 5), or a clean-IV
     calendar revision mis-signals 11% of the composite. No in-theme diversification absorbs it.
4. **Neutralization guard skip freezes the book:** `strategy_target_replay.cpp:320-330`.
   - When amplification or excluded share exceeds its cap, the rebalance is skipped entirely, not partially applied.
   - Failure: on high-dispersion days (Mar 2020, Jan 2021 squeeze), a rank target largely spanned by beta/vol freezes
     the book exactly when signals move most.
   - The fitter context refused 0 TRAIN decisions (`composition_weights.json` `neutralization_refused_decisions: []`).
     I could not verify the NAV skip counts.
5. **Terminal returns absent everywhere on the signal side:** `fit_composition_weights.py:724-725` (plus NAV eta = 0).
   - Failure: a period with more delistings (VAL, or a longer pre-2020 TRAIN under U2) moves admission statistics and
     book P&L in the optimistic direction for value/distress longs. The S3 K = 1 stress is already negative in 13/13
     cells.

## 4. What I could not verify

- The gross-weight share of unlinked names in the NAV book. I read no per-name weights. Ranks compress their composites
  toward the middle, so the share is below 39%.
- Whether CF-R CompanyFacts keeps every filing's originally reported fact. The restatement PIT claim depends on it
  (producer contract §2 asserts latest-clock-wins).
- The r4 bridge `available_at` provenance (the claims are r4's, not re-derived: T19 concern 3).
- The vendor earnFlag and clean-IV earnings-calendar vintage.
- Why |rho| > .90 among the value factors (bm/ep/sp/ebit_ev vs cfp). It could be the common 1/ME driver or a shared
  neutralization hedge on unlinked names: low-coverage candidates are residualized over all used rows
  (`fit_composition_weights.py:792-800`), so their standalone factors carry hedge legs on names without the signal.
- The actual membership churn at the top-3000 boundary. The ~4% blackout is inferred from the coverage ceiling.
- NAV neutralization skip counts, and every lever magnitude (no runs permitted).
