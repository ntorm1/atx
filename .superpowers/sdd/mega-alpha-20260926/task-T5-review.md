# Task T5 review: DSL alpha library v2 (96 = frozen v1 48 + 48 new price/volume)

Reviewer: read-only task reviewer (no edit of reviewed files, no build, no C++ runner, no build-equity/ reads).
Reviewed in `C:/atx-wt/pool-2` at HEAD `c47ffdaa`. That commit carries the same three
`atx-impl/strategies/` files as the implementer commit `2c92d658` (a diff limited to `atx-impl/strategies` is empty).

Inputs: `task-T5-brief.md`, `task-T5-report.md`, `review-T5.diff`, the generated `price_volume_ic96_v2.json` and
`.recipe.json`, and v1 `slow_price_volume_ic48_v1.json`/`.recipe.json` plus `generate_slow_ic_48.py`.

Engine semantics were checked against the following sources in pool-2:
- `atx-engine/src/alpha/registry.cpp`
- `typecheck.cpp` (`is_rolling_ts`, `analyze_call` :415-422)
- `vm.hpp`: `set_cross_section_mask` :420-436, `cs_one_date` :1416-1487, `eval_time_series` :1501-1685
- `cs_ops.hpp` `cs_vec_reduce_row` :563-582
- `ts_sliding.hpp`: CoMoment/CoMomentLane :95-329, `sweep_comoment` :738-771
- `ts_ops.hpp`: `ts_is_missing` :92-113, the Welford state :832-900, batch pair :1285-1320, TsSkew :1099
- `atx-impl/src/strategy_ic_runner.cpp` :425-434 (ResearchFast plus `set_cross_section_mask(role.decision_member)`)
- `strategy_ic_composition.cpp` :85-150
- `atx-engine/tools/prepare_recent_research.py` :340-423 (membership rule and field bases)

Root evidence (not re-run): `--check` passes. Native `--plan-only` compiles all 96, with max slots 8 and required lookback 314.

My own read-only checks (Python, scratch only):
- v1 bytes and hashes;
- the lineage and dsl_sha256 of all 96 rows;
- an independent lookback recomputation of all 96 DSLs. I evaluated each DSL as a Python expression with my own lookback algebra, not the generator's parser.

## Verdicts
- **Spec compliance: PASS.** Every numbered requirement is met (table below). The substitutions are recorded and defensible.
- **Task quality: CHANGES RECOMMENDED before TRAIN measurement.** 0 Critical, 4 Important, 7 Minor.
  - Nothing is wrong in the sense of lookahead, broken v1 identity, a wrong operator order or a wrong recipe.
  - The Important items are structural, and they must be fixed now or never. Once TRAIN is measured, any template change counts as a selection trial (`family_or_template_selection: false`).
  - About 1/3 of the new weight (8 of 24 templates = 16 of 48 new candidates) re-loads v1's momentum, low-risk and size tilts. Split contamination affects one family, and membership masking affects four families.

## Spec compliance

| # | Requirement | Verdict | Evidence |
|---|---|---|---|
| 1 | v1 48 copied exactly (ids, family, DSL bytes, dsl_sha256) | ✅ | See the v1 identity checks below the table. |
| 1 | 8 new families x 3 templates x {s21, s63} with the v1 wrapper | ✅ | 16 families x 6 candidates each. Every new DSL is `decay_linear(rank(base), s)`. `smoothing_exemptions: []` with the rationale recorded. |
| 2 | Fields close/raw_close/volume only | ✅ | Validator field whitelist; confirmed by reading. |
| 2 | Lookback <= 314, computed and asserted | ✅ | Builder, validator and my independent recomputation agree on every candidate (e.g. resid_sharpe/fp_beta/idio_share/vol_term s63 = 314, volume_trend s63 = 313). The max 314 matches the native plan-only result. The rules match `typecheck.cpp:415-422`: shift +d, rolling or pair +(d-1), everything else max(children). |
| 2 | No future refs, unique ids, deterministic, `--check` | ✅ | All windows are positive integer literals. 96 unique ids and 96 unique DSLs. Output is deterministic JSON with `allow_nan=False`. `--check` passes (root). |
| 3 | `mkt = vec_avg(ret)`, `ret = close/delay(close,1)-1` | ✅ | Used verbatim, with the same `ret` text as v1. |
| 3 | Signatures and arity confirmed against the registry | ✅ | `ts_regression`, `correlation`, `ts_var`, `ts_skew`, `signedpower`, `min`, `vec_avg` rows match `registry.cpp`. The generator cross-checks this at runtime. |
| 4 | The 8 listed families with economic priors; substitutions recorded | ✅ | Each family has a one-line literature prior. Deviations (semibeta, same-window residual variance, signed rho, scaled MAX, FP 250-session window) are recorded. The FP sigma window is not recorded; see M1. |
| 5 | Recipe: 1/16 per family, 1/6 within, 1/96 per candidate, TRAIN 21-session signs, validation_selection false, lineage, fixed-before-measurement statement | ✅ | See the recipe checks below the table. |
| 6 | Python syntax, arity and lookback validator | ✅ | Re-parse with canonical round-trip, arity, fields, windows, lookback and slot estimate. Negative tests were reported but not committed, which is acceptable. |
| C | New files only; no v1 or C++ edits; commit trailer | ✅ | 3 new files. v1 hashes unchanged. Co-Authored-By trailer present. |

