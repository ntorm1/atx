# Task T31 report: fitter `ew-theme-aim-v1`, `v5_train.sh`, netting ratio / paired dSR in `nav_summ.py`

**Status:** DONE_WITH_CONCERNS (the concerns are in the brief/acceptance wording, not the code; see "Concerns").
**Worktree / branch:** `C:/atx-wt/pool-4`, `feat/mega-alpha-v5-aimfit-20260927`, base `d4ec515d`.
**Commits:** `e863cd65 feat(fitter): ew-theme-aim-v1 aim gains from TRAIN rank autocorrelation; v5 pipeline; netting ratio (T31)`,
then the report commit `docs(mega-alpha): T31 report` (this file, `git add -f`).
**No C++, no build, no CMake registration.** No real data run (only read-only header/provenance inspection of existing
pool-2 artifacts to pin column names and SHAs, as ruling R-f allows).

## Files

| File | Change |
|---|---|
| `atx-impl/tools/fit_composition_weights.py` | `ew-theme-aim-v1`: constants, rank/autocorrelation/gain primitives, aim work records, weights, `provenance.aim`, gating, docs |
| `atx-impl/tools/test_fit_composition_weights.py` | +14 tests (classes `AimGainRules`, `AimEndToEnd`, `V1BytesUnchangedByAim`) |
| `.superpowers/sdd/mega-alpha-20260926/studies/v5_train.sh` | new: `fit` / `w` / `nav` phases, all inputs pinned |
| `.superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py` | rewritten as importable module + CLI; old per-scenario lines kept |
| `.superpowers/sdd/mega-alpha-20260926/studies/test_nav_summ.py` | new: 6 synthetic tests |

## Design

### Fitter (R4', rulings R-a..R-d, R-g)

Constants exactly as the brief: `AIM_RULE_ID = "ew-theme-aim-v1"`, `AIM_THETA = 0.05`,
`AIM_LAGS = list(range(0, 22)) + list(range(28, 127, 7))` (37 lags), `AIM_GAIN_MIN, AIM_MAX_LAG = 0.05, 126`,
`PRIOR_COMPOSITIONS = (EW_THEME_RULE_ID, AIM_RULE_ID)`, `COMPOSITIONS` extended. Plus `AIM_MIN_NAMES = 50`.
Gating line (FIT:1170 old) is now exactly
`require(prior == (args.composition in PRIOR_COMPOSITIONS), "--composition ew-theme-v1|ew-theme-aim-v1 and --screen v4-prior-v1/v2 go together")`.

Functions (pure numpy, brief signatures kept):
- `standardized_ranks(signal, live, min_names=50)`: the fitter's tie-aware `centered_tied_ranks` (scipy is not imported
  by the fitter, R-g) over `live & isfinite(signal)`, standardized to mean 0 / population SD 1 per decision; NaN on
  decisions with < 50 live names or zero rank variance (all tied, e.g. a constant candidate). Vectorized (no per-day loop).
- `lag_correlations(z, lags)`: per-day `c_j(d) = sum_i z_d,i z_d-j,i / n_both(d)` over names finite on both days,
  NaN when `n_both < 50` or `d < j`. Two `einsum`s per lag (no big temporaries).
- `rank_autocorrelation(z, lags)`: mean over days with a defined `c_j(d)` (the brief's function).
- `aim_gain(rho, lags, theta, max_lag, clip=True)`: brief formula verbatim (`np.interp` over 0..126, NaN -> 0, clip).
- `aim_profile(z, train_mask)`: rho + g over all TRAIN decisions and the two half samples (report only).
- `ew_theme_aim_weights(themes, gains)`: brief formula verbatim (global normalisation), refuses non-finite/zero gains.
- `aim_record(...)` / `aim_record_valid(...)`: the per-candidate cached record (below).
- `aim_provenance(...)`: builds `provenance.aim`.

Wiring:
- `ensure_records(..., aim=False)` now returns `(records, computed, reused, aims)`; `aim=True` only for
  `ew-theme-aim-v1`. For each candidate still to do it loads the verified VM payload once, recomputes the factor record
  only if stale and the aim record only if stale. Without `aim` the path is exactly the pre-T31 one (no aim record is read,
  computed or written; `aims is None`).
- `fit_prior(..., aims)`: `ew_theme_aim_weights(...)` if aim else `ew_theme_weights(...)`; `provenance.rule` and the
  summary's `composition` use `args.composition`; `composition` / `fit_series` texts differ only on the aim path (the v1
  strings are byte-for-byte the old ones); `provenance.aim` and per-candidate `aim_gain` exist only on the aim path.

