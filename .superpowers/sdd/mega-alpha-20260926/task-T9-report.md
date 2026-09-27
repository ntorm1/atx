# Task T9 report: TRAIN-only composition weight fitter (`mv-shrink-0.9-nonneg-v1`)

Status: **DONE_WITH_CONCERNS**. The concerns are small interpretation choices, all listed under "Decisions and concerns".
Worktree `C:/atx-wt/pool-3`, branch `feat/mega-alpha-weights-20260926`, based on `9003f273`.

## Commits
- `403e56fe feat(impl): TRAIN-only composition weight fitter [mega T9]`. It adds only two files:
  - `atx-impl/tools/fit_composition_weights.py`: the tool, numpy only.
  - `atx-impl/tools/test_fit_composition_weights.py`: synthetic unittest fixtures.

No C++ and no CMake changes. Nothing needs building.

## Tests (synthetic only, run by me)
`"C:\Program Files\Python312\python.exe" -m unittest discover -s atx-impl/tools -p test_fit_composition_weights.py -v`
gives **16/16 OK in about 3 s**. pytest collects the same 16 and passes them. pyflakes is clean.

**HandComputed**
- Centered tied ranks: the hand case `[3,1,3,2,NaN]` gives `[1/3,-1/2,1/3,-1/6,0]` bit-exact with the C++ arithmetic. The result matches a literal port of `each_centered_rank` bit-for-bit on 40 random rows with ties and masks.
- MV hand case: `S=[[1,.5,0],[.5,1,0],[0,0,4]]`, `mu=[.1,-.01,.2]`, so `Sh=[[1,.05,0],[.05,1,0],[0,0,4]]`.
  - The raw solution is `[.1005/.9975, -.015/.9975, .05]`.
  - After clipping and normalizing, the weights are `[.1005/.150375, 0, .049875/.150375]`, both to 1e-12.
  - The same weights come out when they are fitted from orthogonal-pattern factor series with that exact covariance.
- A solution that is all nonpositive refuses.
- tau hand case: rows `[.5,-.5,0,0]`, `[.25,-.25,.25,-.25]`, `[.5,-.5,0,0]`, `[.5,-.5,0,0]` give |dq| sums 1, 1, 0, so tau = 2/3 with deployment excluded. A constant book gives 0.

**Exposures**
- The vectorized returns, market and exposures match a literal Python port of `strategy_price_exposures.cpp` loops. The comparison covers d in {0,1,5,40,62,63,100,127,150,165,229,231}, including clipped windows, a genuine split (kept), an uncorroborated adjusted spike (guarded on both intervals), |log adj| > 1.5 (guarded), absent gaps, zero volume and too few beta pairs. Returns agree to atol 1e-15 with identical NaN sets; exposures agree to rtol 1e-11.
- The standalone book q has:
  - sum|q| = 1;
  - X'q < 1e-13 against the rebuilt design;
  - zeros off the used rows;
  - q(sign -1) = -q(sign +1) exactly.
- A 40-member decision is refused as `too-few-usable-names` and recorded.

**EndToEnd** (fixture: 232 x 64 role, 6 candidates, full CLI path)
- Weights match an independent slow reference to rtol 1e-9. The reference uses per-date literal loops, `lstsq` residuals and explicit moments.
- Signs:
  - the sign -1 candidate earns weight once it is oriented;
  - the sign 0 candidate gets weight 0 and null stats, but still reports tau;
  - the wrong-way candidate has a negative MV solution, is clipped to 0 and is marked `clipped`;
  - the one-name candidate is `degenerate-zero-variance` with weight 0 and tau 0.
- tau matches the reference to rtol 1e-9. `weighted_standalone_turnover` equals sum_k w_k tau_k, and `tau_flagged` equals {tau > .70}.
- A Python port of the runner's `composition_weights()` rules accepts the output: schema, library_sha256, every id present, no unknown ids, finite >= 0, no duplicate keys, size <= 1 MiB.
- Bytes are canonical and deterministic: two runs give identical bytes.
- Output is exclusive: a re-run to the same path refuses and the file is unchanged; there is no leftover `.pending`.

**Refusals**
- A role whose score_end_ns is after 2023-01-01.
- A cache sidecar whose role is not `train`.
- A library or orientations pin mismatch.
- A tampered cache payload (SHA mismatch).
- A missing cache entry.
- A tampered role payload (receipt SHA).

## Performance (synthetic, full geometry)
Setup: 1155 x 5627, about 3000 members per decision, 754 decisions, through the real CLI including disk reads and SHA checks.
- 10 candidates: **29 s wall, peak RSS 488 MiB**.
- The context build (exposures, bases, forward returns) takes 13-20 s depending on machine load. Each candidate takes 0.55-0.87 s, covering read, SHA, rank, 2 projections, f and tau.
- Extrapolation: **about 100 s for 100 candidates** (brief limit < 150 s) and about 175 s for 200. RSS stays flat at about 0.5 GiB because candidates are streamed.

