# Task T5 re-review, fix round 1 (2c92d658..f2d5fb97, pool-7)

Scope: findings I1-I4 and M1-M7 from `task-T5-review.md`, plus the fix diff `review-T5-fix1.diff`. File:line references are to `atx-impl/strategies/generate_price_volume_ic96_v2.py` (gen) and `price_volume_ic96_v2.json` (lib) at f2d5fb97.

Check I ran: `python -B atx-impl/strategies/generate_price_volume_ic96_v2.py --check` in pool-7 at f2d5fb97.
- Exit 0.
- Library `b871743e…e2ed` (36336 B) and recipe `c2b6de26…0480` (49733 B) match the committed bytes.
- Output: "registry cross-check ok (17 operators vs registry.cpp/typecheck)".
- `git status` was clean before and after.
- Why I ran it: the report has no Fix round 1 section, so there was no test evidence.

Read-only scratch checks (Python over the HEAD blobs):
- `candidates[:48] == v1.candidates` holds.
- All 96 `dsl_sha256` values re-verified. The recipe `library.sha256` equals the file hash.
- None of the 48 new DSL hashes is in the v1 hash set.
- No new DSL contains `vec_avg`.
- No new DSL reads bare `volume`: every use is `raw_close * volume`.
- Max prior_bars is 314. Native prior bars equal Python prior bars except `lagged_market_corr_240`, where native is 268/310 and Python is 269/311. The native typecheck charges fields 0, and the Python value is the true dependency.

## Finding Verdicts

- **I1: abnormal_volume used raw (split-unadjusted) share volume.** ADDRESSED.
  - gen:74 defines `adj_volume = (raw_close * volume) / close`, which is volume divided by the cumulative factor and so continuous across splits.
  - All three templates use it (gen:116-118; e.g. lib:882 `volume_shock_5_63_s21`).
  - The change is recorded in the family description (gen:114), in REVISIONS I1 (gen:144) and in recipe `data.split_adjusted_volume`.
  - Lookbacks are unchanged: 62, 125 and 251 base.
- **I2: the member-masked `vec_avg` blanks the market templates.** ADDRESSED, per the controller ruling.
  - `vec_avg` was removed from the REGISTRY table and from every DSL. gen:359 asserts it is absent.
  - The market return is now the field `mkt_ret`, declared at gen:37-39 and lib:18. 26 new candidates (13 templates) read it.
  - The name matches the producer exactly: `prepare_research_fields.py:139` (FIELDS key `"mkt_ret"`) and `:773` (`FieldWriter(output, "mkt_ret", role)`).
  - Producer semantics are causal:
    - row d = mean of close[d]/close[d-1]-1 over `member[d-1]` names (membership is fixed before bar d-1), present and priced at d-1 and d, and not guarded;
    - the value is broadcast to every cell present at d (`:776-796`);
    - the clock is the same as the DSL `ret` at d.
  - The broadcast removes the continuous-membership requirement. The only remaining masking is the wrapper `rank`, as in v1.
  - The recipe's `market_window_support` caveat was correctly dropped.
  - The integration dependency on T7 is noted under Out-of-Scope.
- **I3: resmom_6_1/resmom_9_1 were near-duplicates of v1 mom_6_1/mom_9_1.** ADDRESSED.
  - Both were dropped. All three templates are now standardized residual Sharpe at 6-1, 9-1 and 12-1 (gen:84, 90-92; lib:618), as the review's primary fix prescribed.
  - The review's accompanying instruction to record that `resid_sharpe_6_1` still correlates with v1 `risk_scaled_6_1` was not carried out. It does not appear in the generator, recipe or REVISIONS. See N1.
- **I4: illiquidity, ivol and max re-load v1's size and low-vol tilts.** ADDRESSED.
  - (a) The three Amihud levels are replaced by `amihud_shock_21_252`, `roll_autocorr_126` and `signed_volume_reversal_126` (gen:122-124). All three are own-history ratios, autocorrelations or correlations, with no size level.
  - (b) `ivol_21` and `ivol_126` are replaced by `ivol_change_21_252` and `ivol_change_63_qoq` (gen:104-105). `max_ret_21` is replaced by `scaled_max_21` (gen:110).
  - The implementer went beyond the review's minimum: no level template is kept in illiquidity or idiosyncratic_risk. This departs from the brief's requirement 4 wording ("Amihud … averaged over 21/63/126").
  - The departure is recorded in REVISIONS I4a/I4b and in `family_fixing.template_reselection`, and it falls under the "fix pre-measurement" ruling. Root may wish to ratify it explicitly.
  - Residual overlap is covered in N2.
