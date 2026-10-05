# Lane COV re-review 2 (fix round 2)

## Verdict
**APPROVE.** Required: 0. Suggested: 3 (S10, S11, S12; none blocks the merge).
- R1b is resolved.
- R2b is resolved. I found no remaining unchecked wrap in the container.
- COV-10 holds. In my own mirror, atx-cov-v1 meets the band in 115/120 seeds and the a = 0.5 control
  fails it in 100/100. So the outcome at seed 29 is the typical one, and one fixed seed is acceptable.

## Reviewed
- **Scope:** the diff `2639de2e..c8ff4f1c` on `feat/p9-cov-20261003` (pool-24), written to scratch and
  read in full:
  - `fb6201b8`: R2b, the geometry check.
  - `e9fc3668`: R1b and the S7 nit.
  - `c8ff4f1c`: the report and evidence only (`git diff --name-only e9fc3668 c8ff4f1c` lists only
    sprint-dir files).
- **Source files read at the head:**
  - `cov_container.{hpp,cpp}` (the whole reader, writer and encoder);
  - the band and S7 tests;
  - `strategy_risk_model.{hpp,cpp}` (estimator, S2b call, harness);
  - `eigen_adjust.{hpp,cpp}`;
  - the RiskStore (`strategy_spo.cpp:653-828`) and the verb's budget and cov-info paths;
  - `book/risk_target.cpp` `check_model`.
- **Evidence:** `cov-evidence/mega-p9-cov-h-receipt.json` has Source `e9fc3668` (the code head),
  DirtyEntries 0 and exit 0. The `-f2` logs show:
  - RiskCovContainer 21/21;
  - Risk* 431 passed, 1 nightly skipped;
  - Cov* 29/29;
  - unchanged filter 104/104.
- **No build, no gtest re-run.** Everything was decidable by reading plus the mirror. The lane has no
  Python tests (C++ only), so there is no pytest command to re-run.
- **Mirror:** my own numpy 1.26.4 mirror, synthetic only. It ran on the system `Python312`, not the
  atx-db venv. The core is in the Appendix. Scratch files are deleted after this commit.

## (a) R1b: band panel at HL 21, negative control, COV-10. **Resolved.**

**The diff does what was asked, and nothing in the recipe moved.**
- `band_config` (`atx-impl/tests/strategy_cov_fixture.hpp:345-352`) sets vol HL = corr HL = 21 (was
  42). It is a test-only fixture with one user (`strategy_cov_model_test.cpp:627-631`).
