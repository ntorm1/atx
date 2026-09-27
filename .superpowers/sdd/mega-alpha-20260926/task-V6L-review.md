# Task V6-L review: alpha library v6 (commit `9d302e4c`, pool-5)

Reviewer: Opus 5.5, read-only. I edited, built and ran nothing, apart from the pure-Python, no-data checks listed under
"Verified". Neither reading nor checks touched any 2023+ file. From `build-equity/mega-weights-v51-ew/admission.json`
I printed only `id` and `tau`. The fields-v6 manifest was read as metadata only (field names, `mkt_ret` / `xrd_ttm` /
`cfo_ttm` / `earn_recent` definitions and NaN-reason counters).

**Verdict: APPROVED.** 0 Critical, 0 Important, 7 Minor (all optional or controller-side).

## Verified

- `python -B generate_fund_ic_v6.py --check` gives ok. The library sha256 is `5ee66d13…6a` (29,795 B) and the recipe
  is `36c08452…2b`; both match the report. The registry cross-check (16 operators) is ok.
- `check_fund_ic_v6.py --manifest <fields-v6 manifest>` exits 0. It reports added 5, removed 5, changed 31 and
  unchanged 2, which matches the report.
- `pytest test_generate_fund_ic_v6.py` gives 15 passed. The worktree is clean afterwards.
- Canonicalisation is identical to v4/v5: `json.dumps(indent=2, ensure_ascii=True, allow_nan=False) + "\n"`, and the
  generator sha is taken over LF-normalised bytes (`generate_fund_ic_v5.py:206,239`). Output order is deterministic:
  insertion-ordered dicts, the v5 field order, and sorted extras.
- The parents are unmodified: the commit only adds 5 files. The v5.1 pins `9e5ea08c` / `a26670b0` are re-derived and
  asserted.
- Base `04e9d5bc` vs the brief's `e0dfb8c7`: the difference is docs only (5 sdd markdown files). Harmless.
- Fast sleeves: the admission taus I printed myself are ind_adj_rev_5 .1808, seasonality .0938, si_change .0915,
  iv_rv_spread .0898, low_max .0887, and next low_ivol .0753 (< .08). The file sha prefix `4f06bd18` matches the
  recipe pin.

## (1) Spec compliance (brief + prereg v6 V6-L)

