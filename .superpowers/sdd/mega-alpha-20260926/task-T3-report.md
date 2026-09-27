# Task T3 report: causal price-risk exposures and target neutralization

## Outcome
DONE (not compiled or run, as the brief requires). There is one new private module in `atx-impl` plus postimplementation fixtures. No existing file was touched.

## Branch / SHA
`feat/mega-alpha-exposures-20260926` @ `e4a869b7ba6120185bf0f2ea4b0361a34a63c753` in `C:/atx-wt/pool-3`. Everything is committed and the tree is clean. Nothing was pushed.

## Frozen base / lease
base_sha=`5c9cbaeda79d62df7b6c2b1a167df93cbc6a2f05`; worktree=`C:\atx-wt\pool-3`.
The lease was taken by root, not by me. **Observed mismatch:** `C:/atx-wt/pool-3/.atx-lease` still records
run_id=`aes-w0-g0-codex-20260925`, branch=`feat/w0-g0-codex-20260925`, base=`bc5cc646`, keeper_pid=8316.
That does not match this branch or base. Root should confirm that pool-3 is correctly leased for this run.

## Files changed (new only)
- `atx-impl/src/strategy_price_exposures.hpp`: light header with only std, `atx/core/error.hpp` and `atx/core/types.hpp`.
- `atx-impl/src/strategy_price_exposures.cpp`
- `atx-impl/tests/strategy_price_exposures_test.cpp`

## CMake lines root must add
`atx-impl/CMakeLists.txt`: add the source to `add_library(atx-impl-core ...)` next to `src/strategy_target_replay.cpp`:
```cmake
    src/strategy_price_exposures.cpp
```
In the same file, add it to both lists in the Debug clang-cl block:
```cmake
    set_property(SOURCE src/strategy_ic_runner.cpp src/strategy_ic_composition.cpp src/strategy_target_replay.cpp src/strategy_price_exposures.cpp
        APPEND PROPERTY COMPILE_OPTIONS /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/strategy_ic_runner.cpp src/strategy_ic_composition.cpp src/strategy_target_replay.cpp src/strategy_price_exposures.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
```
`atx-impl/tests/CMakeLists.txt`:
```cmake
add_executable(atx-impl-strategy-target-tests EXCLUDE_FROM_ALL
    strategy_target_replay_test.cpp
    strategy_price_exposures_test.cpp)
```
The same file globs `*_test.cpp` into `atx-impl-tests` with `CONFIGURE_DEPENDS`. The new test therefore also joins the ordinary `atx-impl-tests` binary on the next configure, the same way the other strategy tests do.

Suggested root loop: `powershell scripts\atx-build.ps1 check atx-impl\src\strategy_price_exposures.cpp`, then
`powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests`, then
`build\bin\atx-impl-strategy-target-tests.exe --gtest_filter=StrategyPrice*`. The focused target has no ctest registration.

## API (namespace `atx::impl::strategy`)
- `PriceExposureConfig` uses the brief's values verbatim: `beta_window{252}, vol_window{63}, adv_window{63}, min_return_pairs{126}, min_names{50}, clip_z{5.0}`.
- `PriceExposureInput { dates, instruments; span close, raw_close, volume; span present; }`.
- `kPriceExposureCount = 3`. The column constants are `kExposureBeta = 0`, `kExposureVol = 1`, `kExposureLogAdv = 2`.
- `Status compute_price_exposures(const PriceExposureInput&, const PriceExposureConfig&, usize d, PriceExposureScratch&, span<f64> out, span<u8> ok)`
- `Status neutralize_target(span<f64> target, span<const u8> member, span<const f64> exposures, span<const u8> ok, const PriceExposureConfig&, NeutralizeScratch&, NeutralizeStats&)`
- **One-call step for the NAV replay (T2):**
  `Status neutralize_price_risk(const PriceExposureInput&, const PriceExposureConfig&, usize d, span<f64> target, span<const u8> member, PriceRiskScratch&, NeutralizeStats&)`.
  `PriceRiskScratch` holds both scratches plus the `exposures`/`ok` buffers. The replay owns one per run:
  ```cpp
  PriceRiskScratch risk;  // once per replay
  // at each rebalance, after desired_target(...) fills `desired`:
  ATX_TRY_VOID(neutralize_price_risk(prices, cfg, d, desired, in.member.subspan(d * n, n), risk, stats));
  ```
