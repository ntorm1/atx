# Sprint S1 (p2) — Constrained Multi-Horizon Portfolio Optimization — Implementation Progress

**Status:** ✅ SHIPPED — opened & closed 2026-06-13. Subagent-driven development (fresh implementer per unit + two-stage spec/quality review). All 70 S1 tests green; pending user merge.
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

- **D7 — Boundary-pin reality (R7/§0.5):** the oracle `PortfolioOptimizer::solve` is a projected/proximal loop (`kStep=0.5`, `kCapIters=8`, `max_iters=64`); it enforces `Σ|w|=L` **exactly** via gross-normalize (a NONLINEAR projection), not the QP's `Σ|w|≤L` inequality; `MultiPeriodOptimizer::blend_toward` full-step (`trade_rate==1.0`) assigns `target[i]` verbatim (signed-zero-exact — the `bit_cast<u64>` pin mechanism). Because the S1-2 ADMM is a **different algorithm** with a different feasible region, exact bitwise parity on the minimal set is impossible; the plan's §0.5 **dispatch fallback** (minimal `ConstraintSet` ⇒ route to the as-built proximal `PortfolioOptimizer::solve`; the ADMM owns only the augmented-constraint path) is the **mandated** path — S1-4 MUST dispatch the degenerate config through `PortfolioOptimizer::solve` to make R7 byte-identical to `MultiPeriodOptimizer.run`. Not a silent divergence; the architectural directive for S1-4.

- **D8 — `forecast_trajectory` ships as a PURE projection kernel (S1-3), decoupled from `ISignalSource`.** The plan §4.3 signature `forecast_trajectory(span<pair<ISignalSource*, SignalHorizon>>, as_of, H)` cannot be honored verbatim: `ISignalSource::evaluate` (namespace `atx::engine`, header `loop/signal_source.hpp`) needs a full `PanelView` (not an `as_of` index), is non-`const`, and returns a borrowed `SignalView` invalidated on the next call; coupling `horizon.hpp` to the alpha-VM stack is undesirable. **Decision:** S1-3 is the pure numeric kernel `forecast_trajectory(span<pair<span<const f64> alpha_now, SignalHorizon>>, M, H) -> Result<HorizonForecast>` (decay projection + horizon-weighted superposition, NaN⇒0, order-fixed). The S1-4 driver — which already holds the PIT panel — evaluates the `ISignalSource`s at the as-of panel and passes the resulting α_t cross-sections to the kernel. R2 truncation-invariance is proven end-to-end in S1-5's integration test; the kernel proves no-look-ahead structurally (the trajectory is a pure deterministic function of the current α_t only). `ScriptedSignalSource` ignores its panel (baked replay), so a meaningful panel-read R2 test uses `VmSignalSource` or a panel-reading test double at S1-5.

---

## Per-unit status