Work records (R-d): `<work>/<train-sha>/<SEMANTICS_TAG>/aim-<AIM_TAG>/<payload>[.f-<fields>].json`, sealed with
`content_sha256` and bound to `script_sha256` (= `SCRIPT_SHA256`), `context_sha256`, cache payload SHA, fields manifest
SHA, VM identity, `AIM_SEMANTICS`, `theta`, `lags`. Holds `rho` (37, null for undefined), `rho_half`, `gain`,
`gain_unclipped`, `gain_half`, `rank_decisions`, `half_split_decision`, per-decision `coverage`. A tampered or stale record
is recomputed (tested).

`--max-seconds` already existed (T11): it stops cleanly between candidates with **exit 3** and prints the incomplete JSON
on stdout. Kept as is, not changed to exit 0 (that would break the existing exit-code contract and tests); the exit-3 JSON
now also carries `"partial": true` (the R-d marker). `v5_train.sh` retries on exit 3 (or a bounded-runner `time-limit`)
up to 3 passes.

`provenance.aim` schema (aim path only; everything but the weights inputs is report-only):
```
aim: { theta: 0.05, lags: [0..21, 28..126/7], max_lag: 126, gain_min: 0.05, gain_max: 1.0, min_names: 50,
       semantics, gain_formula, rho_definition,
       rho: {id: [37 floats|null]}, gain: {id: g}, gain_unclipped: {id: g_raw}, gain_half: {id: [g_first, g_second]},
       half_split_decision_index, gain_half_note,
       coverage_mean: {id: x}, coverage_effective_theme_weight: {theme: x}, coverage_definition,
       members: [ids with weight > 0] }
themes[t] (aim path): {admitted_count, nominal_theme_weight, aim_theme_weight, admitted}
candidates[k].aim_gain (aim path)
```
Keys are emitted through the existing `canonical_bytes` (sorted keys, `allow_nan=False`), so the v1 layout is untouched.

### `v5_train.sh` (R-e)
Copy of `v4_train.sh`'s structure (cd to `C:/atx-wt/pool-2`, same bounded-runner line, `rc` helper) with:
- **no `u` phase**: R1'-R3' are unchanged, so the fitter reads the v4 unweighted TRAIN pass `mega-v4-train-u-1`
  (orientations `11cfd3e4`, summary `b32bbed0`); screen `v4-prior-v1` (R3').
- every input pinned as a constant **and** verified with `sha256sum` at start (`pin`): library `daa9663e`, recipe
  `62b510f1`, TRAIN role `210fff96`, fields-v6 `32565c32`, orientations `11cfd3e4`, summary `b32bbed0`; nav-time pins for
  `C_ew` `24a6cc76` (`mega-v4w-train-1/train_combined.json`), `9a9c949a` weights and `880a0a6a` admission.
- `fit`: `COMP=ew-theme-aim-v1` -> `build-equity/mega-weights-v5-aim`; `COMP=ew-theme-v1` -> scratch
  `build-equity/mega-weights-v5-ew-refit`. Flags `--work-dir build-equity/mega-fit-work-v5 --max-seconds 150`, up to 3
  passes (`-run1..3`), stop on refusal. Aim success prints the g_k table (id, theme, g, g halves, rho1, rho21, coverage,
  weight; theme nominal / aim / coverage-effective weights; gain range check). The ew re-fit runs the equality check
  against `9a9c949a` + `880a0a6a` (see Concern 1) and exits 1 if it fails.
- `w`: weighted TRAIN IC pass with `W_aim` -> `build-equity/mega-v5w-train-aim-<i>` (+ `.final`, and
  `mega-v5w-train-aim.combined.sha256` = sha256 of `train_combined.json`, re-verified at nav time; the bounded receipt
  records only log hashes, so the pin is taken with `sha256sum` right after the completed pass).
- `nav`: env `COMBINED` (`ew`|`aim`), `THETA`, `DUST`, `RATE` (`fixed`|`per-name`), `LEV` (default 1) ->
  `build-equity/mega-nav-v5-$COMBINED-t$THETA-d$DUST-$RATE` (`-L$LEV` appended only when `LEV != 1`, so the R5' extra cell
  cannot collide with the L = 1 cell); flags `--rule aim-partial-v5 --cadence 1 --trade-fraction $THETA --dust-multiple
  $DUST --aim-leverage $LEV`, plus `--rate per-name-v1 --rate-rra 10 --rate-min .01 --rate-max .15` for `per-name`;
  `--daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824` as v4.
  Never `--band-multiple`. Then `nav_summ.py --weights <W of that combined> [--reference <ew t.05 d.1 fixed>] <cell>`.
- CRLF: blobs are LF; with `core.autocrlf=true` a cherry-pick checks the script out with CRLF. Tested: this Git Bash
  ignores CR (a CRLF copy of `v5_train.sh` passes `bash -n`; CRLF heredocs run). No `.gitattributes` change.

### `nav_summ.py` (R-f, R6')
Now an importable module + CLI: `nav_summ.py NAV_DIR [NAV_DIR ...] [--weights W ...] [--reference REF] [--scenario S]
[--draws 2000] [--block 21] [--seed 20260927] [--json OUT]`. The old per-scenario lines are printed unchanged (null
statistics print `na` instead of crashing); old positional use (`nav_summ.py DIR`, as `v4_train.sh`) still works.
Column names checked against `mega-nav-v4-train-b2-f.25/daily_modeled-1bn-stale5-v1+swap-fin-v1.csv` and the row rules
against `strategy_nav_replay.cpp:1485-1560`:
- return rows = `return_observation == 1` and not row 0; SR = mean / sd(ddof 1) x sqrt(252) (= `Moments::sharpe`).
- mean gross / net / |net| leverage and held names = the previous row's value over return rows (as the summary).
- held_share = summary `construction.v5.mean_held_share` (T30); `na` without it.
- tau_t = `one_way_turnover_gmv` over `executed == 1`, `pretrade_gross_dollars > 0`, deployment (first executed row
  with `traded_dollars > 0`) excluded; mean and numpy-linear p95; cross-checked with the summary mean (warning > 1e-9).
- cost per unit GMV turnover = sum `trade_cost_return` (return rows) / sum tau_t (as T38 Step 5 specifies), plus
  `cost_bps_traded` = 1e4 sum `trade_cost_dollars` / sum `traded_dollars` (executed rows), the unit-clean rate.
- netting ratio = summary `daily_turnover_gmv.mean` / `weighted_standalone_turnover` of the `--weights` file whose
  SHA-256 equals the NAV summary's `composition_weights_sha256` (`--weights` repeatable; one unmatched file is used and
  flagged `UNMATCHED`).