- `NeutralizeStats` has these fields:
  - `used`: member && ok rows.
  - `excluded`: member && !ok rows, which are zeroed.
  - `gross`: entry sum |target| over members, which is also the exit gross.
  - `excluded_gross`
  - `residual_gross`: sum |OLS residual| before rescale.
  - `coefficients[4]`: on [1, z_beta, z_vol, z_log_adv].

## Semantics as implemented (interpretation choices flagged *)
- **Causality:** decision d reads only sessions `<= d`, and presence-byte validation covers only the sessions read. The fixture shows that bytes after d are never read or validated.
- **Return validity:** a return is valid when:
  - both endpoints are present;
  - close and raw_close are finite and positive at both endpoints;
  - `close[t]/close[t-1]-1` is finite;
  - the interval is not guarded: `|la| > 1.5 || |la| > |lr| + .10`, with `la = log(c_t) - log(c_{t-1})`.
  
  These are the same predicate and the same operation order as `rough_return` in `strategy_target_replay.cpp`. *Raw_close must be finite and positive because the guard needs it, exactly as in the replay.
- **Market:** the equal-weight mean of valid returns across all instruments, including nonmembers. It is NaN when no instrument is valid.
- **Beta:** two-pass sample cov/var over the last `beta_window` intervals. The window is clipped at the panel start, and `min_return_pairs` gates it. It is NaN if var == 0.
- **Vol:** two-pass sample SD over the last `vol_window` intervals. It needs at least `max(2, ceil(vol_window/2))` returns (32 for 63). *I used the ceiling for the brief's ">= vol_window/2".
- **log_adv:** the log of the mean of `raw_close*volume` over the `adv_window` sessions ending at d. The denominator is always `adv_window`.
  - *A day counts 0 when it is absent, or present but unusable: raw not finite or not positive, volume not finite or negative, or the product not finite. The brief only covered absent days.
  - *The full ADV window must lie inside the panel (`d+1 >= adv_window`); otherwise the value is NaN. Beta and vol are clipped at the panel start instead.
- `ok[i] = 1` iff all three exposures are finite.
- **Neutralize steps:**
  1. Rows used are member && ok, taken in ascending index order.
  2. Each column is z-scored with the mean and sample SD, then clipped to +-clip_z.
  3. OLS is solved from 4x4 normal equations with Jacobi-equilibrated Cholesky and a pivot floor of 1e-8. That floor is 1 - R^2 of a column on its predecessors, so it trips only on exact collinearity.
  4. One iterative-refinement step drives X'e to rounding level.
  5. The residual replaces the target on the used rows, scaled to the **entry gross over all members**. That gross includes the mass sitting on excluded rows.
  6. member && !ok rows become 0. Nonmembers must already be 0 and are never written.
- **Error codes:**
  - InvalidArgument is for contract violations: config, geometry, flag bytes, nonfinite member target, nonzero nonmember, or a non-finite exposure on an ok row.
  - Unavailable is for data refusals:
    - `used < min_names`;
    - *a constant column (SD <= 1e-12 * max|x|);
    - an ill-conditioned normal matrix;
    - *a residual below 1e-9 of the gross, meaning the target is spanned by the exposures.
  - OutOfRange is for scratch allocation failure or more than 2^26 return cells.
- **Strong guarantee:** on any error the target is left unmodified.
- *A flat target (gross 0) returns Ok unchanged. The `min_names` check runs first, so too few names refuses even for a flat target.
- **Config bounds:**
  - `beta_window` and `vol_window` in [2, 4096]; `adv_window` in [1, 4096];
  - `2 <= min_return_pairs <= beta_window`;
  - `min_names >= 5`, so the residual has at least one degree of freedom;
  - `clip_z` finite and > 0.
- **Determinism:** fixed ascending loop orders, no threads, and no state carried between calls. Scratch only grows and is never read before it is written for the current call.