**v1 identity (requirement 1):**
- `candidates[:48] == v1.candidates` holds. For every row, id, family and DSL bytes are identical.
- `families[:8]` and `fields` are identical.
- The v1 library and recipe SHA256 are `1ec75242…09ff` and `3b6707f7…130b`, as pinned.
- v2 lineage rows 1-48 equal the v1 lineage exactly, plus `origin`.

**Recipe checks (requirement 5):**
- `composition` has family_weight 1/16, within 1/6 and candidate 1/96, which matches the runner (`strategy_ic_composition.cpp:110`: `1/(families x n_in_family)` = 1/(16 x 6)).
- `orientation` is `train-rank-ic21`, and the runner uses `horizons[1]` = 21.
- `trials.validation_selection` is false and `family_fixing.fixed_before_measurement` is true.
- All 96 lineage rows carry family, template, smoothing_sessions, prior_bars, dsl_sha256 and origin. Every dsl_sha256 re-verified.
- `library.sha256` equals the file hash `0c7f3059…`.

## Semantics check per new template

These points were verified in the VM or kernel code:
- `ts_regression(ret, mkt, d)` is the slope of **arg0 on arg1**, so the order is right. See `CoMoment::slope()` "OLS slope of x (dependent) on y (predictor)", and the batch `ts_ops.hpp:1285-1298`, where x = src[0] and y = src[1] (`vm.hpp:1504,1517`; `sweep_comoment` passes `(x, y)` in order).
- `correlation` has arity 3.
- `decay_linear` uses weights 1..d, oldest to newest.
- `rank` is applied per date before the time decay (same nesting as v1).

| template | claim | verdict |
|---|---|---|
| resmom_6_1 / resmom_9_1 | `ts_sum(ret,n) - beta_n*ts_sum(mkt,n)` = n x OLS intercept over [t-21-n+1, t-21] | ✅ correct identity (alpha = ybar - beta*xbar). Formation windows [t-125,t-21] and [t-188,t-21] equal v1 mom_6_1/mom_9_1 exactly (see I3). |
| resid_sharpe_12_1 | n*alpha / (sd * sqrt(1-rho^2)) = n*alpha / sqrt(SSR/(n-1)) | ✅ The sliding corr is clamped to [-1,1] (`ts_sliding.hpp:186`), so `1-rho^2 >= 0` and `signedpower(.,0.5)` is sqrt. See M7 for AuditExact. |
| beta_126 | rolling OLS beta | ✅ |
| fp_beta_250_63 | FP beta: rho(3-day, 250) x sigma_i / sigma_m | ✅ Arithmetic is correct. sigma_m is constant across names on a date and rank-neutral. The sigma window deviates from FP (see M1). |
| neg_semibeta_189 | BLPQ `sum(r- m-)/sum(m^2)` | ✅ Matches the BLPQ definition (non-negative by construction). |
| ivol_21 / ivol_126 | `ts_var*(1-rho^2)` = SSR/(n-1), rank-equivalent to residual sd | ✅ (see I4 for redundancy) |
| idio_share_252 | 1 - R^2 | ✅ (see M2) |
| max_ret_21, scaled_max_63, skew_126 | MAX, MAX/sigma, sample skewness | ✅ `ts_max` uses the NaN-only policy. `ts_skew` returns NaN for d<3 or sd==0 (`ts_ops.hpp:1099-1107`). |
| volume_shock / spike / trend | share-volume ratios | ⚠️ Computes what it claims, but on **unadjusted** share volume (see I1). |
| amihud_21/63/126 | mean(abs(ret) / (raw_close*volume)) | ✅ Split-invariant denominator (see I4). |
| market_corr_63, weekly_market_corr_126 | contemporaneous corr, daily and overlapping-weekly | ✅ |
| lagged_market_corr_240 | corr(week_t(ret), week_{t-5}(mkt)) | ✅ Adjacent non-overlapping weeks, so it is causal. Lookback = max(5, 10) + 239 = 249. |
| vol_of_vol_21_126, vol_term_63_252, variance_ratio_5_240 | CV of rolling vol; sigma63/sigma252; Lo-MacKinlay VR(5) | ✅ The VR numerator spans 244 returns and the denominator 240, which is immaterial. |