| Unit  | Title                                                                                  | Status   | Commit SHA(s) | Tests | Notes |
|-------|----------------------------------------------------------------------------------------|----------|---------------|-------|-------|
| S1-0  | Marker + ledger + kickoff recon amendment (D1–D8)                                       | ✅       | `1ec419f`     | —     | this ledger; plan + p2 ROADMAP committed. |
| S1-1  | Constraint algebra → `(A,l,u)` (`risk/constraints.hpp`)                                 | ✅       | `971cc6b`     | 25    | `GrossNet/PositionCap/FactorExposure/GroupCap/BetaNeutral/TurnoverBudget` + `ConstraintSet::materialize`; A is R×M linear-only, L1 budgets carried as metadata; spec+quality reviewed. |
| S1-2  | Fixed-iteration constrained ADMM (`risk/qp_solver.hpp`)                                 | ✅       | `1ebf01f`     | 13    | OSQP-form ADMM, matrix-free P=2λV via new `FactorModel::apply`/`specific_var`; full L1 aux-split; fixed-iter no early-exit; feasibility→Err (msg names infeasible-OR-unconverged). Default `iters=600` (aux-split convergence). spec COMPLIANT + quality APPROVED post-fix. |
| S1-3  | PIT forward forecast trajectory (`risk/horizon.hpp`)                                    | ✅       | `66a1542`     | 12    | pure kernel (D8): `SignalHorizon::decay`/`identity`, `HorizonForecast`, `forecast_trajectory`; NaN-all-sources preserved, identity⇒constant; spec COMPLIANT + APPROVED. |
| S1-4  | Multi-horizon optimizer + GP aim + boundary pin (`risk/multi_horizon.hpp`)              | ✅       | `70f93ca`     | 15    | reuses S7 schedule walk; `gp_aim`=horizon-AVERAGE (D9, degenerate⇒α_t exact); minimal-constraint dispatch→`PortfolioOptimizer::solve` ⇒ **R7 byte-identical (3 pins: full/partial/capacity-clip)**; augmented→`ConstrainedQpSolver` (all-constraints-satisfied R3). spec COMPLIANT + quality APPROVED. 2 🟢 polish nits deferred. |
| S1-5  | Integration test + bench + close ceremony                                              | ✅       | `dedd3fd`     | 5     | four gates simultaneously (R1/R2/R3/R7) all non-vacuous; `bench/multi_horizon_bench.cpp` (matrix-free O(N·K) witness); 2 polish nits folded; spec COMPLIANT + APPROVED (reviewer reran 20/20). |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `1ec419f` | S1-0 | docs(s1-0): open p2 sprint-1 multi-horizon ledger + kickoff recon amendment |
| `971cc6b` | S1-1 | feat(s1-1): constraint algebra -> (A,l,u) materialization (risk/constraints.hpp) |
| `1ebf01f` | S1-2 | feat(s1-2): deterministic fixed-iteration constrained ADMM (risk/qp_solver.hpp) + FactorModel forward apply |
| `66a1542` | S1-3 | feat(s1-3): PIT multi-horizon forecast trajectory (risk/horizon.hpp) |
| `70f93ca` | S1-4 | feat(s1-4): constrained multi-horizon optimizer + GP aim + S7 boundary pin (risk/multi_horizon.hpp) |
| `dedd3fd` | S1-5 | feat(s1-5): multi-horizon integration capstone (4 gates) + bench + polish |
| _(this)_ | close | docs(s1): close ceremony — ROADMAP flip, residuals, sprint1.md, ledger finalize |

**Test totals:** S1 adds **70** new GoogleTests (25 constraints + 13 qp_solver + 12 horizon + 15 multi_horizon + 5 integration) — all green; full engine suite green (no regression). Each unit two-stage reviewed (spec-compliance then code-quality); the S1-4 boundary pin + the S1-5 four-gates capstone are the load-bearing regressions.

---

## What S1 proves

S1 generalizes `p1` S7's receding-horizon *driver* into a **true constrained multi-horizon optimizer**, and pins it to the proven layer:
- **R1 (determinism):** the new fixed-iteration ADMM (`ConstrainedQpSolver`) runs a fixed outer/inner iteration count with **no residual early-exit**; the trajectory build, gp_aim blend, KKT/PCG, and every clip are order-fixed ⇒ two builds produce a byte-identical book schedule + digest (integration `R1_*`).
- **R2 (no look-ahead):** the forward trajectory is a **pure causal projection** `α_{t+h}=Σ_s decay_s(h)·α_t,s` (D8 pure kernel) and the driver executes only the realized first move ⇒ truncation-invariant at the schedule boundary (integration `R2_*`).
- **R3 (constraint exactness):** the constraint algebra materializes factor-exposure / group / beta / gross-net / position / turnover into exact `l≤Aw≤u` (+ L1 aux-split); every claimed constraint holds at every period's realized book within `feas_tol`; an infeasible/unconverged set returns `Err`, never a silently-clamped book (integration `R3_*`).
- **R4 (factored `V`):** `P=2λV` is consumed matrix-free through the new `FactorModel::apply` (Woodbury-class); no dense M×M is ever formed — the bench witnesses O(N·K) not O(N²).
- **R5/R6 (GP aim + calibrated cost):** trades toward the Gârleanu-Pedersen horizon-decay-weighted aim (D9 horizon-average, persistence-weighted), priced with the S6-calibrated κ via `book::CostInputs`.
- **R7 (reduction to S7):** with one identity-decay horizon, the minimal constraint set, and full trade-rate, `MultiHorizonOptimizer.run` dispatches to the as-built proximal `PortfolioOptimizer::solve` and is **byte-identical** to `MultiPeriodOptimizer.run` (the §0.5 dispatch fallback, D7) — proven at S1-4 (3 pins) and re-affirmed at S1-5 integration.

