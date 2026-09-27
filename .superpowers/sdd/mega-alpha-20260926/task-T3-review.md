# Task T3 review: causal price-risk exposures and target neutralization

Reviewer: read-only task reviewer (no build, no run). Reviewed in `C:/atx-wt/pool-2` at
`b9cf4023` (module + fixtures) and `429cbe43` (root CMake registration). HEAD `c47ffdaa` does not
touch these files. Inputs: `task-T3-brief.md`, `task-T3-report.md`, `review-T3.diff`, and
`.agents/cpp/agent.md` §10. I also cross-checked `atx-impl/src/strategy_target_replay.cpp` (`rough_return`
:153-184, `desired_target` :102-123).

Root evidence (not re-run): a clean compile (clang-cl 18, Debug + scoped /O2), and focused tests 15/15
(7 pre-existing + 8 new).

## Verdicts
- **Spec compliance: PASS.** Every brief requirement is met. The interpretation choices are flagged in
  the report and are defensible.
- **Task quality: APPROVE.** 0 Critical, 0 Important, 8 Minor.

The numerics are correct as far as I can derive by hand: windows, pair counting, the guard, the
equilibrated Cholesky, the refinement and the rescale. The fixtures pin the claims they make, and several
of them are constructed so that an off-by-one or a wrong exclusion would fail the test.

---

## 1. Spec compliance (brief → implementation)

| # | Requirement | Status | Evidence |
|---|---|---|---|
| 1 | New files only. CMake lines are listed in the report and applied by root. | ✅ | `b9cf4023` adds only 3 new files. `429cbe43` (root) adds the core source, the Debug `/O2 /Ob2 /clang:-finline` + `SKIP_PRECOMPILE_HEADERS` lists, and the test TU. |
| 2 | Interface shape: `PriceExposureConfig` defaults, `PriceExposureInput`, `kPriceExposureCount=3`, both signatures. | ✅ | hpp:17-30, :90-113. The additions are a composite `neutralize_price_risk`/`PriceRiskScratch` (requested by root), extra stats fields and column constants. These are documented deviations. |
| 3 | Causality: windows end at session d inclusive and never read d+1. | ✅ | `windows_at` cpp:80-91: interval block `[first, d]`, sessions `[first-1, d]`, ADV `[d+1-adv, d]`. `check_presence` reads only `[first_session, d]` (cpp:92-98). `fill_returns`/`sum_dollars` loop bounds are `< rows` and `< d+1`. |
| 4 | Market = equal-weight mean of valid adjusted simple returns across ALL instruments. | ✅ | cpp:127-144. NaN when there are no valid returns. No membership filter. |
| 5 | Valid return: both endpoints present, finite positive closes, not guarded. | ✅ | `load_logs` cpp:107-116 plus cpp:131-137. It also requires raw_close finite and positive, which the guard needs. This is the same as the replay's `priced`. |
| 6 | Guard identical to the replay: \|la\| > 1.5 or \|la\| > \|lr\| + 0.10. | ✅ | cpp:24, :133-136 vs replay :168-171. It uses the same `log(b)-log(a)` operands, the same `.10` literal and the same `!isfinite(r)` leg, so the result is bit-identical. |
| 7 | Beta = cov(r,m)/var(m) over valid pairs in the last beta_window intervals, gated by `>= min_return_pairs`. | ✅ | `beta_of` cpp:164-183 is pairwise-complete and two-pass. The r and m spans are aligned: `r.last(beta_rows)` against `market + (intervals - beta_rows)`, cpp:371-375. |
| 8 | Vol = sample SD over the last vol_window intervals, with `>= vol_window/2` returns. | ✅ | `vol_of` cpp:185-199. The count is `max(2, ceil(W/2))`, which is 32 for 63 and is the exact reading of "≥ 31.5". |
| 9 | log_adv = log(mean of raw·volume over the last adv_window sessions). Absent days count 0. ok iff > 0. | ✅ | cpp:149-162, :377-378. The denominator is always `adv_window`. There are two extra interpretations, both flagged: the full window must lie inside the panel, and present-but-unusable rows count 0 (see M5). |
| 10 | `out` is row-major N×3. `ok[i]=1` iff all three are finite. | ✅ | cpp:379-382 |
| 11 | Rows used = member && ok. Z-score over those rows, clip ±clip_z, OLS on [1,z1,z2,z3], residual replaces the target. | ✅ | cpp:395-404, `standardize` :230-249, `fit_residual` :329-355 |
| 12 | Rescale so the used-row gross equals the input gross. | ✅ | cpp:409-412. The input gross is the entry gross over all members, which equals the total gross because nonmembers are 0. |
| 13 | member && !ok → 0. Nonmembers untouched (must already be 0). Counts are reported in an out struct. | ✅ | cpp:411. Nonmembers are enforced as exactly 0 (cpp:213-215) and never written. `NeutralizeStats{used, excluded, …}` |
| 14 | Explicit error below min_names or on a singular/ill-conditioned system. No silent fallback. | ✅ | `Unavailable` at cpp:405-406, :336-340. There is also a constant-column refusal and a spanned-target refusal. The strong guarantee holds: the target is written only after `fit_residual` succeeds. |
| 15 | 4×4 solve with a condition check. Deterministic FP order. | ✅ | Jacobi-equilibrated Cholesky with pivot floor 1e-8 (cpp:290-309), plus one refinement step (:344-347). All loops run in fixed ascending order and there are no threads. |
| 16 | O(window×N) per call, no O(N²), no per-call allocations after the first. | ✅ | `grow()` only enlarges (cpp:45-54). Returns scratch is N×max(beta,vol). The neutralize side is O(N). |
| 17 | Light header (std + atx core types/error). | ✅ | hpp:3-7 |
| 18 | Not TDD: postimplementation fixtures (a)–(e). | ✅ | (a) test:158, (b) :185 (+ :211 scratch reuse), (c) :228, (d) :308, (e) :354. See §3 for what each actually pins. |
| 19 | Commit trailer. No push. | ✅ | `b9cf4023` ends with the required `Co-Authored-By` line. |

