# Phase 4 — Signal Combination + Barra Risk + Risk-Aware Construction (user reference)

**Status:** ✅ closed 2026-06-07 · two sprints, in-place on `feat/atx-core-stdlib`
· 4a (P4-0…P4-5, alpha pool + combiner) · 4b (P4-6…P4-10c, factor risk + risk-aware construction)
**Source plan:** [`phase-4-signal-combination-risk-implementation-plan.md`](phase-4-signal-combination-risk-implementation-plan.md)
**Build logs:** [`phase-4a-progress.md`](phase-4a-progress.md) · [`phase-4b-progress.md`](phase-4b-progress.md)

Phase 3 turns a string into one alpha signal. **Phase 4 turns a POOL of alphas into one risk-aware book.**
It screens each alpha on correlation/turnover/fitness gates, combines the survivors into a mega-alpha, builds
a Barra-style factor risk model, and sizes a turnover-penalized dollar-neutral portfolio against it — then
reports the capacity at which market impact erodes the edge. The pipeline is:

```
alphas ──gate──► pool ──combine──► mega-alpha ──┐
(AlphaGate)   (AlphaStore)  (AlphaCombiner)      │   α
                                                 ▼
   panel ──build──► V = XFXᵀ+D (FactorModel)  ──► optimize ──► book w
   (FactorModelBuilder, per-date WLS)            (PortfolioOptimizer)   max αᵀw−λwᵀVw−κ‖w−w_prev‖₁
                                                 │
   book ──► capacity_curve ──► net-edge(AUM) ────┘   (RT §9.6 — report capacity, not just return)
```

Everything is header-only under `atx/engine/combine/` (4a) and `atx/engine/risk/` (4b), consuming `atx-core`
+ the as-built Phase-2/3 engine headers. The combined pool plugs into the Phase-2 `BacktestLoop` as just
another `ISignalSource` (`combine::CombinedSignalSource`). The whole layer is **fit-then-apply**: every fitted
object (`Combination`, `FactorModel`) carries an explicit `[fit_begin, fit_end)` window and a fit function
physically reads only its window — see §6.

---

## 1. The alpha pool — gate → store → combine (4a)

```cpp
#include "atx/engine/combine/metrics.hpp"   // AlphaMetrics
#include "atx/engine/combine/gate.hpp"      // GateConfig, GateVerdict, AlphaGate
#include "atx/engine/combine/store.hpp"     // AlphaStore, AlphaId
#include "atx/engine/combine/combiner.hpp"  // CombineMethod, CombinerConfig, Combination, AlphaCombiner
using namespace atx::engine::combine;
```

- **`AlphaMetrics`** — per-alpha realized summary (`sharpe, turnover, returns, drawdown, margin, fitness,
  holding_days`) computed over an alpha's PnL stream (the Phase-3 `extract_streams` output).
- **`AlphaGate`** (stateless) admits an alpha iff it clears `GateConfig` — `min_sharpe=1.0`, `min_fitness=1.0`
  (BRAIN gold standard), `max_turnover=0.70` (cost gate), `max_pool_corr=0.7` (reject if too correlated with
  an already-accepted alpha). Returns a `GateVerdict`.
- **`AlphaStore`** — insertion-ordered registry of the admitted pool: `insert(source, pnl, positions_flat,
  metrics) -> Result<AlphaId>`, or `ingest_streams(AlphaStreams, sources, metrics)` to bulk-load a Phase-3
  `extract_streams` result.
