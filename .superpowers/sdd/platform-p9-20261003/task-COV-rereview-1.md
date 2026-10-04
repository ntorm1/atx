# Lane COV re-review 1 (fix round 1)

Written early on the OWNER STOP. All five check items were answered. Not done: a Newey-West mirror for
(2), and a C++ run of the proposed R1b fix (see "Not done").

## Verdict
**CHANGES_REQUIRED**: 2 new Required findings (R1b, R2b), both small. R1 and R2 are only partly
resolved. S2, S3, S4, S6 and S7 are fixed. Leaving S1, S5, S8 and S9 is acceptable for P9 (S9 with a
root check).

## Reviewed
- Fix diff `535fb057..2639de2e` (`fe80c1e2` R2 + clang-format, `9cb3f194` R1 + S2-S7, `2639de2e` report
  and evidence) on `feat/p9-cov-20261003` (pool-24). Read in full. I also read the whole current
  `atx-engine/src/risk/cov_container.cpp` (982 lines) for (3).
- Evidence: `cov-evidence/mega-p9-cov-g-receipt.json` (Source `535fb057`, 11 dirty entries, build
  admitted 21:21:37). The mtimes of all 11 code files are 21:10-21:15, before the build, and the tree is
  clean now, so the `p9-cov-g` binaries are the committed `2639de2e` code. Logs:
  - `cov-gt-cov-all-f1.log`: 28/28 Cov*, with the per-rank printout.
  - `cov-gt-engine-risk-f1.log`: 430 passed, 1 skipped; 20 RiskCovContainer.* including both new R2
    tests.
  - `cov-gt-regress-f1.log`: 104/104 on the unchanged filter.
- No C++ build or gtest re-run.
- I ran an independent synthetic mirror of the band setting in numpy 1.26.4 (scratch only, no real
  data). It follows the code exactly:
  - i.i.d. N(0, Sigma) factor returns, Sigma = Q diag(geometric 2e-6..2e-4) Q'.
  - F = diag(s) R diag(s): s from an EWMA at the variance HL, R from an EWMA at the correlation HL.
  - S2b as `eigen_adjust_v2`: 64 sims, T_sim = round(Kish of the correlation-HL weights),
    v = mean sqrt(true / sample), D_hat = gamma^2 D.
  - Eigen series z = u_k' f_{t+1} / sqrt(lambda_k); b = SD(z).

  It reproduces the lane's C++ numbers:

  | | mirror, 8 seeds | lane C++, seed 29 |
  |---|---|---|
  | v1.1 pooled | 1.0448 | 1.043 |
  | v1.1 ranks | 1.099 .. 0.996 | 1.111 .. 0.993 |
  | atx-cov-v1 pooled | 1.0041 | 1.003 |

## (1) R1: the band panel and its teeth. **Partly resolved: R1b (Required).**

**It is the design's band.**
- Per series the test uses 1 +- sqrt(2/T) (cov-design.md:218), with no kurtosis widening.
- The share in band must be >= 0.95. With 7 ranks that means 7/7.
- The pooled family must lie within 1 +- sqrt(2/N). This is the same formula applied to the pooled N,
  so it is tighter, not looser.
- No band was loosened and no test was renamed.

**The panel is fair and not tuned to pass.**
- It is well-posed: a full-rank 7-factor block, no constraint direction, no churn, no floor exclusions.
- Equal half-lives (42 / 42) and NW 0 are needed for the band to test the kernel at all.
- atx-cov-v1 passes with a wide margin. In the mirror, pooled b is 1.004 +- 0.006 across seeds,
  against a band of 0.025. The worst rank deviates 0.014 against 0.065.
- The bias statistic here is self-normalised: the EWMA forecast tracks the realised variance. Its real
  sampling SD is about 0.006 pooled and 0.013 per rank, while the i.i.d. formula gives 0.0327. The
  design's band is therefore about 2.5x wider than the noise. That is the design's band; it only means
  the teeth come from the size of v1.1's excess.

**The production half-lives / NW lags cannot be run through the band as a kernel test on any
well-posed planted panel.** This is stated plainly, from the mirror at variance HL 84 / correlation HL
504, no NW, T = 1,499:
- At K = 7, atx-risk-v1.1 is already in band (pooled 1.006 / 1.006 / 1.010 +- 0.014 over 3 seeds), so
  there are no teeth.
- At K = 30, v1.1 sits at the pooled edge (1.009 / 1.017 +- 0.015). S2b removes only about 40% of the
  smallest rank's excess (1.11 -> 1.07). It simulates Kish(504) ~ 1,454 observations while F's
  variances carry Kish(84) ~ 242 (review S8).