---

## 2. Numerical correctness walk-through (verified, no defect)

- **Windows / off-by-one at d.**
  - For d ≥ block: `first = d+1-block`, so `intervals = block`, covering intervals `(d-block, d]`, i.e. sessions `d-block … d`.
  - For d < block: `first = 1`, `intervals = d`, with d = 0 giving an empty block. `first_session` is correct in both branches.
  - `beta_rows`/`vol_rows = min(window, intervals)`, and `span::last` never exceeds its size.
  - ADV `[d+1-adv, d]` is guarded by `adv_full`, so there is no unsigned underflow.
- **Pair counting.**
  - m is NaN only when no instrument is valid, so r valid implies m valid. The `isnan(m)` test is harmless.
  - Means are taken over the pairwise-complete set, which is the correct estimator for a name with gaps.
- **Market.**
  - It is summed in instrument order, so it is deterministic.
  - Guarded and absent intervals are excluded both from the market and from the name's own pairs.
- **Z-score / clip.**
  - Two-pass mean and sample SD. The divisor `n-1 ≥ 4` because `min_names ≥ 5`.
  - The clamp bounds satisfy lo ≤ hi because `clip_z > 0` is validated.
  - Clipping breaks mean-zero, but the intercept column absorbs that, so orthogonality to [1, z] still holds exactly.
- **Solve.**
  - Scaling: `A x = b ⇔ C(S⁻¹x) = S b` with `C = SAS`. Forward solve uses `L`, back solve uses `Lᵀ` via `l[p][i]`, and the final `x = S u` is correct.
  - The pivot for column j is `1 − R²` of z_j on its predecessors (intercept first), so the floor is scale-free.
  - One refinement step with the same factor brings `X'e` down from about `cond·eps·|X'y|` to rounding level, even near the 1e-8 floor. The test's 1e-12 therefore has large headroom.
- **Rescale.**
  - `scale = gross / residual_gross > 0`. The denominator is bounded away from 0 by the `1e-9·gross` floor. The sign is preserved.
  - Flat target: an early Ok, and member&!ok rows are already 0 because the gross is 0.
- **NaN / UB.**
  - Every division is guarded (count, `sd > floor`, `var > 0`, `root > 1e-4`, `n-1 ≥ 1`).
  - `log` is called only on finite positive values. There is no narrowing and there are no uninitialized locals.
  - `Cholesky`'s arrays are value-initialized.