## Interfaces (verified against the source)
**Output file (`strategy_ic_runner.cpp:367-393`)**
The file is `{"schema":"atx.dsl-composition-weights/v1","library_sha256":<library file sha>,"weights":{id: float>=0 for EVERY library id},"provenance":{...}}`. The runner reads the whole file under `pinned_text`, which applies the 1 MiB limit and the external SHA pin. It then parses with `unique_key_json` and ignores the extra `provenance` key. The runner pin is the SHA-256 of the file bytes, printed as `sha256` in the tool's stdout summary.

**Inputs**
- `orientations.json` is `atx.dsl-ic-orientations/v1`. Its `library_sha256` and `train_manifest_sha256` must equal the pins. Candidate rows must match the library order and have the same id, family and `sha256(dsl)`; `sign` must be in {-1,0,1}.
- The cache is `DIR/<train-manifest-sha>/<id>.{json,f64}`. The sidecar fields are checked exactly as `cached_payload_sha` checks them: schema, candidate_id, dsl_sha256, role_manifest_sha256, eval_mode, layout, dates, instruments and bytes. Two extra checks are made: payload == `<id>.f64` and **role == "train"**. The payload is SHA-verified in full.
- The role is `atx.recent-research-role/v1`:
  - every file receipt is SHA-verified, and the field contract, session axis and score boundary are checked as `strategy_data.cpp` does;
  - **TRAIN seal:** score_end_ns <= 2023-01-01T00:00Z and every session is before it;
  - score_end == dates.

