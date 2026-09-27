# Task T5 report: DSL alpha library v2 (96 = frozen v1 48 + 48 new price/volume)

Status: DONE (commit made; no C++ compiled, runner not run, no market data read, nothing pushed)
Worktree / branch: `C:/atx-wt/pool-7`, `feat/mega-alpha-library-v2-20260926`
Base: `bb5bc25b`. Commit: `2c92d658` ("feat(research): add 96-candidate price/volume IC library v2")

## Files (new only, under `atx-impl/strategies/`)

| file | SHA256 | bytes |
|---|---|---|
| `generate_price_volume_ic96_v2.py` | `da046b800f9cd5129299dda2954a028fb37ff76a5511930c11d85704e97023d1` | 392 lines |
| `price_volume_ic96_v2.json` (library) | `0c7f3059c668882659684eeec123ba538306b8583c71ec9ef37710dd107bf1f0` | 34590 |
| `price_volume_ic96_v2.recipe.json` | `93cd52abeca974d5dd9f7f4561caf74450a738eb90fdc1de7b738c2fae2a4f05` | 43488 |

The committed blobs hash to the same values. `python generate_price_volume_ic96_v2.py --check` passes, and so does v1 `generate_slow_ic_48.py --check`. No v1 files and no C++ were modified. All files are LF.

## How the frozen v1 48 are carried over
- The generator imports `generate_slow_ic_48.py` and calls its `documents()`. It asserts the v1 library SHA256 `1ec75242…09ff` and the v1 recipe SHA256 `3b6707f7…130b`, then copies the v1 `families` and `candidates` verbatim.
- It asserts `candidates[:48] == v1 candidates`. Ids, family ids, family descriptions and DSL bytes are identical, so every `dsl_sha256` is unchanged and cached signals can be reused.
- The v1 lineage rows are copied into the v2 recipe with `origin: frozen_v1` added. Each row's `dsl_sha256` is re-verified against the DSL text.
- The independent Python validator recomputes the lookback for all 48 v1 DSLs, and it matches v1's recorded `prior_bars` for every one. This also cross-checks the validator itself.

## New families: rationale and one-line literature prior
All 8 families and 24 templates were fixed from these priors before any measurement. Nothing under `build-equity/` or any IC, return or turnover output was opened.

| family | economic rationale | one-line prior |
|---|---|---|
| residual_momentum | momentum in beta-adjusted return, which removes market-timing beta exposure from raw momentum | Blitz, Huij & Martens (2011, JEF) residual momentum; Grundy & Martin (2001, RFS) |
| market_beta | low-beta stocks earn higher risk-adjusted returns (leverage constraints); downside co-movement is priced separately | Frazzini & Pedersen (2014, JFE) BAB; Bollerslev, Li, Patton & Quaedvlieg (2022, JFE) realized semibetas |
| idiosyncratic_risk | high idiosyncratic volatility predicts low returns; the firm-specific share of variation reflects the information environment | Ang, Hodrick, Xing & Zhang (2006, JF); Durnev, Morck, Yeung & Zarowin (2003, JAR) |
| lottery_demand | demand for lottery-like right tails overprices them | Bali, Cakici & Whitelaw (2011, JFE) MAX; Amaya, Christoffersen, Jacobs & Vasquez (2015, JFE) realized skewness |
| abnormal_volume | volume shocks raise visibility or attention and precede returns | Gervais, Kaniel & Mingelgrin (2001, JF); Barber & Odean (2008, RFS); Lee & Swaminathan (2000, JF) |
| illiquidity | an illiquidity premium compensates price impact | Amihud (2002, JFM) |
| comovement | synchronicity with the market and delayed incorporation of market news | Hou & Moskowitz (2005, RFS) price delay; Barberis, Shleifer & Wurgler (2005, JFE) |
| volatility_dynamics | uncertainty about risk (vol-of-vol) and short- versus long-run volatility components are priced | Baltussen, van Bekkum & van der Grient (2018, JFQA); Adrian & Rosenberg (2008, JF); Lo & MacKinlay (1988, RFS) variance ratio |