- The recipe's NW lags (5 / 2) add estimator noise that S2b does not simulate, so the real gap is
  larger.
- So equal half-lives are a precondition of the test, not a tuning. The production calibration is
  S8's business (D-COV / P10).

**Teeth are too thin at HL 42 / T 469.** In the mirror, 8 seeds, rate at which the test passes:

| adjustment | test passes |
|---|---|
| correct (atx-cov-v1) | 1.00 |
| none (v1.1) | 0.00 |
| reversed gamma order | 0.00 |
| over-scaled (gamma^4; n_eff x 0.5) | 0.00 |
| **half strength (gamma instead of gamma^2)** | **0.50** (pooled 1.024 against the band 1.0247) |
| **a = 0.5** | **0.62** |
| **n_eff x 2** (e.g. a Kish formula or HL-source error) | **0.50** |
| **uniform scale-up of every eigenvalue by the mean gamma^2** | **0.75** |

So the test cannot reliably tell a correct eigenfactor adjustment from a half-strength one, or from a
plain level shift. That is the class ruling COV-R1 says the band must catch.

- **R1b** `atx-impl/tests/strategy_cov_fixture.hpp:345` (`band_config`, HL 42) and
  `atx-impl/tests/strategy_cov_model_test.cpp:620-635` | **Required** | The band panel's v1.1 excess
  is about 1.8 band widths. A half-strength, wrong-n_eff or uniform adjustment passes the band in 50-75%
  of seeds (mirror above). The planted test therefore does not reliably catch a wrong eigen adjustment
  (ruling COV-R1's stated purpose). | Fix:
  - Shorten both half-lives to 21. This is small_config's own variance HL; Kish ~61 >= K + 1 = 8, so
    the panel stays well-posed and the band, panel size and cost are unchanged.
  - In the mirror at HL 21 / T 469, over 6 seeds:
    - atx-cov-v1 passes 6/6 (pooled 1.0056 +- 0.0017, ranks 0.993..1.011).
    - v1.1 (1.075), half strength (1.039), n_eff x 2 (1.039), uniform (ranks 1.079..0.926) and
      reversed all fail 6/6.
  - Alternatively keep HL 42 and use T ~ 1,500 (all wrong variants also fail 6/6), at about 3x the
    cost.
  - Add one negative control in the test: `eigen_amplification = 0.5` on the same panel must miss the
    band (pooled out, or share < 0.95).
  - Report the C++ per-rank table at the new setting. If atx-cov-v1 then misses the band, that is the
    COV-R1 "kernel defect: stop and report" case.

## (2) Churn panel: pooled b 2.056, smallest rank 6.07. **Note for PM. No kernel defect.**

**The lane's explanation is only partly supported.**
- It is supported for rank 00. Rank 00 is an outlier: 6.07 / 7.12, about 2.8x rank 01. That does not
  fit a sampling-noise eigen bias, which is smooth across ranks (compare the band panel's 1.111 ..
  0.993). It is about 2/3 of the pooled sum of z^2 (36.9 of 54.9). Without it, the pooled b is about
  1.22.
- That rank 00 is the cap-constraint direction is plausible but not shown: there is no eigenvector
  diagnostic. Its floor exclusion is 0: the direction is near-null, not floored, so the test's floor
  rule (< 1e-6 x max) will not remove it.
- The "short variance HL" part is weak:
  - A no-churn mirror with the churn panel's half-lives (K 13, variance HL 21 / correlation HL 504,
    T 120, 12 seeds) gives v1.1 pooled 1.133 (rank 00 1.40, rank 01 1.27). S2b brings it to 1.021
    (rank 00 1.09). S2b removes most of the HL-driven excess there.
  - In C++, the churn panel's v1.1 is 2.33 (rank 01 2.43, rank 02 1.69), and S2b removes only 15-28%
    of each small rank's excess.
  - So most of the excess comes from sources other than the HL mismatch. Candidates are the moving
    constraint direction, IPOs/delistings, missing descriptors, the thin industry, the recipe's NW lags
    5/2 on HL 21 (unsimulated estimator noise; not mirrored, see "Not done"), and the VRA.
  - Not a kernel defect: S2b corrects sampling error and is not meant to fix structural breaks, and
    the band panel isolates the kernel.