- **Scratch statelessness.** Every scratch cell that is read in a call is written earlier in that same call: `logs` prev is preloaded, `returns`/`market` cover `[0, n·intervals)`, and `dollars` is read only when `adv_full`, as are `rows`/`z`/`residual`.
- **Integration precondition.** `desired_target` (replay :102-123) zero-fills nonmembers and emits a finite, demeaned, gross-1 rank target. That satisfies `validate_neutralize`.

---

## 3. Do the fixtures assert what they claim?

Mostly yes, and the construction is deliberate:

- **(a)** alternating ±2%·k with mean(k)=1:
  - the market equals the factor, so beta = k to 1e-11;
  - `sqrt(20/19)` pins the *sample* SD and the exact 20-interval vol window (a 21-interval window shifts the SD by about 0.35%);
  - constant dollar volume pins log_adv.
- **(b)** hostile rewrites after d (NaN/1e9 close, raw = −1, volume 1e300, toggled presence) leave the output bit-identical. Perturbing session d moves all three exposures. Together these pin that the windows end exactly at d.
- **(c)** is the strongest fixture:
  - `min_return_pairs = 29 of 30` makes the one-pair losses (split, crash) ok and the two-pair gap loss not ok. That pins the beta-window length in both directions: a window one too long makes the gap ok, one too short fails the split and crash names.
  - Betas stay exactly k only if the guarded returns are also dropped from the market.
  - The split exercises only the excess-log branch (|la|≈0.69 < 1.5). The crash exercises only the abs branch (|la|≈1.61 < |lr|+0.1).
  - ADV `0.9·6e6` pins the "absent = 0, denominator = adv_window" rule. A present-day-count denominator would give `log(6e6)`.
- **(d)**
  - Excluded rows carry nonzero input weight because `normalize` runs after the masking. So the `gross == 1` check genuinely tests redistribution, and the `w[i]==0` check genuinely tests the member&!ok zeroing.
  - `moments[0]` pins the intercept: the input is demeaned over members, not over used rows.
  - The design is rebuilt independently.
- **(e)** covers every refusal and checks non-mutation bit-for-bit.

The gaps are listed below as M3 and M4.

---

## 4. Findings

### Critical
None.

### Important
None.

### Minor

**M1. The rescale factor is effectively unbounded, and excluded-row gross is silently redistributed.**
- Location: `strategy_price_exposures.cpp:34`, `:350-351`, `:409-412`.
- The only floor is `residual_gross > 1e-9·gross`, so the output can be the residual amplified up to 10⁹×. In addition, the entire `excluded_gross` is moved onto the used rows.
- Failure scenario 1: a role whose desired target is mostly spanned by the exposures, such as a rank-of-trailing-vol (low-vol) or liquidity alpha, has `residual_gross/gross` of about 0.05–0.2. It gets scaled 5–20×, and the published target is the rank-vs-linear-z nonlinearity noise at full gross.
- Failure scenario 2: at a decision where only slightly more than `min_names` (50) members are ok, all member gross is concentrated on those 50 names.
- This is exactly what the brief specifies ("preserved input gross"), and `stats` exposes `gross`, `excluded_gross` and `residual_gross`, so it is a policy gap, not a defect.
- Fix: in T2, check `gross/residual_gross` and `excluded_gross/gross` against explicit caps and refuse or record when they are exceeded. Alternatively, add `max_rescale` / `max_excluded_gross_fraction` to `PriceExposureConfig` and return `Unavailable` when a cap is exceeded. Record the choice in the ledger.

**M2. The spanned-target refusal message is wrong when all gross sits on excluded rows.**
- Location: `cpp:350-351`.
- If every nonzero member weight is on member&&!ok rows, for example a recent-listing alpha whose names all lack 126 pairs, the used-row residual is exactly 0. The call returns `Unavailable: "target spanned by exposures"`, which misdirects debugging.
- Fix: before `fit_residual`, add `if (stats.gross == stats.excluded_gross) return Err(Unavailable, "…all target gross on names without exposures")`, or test the used-row entry gross.