## Shared definitions and wrapper
- `ret = ((close / delay(close, 1)) - 1)`, the same text as v1.
- `mkt = vec_avg(ret)`, the member-masked cross-sectional equal-weight mean, used verbatim as the brief specifies.
- Wrapper: `decay_linear(rank(base), s)` with `s` in {21, 63}, the same as v1. Candidate id is `<template>_s<s>`, horizons are `[5, 21, 63]` and `sign_policy` is `train-rank-ic21`.
- No smoothing exemptions (`smoothing_exemptions: []`):
  - `rank` is needed to tame heavy-tailed bases: Amihud, volume ratios, skew and volatility ratios.
  - The two decay variants keep v1's 3x2 family budget.
  - The already-slow 126-252 session bases only cost extra lookback, and that is checked to be <= 314.
- Lookback rule (typecheck.cpp): `delay` adds d; rolling ops add d-1; element-wise and cross-sectional ops take the max of their children. So s63 adds 62, and every base must be <= 252.

## New template DSL (base expression; the candidate is `decay_linear(rank(<base>), s)`)

#### residual_momentum
Residual cumulative return is `ts_sum(ret,n) - beta_n * ts_sum(mkt,n)`, which equals n times the market-model alpha over the same window (beta is the same-window OLS slope).
- `resmom_6_1`: base prior bars 126; s21 146, s63 188
  `delay((ts_sum(((close / delay(close, 1)) - 1), 105) - (ts_regression(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 105) * ts_sum(vec_avg(((close / delay(close, 1)) - 1)), 105))), 21)`
- `resmom_9_1`: base prior bars 189; s21 209, s63 251
  `delay((ts_sum(((close / delay(close, 1)) - 1), 168) - (ts_regression(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 168) * ts_sum(vec_avg(((close / delay(close, 1)) - 1)), 168))), 21)`
- `resid_sharpe_12_1`: base prior bars 252; s21 272, s63 314. This is alpha / residual std, where residual std = `stddev * sqrt(1 - rho^2)`. `signedpower(., 0.5)` is used because `sqrt` is not registered, and it stays finite if `1 - rho^2` rounds to a tiny negative.
  `delay(((ts_sum(((close / delay(close, 1)) - 1), 231) - (ts_regression(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 231) * ts_sum(vec_avg(((close / delay(close, 1)) - 1)), 231))) / (stddev(((close / delay(close, 1)) - 1), 231) * signedpower((1 - (correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 231) * correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 231))), 0.5))), 21)`

#### market_beta
- `beta_126`: base prior bars 126; s21 146, s63 188
  `ts_regression(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 126)`
- `fp_beta_250_63`: base prior bars 252; s21 272, s63 314. This is the Frazzini-Pedersen beta: correlation of 3-day overlapping returns over 250 sessions times sigma_i/sigma_m over 63. The sigma_m term is constant across names on a date (rank-invariant), but it is kept for fidelity.
  `((correlation(ts_sum(((close / delay(close, 1)) - 1), 3), ts_sum(vec_avg(((close / delay(close, 1)) - 1)), 3), 250) * stddev(((close / delay(close, 1)) - 1), 63)) / stddev(vec_avg(((close / delay(close, 1)) - 1)), 63))`
- `neg_semibeta_189`: base prior bars 189; s21 209, s63 251. This is the negative realized semibeta `sum(r- m-) / sum(m^2)`.
  `(ts_sum((min(((close / delay(close, 1)) - 1), 0) * min(vec_avg(((close / delay(close, 1)) - 1)), 0)), 189) / ts_sum((vec_avg(((close / delay(close, 1)) - 1)) * vec_avg(((close / delay(close, 1)) - 1))), 189))`