| item | result |
|---|---|
| one variant per hypothesis | OK. cfoa's v5.1 citation is literally "BGLN (2016) cash-based operating profitability", so keeping cfoa next to cbop would be two variants. The 1-year issuance fallback already exists twice, so none was added. `ear` already is the EAR-centred member. |
| prior signs vs literature | All 38 are correct (checked individually). **bac**: -corr, long low correlation (AFGP 2020 BAC). **smax**: -(MAX/vol), long low scaled MAX (AFGP 2020 SMAX; BCW 2011). **cbop**: + (BGLN 2016). **value_composite**: + (FF92/LSV94/ILR21). **res_mom**: + (BHM 2011). **seasonality**: + (same-month winners repeat, Heston-Sadka 2008). **Shorting flow** (needs-new-field DSL): `-ts_mean(short_vol/volume,126)`, i.e. negative, as in Wang-Yan-Zheng 2020. **iv_rv_spread**: + on IV-RV (Bali-Hovakimian: RV-IV predicts negatively). **accruals, asset_growth, noa, issuance, SI, rev5**: -1 raw. Every sign has an author-year entry in `PRIOR_SIGN_SOURCES` (generate:112-160). |
| roster <= 48 | 38 |
| fields only from fields-v6 | OK. 37 used, all in the 40-field manifest or the base fields. I confirmed the manifest has no SG&A, COGS or daily short volume, so the needs-new-field entries are genuine. |
| smoothing rule | Applied exactly. The 27 members switched are those whose x holds no Cs op. `ind_mom_12_1` and `within_ind_mom` stay byte-identical because x holds a masked group op, so the blackout binds under either order. The test at test:274-280 demonstrates this. The 4 measured fast sleeves are R(x) with no decay. The blackout basis is correct: field loads keep the observation mask and only Cs ops are member-masked (`vm.hpp:425-432`); ts ops use full windows (`ts_ops.hpp:25-26`). |
| no TRAIN return statistic used | OK. The generator reads no TRAIN artefact; the taus are transcribed constants, and tau is a turnover statistic that the brief mandates. The report discloses that scorecard HAC t was visible (the brief's reading list includes it). Every choice traces to the literature, the VM semantics or the budget. |
| not implemented, correctly documented | Heston-Sadka lags 24/36 need 504/756 bars; the runner admits <= score_begin-63 (`strategy_ic_runner.cpp:595-596`). 5-year CI needs 1260 bars. XFIN needs 6 extras (controller ruled cap 5). Intangible value has no SG&A. FINRA flow has no short-volume field. MAX5 has no top-k op. All are listed under NEEDS_NEW_FIELD; nothing was invented. |

## (2) Code quality

- **DSL validity and bounds.** Max prior bars is 272 (bac: 1+2+249+20; smax: 252+20; res_mom: 1+230+21 = 252),
  under 314. `ts_count_nans` is rolling with lookback d-1 = 0 (`typecheck.cpp:42`), so the private v4 validator row
  matches the native one. The IC runner compiles with the full `al::Library` (`strategy_ic_runner.cpp:556-569`), with
  no op allowlist, so `power` and `ts_count_nans` are admissible. The native slot count and typecheck are still
  unverified, so step 0 `--plan-only` is mandatory before the u pass. Even 8 slots would add only about 52 MB, still
  inside 1536 MiB.
- **cbop R&D fill, `power(x, 1-n) - n` with `n = ts_count_nans(x, 1)`.**
  - `ts_count_nans` returns a finite 0/1 count and never NaN once t >= 0 (`ts_ops.hpp:375-388`).
  - `Pow` is a plain `std::pow` on every path: unfused and fused `vm_apply_ew` (`vm.hpp:255-256`, `fusion.hpp:91`),
    the oracle (`oracle.hpp:511`) and the streaming engine (`streaming_engine.hpp:550`).
  - The DAG strength-reduces only `Const 2.0` (`dag.hpp:298-308`), and fast-math is banned repo-wide
    (`CMakePresets.json:152`).
  - So the construct is exactly as correct as UCRT's `pow(NaN, +0) = 1`, which C99 F.10.4.4 and IEEE 754-2008
    require. Residual risk is low; see m1.
- **Residual momentum.** It is a proxy, not BHM's construction, and it is disclosed.
  - The algebra is right: b = rho·sd_r/sd_m, and the sample/population normalisations cancel in the ratio. The test
    matches v4.2's `ts_regression` base at rtol 1e-9. I checked the v4.2 DSL byte-for-byte against
    `fund_industry_ic_v42.json`.
  - Because the fit is in-window, S_r - b·S_m = n·alpha. The score is therefore the standardised formation-window
    CAPM alpha against the EW ADV-universe market, not a residual relative to a 36-month FF3 fit.
  - It is point in time: `delay(.,21)` wraps the whole expression, and `mkt_ret` row d uses membership at d-1 and
    closes at d-1/d (manifest clock).
- **value_composite.**
  - The ebit_ev truncation is forced, not chosen. Any subset that includes ebit_ev needs oi_ttm, debt, che, me_company
    and grp_ff12 plus at least one more numerator, which is >= 6 extras. So {bm, ep, cfp} is the only feasible 3-subset.
  - Its non-uniform scale (a mean of 3 ranks) does not matter, because composition re-ranks every candidate
    (`strategy_ic_composition.cpp:25-35`).
- **Checker.** Tokenisation is sound and fails safe on unknown identifiers or exponent literals. It uses an allowlist
  plus a denylist, and checks declared vs manifest vs used fields, extras <= 5, DSL <= 4096 B, and the family/theme
  pairing. The tests cover forbidden op, prior_sign -1, missing field, duplicate id and manifest removal.
- **Tests.** They are meaningful:
  - causality and lookback via future permutation and past reversal for all 38 candidates;
  - membership-gap blackout (old form: 21 NaN sessions; new form: the gap day only);
  - cbop zero-fill;
  - v4.2 identity for res_mom;
  - composite = mean of the 3 within-FF12 ranks;
  - smax/bac orientation (bac = -1 on a pure market clone);
  - `load_priors` on the v6 library + recipe.

## Findings (all Minor)

| # | file:line | issue / failure scenario | fix sketch |
|---|---|---|---|
| m1 | `generate_fund_ic_v6.py:347-352`; `test_generate_fund_ic_v6.py:228-229` | The cbop fill relies on UCRT `pow(NaN,0)=1`. If that is non-conformant, cbop silently shrinks to R&D reporters: xrd_ttm has 1,208,598 visible-NaN member cells vs 89,670 for cfo_ttm (manifest nan_reasons). The signal would still be defined, so nothing errors. In `test_pow_nan_zero_is_one`, the `math.pow` clause proves nothing (CPython special-cases NaN before calling libm). The `np.power` clause on the Windows wheel reaches the CRT `pow`, which is weak but real evidence. | Controller: after the u pass, assert cbop coverage >= v5.1 cfoa coverage (a failure would be about 10x lower). Optional: reword the test comment. A pinned-semantics spelling is `(ts_count_nans(xrd_ttm,1) > 0) ? 0 : xrd_ttm`: it typechecks natively (`typecheck.cpp:201-217`), but the v4 Python validator would need ternary support. Not worth it now. |
| m2 | `generate_fund_ic_v6.py:386` | cbop takes tier A- (the v6-literature grade), while cfoa, the same paper and hypothesis, was A. This changes the (tier, roster) redundancy order: opbe (A-, roster 11) now precedes cbop, whereas cfoa preceded opbe. It is declared (`tier_basis_v6`) and literature-based. | Controller ratifies the tier rule together with the roster-order rule. No code change needed. |
| m3 | `check_fund_ic_v6.py:78-79` | "Prior sign present" checks only the library's `prior_sign == 1`. The literature direction and author-year exist only in the recipe lineage, which the independent checker never reads. | Optional: add `--recipe` and require `raw_prior_direction` in {-1, +1} plus a non-empty author-year `prior_sign_source` per id. |
| m4 | `generate_fund_ic_v6.py:510` | `assert len(ids) == ... == V5_CANDIDATES` ties the v6 roster size to v5.1's 38. They match only because 5 went in and 5 came out, so any legitimate roster change (e.g. XFIN under a future cap of 6) trips a misleading assert. | Use `V6_CANDIDATES = 38` or `<= MAX_ROSTER`. Nit. |
| m5 | `generate_fund_ic_v6.py:399-414` | The res_mom citation is imprecise. With the in-window fit, the score is standardised CAPM alpha (closest to Gutierrez-Prinsky 2007 / Chaves 2016), and BHM 2011 is the family anchor. res_mom_12_1 also re-tests a v4.2 trial with an identical base. | Optional: add the closer citation. Controller: record it as a re-trial in the v6 DSR/trial ledger. |
| m6 | smoothing form (brief rule) | Decaying before ranking lets a one-day outlier in a price-scaled ratio (a bad `me` print) dominate `decay_linear` for up to 21 sessions. Before, its influence was bounded to a rank with newest-day weight 21/231. This is inherent to the review-I3 fix. | Info only. |
| m7 | `generate_fund_ic_v6.py:425-433` | smax keeps low_max's numerator (MAX1 over 21 sessions; low_max tau .0887 >= .08) but gets s21 as an unmeasured addition. This is consistent with the brief's measured-tau rule. | Info only: V6-W's fast-sleeve shrink will use smax's measured tau. |

## Rulings on the report's concerns

1. **5-field budget.** The controller ruled to keep the cap at 5, so XFIN stays on the needs-new-field list (its DSL
   is recorded). The composite subset was forced (see above). Accepted.
2. **cfoa replaced by cbop.** Accepted. Keeping both is precisely what one-variant-per-hypothesis forbids, and
   v6-literature §3.6 proposes replacing cfoa (and accruals). Keeping accruals (Sloan 1996, a separate hypothesis) is
   the conservative reading of "add".
3. **pow trick.** Accepted; see m1. Controller gates: step 0 `--plan-only`, and the coverage check after the u pass.
4. **Seasonality re-centred to delay 231/252.** Accepted, and it is required rather than discretionary.
   `generate_fund_ic_v4.py:600-604` documents 224/245 as the decay-compensated version of the undecayed [t-252, t-231].
   Removing the decay without re-centring would misalign the Heston-Sadka window by about 7 sessions.
5. **BAC without vol-quintile conditioning.** Accepted. `quantile()` is F64 and group ops need a Group classifier.
   The review-I2 collinearity with beta252 + vol63 persists, but V6-W(a) drops the theme.
6. **smax MAX1 vs MAX5.** Accepted. The DSL has no top-k statistic: `ts_max`, `med` and the rank-based
   `ts_quantile` are the nearest ops. AFGP 2020 is the correct SMAX source. The brief's Novy-Marx-Medhat attribution
   is loose: v6-literature §3.3 cites NMM 2025 for profitability pricing defensive equity. MAX5 is on the
   needs-new-op list.
7. **Market-only residual.** Accepted as a documented proxy. FF49 via a masked `group_mean` inside a 231-session
   window would black out 231 sessions. See m5.
8. **Roster order** (additions go before the incumbents they upgrade). Accepted.
   - It is literature-based (ILR 2021, BHM 2011) and declared before any TRAIN read.
   - It only matters within a tier: value_composite vs cfp/fcfp/ebit_ev (all B+), and res_mom vs mom_12_1 (both B+).
   - Appending the additions last would be an equally arbitrary choice, one that favours the incumbents.
   - Controller: ratify it in the ledger before the u pass, together with m2.
9. **ind_mom / within_ind_mom blackout unfixed.** This is the correct reading of the brief. The report's alternative,
   decay inside the group op (`rank(group_neutralize(decay_linear(mom,21), grp_ff49))`, and the group_mean analogue),
   would fix it. Controller option for a later revision.
10. **Composite NaN on the intersection.** Accepted and disclosed.
11. **Native compile unverified.** Step 0 is mandatory before the u pass. Expect max_compiled_slots <= 7, lookback 272
    and required_bytes of about 1,449,071,914.

## Verdict

**APPROVED.** The implementer has nothing that must be fixed; m1-m5 are optional polish. Controller actions before or
at the u pass:
- (a) run step 0 `--plan-only`;
- (b) assert cbop coverage >= v5.1 cfoa coverage after the u pass;
- (c) ratify the roster-order and tier-source rules in the ledger;
- (d) count res_mom_12_1 as a re-trial of the v4.2 candidate.