- **M1: FP sigma window was 63, the same factor as v1 `low_vol_63`.** ADDRESSED.
  - gen:97 uses `stddev(ret, 252)` and `stddev(mkt_ret, 252)`; the template is renamed `fp_beta_250_252`.
  - The base lookback stays at 252, and the change is recorded (REVISIONS M1).
- **M2: `market_corr_63` duplicated `idio_share_252` with the sign flipped.** ADDRESSED.
  - It is replaced by `corr_asymmetry_126` (gen:128-129; lib:1014), with base lookback 126.
  - The proxy nature of the construct is not recorded (see N5).
- **M3: Amihud on zero-volume rows gives inf/NaN.** NOT ADDRESSED. The review marked the hardening optional and accepted the risk.
  - The fix widened the exposure. `amihud_shock_21_252` uses `ts_mean(abs(ret)/(raw_close*volume), 252)`, so one zero-volume present row now blanks the name for 252+s sessions instead of at most 126+s. See N4.
- **M4: a single bad print dominates the tail templates for up to 314 sessions.** NOT ADDRESSED. The clip was optional.
  - The market side is now protected by the producer's rough_return guard (`prepare_research_fields.py:787-788`). The per-name `ret` is still unclipped, as in v1.
- **M5: ETFs and ETNs are in the universe and in the market mean.** NOT ADDRESSED. This is a root universe issue, not T5 scope.
  - The producer caveat (`prepare_research_fields.py:145`) now documents that `mkt_ret` includes ETFs.
- **M6: `ts_skew` uses the O(d) batch path.** NOT ADDRESSED. The finding was informational with no action requested, and `skew_126` is unchanged.
- **M7: AuditExact unclamped TsCorr can flip the sign of `resid_sharpe`.** ADDRESSED for the residual-Sharpe templates. gen:83 now uses `signedpower(abs(1 - rho^2), 0.5)`, which keeps the alpha sign.
  - The same hazard reappears in the new ivol_change templates (N3).

## New Breakage in the Fix Diff

None Critical or Important.

- **N1 (Minor): the I3 correlation note was never recorded.** Fix-added `resid_sharpe_6_1` (gen:90) is structurally close to v1 `risk_scaled_6_1`.
  - Both have the same formation window [t-125, t-21].
  - The numerators differ only by beta·M, which the review's own I3 arithmetic sizes at sd ~0.05 against R ~0.25-0.30.
  - The denominators differ only by residualization, where sqrt(1-R²) has log-sd ~0.1-0.15 against ~0.45 for the vol level, and by 105 versus 126 sessions.
  - My prior rank correlation is ~0.85-0.9. It is not identical at the DSL or formula level.
  - Fix: add the expected-correlation note to the recipe. A note does not change any DSL or dsl_sha256, so it is not a selection trial.
- **N2 (Minor): `ivol_change_21_252` is not recorded as correlated with v1 `vol_expansion_21_126`** (gen:104). The template is the review's own suggestion.
  - log(ivar21/ivar252) = log(var21/var252) + log(1-R²₂₁) - log(1-R²₂₅₂). This shares the noisy 21-session variance numerator with v1 −(sd21/sd126).
  - My prior |rank corr| with v1 `vol_expansion_21_126` is ~0.8-0.85 (the sign is absorbed by fitting), plus ~0.7 with v2 `vol_term_63_252`. It is not a formula duplicate.
  - Fix: record it alongside N1. The T11 admission screen (greedy |rho| <= 0.70) will cull any empirical duplicate.
- **N3 (Minor): the M7 hazard is back in the new ivol_change denominators.** `idio_var(n) = ts_var * (1 - rho^2)` without `abs` (gen:85), used as a denominator in `ivol_change_21_252` and `ivol_change_63_qoq` (gen:104-105).
  - Under the AuditExact unclamped batch TsCorr, a tiny negative denominator flips the ratio's sign, and |rho| = 1 exactly gives ±inf.
  - This only affects series near-collinear with `mkt_ret`, such as equal-weight index ETFs (M5), so it is immaterial otherwise.
  - Fix: use `abs(unexplained(n))`, the same hardening as gen:83.
- **N4 (Minor): zero-volume exposure is wider** (M3 carry). One zero-volume present row NaNs `amihud_shock_21_252` for 252+s sessions, against at most 126+s before.
  - Members have ADV > $5M, so this stays rare.
  - Optional fix: `raw_close * max(volume, 1)` in `amihud` (gen:75).