## Performance
- Each call recomputes the returns block. It takes one log per session per price type: about 2 x 253 x 5,600 ≈ 2.8M `log` calls. On top of that there are O(W x N) returns, market and beta/vol passes.
- Returns are stored instrument-major, so the per-name passes are contiguous and the only strided pass is the write.
- No O(N^2) work. Steady-state calls allocate nothing, since buffers are sized to the full `max(beta, vol)` window on the first call.
- Rough estimate: tens of ms per call, so about 3-6 s per role at ~150 calls.
- If that matters, the next step is caching the per-session log rows or returns across consecutive decisions. That needs an explicit same-panel contract, so I left it out.

## Evidence
No compile, build or test was run, per the brief: root builds and returns compiler errors. What I did do:
- Checked every line against the 100-column limit with `awk 'length > 100'`; it printed nothing.
- Confirmed with grep that the test's `using namespace atx` introduces no name collisions from the headers it includes.
- Commit: `git -C C:/atx-wt/pool-3 show --stat HEAD` gives `e4a869b7 ...` with 3 files changed and 976 insertions.

Fixtures, mapped to the brief:
- (a) `StrategyPriceExposures.ExactMarketMultiplesRecoverBetaVolAndDollarVolume`: k = {0.5, 1, 1.5} times an alternating +-2% factor, so beta == k to 1e-11. The closed-form vol is 0.02k·sqrt(20/19). A constant dollar volume gives log_adv == log(V).
- (b) `...RewritingEverySessionAfterDecisionLeavesExposuresBitIdentical`: after hostile rewrites of close, raw, volume and present beyond d, the exposures are bit-identical. Perturbing session d moves all three.
- (b') `...ScratchReuseAcrossClippedAndFullWindowsIsStateless`: runs d = 0 and 3 (no ok names), then 60, then 45 on reused scratch. The result is bit-equal to fresh scratch.
- (c) `...GuardedAndAbsentIntervalsLeaveMarketAndLosePairs`: covers an adjusted-only 2x jump (excess-log branch), a corroborated 5x move (|log| > 1.5 branch) and an absent session (two intervals lost).
  - With `min_return_pairs = 29 of 30`, the 1-pair losses stay ok and the 2-pair loss is refused.
  - All betas equal k exactly because the market stays equal to the factor. That only holds if the excluded returns are dropped from the market.
  - Vol equals the SD of the surviving factor values.
  - The absent day's ADV is log(0.9V).
- Config, geometry, decision and presence-byte rejections, including that a bad byte after d is not validated.
- (d) `StrategyPriceNeutralize.ResidualIsOrthogonalGrossPreservingAndRespectsSupport`: |X'w| <= 1e-12 for the intercept and each z-column, with the design rebuilt from the contract. The gross stays 1 within 1e-12, nonmembers and member&&!ok rows are exactly 0, and the input's real beta loading is confirmed. A rerun on the same scratch is bit-identical.
- (e) `...TooFewNamesOrDegenerateExposuresRefuseWithoutMutation`: too few names, vol = 2*beta + 3, a constant log_adv and a spanned target each return Unavailable. A dirty nonmember and a short `ok` span return InvalidArgument. The target is bit-unchanged in every case.
- `...OneCallStepMatchesPrimitivesAndFlatTargetStaysFlat`: the composite result is bit-equal to the two primitives on two passes over the same scratch. A flat target stays flat.

## Deviations from brief
- Added the composite `neutralize_price_risk` / `PriceRiskScratch` so that T2's call is one line, as root asked. Extra stats fields (`gross`, `excluded_gross`, `residual_gross`, `coefficients`) and the column-index constants were added. Everything else follows the brief's shape.
- The interpretation choices marked * above.

## Integration notes for T2 (NAV replay)
- `TargetReplayInput` has no `volume` span. The NAV replay must load `volume.f64` and build a `PriceExposureInput` from close, raw_close, volume and present.
- Early decisions will return Unavailable until about `min_return_pairs + 1` sessions of history exist for at least `min_names` members, because every name is !ok. The replay must either start decisions late enough or choose an explicit policy for Unavailable. The module deliberately does not fall back silently.
- Members lacking exposures get desired weight 0, so they are traded out through the replay's normal partial-trade path.

## Ledger candidates
- The price-risk neutralizer reuses the target replay's exact guard predicate: |log adj| > 1.5 or > |log raw| + 0.10.
- On neutralize refusal the target is untouched and the error is Unavailable. Callers must choose the policy.