- The diff touches no file under `atx-impl/src`, `atx-impl/tools` or `eigen_adjust.*` (empty diff).
- The atx-cov-v1 constants are the brief's (brief-COV.md:82-87; `strategy_risk_model.hpp:102-104,
  137-140`): HL 84 / 504, NW 5 / 2, 64 simulations, a = 1.0, seed 7, simulated length = Kish of the
  correlation-HL weights. COV-5 holds.

**The panel was not tuned to fit.**
- The panel, `BandPanel(500, 300, 29)`, is unchanged since round 1. The seed was not re-picked after the
  HL change. K = 7 is unchanged.
- The band (per rank 1 +- sqrt(2/T); pooled 1 +- sqrt(2/N), i.e. +-0.025 at N 3,283) is the design's
  formula. It is not widened.
- HL 21 was the re-reviewer's prescription, not the lane's choice.

**The negative control is genuine.** (`strategy_cov_model_test.cpp:629-631, 644-646`)
- It uses the same panel and the same CovV1 config with `eigen_amplification = 0.5`.
- That value reaches `eigen_adjust_v2` (`strategy_risk_model.cpp:201-203`). The per-session seed is the
  same, so v_k is identical and only gamma = 1 + 0.5(v - 1) differs.
- `EXPECT_EQ(half.pooled_n, cov.pooled_n)` ties it to the same observations.
- Its predicate `|pooled - 1| > band || share < 0.95` is the exact negation of the two atx-cov-v1
  assertions (:639-640).
- Side benefit: if a caller ever dropped the config and used the kernel default (1.4), the control
  would land in the band and the test would fail. So the plumbing of a is tested too.

**The report's per-rank table matches the log.** I compared it with `cov-gt-cov-all-f2.log:37-41`
digit for digit:
- atx-cov-v1: 1.013 0.998 1.013 1.020 0.993 1.021 1.009, pooled 1.009, 7/7.
- a = 0.5: 1.085 ... 1.007, pooled 1.042, 6/7.
- v1.1: 1.181 ... 0.996, pooled 1.080, 3/7.
- The churn rows equal round 1's.
- The margins are also right: 0.36 / 1.70 / 3.24 pooled band widths; worst rank 0.021 / 0.065 = 0.32.

**COV-10 mirror.**

*What the mirror reproduces* (by code reading at c8ff4f1c):
- BandPanel's generator and `standardize_style`: the 3.5 x 1.4826 MAD fence, clip 3, the short-interest
  style standardised twice.
- The OLS cross-section with the residual industry constrained to 0, which gives K = 7.
- The **demeaned** EWMA covariance (`LaggedEwma::zero_lag`), Kish over sessions 1..t, and
  `eigen_adjust_v2`: uncentred (1/T) f f', T = round(Kish), 64 sims, D_adj = gamma^2 D.
- The harness: first formation at t = 30, so T = 469 per rank; ranks re-sorted on the forecast block;
  b = SD with ddof 1.

*Pass rates:*

| setting | seeds | meets the band (pooled in AND 7/7 ranks) | pooled b, mean +- SD |
|---|---|---|---|
| **atx-cov-v1 (a = 1)** | 120 | **115/120 = 0.96** | 1.0145 +- 0.004 |
| **a = 0.5 control** | 100 | **0/100 (fails 100/100)** | 1.047 +- 0.0045 |
| atx-risk-v1.1 | 100 | 0/100 (pooled above the band 100/100) | 1.085 +- 0.0047 |
| whole test (all four assertions) | 100 | **97/100** | |
| a = 0.75 | 100 | 7/100 | 1.030 |
| gamma instead of gamma^2; n_eff x 2; uniform eigen scale | 40 each | 0 / 0 / 0 | 1.048 / 1.049 / 1.010 (uniform fails 7/7 in every seed) |
| a = 1.25 | 60 | 60/60 | 1.000 |
| a = 1.5 | 60 | 52/60 | 0.986 |
| a = 1.75; a = 2 | 20 each | 0/20; 0/20 | 0.974; 0.962 |

*The five atx-cov-v1 misses:*
- Four are rank-share misses (seeds 2, 10, 103 and 107: 5/7 or 6/7) with the pooled b in band.
  - Each comes from a near-degenerate bottom pair. Regression noise of about 3.3e-7 per factor
    compresses the two smallest true eigenvalues, e.g. to 2.47e-6 / 3.97e-6 at seed 2.
  - The two ranks then mix and show opposite-sign b (1.10 / 0.91). That is sampling mixing, not
    mis-scaling.
- One is a pooled miss by 0.0003 (seed 28).

*Seed 29 is typical.*
- The C++ values (1.009 / 1.042 / 1.080) lie 1.1-1.25 SD below the mirror means. That is one correlated
  draw, since the three settings share a seed's data.
- The C++ v1.1 per-rank b (1.181 1.108 1.104 1.084 1.039 1.036 0.996) agrees with the mirror's rank
  means (1.181 1.131 1.102 1.080 1.057 1.036 0.998).
- At seed 29 the test passes with margins of 0.36 pooled and 0.32 worst-rank band widths. A Debug /
  Release round-off difference cannot flip it.

*The level offset is the estimator, not the kernel.* atx-cov-v1 sits at about +0.6 band widths, the
same across ranks (the largest rank is 1.010).
- It is the demeaned EWMA's lost degree of freedom (E sigma_hat^2 = sigma^2 (1 - 1/Kish)), which the
  uncentred S2b simulation does not model.
- Check: with demeaning switched off in the mirror (10 seeds), atx-cov-v1 is 1.0066 and its top rank
  0.9995, against 1.017 / 1.0105 with demeaning.
- This also explains the first re-reviewer's 1.0056. It is not a kernel defect: at production Kish(84)
  ~ 242 the term is about 0.2%, and the VRA absorbs level.

## (b) R2b: K = 1 + G + S without wrap. **Resolved.**

**The fix is applied in all four places, in 64 bits:**
- `cov::geometry_ok` (`cov_container.hpp:76-82`): G <= 4096, S <= 4096, and u64 K == 1 + u64 G + u64 S.
- The header check (`cov_container.cpp:637-638`) runs before the trailer and root, so a wrapped header
  is refused without hashing.
- `parse_cov_block` (`:213-215`).
- `check_block` (`:100-101`) bounds G and S before `spec.factors()` (a u32 sum) is formed.
- `CovContainerWriter::create` (`:407-408`) uses the same constants. `append` and `finalize` run only
  on a spec that `create` accepted.

**Whole-file rescan.** I followed every file-derived size, offset and count that feeds an allocation,
an index or a pointer:
- **Header and trailer:** size >= header + trailer, then `size - sizeof(trailer)`.
- **Section table:** remaining-length form; count <= 64.
- **Section extents and sizes.**
- **Padding walk:** `covered` is monotone and <= data_end.
- **Names.**
- **Index, hashes and blocks:** count == d with exact record sizes. So the `make_unique<atomic<u8>[]>(d)`
  allocation is bounded by the file size.
- **Block walk.**
- **`typed<T>`:** count x size <= remaining, 64-byte offset, 8-byte base.
- **Products, all safe in u64:** `rows * dense` <= 2^32 x 4096, `k * k` <= 8193^2.
- **`factor_risk_view`:** `p * s` and `k * k` resizes are bounded by the block's own arrays.
- **`stored_hash` and `block_bytes`:** t < d.

No wrap or out-of-bounds path remains. Downstream:
- K <= 8,193 makes `book::check_model`'s usize `k * k` (`risk_target.cpp:35`, pre-existing) safe through
  a container.
- The one value the reader does not validate is the per-row group (S11). Both consumers check it before
  indexing: `scatter_cov_block` (`strategy_risk_model.cpp:1641`) and `check_model` (:38-40).

**The lane's "the test fails on the old code" holds.** I verified it by tracing `2639de2e` with the
test's inputs (make_spec: G 4, S 3, K 8; 12 rows).
- **Block** (0, 2^32-1, 0) and (0, 0, 2^32-1):
  - The old u32 sum is 0 == K, so the geometry check passes.
  - Exposures then need rows x S, which is 0 or 12 x (2^32-1) in u64, against a count of 36. The
    refusal is "array dtype or count".
  - The same holds for (4098, 4097, 0).
  - So `has(r, "block geometry")` fails on the old code.
- **File** (root re-hashed; `factor_count` lies inside the 224-byte root prefix):
  - The old header check passes, the root matches, and the refusal is "factor names: K NUL-terminated
    names expected" (:760, before the index loop).
  - So `has(r, "header geometry")` fails on the old code.
- **Encoder:** the old code refuses with InvalidArgument, but with no "geometry" in the message, so the
  `find("geometry")` expectation fails. The report names the wrong old message (S12, a nit).
- **Not discriminating, but correct:**
  - `CovContainerWriter::create` already had the 4096 bound.
  - The G = 4096 edge accepts the geometry and is then refused by the array check.

**The count-clause nit is now exercised.**
- Test: `risk_cov_container_test.cpp:945-957`.
- The table offset is in range and `section_count = 64` (allowed, since the check is `> 64`). Every
  other clause holds, so the refusal can only come from `count x 64 > data_end - offset`. The test
  asserts this premise (`ASSERT_LT`) and checks the message.

## (c) Suggested items from earlier rounds
- **S7 counter: fixed.** `CovBias.UnformedExtendedBooksAreCounted` (`strategy_cov_model_test.cpp:846`)
  drives `form_extended` three ways:
  - a non-finite off-diagonal (count 1, no book);
  - an LLT failure from a negative industry variance (count 1);
  - a positive-definite control (count 0, all 4 names held).

  It is not a tautology: each case takes a different branch.
- **S3: "unreachable through the verb" verified.**
  - `config.eigen_max_bytes` is `--max-bytes` when given (`strategy_risk_verb.cpp:1117`), else the
    kernel default of 256 MiB.
  - `load_role` refuses any budget <= 64 MiB + cells x 96 (:124).
  - The verb sets no half-life, so n_eff is at most Kish(504) = (1+d)/(1-d) ~ 1,454.
  - So the kernel's worst workspace is 62^2 x 128 + 1,454 x 62 x 64 = 6,261,504 B (6.0 MiB), far below
    the smallest admissible budget. The lane's arithmetic is exact.
  - The guard and its kernel-boundary test (51,199 / 51,200 B) are a sufficient cover.
- **S7 "gross 0 skipped silently": accepted as a nit.** w = V^-1(alpha - c 1) is 0 only for alpha
  constant on the held names. With continuous random scores that has probability zero.
- **S1, S5, S8, S9:** the dispositions from re-review 1 stand. S5 is superseded by COV-7; S9 is a root
  check per COV-9.

## (d) Flag-absent identity and the as-of invariant
**Untouched.**
- The round's only non-test code adds a pure `constexpr` predicate and two constants, and tightens three
  refusals.
- The default recipe (v1.1, no `--emit-container`) never reaches the container code.
- With the container, the impl geometry is 62 / 50 / 11. That passes `geometry_ok`, and the encoder
  writes the same bytes as before.
- Every existing store (directory or container) opens as before.
- No line of the estimator, harness, verb, RiskStore or NAV path changed. So the as-of rule (block t
  reads sessions <= t; `update_vra(t)` pushes f_t before `factor_forecast(t)`) and the truncation /
  leak tests are as re-review 1 left them.

## (e) Expected values and the seal
- No expected value, hash or pin was edited. Assertions were only added, and the band formula is
  unchanged.
- The band panel's sessions are synthetic weekdays from 2019-01-02 (500 sessions, ending in 2020).
- I opened no real data and nothing dated >= 2024-01-01. I read no 2020-2023 statistic: the mirror is
  synthetic, and the evidence logs are synthetic test output.

## Findings

### Suggested
1. **S10** `atx-impl/tests/strategy_cov_model_test.cpp:644-646` | Suggested | The band has teeth on
   under-correction only.
   - Problem: in the mirror, a = 1.25 passes 60/60 and a = 1.5 passes 52/60, while a >= 1.75 fails
     20/20.
   - Why it is not Required:
     - COV-10's criterion is met.
     - a is fixed by COV-5 and recorded in the manifest.
     - The a = 0.5 control already proves the value is plumbed through.
   - Fix (one more run, about 13 s Debug): add a second control at `eigen_amplification = 2.0` that must
     miss the band, with the same predicate. In the mirror it fails 20/20 (pooled 0.962). This gives
     the band two-sided teeth.
2. **S11** `atx-engine/src/risk/cov_container.cpp:202-327` (and `factor_risk_view` :329) | Suggested
   (for COV-MV) | Group values are not validated on read.
   - Problem: `parse_cov_block` checks geometry but not the writer's row rule. In particular a per-row
     group <= G is unchecked, so a re-hashed file can deliver a group > G to `factor_risk_view`.
   - Today it is safe: both consumers validate the group before indexing.
   - Fix: either enforce g <= G in `parse_cov_block` (one loop over `v.group`, ParseError), or state in
     `cov_container.hpp` that a `FactorRiskView` from a container must go through `book::check_model`.
     COV-MV, the zero-copy consumer, should do one of these.
3. **S12** `task-COV-report.md:492-494` | Suggested (report nit) | The report misquotes one old-code
   message.
   - Problem: on the old code, the encoder refusal of G = 2^32-1 would be "exposures are not rows x
     dense" (`check_block` checks exposures before the covariance), not "factor covariance is not K x
     K".
   - The conclusion still holds.
   - Fix: correct the sentence if the report is edited again. No code change.

## What root must check at merge
1. **S9 (COV-9).** Before every COV run (steps 3-5 and D-COV), confirm that the role manifest has
   `universe.delisting.returns_applied == false`. It was false for `train-2020-2023-lo3` in review 1. The
   verb guard is P10's.
2. **Bench link and run (root step 6).**
   - Steps:
     - Use the `rel` preset with `ATX_BUILD_BENCH=ON`.
     - Build `atx-engine-bench` target-scoped.
     - Run `--benchmark_filter=CovContainer --benchmark_repetitions=5`.
     - Log the medians against cov-design 4.12.
   - The bench TU has had only a `/Zs` syntax compile. It has never been linked or run. A compile or link
     error there goes back to the lane.
   - This round's header additions are `constexpr` and unused by the bench.
3. **COV-7: Debug = Release before D-COV.**
   - Before step 5 / D-COV, show Debug = Release byte identity of the container for both recipes on the
     lane fixture: equal root SHA-256, and `cov-diff` reports no differing block.
   - If they differ, the Release ruling is void and the run waits.
   - The band test's margins (0.36 / 0.32 band widths) are not the evidence for this. Identity must be
     shown on bytes.
4. **Root step 1 counts:** `RiskCovContainer.*` 21, `Cov*` 29, unchanged filter 104, `Risk*` 431 plus 1
   nightly skip.
   - Root step 2 (the base-anchored flag-absent identity on real data) remains the proof of S1.
   - Keep both C2's and COV's tail lines in `atx-impl/tests/CMakeLists.txt`.
5. **If the band test ever fails after a toolchain or seed change:** re-run a multi-seed mirror (COV-10).
   Never widen the band.
6. **Engine owner note:** `atx-engine/src/book/risk_target.cpp:35` computes `k * k` in usize. This is
   pre-existing and now unreachable through a container (K <= 8,193).

## What the churn-panel small-rank bias means for reading D-COV
The churn panel differs from the band panel in four ways:
- the cap-weighted industry constraint moves with membership churn;
- variance HL is 21 against correlation HL 504, with NW 5 / 2;
- the VRA is on;
- T = 119.

There atx-cov-v1 cuts eigen-00 only from 7.12 to 6.07 and eigen-01 from 2.43 to 2.15, about 15-28% of each
small rank's excess. Ranks 00-05 all stay at or above 1.12. On the well-posed band panel, the same kernel puts
every rank in band. So the residual is not a kernel defect. It comes from four sources S2b is not built to
fix:
- the near-null constraint direction, a structural error that eigen-00 carries (about 2/3 of the pooled
  sum of z^2);
- S2b simulating Kish(HL_corr) observations while F's variances carry Kish(HL_var) (S8);
- unsimulated NW noise;
- VRA level effects.

D-COV on TRAIN therefore cannot be read against the absolute band. On production settings, small ranks
above band are the expected design result. Read it like this:
1. **Per rank, not pooled.** Print per-rank b, and the pooled family with and without the lowest rank
   (or the rank aligned with the industry cap-share vector).
2. **Relative to atx-risk-v1.1 on identical observations.** Look at the per-rank reduction of |b - 1|,
   minvar / optimized b, QLIKE and MRAD.
3. **Read adjacent ranks with opposite-sign deviations as a mixing pair**, as in the mirror's
   near-degenerate seeds, not as two mis-scaled ranks.
4. **Do not count a uniform level shift of a few tenths of a percent across all ranks as an eigen
   effect.** It comes from the demeaned estimator, and at large ranks it can also come from the VRA
   reading the adjusted prior (read it with lambda^2_factor).
5. **The size of the remaining small-rank excess is the P10 recipe input.** Candidates: simulated length
   = Kish of the variance HL, or simulating the EWMA estimator itself. Under COV-5, any parameter change
   after D-COV is a new recipe id.

## Appendix: mirror core (numpy 1.26.4; synthetic; parameters as in (a))
```python
def standardize(raw):                    # standardize_style, equal caps
    med = np.median(raw); s = 1.4826 * np.median(np.abs(raw - med))
    v = np.clip(raw, med - 3.5 * s, med + 3.5 * s); mu = v.mean()
    sd = np.sqrt(((v - mu) ** 2).sum() / (len(v) - 1))
    return np.clip((v - mu) / sd, -3.0, 3.0)