- **N5 (Minor, docs):**
  - (a) The `mkt_ret` basis text (gen:38-39; lib:18; recipe `data.market_return`) omits two parts of the producer definition (`prepare_research_fields.py:142`): the present[d-1]/present[d] requirement and the rough_return guard, which excludes |log adj ratio| > 1.5 or |log adj| > |log raw| + 0.10.
  - (b) The new proxies are cited without recording their deviation from the cited definitions:
    - `corr_asymmetry_126` is the correlation of truncated series, not Ang-Chen exceedance (conditional) correlation;
    - `signed_volume_reversal_126` is a correlation with relative dollar volume, not the Pastor-Stambaugh regression gamma on dollar volume with an r_t control;
    - `roll_autocorr_126` is an autocorrelation, not the Roll autocovariance.

    REVISIONS lists the swaps but not these deviations.
- **N6 (Minor, process): the report has no fix-round section.** `task-T5-report.md` has no "Fix round 1" section, so there are no covering-test claims. The `--check` run above supplies the evidence: it passes, and the registry cross-check covers the new `max` row (gen:160 against registry.cpp `MaxP`).

**Causality of fix-added candidates:** all causal.
- Every window and delay is a positive literal.
- `roll_autocorr_126` pairs ret_t with ret_{t-1}.
- `signed_volume_reversal_126` pairs excess_t with delay(sign(excess)·DV/mean63(DV), 1).
- `ivol_change_63_qoq` is delay 63 of a trailing quantity.
- `mkt_ret` at d uses only closes at d-1 and d and member[d-1].

**Split handling of fix-added candidates:**
- Where volume enters, it is either split-adjusted `(raw_close*volume)/close` (abnormal_volume) or split-invariant dollar volume `raw_close*volume` (Amihud shock, and the signed-volume reversal normalized by its own 63-session mean).
- No candidate uses bare `volume`.

**v1 duplication:**
- No dsl_sha256 collisions.
- No formula-identical construct.
- Near-duplicate priors are listed in N1 and N2.

**Turnover observation (tau_k <= 0.70):** no v2 candidate is a raw one-day signal, so none is flagged.
- Every new candidate is `decay_linear(rank(base), s)` with s in {21, 63}.
- Even a white-noise base under s = 21 has lag-1 autocorrelation Σ_{m=1}^{20} m(m+1) / Σ_{m=1}^{21} m² = 3080/3311 ≈ 0.930. That gives a Gaussian-approximate one-way tau ≈ sqrt(2(1-0.93)) ≈ 0.37 per day, before membership churn.
- The fastest bases are the s21 variants of `volume_shock_5_63` (5-session numerator), `scaled_max_21`, `ivol_change_21_252` and `amihud_shock_21_252`. Their bases are already autocorrelated, so they should sit below that bound.
- The one-day components (`roll_autocorr_126`, `signed_volume_reversal_126`) sit inside 126-session correlations, so they are slow.
- The actual tau_k is measured at admission.

## Out-of-Scope Observations

- **Integration (relay to t7-fields): the current root runner rejects the whole v2 library, including the v1 48.**
  - `atx-impl/src/strategy_ic_runner.cpp:275-277` requires the library `fields` set to equal {close, raw_close, volume}, and fails with "IC runner: declared field contract". v2 now declares a fourth field, `mkt_ret` (lib:18).
  - `:300-301` also requires every program field to be declared, so the declaration itself is correct.
  - T7 must relax the equality to "base fields ⊆ declared, and each extra declared or referenced field is in the pinned fields manifest". The T7 brief's requirement 3 does not name this check explicitly.
- **Stale plan-only result.** The v2 library SHA changed from `0c7f3059…` to `b871743e…`. Root's earlier native `--plan-only` result (96 compile, max slots 8, lookback 314) is stale and must be re-run once T7 lands.
- **Producer check for root.** `mkt_ret` is NaN for all names on zero-contributor sessions, which would blank every mkt_ret template for up to 314 sessions. Root should confirm in the TRAIN fields manifest that `contributors_per_session.zero_contributor_sessions` is 1 (row 0 only). Row 0 lies outside the scored windows given the score_begin - 63 admission margin.

## Verdict

**Fix round:** All findings addressed, no new Critical/Important breakage.
- I1-I4 and M1, M2 and M7 are ADDRESSED.
- M3 and M4 (optional), M5 (root scope) and M6 (informational) remain open as accepted non-blocking Minors.
- New breakage is Minor only: N1-N6. N1 and N2 are recipe notes that are cheap to add before TRAIN measurement.
