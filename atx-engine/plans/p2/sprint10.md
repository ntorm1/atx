# Sprint S10 — user reference (Conviction Sizing & Regime-Adaptive Integration)

Public API shipped by Sprint S10. Test counts + per-unit detail live in the
[ledger](sprint-10-progress.md); this is the user-facing surface + examples + limitations.

S10 closes five RenTech-mapping gaps (G1–G5) on top of the already-mature discovery/validation/
optimization spines. Every entry below is **pure, no-RNG, order-fixed** (byte-identical run to run).

---

## 1. Conviction score — `combine/conviction.hpp`

Turns the binary gate verdict into a continuous **[0,1] confidence** that downstream sizing scales by.

```cpp
#include "atx/engine/combine/conviction.hpp"
using namespace atx::engine;

eval::DsrResult dsr = /* from eval/deflated_sharpe */;
eval::PboResult pbo = /* from eval/pbo */;
combine::ConvictionScore c =
    combine::conviction(dsr, pbo, /*oos_is_ratio=*/0.9, combine::ExplainFlag::HeadScratcher);
// c.score in [0,1]; HeadScratcher applied the 0.5 discount ("trade unexplained at reduced size").
```

`score = (w_dsr·clamp(dsr.dsr) + w_pbo·clamp(1−pbo.pbo) + w_stability·clamp(oos_is_ratio)) · explain_mult`.
Weights (default 0.40 / 0.35 / 0.25) sum to 1; `explain_mult` ∈ {1.0 Explained, 0.75 PartlyExplained,
0.50 HeadScratcher}. Tune via `ConvictionConfig`. **Precondition:** DSR/PBO/ratio finite (ATX_ASSERT).
The OOS/IS stability ratio is **caller-supplied** (`AlphaMetrics` has no IS/OOS field).

## 2. Fractional-Kelly sizing — `risk/kelly_sizing.hpp`

Explicit conviction-scaled fractional-Kelly target over the **factored** covariance.

```cpp
#include "atx/engine/risk/kelly_sizing.hpp"
risk::KellyWeights kw =
    risk::kelly_size(expected_alpha, factor_model, conviction, {.kelly_fraction = 0.25});
// kw.weights = quarter-Kelly V^{-1}mu, per-name conviction-scaled, gross-clamped to cfg.max_gross.
```

`f* = V⁻¹μ` via `FactorModel::apply_inverse` (Woodbury — no MxM inverse); scaled by `kelly_fraction`
(default quarter-Kelly) and per-name `conviction` (zero conviction ⇒ zero weight); gross `Σ|w|` clamped.
This is the GP optimizer's **target**, not a QP replacement. **Precondition:** matching lengths, finite
inputs, conviction ∈ [0,1] (ATX_ASSERT).

## 3. Regime-conditioned combination — `combine/regime_combiner.hpp`

One combo per market regime, blended point-in-time by the HMM posterior.

```cpp
#include "atx/engine/combine/regime_combiner.hpp"
auto rc = combine::fit_regime_combiner(pool, regime_labels, n_regimes, fit_begin, fit_end).value();
auto post = learn::regime_posterior_at(fitted_hmm, obs, t);   // PIT: data <= t only
std::vector<double> w = rc.blend({post.data(), (size_t)post.size()});  // convex blend
```

`fit_regime_combiner` fits each regime over a **masked sub-pool** (reusing `AlphaCombiner` verbatim).
`blend(posterior)` returns `Σ_r posterior[r]·per_regime[r].weights` — convex for a probability posterior.
**Single-regime fallback is byte-identical** to `AlphaCombiner::fit`. Under-populated regimes (< 2
periods) fall back to the global combo. No look-ahead (posterior is PIT).

## 4. Crowding / capacity de-correlation — `combine/crowding.hpp`

Shrinks redundant (mutually correlated) and capacity-limited signals in a fitted weight vector.

```cpp
#include "atx/engine/combine/crowding.hpp"
std::vector<double> adj =
    combine::decorrelate_weights(weights, pool, fit_begin, fit_end, capacity,
                                 {.corr_penalty = 1.0, .capacity_floor = 0.0});
// n perfectly-correlated copies collapse to ~one signal's weight. corr_penalty=0 => exact passthrough.
```

`out_i = cap_scale_i · w_i / (1 + corr_penalty·Σ_{j≠i}|corr(i,j)|)`. Capacity is **caller-supplied**
per name (compute from `cost/capacity.hpp` upstream). Result is **not** renormalized.

## 5. Breadth instrumentation — `eval/breadth.hpp`

Effective number of independent bets and the Fundamental Law decomposition.

```cpp
#include "atx/engine/eval/breadth.hpp"
double n_eff = eval::effective_breadth(signal_cov);            // (Σλ)²/Σλ²
eval::BreadthResult b = eval::breadth_decomposition(signal_cov, /*ic=*/0.05);  // b.ir = IC·√N_eff
```

K orthogonal equal-variance bets ⇒ `N_eff = K`; K identical bets ⇒ `N_eff = 1`. Eigenvalues via
atx-core `symmetric_eig`, clamped `≥0`; zero matrix ⇒ `N_eff = 0`.

---

## Known limitations / deferred

- **Breadth report line** is **not** wired into `atx-impl run_report` yet — the report stage has no
  per-name return-covariance source in hand (it would need new pipeline plumbing to retain per-name PnL
  contributions). The `eval/breadth` library is complete and unit-tested; only the CLI emission is pending.
- **S10-6** (walk-forward adaptation) and **S10-7** (cross-source data validation) were deferred to a
  later sprint (RenTech gaps G6/G7) — specs frozen in the [impl-plan](../../research/rentech-improvement-sprint-plan.md).
- **Build note:** the `eval` test group must be built per-TU (`ATX_UNITY_BUILD=OFF`) — a *pre-existing*
  (not S10) unity-batch `struct Lcg` name collision between two eval test files breaks the unity build.