#### idiosyncratic_risk
Residual variance is `ts_var(ret) * (1 - rho^2)`. This equals the same-window OLS residual variance and is rank-equivalent to residual std, so no sqrt is needed under `rank`.
- `ivol_21`: base prior bars 21; s21 41, s63 83
  `(ts_var(((close / delay(close, 1)) - 1), 21) * (1 - (correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 21) * correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 21))))`
- `ivol_126`: base prior bars 126; s21 146, s63 188
  `(ts_var(((close / delay(close, 1)) - 1), 126) * (1 - (correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 126) * correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 126))))`
- `idio_share_252`: base prior bars 252; s21 272, s63 314 (1 - R^2)
  `(1 - (correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 252) * correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 252)))`

#### lottery_demand
- `max_ret_21`: base prior bars 21; s21 41, s63 83
  `ts_max(((close / delay(close, 1)) - 1), 21)`
- `scaled_max_63`: base prior bars 63; s21 83, s63 125. This is MAX over 63 sessions divided by sigma_63. It isolates the right tail from the volatility level; see substitutions.
  `(ts_max(((close / delay(close, 1)) - 1), 63) / stddev(((close / delay(close, 1)) - 1), 63))`
- `skew_126`: base prior bars 126; s21 146, s63 188
  `ts_skew(((close / delay(close, 1)) - 1), 126)`

#### abnormal_volume
All three use share volume. They are not v1's raw-dollar `dollar_expansion_21_126` or `dollar_instability_63`, and the windows differ.
- `volume_shock_5_63`: base prior bars 62; s21 82, s63 124
  `(ts_mean(volume, 5) / ts_mean(volume, 63))`
- `volume_spike_21_126`: base prior bars 125; s21 145, s63 187
  `(ts_max(volume, 21) / ts_mean(volume, 126))`
- `volume_trend_63_252`: base prior bars 251; s21 271, s63 313
  `(ts_mean(volume, 63) / ts_mean(volume, 252))`

#### illiquidity
Amihud over the windows given in the brief.
- `amihud_21`: base prior bars 21; s21 41, s63 83
  `ts_mean((abs(((close / delay(close, 1)) - 1)) / (raw_close * volume)), 21)`
- `amihud_63`: base prior bars 63; s21 83, s63 125
  `ts_mean((abs(((close / delay(close, 1)) - 1)) / (raw_close * volume)), 63)`
- `amihud_126`: base prior bars 126; s21 146, s63 188
  `ts_mean((abs(((close / delay(close, 1)) - 1)) / (raw_close * volume)), 126)`

#### comovement
- `market_corr_63`: base prior bars 63; s21 83, s63 125
  `correlation(((close / delay(close, 1)) - 1), vec_avg(((close / delay(close, 1)) - 1)), 63)`
- `weekly_market_corr_126`: base prior bars 130; s21 150, s63 192
  `correlation(ts_sum(((close / delay(close, 1)) - 1), 5), ts_sum(vec_avg(((close / delay(close, 1)) - 1)), 5), 126)`
- `lagged_market_corr_240`: base prior bars 249; s21 269, s63 311. This is the price-delay proxy: the stock's weekly return against the previous week's market return.
  `correlation(ts_sum(((close / delay(close, 1)) - 1), 5), delay(ts_sum(vec_avg(((close / delay(close, 1)) - 1)), 5), 5), 240)`

#### volatility_dynamics
- `vol_of_vol_21_126`: base prior bars 146; s21 166, s63 208. This is the coefficient of variation of rolling 21-session volatility.
  `(stddev(stddev(((close / delay(close, 1)) - 1), 21), 126) / ts_mean(stddev(((close / delay(close, 1)) - 1), 21), 126))`
- `vol_term_63_252`: base prior bars 252; s21 272, s63 314
  `(stddev(((close / delay(close, 1)) - 1), 63) / stddev(((close / delay(close, 1)) - 1), 252))`
