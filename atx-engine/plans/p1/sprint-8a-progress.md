# Sprint S8-a — Covariance Construction Core: progress ledger

**Sub-sprint:** S8-a (units S8.1–S8.4) of [S8](sprint-8-risk-covariance-construction.md). The
"build the matrix right" half: robust regression, EWMA+Newey-West factor covariance, Monte-Carlo
eigenfactor de-biasing, specific-risk model.
**Frozen how:** [`sprint-8a-covariance-construction-implementation-plan.md`](sprint-8a-covariance-construction-implementation-plan.md).
**Branch:** `feat/s8` (isolated worktree `C:\Users\natha\atx-wt\s8` — no shared-ref race).
**Base:** `feat/atx-engine-book` @ `14fccd3`.
**Execution:** subagent-driven (fresh implementer per unit → spec review → quality review).

## Build/test (the worktree incantation)

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:VCPKG_ROOT='C:\Users\natha\vcpkg'; $env:ATX_DEPS_DIR='C:\atx-cache\deps'
Set-Location 'C:\Users\natha\atx-wt\s8'
cmake --build --preset ninja --target atx-engine-tests
ctest --preset ninja -R '<Suite>' --output-on-failure
```
Baseline (S8.0): `atx-engine-tests` builds clean; **1064/1066 ctest pass** (the 2 "failures" are
`atx-core-tests_NOT_BUILT` / `atx-tsdb-tests_NOT_BUILT` sentinels — only the engine target was built;
expected, ignore unless those targets are built).

## Carried-forward invariant stress points (this sub-sprint)

1. **Determinism** — S8.3 Monte-Carlo eigenfactor adjustment is the ONLY new RNG: seeded, seed recorded,
   fixed-count sims, byte-identical replay.
2. **No look-ahead** — every estimator trailing-window only; truncation-invariance test per unit.
3. **Backward-compat** — `CovarianceConfig{}` default reproduces P4; existing risk suite is each unit's gate.

## Unit ledger

| Unit | Title | Status | Commit | Tests |
|---|---|---|---|---|
| S8.0 | Marker + ledger + frozen plan | ✅ done | _this commit_ | — |
| S8.1 | Robust √-cap + Huber IRLS regression (reuse `cost::irls_huber`) | ✅ done | `77c4562` | `risk_robust_regression_test.cpp` (9) |
| S8.2 | EWMA split-HL factor cov + Newey-West | ✅ done | `c195d29` | `risk_cov_ewma_test.cpp` (8) |
| S8.3 | Monte-Carlo eigenfactor adjustment (seeded, a=1.0) | ⏳ pending | — | `risk_eigen_adjust_test.cpp` |
| S8.4 | Specific risk: EWMA + NW + structural blend | ⏳ pending | — | `risk_specific_risk_test.cpp` |

## Decisions / discoveries

- **S8.1 reuses the existing engine Huber kernel** `cost::irls_huber` (S6-1, [`cost/robust_ls.hpp`](../../include/atx/engine/cost/robust_ls.hpp))
  — deterministic, tested. √-cap composed as a prior-weight (minimal additive generalization of the kernel
  to accept an optional `w0`, default `Ones` so S6 tests are unchanged). No second IRLS kernel.
- New estimators are **opt-in via `CovarianceConfig cov`** on `FactorModelConfig`; defaults = P4 path.
- Tests are **GoogleTest** (`TEST(...)`), flat + CMake-globbed under `atx-engine/tests/` (the spec's
  `ATS_TEST` shorthand reconciles to GoogleTest here).

## Residuals → backlog (for S8-b / atx-core)

- atx-core L7 `huber_irls` (promote `cost::irls_huber`), `apca`, `nonlinear_shrinkage`/`mp_clip`/`nearest_corr`.
- Newey-West PSD repair (Higham) — engine-side eigen-floor in S8-a, full repair toolkit in S8.7.
- ISC (issuer-specific covariance) off-diagonal wiring into the Woodbury path — interface only in S8.4.