**Lookahead.**
- Every op is trailing.
- `vec_avg` at t reduces over the date-t member set. That set is computed before bar t (`prepare_recent_research.py:344-353`).
- The panel instrument union spans the whole role, but it cannot leak through `vec_avg` because the reduction is masked to date-t members.
- Split and dividend factor levels cancel in every ratio used.
- No lookahead found.

## Findings

### Critical
None.

### Important

**I1: abnormal_volume uses raw (split-unadjusted) share volume, so splits and reverse splits read as volume shocks.**
- Candidates: `volume_shock_5_63_s21/s63`, `volume_spike_21_126_s21/s63`, `volume_trend_63_252_s21/s63` (6 candidates, a full family).
- The role manifest declares `"volume_basis": "raw-share-volume"` (`prepare_recent_research.py:418`), and the v1 field note says the same.
- **Failure scenario.** Take a 20:1 forward split in 2022 (e.g. AMZN or GOOGL; 4:1 or 5:1 splits such as AAPL, TSLA and NVDA in 2020-21 behave the same way).
  - Share volume steps up about 20x on the effective date.
  - `volume_trend_63_252` reads about 20x at first and stays inflated for about 252 sessions. It is pinned at the top rank for months.
  - `volume_shock_5_63` is pinned for about 63 sessions.
  - Reverse splits (common among formerly sub-$5 names entering the universe) pin names at the bottom rank.
- **Why it matters.**
  - Split names occupy exactly the tails that drive a rank IC.
  - Forward splits follow large run-ups, so the family's extremes partly become a stale momentum or split-event proxy rather than attention or volume shocks.
  - v1 avoided this deliberately: its liquidity family uses split-invariant `raw_close * volume`. The v2 family description even says "share volume, not v1 raw-dollar ratios", so the choice was intentional but the consequence was not recorded.
- **Fix.** Use split-adjusted share volume. `close = raw_close * cumulReturnFactor`, so `((raw_close * volume) / close)` = volume / F_t is continuous across splits, apart from a slow drift of about 2% a year from the dividend factor.
  - Example: `(ts_mean(((raw_close * volume) / close), 5) / ts_mean(((raw_close * volume) / close), 63))`.
  - The lookback is unchanged. The family stays distinct from v1's dollar ratios because the price-level change is excluded.
  - Record this in the family description and in the substitutions.
  - Note that vendor rows whose factor misses a split (the KLAC 2026-06-12 case in `atx-impl/docs/EQUITY_BOOK_BASELINE.md`) corrupt `close` and every template alike. That is a data-contract issue, not something this fix addresses.

**I2 (implementer concern 1): confirmed. The member-masked `vec_avg` makes the 24 market-return candidates NaN unless the name was a member on every session of a 41-314-session span.**