- Some mid/large ranks rise under atx-cov-v1 (ranks 04 / 05 / 08: +0.01 / +0.06 / +0.07). Likely
  cause: the VRA, which is on in small_config and reads the adjusted prior (review-noted design),
  rescales the whole block. Eigen order swaps are the other possible cause.

**What it implies for D-COV (real-data adoption print):**
- (a) The pooled eigen b is the wrong headline. One constraint rank can dominate it. D-COV should
  print per-rank b (as the test now does) and give the pooled family with and without the lowest rank
  (or the rank aligned with the industry cap-share vector).
- (b) Expect atx-cov-v1 to stay above band in the small ranks on TRAIN, by design (S8: Kish(504)
  against variance Kish(84), plus the NW lags not simulated). "Not in band" is not a kernel defect. The
  adoption reading must be relative (per-rank, minvar / optimized b, QLIKE against v1.1), and the size
  of the residual excess is the P10 recipe input.
- (c) Rises at the large-rank end can be a VRA level effect. Read them together with lambda^2_factor
  before calling them harm.

## (3) R2: overflow safety, whole-file rescan of `cov_container.cpp`. **Partly resolved: R2b (Required).**

**The four cited sites are fixed and safe:**
- Array table: `:235-238`. Offset first, then the count against the remaining length; `previous_end`
  is formed only after that check.
- Section table: `:677-680`.
- Section extents: `:699-701`.
- Section sizes: `:703`, as `count != bytes / elem`.
- Block walk: `:771-786`. `block_bytes > blocks_end - next_block` is checked before the add, with the
  final check `next_block == blocks_end`.

**Also clean:**
- `table_end` (array_count <= 64).
- The index, hash and blocks counts tied to d and to the section bytes.
- `rows * dense` (a u32 x u32 product in u64).
- The Full-verify partition arithmetic.
- `first_difference`.

**The new tests reach each fixed check:**
- `ParseBlockRefusesArrayBoundsThatWrapOrLeaveTheBlock`: offset past the block, offset at SIZE_MAX-63,
  huge counts, and the exact-end accept / one-short refuse pair.
- `RefusesSectionTableAndBlockSizesThatWrap`:
  - a table offset near 2^64, refused before the root check;
  - `block_bytes` wrapping at t = 0 and t = 2, root re-hashed, under Full and Lazy;
  - META bytes near 2^64.
- Not exercised: the second clause of the table check (count x 64 > remaining with an in-range
  offset). It is a nit.

**Remaining wrap:**
- **R2b** `atx-engine/src/risk/cov_container.cpp:631` (also `:208-209` in `parse_cov_block`) |
  **Required** | `h.factor_count != 1U + h.group_count + h.dense_count` is u32 arithmetic and wraps.
  - A hash-consistent header with `group_count = 0xFFFFFFFF, dense_count = 0, factor_count = 0` passes
    `open()` and `view()`. Blocks parse with K = 0, and the header's own invariant K = 1 + G + S is
    broken.
  - `factor_risk_view` (`:324`) then yields `groups = 2^32 - 1` with an empty covariance.
  - In `book::factor_variance`, `check_model` (`atx-engine/src/book/risk_target.cpp:33-35`, pre-existing)
    compares `covariance.size()` with `k * k`. With k = 2^32 that product wraps to 0 in usize, so the
    check passes. The function then allocates 2^32 doubles and reads `m.covariance[a*k+b]` out of
    range: UB instead of ParseError.
  - Same threat model as R2 (a malformed file opened without a pin, or the public API).
  - It is latent today: the impl's RiskStore checks 62/50/11 first, `cov-info` / `cov-diff` index no
    factor arrays, and the only `factor_risk_view` callers are tests. COV-MV (wave 3b) is the planned
    zero-copy consumer.
  - Fix:
    - compute in u64 (`u64{h.factor_count} != 1 + u64{h.group_count} + h.dense_count`), and/or refuse
      G or S > 4096 as `CovContainerWriter::create` (`:402`) does;
    - do the same in `parse_cov_block`;
    - add one test (header with the wrapping geometry and the root re-hashed -> ParseError; direct
      `parse_cov_block(…, 0, 0xFFFFFFFF, 0, …)` -> ParseError).
  - Root may apply this at merge instead of a lane round: two lines and one test.

## (4) S2, S3, S4, S7 and v1.1 identity. **Fixed. v1.1 bytes unchanged.**

**S2:** `CovPanel::with_future_mean_return_value()` (`strategy_cov_fixture.hpp:235-252`) is a per-name
full-axis statistic on the estimator input, so standardisation does not remove it.
`DetectsAPlantedLeak` requires the comparator to find it at or before the cut under both recipes
(`strategy_cov_model_test.cpp:124-133`). Fixed.

