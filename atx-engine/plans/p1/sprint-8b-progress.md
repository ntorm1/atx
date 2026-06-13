# Sprint S8-b — Regime, Statistical Factors, Shrinkage & Integration: progress ledger

**Sub-sprint:** S8-b (units S8.5–S8.8) of [S8](sprint-8-risk-covariance-construction.md). The
"regime / statistical / model-free / integrate" half: VRA, APCA statistical factors, model-free
shrinkage+RMT+PSD-repair toolkit, short/long-horizon blend + end-to-end integration + bench + close.
**Frozen how:** [`sprint-8b-regime-statistical-shrinkage-implementation-plan.md`](sprint-8b-regime-statistical-shrinkage-implementation-plan.md).
**Builds on:** S8-a (shipped — [`sprint-8a-progress.md`](sprint-8a-progress.md)).
**Branch:** `feat/s8` (isolated worktree `C:\Users\natha\atx-wt\s8` — no shared-ref race).
**Base for S8-b:** `feat/s8` @ `acbdeca` (S8-a close).
**Execution:** subagent-driven (fresh implementer per unit → spec review → quality review).

## Build/test (the worktree incantation)

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:VCPKG_ROOT='C:\Users\natha\vcpkg'; $env:ATX_DEPS_DIR='C:\atx-cache\deps'
Set-Location 'C:\Users\natha\atx-wt\s8'
cmake --build --preset ninja --target atx-engine-tests
ctest --preset ninja -R '<Suite>' --output-on-failure
# bench (S8.8): cmake --build --preset ninja --target atx-engine-bench
```
Baseline (S8-a close): `atx-engine-tests` builds clean; **1096/1098 ctest pass** (the 2 "failures" are
`atx-core-tests_NOT_BUILT` / `atx-tsdb-tests_NOT_BUILT` sentinels — only the engine target was built;
expected, ignore unless those targets are built).

## Carried-forward invariant stress points (this sub-sprint)

1. **Determinism** — NO new RNG site. S8.3 `eigen_adjust` remains the only model RNG; the S8.8 integration
   test proves byte-identical replay of the FULL all-features-on pipeline (incl. the seeded eigen-adjust).
2. **No look-ahead** — every new estimator (VRA λ², APCA factors, horizon blend) trailing-window only;
   truncation-invariance test per unit.
3. **Backward-compat** — `CovarianceConfig{}` + `n_stat_factors==0` reproduces S8-a/P4; existing risk
   suite is each unit's gate.

## Unit ledger

| Unit | Title | Status | Commit | Tests |
|---|---|---|---|---|
| S8-b.0 | Marker + ledger + frozen plan | ✅ done | _this commit_ | — |
| S8.5 | Volatility Regime Adjustment (VRA) + bias-stat diagnostic | ✅ done | `a939e67` | `risk_vol_regime_test.cpp` (8) |
| S8.6 | APCA statistical factor model (retires `n_stat_factors` NotImplemented) | ✅ done | `a74da77` | `risk_stat_factor_test.cpp` (10) |
| S8.7 | Model-free shrinkage (const-corr LW + MP clip) + PSD repair (Higham + eigen-clip) | ✅ done | `3ce8cfe` | `risk_shrinkage_psd_test.cpp` (9) |
| S8.8 | Short/long-horizon blend + integration + bench + close | ✅ done | `89d7c74` | `risk_covariance_integration_test.cpp` (9) + `bench/risk_covariance_bench.cpp` |

**S8-b CLOSED** — all 4 code units shipped, each TDD + spec-compliance review + code-quality review passed,
plus a final cross-unit holistic review (**SHIP**: config coherent, build() pipeline order correct, S8.3
eigen_adjust still the ONLY model RNG, PSD-by-construction at every stage, `FactorModel` apply interface
byte-unchanged, DRY — S8.8 reuses the S8.2/S8.4 kernels). Full `atx-engine-tests` green: **1133/1133 real
tests pass** (+9 over the 1124 S8.7 baseline; only the 2 engine-only `*_NOT_BUILT` sentinels "fail").
Covariance bench (§10): `cond_p4 14.77 → cond_blend 9.94 → cond_s8 9.48` (~33% better-conditioned).
**With S8-a, Sprint 8 is complete.** `feat/s8` is intentionally NOT merged — the merge is a controller/user
decision (the worktree carries S8-a + S8-b; merge after the user reviews).

## Decisions / discoveries

- **VRA D-analogue (S8.5):** the factor-derived `λ²` is applied to BOTH F and D (one market-wide vol-regime
  multiplier). Per-name specific VRA (a separate `λ²_D` from a dense specific-return panel) is a documented
  backlog residual — the market-wide multiplier is the dominant effect and keeps the unit testable.
- **APCA forms the T×T Gram (S8.6):** atx-core `pca()` forms the N×N sample covariance (verified
  `pca.hpp:67`) — WRONG for N≫T. The engine builds the `Ω=(1/N)RᵀR` Gram explicitly + `symmetric_eig`.
  `// PATTERN-B: atx-core apca` is the recorded L7 lift.
- **One canonical LW (S8.7):** the const-corr target is a GENERALIZATION of the scaled-identity LW
  (`combine::detail::ledoit_wolf_intensity` stays the scaled-identity home); `shrinkage.hpp` adds the
  const-corr target + its `ρ̂`. Nonlinear (QIS) shrinkage is NOT built — Pattern-B lift.
- **Horizon blend keeps F/D internal (S8.8):** `build` constructs `F_short`/`F_long` from the same `fkept`
  at two half-life sets and blends — the `FactorModel` apply interface and its private F/D stay untouched.

## Residuals → backlog (for S8 close / atx-core / S7)

- atx-core L7 lifts: `huber_irls` (S8.1), `apca` (S8.6), `nonlinear_shrinkage`/`mp_clip`/`nearest_corr` (S8.7).
- ISC (issuer-specific covariance) off-diagonal Woodbury wiring (S8.4 interface-only).
- Per-name specific VRA `λ²_D` from a dense specific-return panel (S8.5 applies the market-wide `λ²` to D).
- Risk-attribution diagnostic (factor vs specific variance decomposition) — S8.8 ships the integration; the
  full attribution surface is a thin follow-up if not folded into S8.8.
- S8.6 APCA append seam is the path S7.3 (dead-alpha→risk-factor) reuses.
- **APCA path does not compose with eigen_adjust / VRA / EWMA-blend / structural-D** (holistic-review finding):
  the statistical variant is a self-contained model (F = canonical LW of factor returns, D = APCA residual
  floored). The regime-aware / de-biasing / blend features apply to the FUNDAMENTAL path only. Composing them
  onto the statistical variant is a follow-up.
- **`horizon_blend=true` + `factor_cov_method=LedoitWolfSingle` is a silent no-op** (holistic-review finding):
  the long half-lives are EWMA-only, so the LW-single case ignores the blend (documented at the call site but
  not validated). Consider a debug assert / config validation so a mis-set pair fails loud.
- **Stale comment** `risk_factor_builder_test.cpp:25` still says "n_stat_factors>0 → NotImplemented" (S8.6
  retired that; the test body asserts the retirement correctly). Comment-only; fix opportunistically.
- MultiPeriodOptimizer flow-through: the S8.8 integration test exercises `PortfolioOptimizer::solve`; a
  `MultiPeriodOptimizer::run` flow-through of the deepened V is a thin additional integration test.