- `--reference`: nets aligned on common `session_ns`; dSR = SR(cell) - SR(ref); rho; **Memmel (2003)** SE
  `sqrt([2 - 2rho + (a^2 + b^2 - 2ab rho^2)/2] / T) x sqrt(252)` (a, b per-session SRs); **circular block bootstrap**
  (2000 draws, block 21, seed 20260927) percentile 95% CI; **Ledoit-Wolf (2008) studentized** circular block bootstrap
  95% CI + p-value (R6' names this one): delta-method SE on (mu_a, mu_b, E a^2, E b^2), Bartlett HAC lag 20 on the
  sample, block estimator `Psi* = sum_j S_j S_j' / T` on each resample, symmetric `|t*|` 95% quantile,
  p = (#{t* >= |d|/s} + 1)/(M + 1). LW works in population moments; its centre `dSR_pop` is printed (differs O(1/T)).

## Decisions (small ambiguities decided; none changes a pre-registered quantity)

1. **Live names** for ranks = the context's used rows (member & present & price-risk exposures ok) with a finite signal:
   the universe the standalone book ranks over. Decisions outside TRAIN are NaN (none exist in the real role).
2. **Correlation** = the brief's code: mean over the names finite on both days of `z_d * z_{d-j}`, `z` standardized over
   each day's full live set (the autocovariance of the standardized signal; equals Pearson over the intersection when
   the live sets coincide). Written into `provenance.aim.rho_definition`. `min_names = 50` for both ranks and pairs.
3. **Degenerate members**: unchanged shared rule (admitted, factor SD 0 -> `degenerate-zero-variance`, weight 0). A
   constant candidate never reaches it: its book is never live, so it has 0 live TRAIN days and the screen rejects it
   (`reject_insufficient`, weight 0); its rho is all null and g = 0.05 is reported. An admitted member whose ranks are
   undefined (< 50 finite names every day) keeps the **floor gain 0.05** (R4' letter: NaN lag -> 0 -> clip), not weight 0.
   Irrelevant on real data (every admitted candidate is live on 754/754 decisions over thousands of names) but pinned by
   a fixture (`medium_half`).
4. **Coverage** `c_k(d)` = live names / used rows (name coverage). A day-boolean "book live" definition would make the
   coverage-effective weight identical to `aim_theme_weight` on real data (all 31 members live every decision), i.e.
   uninformative. Report only.
5. **Half samples**: split at `h = first TRAIN decision + floor(n/2)`; lag pairs straddling `h` are in neither half.
6. Extra report-only keys beyond R-c: `gain_unclipped` (shows how far below the floor), `coverage_mean`, `members`,
   the semantics strings, `half_split_decision_index`; per-candidate `aim_gain` (aim path only).
7. `--max-seconds`: existing exit-3 semantics kept (see above); `partial: true` added to the exit-3 JSON.
8. Tests are `unittest` classes like the rest of the module; the brief's `run_fitter` / `v4_fixture` /
   `BYTE_STABILITY_V1_SHA256` do not exist in the module, so the existing `Fixture` / `v4_world` / `V4_ARGS` are reused.

## Concerns

1. **The literal "re-fit reproduces `9a9c949a`" acceptance (T38 Step 1, plan-s11 item 6, R-a "SAME digest constant")
   cannot hold for any edited fitter.** There is no digest constant in the test module (no `byte_stability` test by that
   name). `composition_weights.json` embeds `provenance.script_sha256` (the fitter's own SHA-256), `context_sha256` (digest
   of metadata that includes the script SHA) and `admission_sha256` (whose admission.json embeds both). `9a9c949a` embeds
   `69c18270` (T23 fitter); even the unedited HEAD fitter (T27, `a71f768e` in pool-2) would not reproduce it. The existing
   byte-stability contract (T23 `DefaultBytesUnchanged`, T27 `PriorV1BytesUnchanged`) is "identical bytes after replacing
   those derived SHAs", against the previous fitter from git history. T31 adds `V1BytesUnchangedByAim` (vs blob
   `fd644cba` = fitter at `d4ec515d`, both `v4-prior-v1` and `v4-prior-v2`, outputs **and** work-cache keys; green), and
   `v5_train.sh` (`COMP=ew-theme-v1 … fit`) does the real-data version: it pins `9a9c949a`/`880a0a6a`, substitutes the
   three derived SHAs and requires byte identity of `composition_weights.json` and `admission.json`
   (`EW-REFIT … IDENTICAL`), failing loudly otherwise. **Root: record that normalized result instead of SHA equality.**
2. The brief's `test_aim_gain_ar1_matches_closed_form` asserts `|g - closed| < 1e-9` with `AIM_LAGS`; that is false by
   construction (linear interpolation of the convex `(1-phi)^j` between 21..28, 28..35, ... overstates it by ~1e-4).
   Replaced by: exact equality (1e-12) on the full 0..126 lag grid, and on `AIM_LAGS` `0 < g - exact < 1e-3` plus the
   untruncated GP closed form `theta/(theta + phi(1-theta))` within 1e-3.
3. Sanity expectation (T38, not a gate): "slow themes g >= 0.8". With theta = 0.05, g <= 1 - 0.95^127 = 0.9985 and
   an AR(1) rank with daily decay phi needs phi <= ~0.013 (half-life >= ~53 sessions) for g >= 0.8; quarterly-refresh
   fundamentals should clear it, but the synthetic AR(0.97) world gives 0.62. Report the table; do not tune.
4. `cost per unit GMV turnover` as specified mixes a NAV-unit numerator with a GMV-unit denominator and includes the
   deployment-session cost in the numerator but not its tau; printed as specified, with `cost_bps_traded` alongside.

## Tests (exact output)

```
$ "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/tools/test_fit_composition_weights.py -q
.....................................................................    [100%]
69 passed in 16.95s

$ "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider .superpowers/sdd/mega-alpha-20260926/studies/test_nav_summ.py -q
......                                                                   [100%]
6 passed in 1.09s
```
55 pre-existing fitter tests unchanged and green (incl. `DefaultBytesUnchanged` vs the pre-T23 blob and
`PriorV1BytesUnchanged` vs the pre-T27 blob). New fitter tests (14): `test_declared_constants`,
`test_aim_gain_ar1_matches_closed_form`, `test_aim_gain_degenerate_and_gappy` (plan-s11 item 4: flat stretch + all-NaN
stretch -> finite rho, g in [0.05, 1]; constant -> all NaN ranks, g at floor; lags beyond the overlap -> NaN -> 0),
`test_standardized_ranks_match_rankdata_port`, `test_rank_autocorrelation_matches_loop_port`,
`test_half_sample_profile_split`, `test_aim_weights_global_normalization` (brief case = [0.4, 0.4, 0.2]; equal gains ==
ew-theme-v1), `test_members_weights_and_gains` (same members/signs/admission bytes as v1; w = g/(T n)/sum; AR(.97) members
g ~0.62 > AR(.85) member g ~0.24 + 0.2; runner port accepts), `test_aim_block_report_only_fields`,
`test_coverage_effective_weight_formula` (hand case), `test_ew_theme_v1_has_no_aim_keys`,
`test_incremental_resume_and_shared_factor_records` (exit 3 + `partial`, resume byte-identical, v1 re-fit reuses all
factor records, tampered aim record recomputed, v1 work dir grows no `aim-*`), `test_combination_refusals`,
`test_ew_theme_v1_bytes_unchanged` (plan-s11 item 6). nav_summ (6): closed-form paired SR/rho/Memmel (orthogonal +-1
patterns, rho = 0 exactly; identical series -> CI [0, 0]), CBB index structure, seeded + calibrated bootstrap (CBB width /
(3.92 Memmel SE) = 1.00, LW 1.06 on iid normal T = 1500), LW p < 0.01 for a clear difference, construction stats and
netting ratio by hand, end-to-end CLI (weights SHA matching, `UNMATCHED`, alignment on common sessions, `--json`).

Real-scale cost estimate (synthetic 754 x 2500 panel): ranks 0.16 s + profile 0.07 s, 108 MiB traced peak -> ~0.5 s and
~215 MiB per candidate at 754 x 4996, on top of the ~0.7 s factor record (v4 fit: context 15 s, 37 x 0.7 s, 40 s,
506 MiB). Expected aim fit: ~60-75 s, < 700 MiB, **1 pass** (<= 3 guaranteed by `--max-seconds 150`).

## Root command lines (from `C:/atx-wt/pool-2`, after cherry-picking both T31 commits; tree clean)

No build targets (Python and bash only). Test: the two pytest lines above.

```bash
S=.superpowers/sdd/mega-alpha-20260926/studies
# T38 Step 1 - aim fit: expect 1 pass (~60-75 s, < 700 MiB); prints the g_k table and W_aim SHA
COMP=ew-theme-aim-v1 bash $S/v5_train.sh fit
# ew-theme-v1 re-fit check: expect 1 pass, computed 0 (all factor records reused), "EW-REFIT ... IDENTICAL" x2
COMP=ew-theme-v1 bash $S/v5_train.sh fit && rm -rf build-equity/mega-weights-v5-ew-refit   # keep the -run* receipts
# T38 Step 2 - weighted TRAIN IC pass with W_aim: expect 1 pass (v4: 34 s, 504 MiB, warm cache)
bash $S/v5_train.sh w
# T38 Step 3 - NAV grid, 10 bounded invocations (1 pass each; v4 cell 26 s / 338 MiB). Reference cell first so every
# later cell prints its paired dSR against it.
for C in ew aim; do
  for cell in "t.05 d.1 fixed" "t.03 d.1 fixed" "t.08 d.1 fixed" "t.05 d0 fixed" "t.05 d.1 per-name"; do
    set -- $cell; THETA=${1#t} DUST=${2#d} RATE=$3 LEV=1 COMBINED=$C bash $S/v5_train.sh nav
  done
done
# T38 Step 4 (only if the reference cell's mean gross < 0.90): +2 disclosed cells, dirs get -L$LEV
LEV=<round(1/mean_gross_ref, 3)> THETA=.05 DUST=.1 RATE=fixed COMBINED=ew  bash $S/v5_train.sh nav
LEV=<same>                       THETA=.05 DUST=.1 RATE=fixed COMBINED=aim bash $S/v5_train.sh nav
# T38 Step 5 - grid table (netting ratio per matched W, dSR vs the reference, Memmel SE, CBB + LW CIs)
"C:/Program Files/Python312/python.exe" $S/nav_summ.py \
  --weights build-equity/mega-weights-v4/composition_weights.json \
  --weights build-equity/mega-weights-v5-aim/composition_weights.json \
  --reference build-equity/mega-nav-v5-ew-t.05-d.1-fixed --json build-equity/mega-nav-v5-grid-summary.json \
  build-equity/mega-nav-v5-{ew,aim}-t.05-d.1-fixed build-equity/mega-nav-v5-{ew,aim}-t.03-d.1-fixed \
  build-equity/mega-nav-v5-{ew,aim}-t.08-d.1-fixed build-equity/mega-nav-v5-{ew,aim}-t.05-d0-fixed \
  build-equity/mega-nav-v5-{ew,aim}-t.05-d.1-per-name
```
Expected invocations: fit 1 (+1 re-fit check), w 1, NAV 10 (+2 conditional). The NAV flags come from T30/T36 and could
not be exercised here; they are emitted exactly as R-e specifies.

## Fix round 1

Scope: review `task-T31-review.md` Important 1 (R6' "DSR at N = 10 (skew/kurtosis from daily net)" had no producer).
The ⚠️ items are root checks at T38. The controller ratified the `-L$LEV` suffix, the coverage-effective definition, the
extra `provenance.aim` keys and the CSV-recomputed `mean_gross_leverage`. The Minors were not assigned and are unchanged.

**What changed (`studies/nav_summ.py`):**
- Standard library + numpy only:
  - `norm_cdf` via `math.erfc`.
  - `norm_ppf` by bisection on `norm_cdf` (matches `statistics.NormalDist.inv_cdf` to ~1e-15 in the range used).
  - `net_moments` gives the per-session SR (ddof 1), skewness and non-excess kurtosis (population moments) of the daily
    nets on the return rows.
  - `expected_max_sr(V, N)` computes SR0 = sqrt(V)[(1-γ)Φ⁻¹(1-1/N) + γΦ⁻¹(1-1/(Ne))], γ = 0.5772156649.
  - `deflated_sharpe(SR, T, γ3, γ4, SR0)` computes Φ[(SR-SR0)√(T-1)/√(1-γ3·SR+(γ4-1)SR²/4)]. It returns None if the
    denominator is <= 0.
  - `dsr_rows(moments, N)` takes V[SR_n] as the sample variance (ddof 1) of the per-session SRs of **all NAV dirs on the
    command line**. With a single dir it falls back to the Lo (2002) (1 + SR²/2)/T, and `variance_source` says so.
- `main` now analyses every dir first, since V spans the dirs, and then prints. The new flag is `--dsr-n N` (default 10,
  must be >= 2).
- Each dir's printout gets a `deflated SR (N=…)` line: DSR, SR and SR0 (per session and annualised), skew, kurtosis, T,
  V[SR_n] and its source. It sits right after the `paired vs` (dSR) line. The `--json` output carries it under
  `"deflated"` (plus `"net_moments"`) alongside `"paired"`.
- For T38 Step 5, pass all 10 grid cells (the ew reference included) as positional dirs. The `nav_summ.py` line in
  "Root command lines" above already does that, so V[SR_n] comes from the 10 cells and N = 10. The per-cell
  `v5_train.sh nav` call passes one dir, so it prints the Lo-fallback DSR, labelled as such.

**New fixtures (`studies/test_nav_summ.py`, +6):**
- `test_normal_helpers_match_the_standard_library`
- `test_net_moments_hand_case`: [0,0,0,1] gives SR 0.5, skew 2/√3, kurtosis 7/3.
- `test_deflated_sharpe_hand_case_n10_t754`: N = 10, T = 754, SR = 0.08/session, γ3 = -0.3, γ4 = 5, V = 4e-4. It
  checks against a longhand `statistics.NormalDist` computation to 1e-12, and against the pinned values
  SR0 = 0.03149196602689943 and DSR = 0.9051249340733833 to 1e-6.
- `test_deflated_sharpe_gaussian_case` has three parts:
  - exact γ3 = 0, γ4 = 3 gives the denominator √(1+SR²/2);
  - simulated Gaussian nets (200k) give skew ≈ 0 and kurtosis ≈ 3, and the denominator is within 1e-3;
  - the plan-4.E null expected max annual SR over 3 years is 0.69 / 0.91 / 1.10 for N = 5 / 10 / 20, reproduced to
    ±0.005.
- `test_dsr_rows_cross_cell_and_single_cell_fallback`
- `test_main_reports_dsr_for_every_cell`: 3 dirs plus `--reference`. It checks the cross-cell V, that the JSON
  `deflated` matches the longhand, and a single dir with `--dsr-n 5` (Lo fallback, labelled).

**Command and output:**
```
$ "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider .superpowers/sdd/mega-alpha-20260926/studies/test_nav_summ.py atx-impl/tools/test_fit_composition_weights.py -q
........................................................................ [ 88%]
.........                                                                [100%]
81 passed in 19.96s
```
The 81 tests are 12 nav_summ (6 existing + 6 new) and 69 fitter (unchanged). The fitter and `v5_train.sh` are untouched
in this round.