The two genuinely-new numeric kernels — the fixed-iteration constrained ADMM (R1/R3/R4) and the GP multi-horizon aim (R5) — are each differential-tested against an obvious reference (dense-ADMM and hand-rolled decay/average). The `p2` spine exists.

## Residuals → p2 / atx-core backlog
- **L7 `core::linalg::qp_admm`** (Pattern-B lift): the dedicated, KKT-pre-factorized OSQP solver. Shipped on the engine-local `ConstrainedQpSolver` (matrix-free factored-`V`); the atx-core kernel with Ruiz-equilibration + warm-start + infeasibility certificates is the recorded lift (§2.1).
- **ADMM iteration-count for dense augmented sets:** the L1 aux-split (gross/turnover) roughly triples primal dim; tight-feasibility on a dense augmented set needs `qp.iters≈1200–2000` (R3 integration used 1600/120). The default is 600 (clears the common augmented path). The atx-core `qp_admm` (warm-started, equilibrated) and/or a true infeasibility certificate (to distinguish infeasible from unconverged — currently both surface as `InvalidArgument`) is the resolution.
- **Stacked-MPC QP** (`stacked_mpc=true`): the full O(N·H) stacked horizon QP — currently `Err(NotImplemented)`; the GP aim-collapse is the production path (§0.6). Benchable lift.
- **Horizon-from-IC estimation → S2:** `SignalHorizon` is caller-supplied (fixtures here); estimating it from realized IC-decay is the S2 sleeve's job (§0.7).
- **Sparse constraint matrix `A`:** S1-1 materializes `A` dense (R×M); box rows make it M×M-ish at scale. A sparse `A` (most rows 1-hot / X-columns) is the efficiency lift for the 3–5k-name regime (recorded with the `qp_admm` request).

## Close-out notes
- `p2/ROADMAP.md` S1 row flipped `⏳ proposed → ✅ DONE` with per-unit SHAs; `Last reviewed` current. **Merge is the user's gate** (branch `feat/p2-s1-multi-horizon`, not pushed) — mirrors the `p1` S7/S8 close discipline.
- User reference [`sprint1.md`](sprint1.md) created (the plain-language "what S1 shipped").
- Engine-source touch outside new `risk/` headers: **only** `risk/factor_model.hpp` (+`apply`/`specific_var` read-accessors, D3) — additive, behavior-preserving, FactorModel suites stay green. No `atx-core/*` / `atx-tsdb/*` touched.
- **Full-suite ctest caveat (not a regression):** a full `ctest --preset ninja` reports "4 tests failed out of 1246" — all four are **outside S1's diff** and confirmed not S1-caused: (1) `atx-core-tests_NOT_BUILT` + (2) `atx-tsdb-tests_NOT_BUILT` are sentinel "Not Run" markers because only the `atx-engine-tests` target is built (not those sibling targets); (3) `LibraryIntegration.RoundTripsLargeFixtureZeroCopy` + (4) `LibraryIntegration.IncrementalGateMatchesExactGate` are slow large-fixture/mmap-zero-copy tests (~80s / ~18s) that **exceed the per-test ctest timeout under full-parallel load** — both **PASS in isolation** (`--gtest_filter=LibraryIntegration.*`). S1 touches zero `library/`/`store/`/`tsdb` files (`git diff --name-only f85f3d3..HEAD` is risk-only), so these are pre-existing timeout flakiness in an untouched subsystem. The 70 S1 tests + the FactorModel suites are green.

## Baton → S2
S2 (multi-strategy meta-book) wraps `MultiHorizonOptimizer` directly: a `Sleeve` = a universe × horizon × signal-family book = one `MultiHorizonOptimizer` + its `SignalHorizon` assignment (the horizon-from-IC estimation S1 batoned). S2's meta-allocator nets the sleeve books into one fund under a cross-sleeve risk budget. S1 also hands **S4** the target/turnover series its execution scheduler trades, and **S8** the optimizer the full-fund orchestrator routes every signal through. With S1 closed, the `p2` spine is live.

---

## Shared-branch / discipline
Dedicated worktree `feat/p2-s1-multi-horizon` (true isolation). Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`); `git show HEAD --stat` after each commit; NEVER touch `atx-core/*` or `atx-tsdb/*`; do not push. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
