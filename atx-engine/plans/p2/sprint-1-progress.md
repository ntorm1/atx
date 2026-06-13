# Sprint S1 (p2) — Constrained Multi-Horizon Portfolio Optimization — Implementation Progress

**Status:** 🚧 IN PROGRESS — opened 2026-06-13. Subagent-driven development (fresh implementer per unit + two-stage spec/quality review).
**Worktree:** `C:/Users/natha/atx-wt/p2-s1-multi-horizon` (dedicated, isolated)
**Branch:** `feat/p2-s1-multi-horizon`
**Base:** `main` @ `f85f3d3` (the merged p1 S1–S8 engine)
**Started:** 2026-06-13
**Source plan:** [`sprint-1-multi-horizon-optimization-implementation-plan.md`](sprint-1-multi-horizon-optimization-implementation-plan.md)
**Build gate:** `cmake --preset ninja` (VS Dev env + `VCPKG_ROOT`; no sccache/ATX_DEPS_DIR in this env, so `dev` preset is not used) →
`cmake --build --preset ninja --target atx-engine-tests` (`/W4 /permissive- /WX` + `/fp:precise`) → `ctest --preset ninja -R <Suite>`.

---

## §0 — As-built reconciliation amendment (kickoff recon vs `main` @ `f85f3d3`)

The controller's kickoff recon (the §0 first act) against the **actual merged tree** surfaced the load-bearing corrections below. These
supersede the plan's §0 sketch where they conflict; each is briefed verbatim into the affected unit's implementer.

- **D1 — All upstream deps MERGED in `main`. The §0 recon-target ("merged p1 S1–S8") is SATISFIED; the sprint cuts from `main` directly.**
  Present & verified on `main` @ `f85f3d3`:
  - `risk/optimizer.hpp` — `PortfolioOptimizer::solve(std::span<const f64> alpha, const FactorModel& V, std::span<const f64> w_prev) -> Result<std::vector<f64>>`; `OptimizerConfig{risk_aversion, turnover_penalty, gross_leverage, name_cap, dollar_neutral, max_iters=64}`. The boundary-pin oracle. NaN α ⇒ exactly 0 weight, excluded from reductions.
  - `risk/multi_period.hpp` — `MultiPeriodOptimizer::run(const RebalanceSchedule&, std::function<std::span<const f64>(usize)> alpha_at, std::function<const FactorModel&(usize)> model_at, const book::CostInputs&) -> Result<MultiPeriodResult>`. The reusable deterministic schedule walk (threads `w_prev`, overrides κ to `cost.kappa`, capacity-bounds gross, `blend_toward` full-step is signed-zero exact).
  - `risk/factor_model.hpp` — `FactorModel` (V=XFXᵀ+D factored), `FactorModelBuilder::build_components(...) -> Result<FactorComponents{MatX X; MatX F; VecX D; usize fit_end;}>`, `FactorModel::exposures() -> const MatX&` (X), `apply_inverse(span in, span out)` (V⁻¹ via Woodbury), `risk(w)` (wᵀVw), `n_instruments()`, `n_factors()`.
  - `risk/exposures.hpp` (`build_exposures`, `ExposureMatrix`, `FactorModelConfig`), `cost/cost_aware.hpp` (`cost_aware_knobs`, `CostKnobs{kappa,...}`), the S8 covariance files (`shrinkage`, `psd_repair`, `eigen_adjust`, `stat_factor_model`, `specific_risk`, `cov_ewma`, `dead_factor`, `horizon_blend`), `book/{allocation,pipeline,report}`.
  - **No merge blocker remains.** (Note: `.agents/atx-engine/agent.md` is STALE — it still says "S7 plan frozen, NOT built"; the as-built tree has shipped S7+S8.)

- **D2 — The error vocabulary is `atx::core::ErrorCode` (an `enum class : u16`), NOT a `Status` enum; `Status` is the alias `Result<void>`.**
  The plan's §4.2 pseudocode writes `Err(Status::Infeasible)`. **There is NO `Infeasible`, `Unbounded`, `DimensionMismatch`, or `Empty` enumerator.** The complete `ErrorCode` set is: `Unknown, InvalidArgument, OutOfRange, NotFound, AlreadyExists, PermissionDenied, Unavailable, Internal, NotImplemented, IoError, ParseError`. **Amendment:** an infeasible constraint set / unbounded QP / dim-mismatch returns `Err(ErrorCode::InvalidArgument, "<descriptive msg>")` (an out-of-range index ⇒ `OutOfRange`). R3 ("infeasible ⇒ `Err`, not a silent clamp") is honored via `InvalidArgument` with a message naming the violated constraint. Do NOT add an enumerator to `atx-core` (forbidden touch).