- `variance_ratio_5_240`: base prior bars 244; s21 264, s63 306. This is the variance of 5-day returns divided by 5x the daily variance.
  `(ts_var(ts_sum(((close / delay(close, 1)) - 1), 5), 240) / (5 * ts_var(((close / delay(close, 1)) - 1), 240)))`

The maximum prior bars across all 96 candidates is 314. The generator asserts it, and the validator recomputes it independently.

## Substitutions and deviations (with reasons)
- **Downside beta:** implemented as the negative realized semibeta (BLPQ 2022) rather than the Ang-Chen-Xing conditional (m < mean) OLS beta.
  - The semibeta is exactly expressible with `min()` and `ts_sum`.
  - The conditional beta needs masked conditional moments, and its expression would be about 4x larger.
- **Residual std:** computed as `var(ret) * (1 - rho^2)` over the same window, not as `stddev(ret - beta_t * mkt)`.
  - The nested form would mix time-varying betas and add lookback.
  - The same-window identity is the exact market-model residual variance.
- **Residual momentum:** uses `ret - beta * mkt`, which includes alpha. With a same-window beta, the intercept-free residuals sum to zero, so the alpha-inclusive (Jensen) form is the informative one.
- **Comovement:** uses signed rho, not R^2. R^2 over a window is rank-identical (up to sign) to `idio_share` over that window, and signs are fitted, so using R^2 would duplicate a candidate.
- **Lottery demand:** the third template is `scaled_max_63` (MAX/sigma) rather than a plain MAX over 63 or a second skew. Plain MAX at 21 and 63 would both track volatility, and v1 `low_vol_63` already covers volatility.
- **Frazzini-Pedersen beta:** uses a 250-session correlation of 3-day returns instead of FP's 5-year window, to fit the 314 lookback budget.
- **Unlisted but consistent choices:** `ts_skew`/`ts_var`/`signedpower` spellings, the variance-ratio constant 5, and the sigma_m divisor. Each keeps formula fidelity and is rank-neutral where noted.

## Recipe (`price_volume_ic96_v2.recipe.json`)
- The schema mirrors v1: `atx.dsl-ic-experiment/v1`, id `price_volume_ic96_v2_initial`, with the library path and SHA.
- `generation`:
  - rule `frozen-v1-48-plus-eight-by-three-by-two-v2`: 96 candidates, 16 families, 3 templates per family, smoothing [21, 63].
  - wrapper, `smoothing_exemptions: []` with the rationale, `max_prior_bars` 314, `complete_observations` 315.
  - `frozen_v1` pins (generator, library and recipe SHAs), plus `new` counts.
- `family_fixing`: `fixed_before_measurement: true`, `measurement_consulted: false`, plus the statement.
- `family_priors`: all 16 families. The v1 rows reuse the v1 descriptions.
- `templates`: 24 rows with the base DSL and base prior bars.
- `lineage`: 96 rows with id, family, template, smoothing_sessions, prior_bars, dsl_sha256 and origin.
- `data`: the v1 data block plus `daily_return`, `market_return` and a `market_window_support` caveat.
- `orientation`: v1's `train-rank-ic21` (TRAIN 21-session sign fit; frozen before validation; selection none).
- `composition`: `fixed-family-centered-tied-rank-v1` with family 1/16, within-family 1/6 and candidate 1/96. The runner's `strategy_ic_composition.cpp` computes 1/(families x n_in_family) dynamically, so no code change is needed.
- `planned_turnover`: v1's block.
- `trials`: 96 generated (48 reused + 48 new), `validation_selection: false`, `family_or_template_selection: false`.
- `static_validation`: summary block.
- `qualification`: native parse pending.