*Code path:*
1. `strategy_ic_runner.cpp:432-434` evaluates under ResearchFast with `set_cross_section_mask(role.decision_member)`.
2. `cs_one_date` (`vm.hpp:1430-1436`) NaN-fills every cell and admits only `!isnan(x) && mask != 0`.
3. `cs_vec_reduce_row` (`cs_ops.hpp:569-582`) writes the average only to valid cells. So `mkt` is NaN at every non-member (and every NaN-ret) cell.
4. The pair lanes return NaN whenever `miss != 0` (`ts_sliding.hpp:285-287`). `ts_sum`/`ts_mean`/`stddev`/`ts_var` treat any non-finite cell as missing, which gives a NaN window (`ts_ops.hpp:92-113`).
5. `LoadField` NaNs only non-present cells (`vm.hpp:1305`), so `ret` itself is finite on non-member days. Only the Cs ops (`vec_avg` and the wrapper `rank`) introduce the gaps.

*Required continuous-membership span (sessions ending at t).* The span equals prior_bars here, because the `+1` of `delay(close,1)` needs no membership:

| template | s21 | s63 |
|---|---|---|
| ivol_21 | 41 | 83 |
| market_corr_63 | 83 | 125 |
| beta_126, ivol_126, resmom_6_1 | 146 | 188 |
| weekly_market_corr_126 | 150 | 192 |
| resmom_9_1, neg_semibeta_189 | 209 | 251 |
| lagged_market_corr_240 | 269 | 311 |
| resid_sharpe_12_1, fp_beta, idio_share_252 | 272 | 314 |

v1 already needs s consecutive member sessions, because the masked `rank` sits inside `decay_linear` (63+s for `return_dollar_rank_corr63`). The implementer's "milder form in v1" statement is correct.

*Qualitative impact (unmeasured).*
- **Churn in the membership rule.** Membership is recomputed daily with three conditions:
  - top-3000 by prior-63 ADV;
  - ADV > $5M and prior raw close > $5;
  - a complete 63-session prior window. One missing vendor row makes ADV NaN and evicts the name for about 63 sessions (`prepare_recent_research.py:346-356`).
- **Where coverage survives.** The stable core (roughly the top ~2000 by ADV, price well above $5) keeps near-full coverage.
- **Where coverage is lost.**
  - The ADV-rank boundary band and names hovering near $5 lose these candidates for long stretches after any single exit.
  - Newly eligible names (IPOs, spin-offs, SPAC de-SPACs, names recovering above $5, which were common in TRAIN 2020-22) get no market-family contribution for 146-314 sessions.
- **Why the lost subset matters.** It is the small, illiquid, volatile end of the cross-section. That is where the IVOL, MAX, BAB and illiquidity-type premia are documented to be strongest, so the masking removes these families' coverage where their priors are strongest.
- **Rough size.** For 252-base s63 variants, expect coverage well below v1's s63 coverage, plausibly 10-25% fewer names, concentrated in the bottom liquidity quintile. This is an estimate, not a measurement.
- **Effect on the blend.** `strategy_ic_composition.cpp:141-148` adds 0 for missing names, with no renormalization. These 4 families are 1/4 of the blend weight, so boundary names get systematically smaller-magnitude blends. That is benign for turnover but biases the book toward stable names.
- **Effect on the ICs.** The per-candidate ICs of these families are computed on a stable-member subsample, so they are not like-for-like with v1 or the non-market families.
- **What this is not.** It is causal, so this is not lookahead or survivorship.

*Fix.* There is no DSL-only fix; I agree with the implementer:
- every Cs op honors the mask;
- `ts_backfill` injects a stale market return;
- a `select`/`ts_count_nans` zero-fill injects a false market return.

Native options (out of T5 scope; escalate to root):
- (a) a `vec_avg`/`vec_sum` masked-reduce, broadcast-to-all-present variant, or a mask policy in which Cs reductions use the mask but `CsVecAvg`/`CsVecSum` broadcast to every finite-input cell;
- (b) an IC-runner option to pass a separate reduction mask.

Until then:
- the recipe caveat (`data.market_window_support`) is accurate and should stay;
- root should read each market-family candidate's valid-names and contribution coverage in TRAIN diagnostics before interpreting its IC or its share of the blend.

