# Review X-5 `theme-erc-v1` (read-only, adversarial)

**VERDICT: SOUND, with qualifications. Confidence about 60% that a positive gain survives out of sample, and about 70% that it is materially below +.35.**
I found no code defect and no look-ahead beyond the declared full-TRAIN fit. The comparison is fair.
The mechanism is a lower book volatility at matched gross. It comes from de-concentrating the parent's risk, which sat in the momentum cluster and in `filing_events`.
That accounts for about +.31 of the +.35. The in-sample part is that the book's return held up when risk was moved to the low-volatility themes. In 2020-2023 those themes were also the high-Sharpe ones.
Under the rule's own equal-Sharpe premise, the expected gain is about +.15.

Everything below was read from files already on disk. The only computation was arithmetic in a scratch Python script on the two `composition_weights.json` files and the two NAV `summary.json` files. Nothing was run in root.

## 1. Look-ahead
- **Factor clock.** f_k(d) = sum_i q_i(d) * r_i[d+2] (`fit_composition_weights.py:308-310`: `fwd=r[d+2]-valid-else-0`, `f=sum(q*fwd)`). The decision is at d, the fill at d+1, and the return earned at d+2. That is the book's clock (the S2 cost model reads session d+1: `task-XCOMB-report.md:21`, the inv-vol row). There is no same-session term.
- **Sleeves.** r_t(d) = sum_k a_k s_k f_k(d), with flat decisions set to 0 (`composition_theme_erc.py:156-168`; the call is at `fit_composition_weights.py:2373-2375`, with `prior_signs*zero_filled` from `:2291`).
- **Covariance window.** The whole of TRAIN, as one constant estimate, not expanding.
  - `train_mask` is decision sessions in [TRAIN_BEGIN, TRAIN_END) (`fit_composition_weights.py:2226`).
  - The sample covariance uses divisor n-1 (`composition_theme_erc.py:171-181`). Provenance gives `decisions` 1004.
  - The first decision is 2020-01-02 and the last 2023-12-27. The last label session is 2023-12-29 (weights `provenance.window`), so nothing from 2024 enters.
  - So every weight used at t depends on returns through the end of 2023. The fit is in-sample by design (PM7-37) and is not a leak of the code.
- **The runner never re-estimates C.** `verify_theme_erc` (`strategy_ic_admission.cpp:599-679`) re-applies ERC and the cap to the *recorded* matrix (`:615-639`, `:663`). It checks weights to 1e-12 (`:672`).
  - Provenance of C is therefore trusted to the fitter and the weights sha pin.
  - This is also what makes a carried-back or a split-sample covariance usable without runner changes.

## 2. What moved (theme weights after the cap, sum to 1; parent = X-3, 1/T then the cap 1/22)
| theme | parent | X-5 | ERC b | sleeve vol (ann.) | sleeve TRAIN SR | dW x mu, share of the linear mean gain |
|---|---|---|---|---|---|---|
| investment_issuance | .0978 | **.1410** | .1423 | 1.92% | +.83 | +38% |
| low_risk | .0978 | **.1292** | .1228 | 2.78% | +.80 | +39% |
| reversal_seasonality | .0978 | **.1275** | .1211 | 1.91% | +1.05 | +33% |
| profitability_quality | .0978 | .1136 | .1080 | 1.96% | +1.22 | +21% |
| ownership_flow (both members capped) | .0863 | .0909 | .1127 | 2.54% | -.23 | -1% |
| options_implied (1 member, capped) | .0455 | .0455 | .0585 | 4.18% | +.38 | 0 |
| value | .0978 | .0904 | .0859 | 3.65% | +.43 | -6% |
| short_interest | .0978 | .0829 | .0787 | 2.88% | +.10 | -2% |
| earnings_momentum | .0978 | .0713 | .0678 | 2.77% | +.17 | -7% |
| filing_events | .0863 | **.0509** | .0483 | 5.48% | **-.19** | +21% |
| price_momentum | .0978 | **.0567** | .0538 | 5.74% | +.27 | -35% |

Sources: weights files `build-equity/mega-weights-v8x-theme-erc/composition_weights.json` (`provenance.theme_erc`) and `.../mega-weights-v8-r1-std-v8x3b/composition_weights.json`. Sleeve SR is computed from each member's on-disk `factor_mean` mixed by a_k, over the covariance's sd.

- **ERC b (one line):** EM .068, FE .048, II .142, LR .123, OI .058, OF .113, PM .054, PQ .108, RS .121, SI .079, V .086, against 1/11 = .0909.
- **Convergence.** Dispersion 4.4e-16. The cap was binding on issuance_xbrl, iv_rv_spread, ins_opp_buy and inst_best_ideas, and moved .040 of weight.
- **The gain is not one theme.** Four themes gain and the weight comes out of PM, FE and EM.
  - Linear risk shares in the parent: PM .262, FE .198 and EM .156 (EM and PM correlated .77), so 62% of the parent's risk sat in three low-Sharpe themes.
  - Under X-5 every theme sits at .06-.10.