def s2b_bias(u, d, t_sim, rng):          # eigen_adjust_v2's v_k (64 sims, uncentred, sample basis)
    f_true = (u * d) @ u.T
    b = rng.standard_normal((64, len(d), t_sim)) * np.sqrt(d)[None, :, None]
    f_sim = np.einsum('ij,mjt->mit', u, b); f_m = np.einsum('mit,mjt->mij', f_sim, f_sim) / t_sim
    dh, uh = np.linalg.eigh(f_m)
    return np.sqrt(np.maximum(np.einsum('mik,ij,mjk->mk', uh, f_true, uh), 0) / dh).mean(axis=0)

# per seed: Q = qr(N(0,1) 7x7); lam = 2e-6 * 100**(k/6); f_t ~ N(0, Q diag(lam) Q'); x ~ N(0,1) 300x6;
# r = f0 + x f_s + 0.01 e; Z = standardize (si twice); f_hat_t = OLS of r_t on [1, Z], t = 1..499.
# t = 1..499: s0, s1, P <- decayed (d = 2^(-1/21)) sums of 1, f_hat, f_hat f_hat'; Kish from sum w, sum w^2.
# t = 30..498: F = P/s0 - m m' (m = s1/s0); D, U = eigh(F); v = s2b_bias(U, D, round(Kish));
#   D_adj = (1 + a (v - 1))^2 D; (l, W) = eigh(U diag(D_adj) U'); z_{t+1} = W' f_hat_{t+1} / sqrt(l).
# b_k = SD(z_k, ddof 1) over T = 469; pooled over N = 3283; band 1 +- sqrt(2/T) / 1 +- sqrt(2/N).
```