- **`AlphaCombiner::fit(pool, fit_begin, fit_end) -> Result<Combination>`** fits per-alpha weights over the
  `[fit_begin, fit_end)` PnL window. `CombinerConfig.method`:

  | `CombineMethod` | weights |
  |---|---|
  | `EqualWeight` | `w_i = 1/N` |
  | `RankAverage` | uniform 1/N here (rank-space combine is the source's job) |
  | `IcWeighted` | `w_i ∝ max(window-sharpe_i, 0)` |
  | `ShrinkageMv` (default) | Ledoit-Wolf shrunk mean-variance (the ONE canonical LW closed form, reused in 4b) |
  | `BoundedRegression` | ridge MV in top-`n_pcs` PC space, clipped to `|w| ≤ weight_bound` |

  Knobs: `shrinkage` (LW AUTO if `<0`, else fixed `[0,1]`), `weight_bound=0.10`, `ridge_lambda=1e-3`,
  `n_pcs` (0 = all PCs; else top-k SCM PCs for `N ≫ T`). The `Combination` carries the weights +
  `[fit_begin, fit_end)`.

### Production source adapter
```cpp
#include "atx/engine/combine/combined_source.hpp"   // CombinedSignalSource
CombinedSignalSource src{std::move(sources), combo, CombineMethod::EqualWeight};
// src is an ISignalSource: evaluate(PanelView) blends the constituents by the fitted weights;
// max_lookback() forwards the deepest constituent lookback. Drives the Phase-2 BacktestLoop directly.
```
Linear-method blend is gross-preserving: `out[k] = (Σ w_i·s_i[k]) / (Σ|w_i|)` over the non-NaN constituents.

---

## 2. The factor risk model — V = XFXᵀ + D, kept factored (4b)

```cpp
#include "atx/engine/risk/exposures.hpp"     // StyleFactor, FactorModelConfig, build_exposures
#include "atx/engine/risk/factor_model.hpp"  // FactorModel, FactorModelBuilder
using namespace atx::engine::risk;
```

The risk model is a Barra-style factored covariance `V = X F Xᵀ + D` (`X` = M×K exposures, `F` = K×K factor
covariance, `D` = M specific variances) that is **never materialized as a dense M×M matrix**.

- **Exposures `X`** (`StyleFactor`: `Size, Momentum, Volatility, Beta, Liquidity`). The as-built `PanelView`
  is OHLCV-only, so **Momentum/Volatility/Beta/Liquidity** are derived purely from price/volume; **Size**
  (ln cap) and **sector dummies** require optional external `market_cap` / `group_id` spans and are omitted
  when absent (config-gated via `FactorModelConfig{ sector_factors, style_mask=0x1F (all 5), … }`). Each style
  column is z-scored cross-sectionally; a style-NaN instrument is dropped (§3.3); column order is
  deterministic (sectors ascending, then styles in enum order).
- **`FactorModelBuilder::build(panel, window, market_cap, group_id) -> Result<FactorModel>`** estimates
  `(X, F, D)` by a FIXED deterministic two-pass per-date cross-sectional regression over the trailing
  `window`: Pass A OLS → initial specific variances; Pass B WLS (`1/d0`) → factor-return series + final
  residuals. `F = LedoitWolf(cov(factor returns))` (the canonical 1/T² LW reused from the combiner); `D` =
  per-instrument residual variance, floored. Built at row 0 = current date; fit over `[0, window)`. An
  under-determined date (`M_s < K`) is skipped; the PCA stat/dead-factor rungs return `Err(NotImplemented)`
  (explicit default-off deferral).
- **`FactorModel`** — the value type + apply math:
  ```cpp
  usize n_factors() / n_instruments();
  f64  risk(span<const f64> w) noexcept;             // wᵀVw = (Xᵀw)ᵀF(Xᵀw) + Σ D_i w_i²  — O(MK+K²), alloc-free
  void apply_inverse(span<const f64> in, span<f64> out);  // V⁻¹·in via Woodbury, cached K×K capacitance Cholesky — O(MK+K³)
  void neutralize(span<f64> signal);                 // residualize on X: s − X(XᵀX+εI)⁻¹Xᵀs  (factor-neutralization primitive)
  usize fit_begin() / fit_end();                     // the fit/apply firewall window
  // construct via FactorModel::create(X, F, D, fit_begin, fit_end) — validates shapes, floors D, requires F SPD.
  ```
  `create` caches everything the apply path needs (`D⁻¹`, `F⁻¹`, and the K×K capacitance `C = F⁻¹ + XᵀD⁻¹X`
  Cholesky), so each apply is matvecs + one cached K×K solve — never a refactor, never O(M²).

---

## 3. Risk-aware construction — the optimizer (4b)

```cpp
#include "atx/engine/risk/optimizer.hpp"   // OptimizerConfig, PortfolioOptimizer
PortfolioOptimizer opt{cfg};
ATX_TRY(std::vector<f64> w, opt.solve(alpha, V, w_prev));   // alpha = mega-alpha; V = FactorModel; w_prev = previous book
```
Maximizes `αᵀw − λ·wᵀVw − κ·‖w − w_prev‖₁` subject to `Σw = 0` (dollar-neutral), `Σ|w| ≤ L` (gross), and
`|w_i| ≤ cap`. `OptimizerConfig`: `risk_aversion λ=1.0`, `turnover_penalty κ=0.0`, `gross_leverage L=1.0`,
`name_cap=1.0`, `dollar_neutral=true`, `max_iters=64` (FIXED — determinism). The solve is a deterministic
fixed-iteration gradient→proximal-L1→project loop that uses ONLY the factored `apply_inverse` (Woodbury,
never a dense V).

- `κ = 0` ⇒ pure risk-aware mean-variance book.
- `λ = κ = 0`, caps off ⇒ recovers the `WeightPolicy` dollar-neutral book `gross_normalize(demean(α))`
  within 1e-9 (the regression check).
- The turnover penalty is the RenTech cost-throttle: a position only churns when the edge change clears the
  modeled round-trip cost.
- *Note:* because the gross constraint is realized as a scaling normalization (the WeightPolicy precedent),
  the absolute `(1/2λ)` magnitude washes out — the λ effect on the final book is binary (λ=0 vs λ>0, the
  V⁻¹ risk tilt), not a graduated shrink. This is forced by fixing `Σ|w|=L` on a dollar-neutral book.

### `WeightPolicy` neutralization (the simple path, 4b/P4-8)
`loop::WeightPolicy` (the Phase-2 sizing) gained two now-live stages, both **bit-identical-when-off** (the
20 Phase-2 tests pass unchanged): `industry_neutral` (per-group demean, needs a universe-aligned `group_map`)
and `truncation` (per-name `|w_i|` cap in final gross-normalized units, fixed-iteration clip-renorm). DECAY +
factor-neutralize policy wiring are deferred (see §7). `WeightPolicy` remains the default for simple
strategies; the optimizer is the opt-in risk-aware book.

---

## 4. Capacity — net edge vs AUM (4b/P4-10a)

```cpp
#include "atx/engine/risk/capacity.hpp"   // CapacityPoint, capacity_curve
std::vector<CapacityPoint> curve = capacity_curve(weights, panel, sim, aum_grid);
// CapacityPoint{ f64 aum; f64 net_edge_bps; }
```
For a fitted book, sweeps AUM and reports `net_edge_bps = gross_edge_bps − √impact_cost(AUM)`, where the
√-impact (`temp = Y·σ·part^δ`, `part = shares/ADV`) reads the SAME `exec::ExecutionSimulator` coefficients
(via its `impact_cfg()` accessor — ONE cost surface, no second cost path). Net edge is monotone-decreasing in
AUM and crosses zero at the capacity point (RT §9.6). Scoped to the size-dependent √-impact term only.

---

## 5. End-to-end (the combined backtest)

```
gate → store → AlphaCombiner::fit (rolling window) → CombinedSignalSource
   → BacktestLoop (WeightPolicy)  ─┐
   → FactorModelBuilder::build → PortfolioOptimizer::solve (per rebalance, parallel risk book)
   → ExecutionSimulator → Portfolio
```
The loop is `WeightPolicy`-driven (the optimizer + factor model run as a per-rebalance risk book alongside —
wiring the optimizer through the loop is a future change). See `tests/phase4_integration_test.cpp` for the
walk-forward harness over a synthetic panel with delisted symbols + NaN gaps.

---

## 6. Guarantees (what Phase 4 proves — see `phase4_integration_test.cpp`)

- **Fit/apply firewall (no look-ahead via fitting).** Every fitted object carries `[fit_begin, fit_end)` and
  a fit function reads only its window. **Truncation-invariance:** corrupt panel/PnL rows in the future
  (≥ fit_end / ≥ window), re-fit + re-apply combiner → CombinedSignalSource → FactorModelBuilder → optimizer,
  and the output book for dates ≤ t is **byte-identical** — future rows are provably invisible. (The
  apply-after-fit_end abort is currently caller-enforced; a self-asserting guard is a future hardening — §7.)
- **Determinism (§3.2).** No RNG; iterative solvers run a FIXED iteration count in FIXED order (no
  convergence-dependent early exit); reductions are order-fixed. A wyhash (`atx::core::hash_combine`) over the
  ordered `(date, instrument, weight-bits)` stream is stable run-to-run and FLIPS when any input changes
  (non-vacuous).
- **Cost honesty.** The walk-forward backtest is cost-honest: with costs off it recovers the frictionless
  mark-to-market equity exactly; with costs on, equity is strictly lower (Phase-2 invariant, extended to the
  combined pipeline).
- **Capacity, not just return.** A book's net edge is reported as a function of AUM, crossing zero at the
  capacity point.
- **Factored throughout.** `V` is never materialized M×M; `risk`/`apply_inverse` work in factor space
  (K ≪ M). The factor covariance reuses the ONE canonical Ledoit-Wolf closed form (no second estimator).

---

## 7. Known residuals (lifted to the ROADMAP backlog)

- **P4-8 DECAY + FACTOR-neutralize policy wiring** — a stateless `WeightPolicy` holds no signal history
  (temporal decay needs a stateful policy); the `FactorModel::neutralize` primitive ships + is tested but
  carries no instrument→universe map. Ledger residuals, not code stubs.
- **Apply-window self-guard** — `FactorModel`/`Combination` carry `[fit_begin, fit_end)` but do not yet
  self-`ATX_ASSERT` apply-after-fit_end (caller-enforced; proven structurally by truncation-invariance).
- **PCA stat/dead-factor rungs** (`FactorModelBuilder`) — return `Err(NotImplemented)` (explicit, default-off).
- **Optimizer λ>0 gross-collapse** — any two λ>0 give the same book under fixed-gross normalization (a
  forced consequence, documented + spec-accepted).
- **Production single-`compile_batch` `CombinedSignalSource`** (§0-C), position-based combiner rung,
  IC-from-raw-signal, caller-scratch apply overloads, computed-`V` parallelism Linux-TSan pass.
- **Deflated-Sharpe + the full multi-fold walk-forward validation harness → Phase 5.**

All Phase-4 knobs (gate thresholds, combiner method, risk-aversion, name caps, truncation, capacity windows)
default to inert/no-op or generous values — **Phase 5 calibrates them**.

---

## 8. Next sprint (the baton → Phase 5 validation)

Phase 4 ships the *models* + the IS/OOS split + the truncation-invariance proof. Phase 5 deepens the
validation: deflated-Sharpe + a multi-fold walk-forward harness over the combined+risk layer, lifts the
residuals above, wires the production single-`compile_batch` source, and calibrates the knobs.