**S3:** fixed. The kernel boundary is tested; the verb-level refusal is verified by reading only (a
nit).
- The pre-check `strategy_risk_model.cpp:193-197` is the kernel's own rule (`eigen_adjust.cpp:17-18`),
  identical arithmetic. It cannot underflow because the first clause short-circuits.
- `BudgetRefused` is counted as `refused_budget`.
- The verb refuses the run before `files_sink.close` (`strategy_risk_verb.cpp:1146-1152`), so there is
  no manifest and the container stays `.partial`.
- The boundary test is 51,199 bytes refused / 51,200 adjusted.
- The verb-level refusal (`--max-bytes` small -> exit with no manifest) has no test.

**S4:** `strategy_spo.cpp:754-758` refuses `last_session_ns >= kSealBeginNs && !unsealed()` on the
container path only. `kSealBeginNs` is 2024-01-01Z. The test uses synthetic sessions shifted onto the
seal. Fixed.

**S7:** fixed (with the two nits below).
- Held == the complete set is asserted (`set_mismatches == 0`).
- The dense V^-1 oracle at 1e-10 covers minvar and five optimized books.
- `extended_unformed` counts a non-finite block, an LLT or capacitance failure, and a bad `1'V^-1 1`.
  The eigen family keeps its old behaviour on a non-finite block (`eigen_` is not updated).
- Nit: the counter is only ever checked at 0; no test forces a failure.
- Nit: a single optimized book with gross 0 is still skipped silently (`:1490`).

**v1.1 identity:**
- Every edit to strategy_risk_model / verb is CovV1-gated (`:998`, the manifest extras `:1156`),
  extended-only, on the container path, or a pure reflow (the `validate_config` text is the same
  string).
- `std::array<usize, 5>` matches the enum (`BudgetRefused = 4`).
- 104/104 on the unchanged filter, `FlagsAbsentOutputsUnchanged` and `V1FamiliesBytesUnchanged` passed
  on the binaries of the committed code.
- The base-anchored proof is still root step 2.

**S6:** 0 lines over 100 columns in the new files; the owning files are at their base counts
(13 / 1 / 16). Confirmed.

## (5) Items left by the lane

**S1: acceptable.**
- The two names are the brief's acceptance contract (brief-COV.md:143, :149). Ruling COV-R1's "no
  rename" was about the band test, but the conclusion holds through the brief.
- Root step 2 is the proof.

**S5: acceptable.** It is superseded by Ruling COV-7: Release tree, with Debug = Release byte identity
of the container checked first.

**S8: acceptable as design.** See (1) and (2) for the D-COV note and the evidence that it matters.

**S9: safe for P9, but only as a root check, and the lane's reason is partly wrong.**
- The lane says a guard would change v1.1 manifest bytes. Recording `universe.delisting.returns_applied`
  only under CovV1, or only in the container META, and refusing only under CovV1, changes no v1.1
  byte.
- For P9 it is safe because the pinned role `train-2020-2023-lo3` has `returns_applied: false`
  (verified in the first review).
- Root should check that flag in the role manifest before every COV run: steps 3-5 and D-COV. P10 adds
  the guard.

## Notes for PM / root
1. The two Required findings are small:
   - R1b: one fixture constant, one negative-control assertion, and a C++ per-rank table.
   - R2b: two lines and one test.

   A scoped re-review of only these is enough.
2. D-COV: print per-rank eigen b. Report the pooled family with and without the constraint rank.
   Read atx-cov-v1 relative to v1.1, not against the absolute band (S8 under-correction; at K 30 in
   the mirror S2b removes about 40% of the smallest rank's excess).
3. S9: a root check of `returns_applied == false` before every COV run.
4. `atx-engine/src/book/risk_target.cpp:35` (`k * k` in usize) is a pre-existing engine wrap. It is
   harmless once R2b bounds K; record it for the engine owner.

## Not done (cut short by the OWNER STOP)
- I did not mirror the NW lags (5 / 2 with `nw_halflife`) in the churn-panel or production-HL mirrors.
  The (2) attribution of the excess beyond HL mismatch is therefore by elimination, and the production
  statement in (1) is a lower bound on the gap.
- I did not run the C++ test at the proposed HL 21: the mirror predicts a pass for atx-cov-v1 and a
  fail for every wrong variant.
- The mirror scripts were scratch only and have been deleted. Their parameters are given above.