**I3: residual_momentum `resmom_6_1` and `resmom_9_1` are structural near-duplicates of v1 `mom_6_1` and `mom_9_1`.**
- Candidates: `resmom_6_1_s21/s63`, `resmom_9_1_s21/s63`.
- **Why they duplicate.**
  - The formation windows are identical: [t-125,t-21] and [t-188,t-21], the same as v1 `skipret(21,126)` and `skipret(21,189)`.
  - With a same-window single-factor beta, the residual sum is R_i - beta_i*M, where M is the market cumulative return. M is common to all names on a date, so the cross-sectional deviation from raw momentum is only -beta_i*M.
- **Typical size of the deviation (105-session window).**
  - Cross-sectional sd of R is about 0.25-0.30.
  - sd(beta) is about 0.4-0.5 and |M| is about 0.1, so sd(beta*M) is about 0.05.
  - That gives corr(R, R-beta*M) of about 0.98 in normal regimes, and only about 0.93 even around the 2020 crash window.
- **Effect.** After TRAIN sign fitting, the family behaves like a second momentum budget (momentum gets about 2/16 of the blend plus trend_quality). This is the "winner" tilt the brief asked to diversify away from.
- **Contrast with BHM (2011).** Their residual momentum differs from raw momentum mainly because:
  - the 36-month regression window is much longer than the formation window, so a long-run alpha is also removed;
  - it is **standardized by the residual sd**.

  The unscaled same-window form keeps neither feature. Only `resid_sharpe_12_1` carries the standardization.
- **Fix, before measurement.** Make all three templates the standardized form BHM actually use: residual Sharpe at 6-1, 9-1 and 12-1. The existing `resid_sum/resid_sd` builder does this at the same lookbacks (base 126/189/252).
  - Also record that `resid_sharpe_6_1` will still correlate with v1 `risk_scaled_6_1` (R/sigma_126).
  - Alternatively, keep the templates and state in the recipe that the two unscaled templates are expected to be about 0.95 rank-correlated with v1 momentum, so nobody later reads their IC as independent evidence.

**I4: two brief-mandated families structurally re-load v1's low-risk and size tilts (escalate: the brief specified these constructs).**
- **(a) illiquidity: all 3 templates (`amihud_21/63/126`, 6 candidates).**
  - Cross-sectionally, log Amihud is approximately log mean|r| - log DV.
  - Across ~3000 names, sd(log DV) is about 1.3-1.5, against sd(log mean|r|) of about 0.4, and the two are negatively related.
  - So rank(Amihud) is about -rank(dollar volume), which is v1 `dollar_liquidity_63` sign-flipped. My prior is |rank corr| of about 0.9-0.97, and at least 0.95 across the three windows.
  - With sign fitting, the family becomes a second, larger copy of the size/liquidity tilt: 1/16 plus v1's 2/96.
  - The implementer flagged this (concern 2).
- **(b) idiosyncratic_risk `ivol_21`/`ivol_126` and lottery `max_ret_21` (6 candidates).**
  - log sigma_idio = log sigma + 0.5 log(1-R^2). The second term has sd of about 0.1, against about 0.45 for the first.
  - So ivol_126 is roughly equal to v1 `low_vol_63` and `low_downside_126` (prior |rank corr| about 0.85-0.95), and ivol_21 is about 0.8-0.9.
  - MAX is known to be about 0.75-0.9 correlated with IVOL (Bali et al. 2011).
- **Net effect.**
  - (a) plus (b) plus I3 comes to 8 of 24 new templates (16 of 48 new candidates) that load v1's winner, low-risk and size tilts.
  - The brief's premise was that v1 is dominated by exactly these tilts, so v2's diversification is substantially overstated by the family count.
- **Fix.** This is a root or brief decision, and it must be made before any TRAIN measurement.
  - Keep one level template per family: `amihud_63` and `ivol_126`.
  - Replace the rest with size- or vol-orthogonal constructs from the same priors. Examples:
    - illiquidity shock, per Amihud (2002) "unexpected illiquidity": `ts_mean(amihud_x,21)/ts_mean(amihud_x,252)`, where `amihud_x` = `(abs(ret)/(raw_close*volume))`; base lookback 252;
    - idio-vol change: `ivol_21/ivol_252`;
    - `scaled_max_21` (MAX/sigma, which the implementer already uses at 63).
  - Record the re-selection as structural, not measured.