## Static validation (no C++ runtime)
The generator re-parses every one of the 96 DSL strings and checks:
- canonical text round-trip;
- operator name and arity against an embedded table of `registry.cpp` rows;
- declared fields only (close, raw_close, volume);
- every window/delay argument is a positive integer literal, so there are no future references;
- no scalar series operand;
- lookback recomputed with the typecheck rules and required to equal the builder's value (<= 314);
- DAG node count and estimated peak slots. The estimate mirrors dag.cpp post-order interning and bytecode.cpp refcount retirement, and must be <= 64, the runner limit.
- DSL <= 4096 bytes, ids and families in `[a-z0-9_]{1,64}`, 96 unique ids and 96 unique DSLs.

In `--check` and generate mode it also cross-checks the embedded arity table and lookback classes against `atx-engine/src/alpha/registry.cpp`, `typecheck.cpp` and `typecheck.hpp`. Result: "registry cross-check ok (17 operators)".

Negative tests, run from scratch and not committed, were all rejected: zero, negative or fractional window; undeclared field; wrong arity; unregistered op; unbalanced parentheses; non-literal window; scalar series operand; trailing junk.

Estimated peak slots: the new candidates reach at most 8 (`resid_sharpe_12_1`); v1 ranges 3-7 under the same estimator. DAG nodes max 25. The longest DSL is 500 bytes.

## Concerns
1. **Membership-masked market return costs coverage.**
   - `CsVecAvg` broadcasts only to masked-valid cells, and the runner sets `set_cross_section_mask(role.decision_member)`. So `mkt` is NaN at every non-member cell.
   - Any trailing window containing one such cell is NaN.
   - The 24 candidates in residual_momentum, market_beta, idiosyncratic_risk and comovement are therefore defined only for names that were as-of members on every session read: up to 314 sessions for the s63 variants of 252-session bases.
   - Membership is recomputed daily (`prepare_recent_research.py`: top-3000 by prior-63 ADV > $5M with raw price > $5, and a full 63-day finite ADV). Boundary names churn, so coverage loss could be material, mostly among smaller names. It is unmeasured.
   - Missing contributes 0 (neutral), so these families carry less weight on churned names.
   - v1 has a milder form of this, because the inner `rank` sits inside `decay_linear` windows and inside `return_dollar_rank_corr63`.
   - No DSL-only fix exists: all cross-sectional ops honor the mask, and `ts_backfill` would insert stale market values. A native unmasked-broadcast `vec_avg` variant, or a runner mask policy change, would remove it; both are out of scope here.
2. **Illiquidity family is kept exactly as the brief specifies (Amihud 21/63/126), but its diversification is doubtful.** It is likely strongly rank-correlated with v1 `dollar_liquidity_63` (a size/liquidity proxy) and across its own three windows.
3. **Other likely positive correlations (by prior, unmeasured):**
   - `ivol_21` with `ivol_126`;
   - `max_ret_21` and ivol with v1 `low_vol_63`;
   - `volume_trend_63_252` with v1 `dollar_expansion_21_126`;
   - `vol_term_63_252` with v1 `vol_expansion_21_126`.
   These are distinct definitions and windows, not duplicates.
4. **Equal-weight `vec_avg(ret)` is used verbatim, with no winsorization.** The role data contract guarantees finite positive closes, so `ret` is finite, but a single extreme name-day return moves `mkt` for every regression window that contains it.
5. **Amihud and zero volume:** a zero-volume day gives inf or NaN, and the running-sum ops treat inf as missing, so the whole window becomes NaN. This should be rare for members, given ADV > $5M.
6. **First atx-impl use of `ts_regression`, `vec_avg`, `ts_skew`, `ts_var` and `signedpower`.** The ResearchFast pair ops use the O(1) sliding comoment path. The native `--plan-only` parse/typecheck is still pending at root. The slot estimate (max 8 vs v1 7) implies a small increase in per-cell VM memory admission.
7. **Python version:** the generator needs Python >= 3.10 (`zip(strict=True)`); it was run with 3.12.2. In a checkout without `atx-engine` sources, the registry cross-check is skipped with a printed note.