**M3. The clip path and the z-score SD convention are untested.**
- Location: `strategy_price_exposures_test.cpp:104`, `:110-112`. The section exposures are uniform with |z| < √3 by design, so `clip_z` never binds in any fixture.
- Without clipping, orthogonality to `c·z` is invariant to the SD convention. So a regression that drops the clamp, clips raw exposures instead of z, or switches to the population SD would pass every test.
- Fix: add one (d)-style case with a few extreme outliers (e.g. beta = 40 on 2 rows) and `clip_z = 2`, and assert |X'w| ≤ 1e-12 against `design_z` (which already clamps). Optionally assert the clipped rows' z equal ±clip.

**M4. The refusal fixtures assert only `ErrorCode`, not which refusal fired.**
- Location: `test:358-363`, `:367-379`. The collinear, constant and spanned cases all return `Unavailable`.
- Failure scenario: if the pivot floor were broken (e.g. `kMinPivot = 0`), the collinear case would most likely still refuse through the `!isfinite(gross)` / spanned path, and the test would stay green.
- Fix: also match the message substring ("ill-conditioned", "constant", "spanned") or assert `stats.residual_gross == 0` for the pre-fit refusals.
- Optionally add a near-threshold pair: `1−R² ≈ 1e-6` accepted with orthogonality, and exact collinearity refused.

**M5. ADV quietly undercounts present days with missing volume, and the ADV window is not clipped at the panel start the way beta/vol are.**
- Location: `cpp:157-159`, `:87`, `:377`.
- A present session with NaN volume or raw price contributes 0 against a denominator of `adv_window`, so a feed gap in volume alone biases log_adv low without any signal. Early decisions with `d+1 < adv_window` are NaN, while beta/vol are clipped.
- Both behaviours are documented and conservative. Worth a ledger line.
- Fix, if desired: count present-but-unusable volume days separately in a diagnostic counter, or treat them like absent rows explicitly in the header contract (already done in the header comment; an additional stat would help T2 audit).

**M6. The 1e-8 pivot floor is a proxy, not a condition-number bound.**
- Location: `cpp:28`, `:299`.
- A Schur-complement pivot is ≥ λ_min, so the floor bounds λ_min(C) only up to a small 4×4 factor. It accepts systems with 1−R² down to about 1e-8 (cond(C) ≈ 1e8). The residual is still accurate thanks to the refinement, so there is no correctness impact, but `stats.coefficients` can be huge near the floor.
- Fix: none required. Optionally document "coefficients are unreliable when a pivot is near 1e-8", or also refuse when the post-refinement `max|X'e|/max|X'y|` exceeds 1e-10 as a direct orthogonality check.

**M7. The composite recomputes the whole returns block per call.**
- Location: `cpp:416-428`, `:119-147`.
- Each call takes about 2×(block+1)×N `log`s, roughly 2.8M at N = 5,600. That is fine at ~150 calls per role. But if T2 neutralizes several targets at the same decision (multiple scenarios or blends on one panel), the exposures are target-independent and are recomputed identically.
- Fix: in T2, call `compute_price_exposures` once per (panel, d) and reuse `exposures`/`ok` across targets through `neutralize_target`. Document this next to `neutralize_price_risk`.

**M8. House style: missing `noexcept`.**
- Location: `cpp:164` `beta_of`, `:185` `vol_of`, `:230` `standardize`, `:250` `design_row`, `:256` `normal_matrix`, `:268` `cross`, `:276` `subtract_fit`, `:290` `factor`, `:311` `solve`, plus `load_logs`/`fill_returns`/`sum_dollars`/`windows_at`.
- These are pure non-allocating leaf math functions. §10 asks for `noexcept` where it holds.
- Fix: mark them `noexcept`.

---

## 5. Notes (no action required)

- The worktree lease mismatch that the implementer reported (pool-3 `.atx-lease` recording another run) is a root/PM concern and outside this code review.
- Scratch is about 11 MB of returns at N = 5,600 × 252. "O(N)-state" in the brief's Why is satisfied in the sense that no state is carried between calls.
- The FP result is deterministic per build. Under the opt-in `rel-avx2` preset, clang may contract to FMA, so bits can differ *across* presets but not across runs.
- T2 must pick an explicit policy for `Unavailable` at early decisions: every name is !ok until about 127 sessions of history exist. The implementer already notes this.