- **The shares line up with in-sample Sharpe.**
  - Spearman(dW, sleeve TRAIN SR) = **+.65**. Spearman(b, 1/sd) = +.82. Spearman(sleeve sd, sleeve SR) = **-.52**.
  - ERC reads no mean. But in 2020-2023 the low-volatility sleeves were the high-Sharpe ones, so ERC incidentally loaded on in-sample winners.
  - It also cut the one negative-Sharpe theme: FE, added in X-3, carried 20% of the parent's risk at SR -.19.
- **Fitter-space (linear) blend.** From the on-disk covariance and means, and the on-disk `blend_in_sample_TRAIN_diagnostic`, which shows SR 1.001 against 1.329:
  - mean x1.16, sd x0.85, SR .955 -> 1.312 (x1.37).
  - **Counterfactual with sleeve means proportional to sleeve sd (the rule's premise):** SR x1.07. With equal sleeve means: x1.18.

## 3. Fairness: clean
- **NAV `recipe.json`** (207 keys): only `aim_leverage` (1.1414 -> 1.172) and `combined_sha256` differ.
- **IC w-pass `recipe.json` and `train_combined.json`:** only `composition_standardise` and the weights sha differ.
- **Weights files:**
  - Same library `32f8d69f`, role, signs, member set and theme map (47 of 47).
  - `candidates` rows are identical except `weight`.
  - Within-theme shares a_k equal the parent's `std.weights_before_cap` to 2.8e-17.
  - The only `provenance.std` difference is `registry_sha256`: `7ff10f4e` (the registry after X-4) against `b55d8fdc`. All derived tiers and scores are equal, so it is harmless.
- **Gross and construction.** All-rows gross .98623 against .98617, and `exposure.mean_gross_leverage` .98615 against .98610. Same S2 scenario, same `aim-partial-v5+neutral-price-risk-v1`, same neutralizer settings. Median neutralizer amplification is 1.108 against 1.118.
- **Matched gross is not matched risk** (PM6-6). Sharpe is scale-free, and cost per traded dollar is unchanged (12.56 against 12.53 bps), so this does not bias the comparison.

## 4. Decomposition (S2, from `summary.json` of both NAV directories)
| | X-3 | X-5 |
|---|---|---|
| net ann. mean | 4.969% | 5.078% |
| ann. vol | 3.498% | **2.870% (-18%)** |
| net Sharpe | 1.4205 | 1.7695 |
| gross of cost | 6.22% | 6.45% |
| trade cost / yr | .715% | .836% |
| borrow / yr | .335% | .330% |
| tau / max drawdown | .02303 / 2.87% | .02684 / 2.06% |

- **dSR +.349 =**
  - volatility **+.311**: 4.969/2.870 - 4.969/3.498, the parent's return at X-5's vol;
  - gross alpha **+.080**: +.23 pp / 2.870;
  - trade cost **-.042**: +.12 pp / 2.870;
  - borrow **+.002**.
- **The vol cut appears in every year.** Approximate ratios (net return over net SR): 2020 .76, 2021 .87, 2022 .84, 2023 .82. The return gain is in 2021-2022 only: 2020 -.46 pp, 2021 +.70, 2022 +.52, 2023 -.17 (`integration-log.md:5775-5780`, `:5896-5898`).
- **Cost.** ERC shifts weight to fast themes: RS (ind_adj_rev_5 tau .58, seasonality .29, season_y2_5 .30) and LR (smax / smax5 / vol_beta .08-.09). It takes weight from slow PM and value.
  - Weighted standalone tau .0808 -> .0874. Executed tau +16.5%.
  - The registered turnover criterion failed (.02722 against .02336). The cell was accepted only under PM7-34.
- **Reading.** About 89% of the gain is a lower book volatility at matched gross.
  - In-sample optimism in the vol itself is small: 10 free shares, 11 sleeves, 1004 days, (1-N/T) is about .99. ERC is far less aggressive than minimum variance.
  - The fragile part is that **return did not fall**. If sleeve Sharpes were equal, moving weight to low-vol themes at matched gross would cut return by about 10% (linear proxy .905).
  - That gives SR about 4.50/2.87 = 1.57 (**+.15**) instead of +.35. The +.20 gap is the in-sample alignment of section 2. It is about one paired SE (.20).

## 5. Defects: none found
- **Kernel** (`group_erc.hpp:54-100`, `composition_theme_erc.py:94-138`).
  - It refuses a non-positive diagonal or asymmetric input (a zero-variance sleeve fails loudly) and indefinite iterates or contributions.
  - It has no early exit. It uses the root without cancellation for both signs of beta, so negative covariances are handled.
  - Dispersion 4.4e-16 against tolerance 1e-10.
- **Runner** (`strategy_ic_theme_erc.cpp:23-56`).
  - Shares sum to 1 per theme, ERC follows, then w = a*b, then `cap_across_groups`. This is the same order as the Python `rule_weights` (`composition_theme_erc.py:184-209`).
  - Python uses admission order and C++ uses library order. Theme order in both is the recorded covariance order. The run passed the 1e-12 check.
- **No silent fallback.**
  - `check_args` refuses a wrong parent, `--theme-resid` and `--era` (`:290-296`).
  - The rule-table row requires the verify and `rerank` true (`strategy_ic_admission.cpp:511,700`).
  - The recipe and the NAV summary both record `composition_standardise theme-erc-v1` with weights `8310da2c`.
- **Single-member theme and the cap.** options_implied (one member) cannot exceed 1/(2T) = .0455. ownership_flow (two members) is capped at .0909.
  - So ERC is only partly realised: post-cap risk shares are OF .059 and OI .061 against about .09-.10 for the rest.
  - This is as registered (ERC, then the cap, not a cap-aware ERC). It is not a bug.
- **filing_events (new):** 2 members, 0 flat decisions, nothing special.
- **Design approximation, not a defect.** C is the covariance of *linear* sleeves (open risk 2 in the lane report). The runner blends per-date **re-ranked** theme planes times W_theme (`strategy_ic_composition.cpp:333-343`).
  - Re-ranking restores full rank dispersion to themes whose linear sleeve is low-vol only through within-theme diversification. RS, with five heterogeneous members, is the clearest case.
  - So the book's realised risk contributions are not equal. ERC over-weights diversified multi-member themes relative to true ERC.
  - The realised book shares cannot be measured from disk.

## 6. How in-sample, and an honest check
- **Fitted parameters.** 10 free theme shares (9 effective; OI is cap-bound in both books), from 66 covariance moments over the 1004 TRAIN decisions it is then scored on. The parent fits 0 theme parameters. DSR counts one trial but does not penalise in-sample estimation.
- **The planned history read (2015-2019, TRAIN shares unchanged) is a valid holdout for this rule.**
  - The covariance never saw those returns.
  - The runner accepts the recorded TRAIN matrix as is (section 1).
  - Both arms share the TRAIN-admitted library, so a paired dSR isolates the shares.
- **Conditions for that read:**
  1. Run X-5 against X-3 paired on the same window.
  2. Re-check gross: L was calibrated on TRAIN, so print G and, if it is more than .005 off, match it as in PM6-6.
  3. Print member and theme coverage by year. A theme with no pre-2020 data (check iv_rv_spread, ftd_fail, sv_flow, k8 and nt filings) changes the effective shares in both arms.
  4. **Print the vol ratio and the return ratio separately.** The prediction if the mechanism is real is a vol ratio of about .80-.87 and a return ratio of about .90-1.0. A return ratio below about .85 means the TRAIN gain was mostly mean alignment.
- **Split sample within TRAIN (describe only).**
  - Two-fold cross-fit by calendar halves. C_A comes from decisions 2020-01-02..2021-12-31 and C_B from 2022-2023.
  - Apply b_A to the parent's full-TRAIN w and NAV pass but score only the 2022-2023 rows, paired against X-3 on the same rows. Do the converse for b_B and 2020-2021.
  - Report dSR, vol ratio and return ratio per fold, plus |b_A - b_B| (share stability).
  - The runner needs no change (verify accepts any recorded C). The fitter needs a diagnostic mask option, or C can be computed offline from the cached factor series and the block written with `rule_weights`.
  - Label these runs as diagnostics: not trials, not ledgered, TRAIN only.
  - A real-time ERC (expanding C from decisions <= d-2) would need per-date weights, which the runner does not support.

## Top findings
1. **No defect and no leak.** The sleeves use the book's d -> d+2 clock. C is a single full-TRAIN estimate. The runner verifies against the recorded C and does not recompute it. The recipes differ only in the rule and L.
2. **The gain is volatility: +.311 of +.349.** Vol 3.50% -> 2.87% at matched gross, in all four years. Cost is -.042 (+16.5% tau, so the registered turnover criterion failed) and gross alpha is +.080.
3. **The magnitude is inflated in-sample.** ERC shares track in-sample sleeve Sharpe (Spearman +.65; sd against SR -.52), and the cut to the negative-Sharpe `filing_events` adds +21% of the linear mean gain. Under the rule's own equal-Sharpe premise the expected gain is about +.15. Judge it on the 2015-2019 read through the vol and return ratios, not only dSR.