- **D3 — `FactorModel` exposes only `apply_inverse` (V⁻¹) + `risk(w)` (wᵀVw); there is NO forward `Vw` apply, and `F`/`D` are PRIVATE (only `exposures()` returns `X`).**
  Consequence for **S1-2**: the ADMM KKT step needs `(P+σI)⁻¹ = (2λV+σI)⁻¹` applied matrix-free. `apply_inverse` gives `V⁻¹` (wrong operator), so the solver must build its OWN Woodbury for `(2λ(XFXᵀ+D)+σI)⁻¹` from `(X, F, D)`: with `A_diag = 2λD+σI` (diagonal, O(N) invert) and low-rank `X(2λF)Xᵀ`, capacitance `C = (2λF)⁻¹ + Xᵀ A_diag⁻¹ X` (k×k, Cholesky once). **Amendment:** S1-2 adds read-only const accessors `FactorModel::factor_cov() -> const MatX&` (F) and `FactorModel::specific_var() -> const VecX&` (D) to `risk/factor_model.hpp`, mirroring the existing `exposures()` accessor. This is the sprint's **one additive engine-source touch** outside the new `risk/` headers — recorded here, kept to a 2-line read-passthrough, no behavior change (cf. S7-3's `Library::positions` precedent). `gp_aim`'s Markowitz target `(γV)⁻¹α` still routes through the existing `apply_inverse` directly (R4).

- **D4 — `book::CostInputs` is defined in `risk/multi_period.hpp`** (namespace `atx::engine::book`), fields `{f64 kappa=0.0; f64 round_trip_cost_bps=0.0; f64 capacity_gross=1e9;}` — names confirmed exact. Reuse; do NOT redefine. The κ the QP turnover term prices (R6) is `cost.kappa`.

- **D5 — `ISignalSource` is in namespace `atx::engine`** (header `loop/signal_source.hpp`), not `atx::engine::loop`. Interface: `evaluate(PanelView) -> Result<SignalView{span<const f64> values}>` + `max_lookback() noexcept`. The plan's §4.3/§4.4 `loop::ISignalSource` ⇒ read `atx::engine::ISignalSource`. `combine::CombinedSignalSource` is the multi-source blender; for S1's own tests, horizon sources are fixture doubles (`ScriptedSignalSource`).

- **D6 — Canonical Sharpe / metrics:** `combine::compute_metrics(...)` is the one Sharpe convention (`eval::compute_return_metrics` delegates; it lives in `src/eval/perf_metrics.cpp`, link-only). Reuse, do not reinvent (R8 / API discipline).

- **D7 — Boundary-pin reality (R7/§0.5):** the oracle `PortfolioOptimizer::solve` is a projected/proximal loop (`kStep=0.5`, `kCapIters=8`, `max_iters=64`); `MultiPeriodOptimizer::blend_toward` full-step (`trade_rate==1.0`) assigns `target[i]` verbatim (signed-zero-exact — the `bit_cast<u64>` pin mechanism). Because the S1-2 ADMM is a **different algorithm**, exact bitwise parity on the minimal set is unlikely; the plan's §0.5 **dispatch fallback** (minimal `ConstraintSet` ⇒ route to the as-built proximal `PortfolioOptimizer::solve`; the ADMM owns only the augmented-constraint path) is the **expected** outcome and is the sanctioned, recorded path — not a silent divergence. Decided concretely at S1-2; confirmed at S1-4 (the byte-identical `MultiPeriodOptimizer.run` reduction).

---

## Per-unit status

| Unit  | Title                                                                                  | Status   | Commit SHA(s) | Tests | Notes |
|-------|----------------------------------------------------------------------------------------|----------|---------------|-------|-------|
| S1-0  | Marker + ledger + kickoff recon amendment (D1–D7)                                       | 🚧       | —             | —     | this ledger; plan + p2 ROADMAP committed. |
| S1-1  | Constraint algebra → `(A,l,u)` (`risk/constraints.hpp`)                                 | ⏳       | —             | —     | `GrossNet/PositionCap/FactorExposure/GroupCap/BetaNeutral/TurnoverBudget` + `ConstraintSet::materialize`. |
| S1-2  | Fixed-iteration constrained ADMM (`risk/qp_solver.hpp`)                                 | ⏳       | —             | —     | OSQP-form, factored-V KKT (own Woodbury via D3 accessors), no early-exit; boundary pin / §0.5 dispatch. |
| S1-3  | PIT forward forecast trajectory (`risk/horizon.hpp`)                                    | ⏳       | —             | —     | `SignalHorizon`/`HorizonForecast`/`forecast_trajectory`; truncation-invariant. |
| S1-4  | Multi-horizon optimizer + GP aim + boundary pin (`risk/multi_horizon.hpp`)              | ⏳       | —             | —     | reuses S7 schedule walk; `gp_aim` (G&P Eq.15); byte-identical-to-`MultiPeriodOptimizer` regression. |
| S1-5  | Integration test + bench + close ceremony                                              | ⏳       | —             | —     | four gates simultaneously; `multi_horizon_bench.cpp`; ROADMAP flip + `sprint1.md`. |

---

## Shared-branch / discipline
Dedicated worktree `feat/p2-s1-multi-horizon` (true isolation). Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`); `git show HEAD --stat` after each commit; NEVER touch `atx-core/*` or `atx-tsdb/*`; do not push. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