### Minor

- **M1 (`fp_beta_250_63_s21/s63`).**
  - Frazzini-Pedersen estimate sigma_i from **1-year** daily returns. The template uses `stddev(ret, 63)`, which is the exact same factor as v1 `low_vol_63`.
  - `stddev(ret, 252)` costs no lookback (the base stays at 252 because the corr term is already 252), is FP-faithful, and reduces the mechanical overlap with v1.
  - The 63-session sigma window is not listed under the substitutions.
- **M2 (`market_corr_63` vs `idio_share_252`).**
  - The rationale for using signed rho ("R^2 would duplicate idio_share") does not hold in practice. Nearly every stock has rho > 0 against the equal-weight member mean, so signed rho is rank-monotone in R^2.
  - `market_corr_63` is therefore the synchronicity construct of `idio_share_252` with the sign flipped, at a different window (prior |rank corr| about 0.6-0.8), split across two families.
  - Consider a distinct comovement template instead, e.g. downside correlation `correlation(min(ret,0), min(mkt,0), 126)` (Ang-Chen-Xing / Hong-Tu-Zhou asymmetric correlation). Alternatively, accept it and correct the stated rationale.
- **M3 (Amihud and zero-volume or halt rows).**
  - The prep script accepts `volume >= 0`.
  - On a halted or zero-volume row, `abs(ret)/0` gives +inf, or NaN when ret = 0. `ts_mean` treats either as missing, so the name is NaN for 21/63/126 + s sessions.
  - This is rare for members (ADV > $5M), so the implementer's concern 5 is correct and it is acceptable.
  - An optional hardening: `raw_close * max(volume, 1)`.
- **M4 (single bad prints).**
  - A vendor factor that misses a split (e.g. KLAC in `EQUITY_BOOK_BASELINE.md`) yields ret of about -90%.
  - That one value dominates `max_ret`, `skew_126`, `ivol`, `vol_term`, `vol_of_vol`, `variance_ratio` and Amihud for that name for up to 314 sessions.
  - Its effect on `mkt` is negligible (about 0.9/3000).
  - v1 shares the same exposure. Optional per-name clip in the new templates: `min(max(ret, -0.5), 1)`.
- **M5 (universe composition).**
  - The role manifest says `common_stock_verified: False`, so ETFs, ETNs and leveraged or inverse products can be members.
  - They enter the equal-weight `mkt`.
  - They will sit at the rank extremes of market_beta (roughly ±3x products), comovement, lottery and vol-dynamics (VIX ETNs).
  - This is a universe-level issue for root, not a T5 defect.
- **M6 (implementer concern 6 / cost).**
  - `ts_skew` has no sliding kernel: it takes the batch O(d)-per-cell path (d = 126).
  - Everything else new is O(1) sliding: comoment, Welford, the running sum, and the deque max.
  - Expect `skew_126` to be the slowest new candidate. This is informational only.
- **M7 (AuditExact re-scoring).**
  - The batch `TsCorr` (`ts_ops.hpp:1307-1320`) is not clamped. If |rho| exceeds 1 by an ulp, then 1-rho^2 < 0, and `signedpower` returns a tiny negative, which flips the sign of `resid_sharpe_12_1`.
  - This only happens for series nearly collinear with the member mean, e.g. an equal-weight-index ETF (see M5). It is immaterial otherwise.
  - Could be hardened with `max(1 - rho*rho, 0)`.

Nit: the report says "All files are LF". The committed blobs are LF (the blob SHA `da046b80…` matches), but the pool-2 working-tree `.py` is CRLF. `.gitattributes` pins only `*.json` and `*.md`, so autocrlf applies to `*.py`. This is harmless.

## What is good
- The frozen-v1 import pins both v1 SHAs and asserts element-wise equality of the candidates, lineage and dsl_sha256. This is the right way to guarantee signal-cache reuse.
- The static validator is a real second parser. It cross-checks against `registry.cpp`, `typecheck.cpp` and `typecheck.hpp`, and its lookbacks agree with my independent algebra and with the native maximum.
- The concerns section is candid and mostly correct (concerns 1, 2, 3, 5 and 6 are verified here). The recipe caveat on market-window support is accurate.