**Provenance**
Rule id, lambda, method strings, library/role/source/orientations/orientations-recipe SHAs and `script_sha256` (on-disk bytes, so the same value as the bounded runner's `--bind`). The window has decision indexes and session ns for the decisions and labels, plus `turnover_transitions`. Also recorded: refused decisions and the used-row range. `blend_in_sample_TRAIN_diagnostic` holds the mean, SD and Sharpe of w.F. It is in-sample only; do not treat it as evidence.

Per candidate:
- `sign`, `status` (`fitted` | `unoriented-sign-0` | `degenerate-zero-variance`), `weight`, `mv_solution`, `clipped`;
- `cache_payload_sha256`, `dsl_sha256`;
- `factor_mean`, `factor_sd` (ddof 1), `factor_sharpe_annualized` (x sqrt 252);
- **`tau`, `tau_over_limit`** (> 0.70), `flat_decisions`.

At blend level: **`weighted_standalone_turnover` = sum_k w_k tau_k** and `tau_flagged`. **The netting ratio is NR = NAV-replay daily-full book tau / `weighted_standalone_turnover`.**

## How close the neutralization is to C++ `price-risk-v1`
**Identical definitions:**
- **Returns:** valid if both endpoints are present with finite positive close and raw_close, the return is finite, and the guard does not trip (|log adj| > 1.5 or > |log raw| + .10, using log differences as in C++).
- **Market:** equal-weight mean across **all** instruments, NaN if none.
- **beta:** 252 intervals, pairs where both are valid, at least 126, two-pass, var > 0.
- **vol:** 63 intervals, sample SD, at least 32.
- **log_adv:** mean of usable raw*volume over 63 sessions, where absent or unusable days add 0. It is NaN before a full window or when the mean is <= 0.
- **z-scores:** sample SD over the used rows, constant check sd > 1e-12*max|x|, clip +-5.
- **Refusal thresholds:** min_names 50; pivot 1e-8 on the Jacobi-equilibrated 4x4 Cholesky, a literal port; residual <= 1e-9 * entry gross, which makes that candidate-date flat.

**Differences, all at rounding level:**
- **Solver:** the fitter uses thin QR plus one re-projection, where C++ uses normal-equation Cholesky plus one refinement. Both give the OLS residual to about 1e-15.
- **Summation order:** numpy vs sequential loops.
- **Scaling:** the fitter scales to sum|q| = 1; C++ scales to the entry gross.
- Not bit-identical to C++.

**Measured agreement:** against the literal port, exposures agree to rtol 1e-11; the residual satisfies X'q < 1e-13.

## Decisions and concerns (please rule if you disagree)
1. **Member mask** = decision `member.u8` AND `present.u8`. This is the runner's `effective` mask (the support of the composed blend) and the NAV replay's `member&present&close>0`; the two agree under the role contract. The brief says "member" and the study used raw `member.u8`. The difference is members that are absent at d, which C++ also excludes downstream.
2. **Neutralization-refused decisions** (fewer than 50 used rows, a constant exposure or ill-conditioning) give **flat q = 0 for every candidate** that day and are recorded in `neutralization_refused_decisions`. The C++ leaves this policy to the caller. On real TRAIN (score_begin 399, about 2800-3000 members) I expect none; check that the list is empty.
3. **tau_k** is taken over consecutive scored decisions. The first (deployment) decision is excluded, matching the book metric, and there is no drift. A day that becomes flat or leaves flat counts as 1.0. Sign-0 candidates still report tau, which does not depend on the sign.
4. **Degenerate candidates** (sign != 0 but factor SD = 0, e.g. never two finite names) are dropped from the solve with weight 0 and status `degenerate-zero-variance`. The alternative was a singular-matrix error.
5. Rank rows are the used rows with a finite signal, per the brief. The IC-runner blend ranks over every effective member with a finite signal. This affects only the standalone factor definition, not the pinned composition.
6. **Commit trailer:** I used the lane-contract/system form `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, not the brief's "(1M context)" variant.
7. **Line endings:** `.py` checks out CRLF on Windows (autocrlf), so `script_sha256` is the checkout's on-disk bytes, not the git blob. Pin by the bounded runner's `--bind` receipt.

## Divergences from the study prototype (`studies/compose_study.py`); the brief won each time
| Item | Study | Fitter |
| --- | --- | --- |
| Signs | sign of own 2020-21 factor mean | runner `orientations.json` sign (21d rank IC, TRAIN); sign 0 gives weight 0 |
| Fit window | 2020-21 (2022 holdout) | all TRAIN scored decisions [score_begin, score_end-2) |
| Normalization | w / sum\|w\|, negatives kept | clip at 0, then w / sum w |
| Ranks | ordinal (stable argsort), ties broken by position | average tied ranks, C++ `each_centered_rank` formula |
| Market for beta | mean over members at t-1 | mean over all instruments (C++) |
| z-score SD | population | sample, as C++ (matters only through the +-5 clip) |
| Guard | log1p(r); an invalid raw close kept the return | C++ log-difference form; raw must be finite positive (moot under the role contract) |
| Exposure/used mask | member.u8 | member & present |
| Refusals | none | C++ min_names / constant / ill-conditioned / spanned-residual policies |
| beta/vol | one-pass rolling sums | two-pass windows, as C++ |

## Root: exact real TRAIN run
Run from `C:/atx-wt/pool-2` after cherry-picking `403e56fe`. The bounded runner requires a clean tree and runs with cwd set to the root. These are the **v1 values available today**: 48 candidates, whose cache and orientations exist.

```powershell
& "C:\Program Files\Python312\python.exe" scripts/run_bounded_research.py --output build-equity/mega-weights-v1-run --seconds 300 --max-rss-mib 2048 --min-free-mib 512 --bind atx-impl/tools/fit_composition_weights.py --bind atx-impl/strategies/slow_price_volume_ic48_v1.json --bind build-equity/recent-fast-train-2020-2022-v1/manifest.json --bind build-equity/mega-v1-train-cache-b/orientations.json -- "C:\Program Files\Python312\python.exe" atx-impl/tools/fit_composition_weights.py --library atx-impl/strategies/slow_price_volume_ic48_v1.json --library-sha256 1ec75242f0328a459ac114256f99f534eb0b8ec2e642794d3f4ad846779a09ff --train build-equity/recent-fast-train-2020-2022-v1/manifest.json --train-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493 --orientations build-equity/mega-v1-train-cache-b/orientations.json --orientations-sha256 c014e71f7d6a16da894d680dda583d1e644e70d5ccc43e7f19c983d32d9f9312 --candidate-cache build-equity/mega-candidate-cache --output build-equity/mega-weights-v1/composition_weights.json
```

**Expected cost:** v1 (48 candidates) takes about 55 s. At 100 candidates expect about 100 s, and at 200 about 175 s (the 300 s limit covers both). Peak RSS is about 0.5 GiB, so 2048 MiB is a generous cap. stdout carries a one-line JSON summary with `sha256`, the pin for `--composition-weights-sha256`, and `weighted_standalone_turnover`. stderr carries per-candidate progress.

**For v2 / 96-200 candidates:**
1. First run the IC runner TRAIN with `--candidate-cache build-equity/mega-candidate-cache` (plus the T7 `--*-fields` if used). That writes `<run>/orientations.json` and the cache entries.
2. Then substitute the library path and SHA, the orientations path and SHA, and a new `--output` / run dir.

The same TRAIN manifest and SHA apply unless the role changes. The runner's cache directory is keyed by role SHA, and the fitter reads `build-equity/mega-candidate-cache/<train-sha>/`.

**Then the pinned TRAIN run of the IC runner** is the command above plus `--composition-weights build-equity/mega-weights-v1/composition_weights.json --composition-weights-sha256 <sha from stdout>`.

**Checks for root after the run:**
- `neutralization_refused_decisions == []`;
- `used_rows_unrefused` is about 2700-3000;
- the `tau_flagged` list, which feeds the 0.70/day admission decision;
- `weighted_standalone_turnover`, the NR denominator;
- the count of nonzero weights.
