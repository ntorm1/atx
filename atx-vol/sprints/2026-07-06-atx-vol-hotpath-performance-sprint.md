# atx-vol — Hot-Path Pricing Performance Sprint

**Date:** 2026-07-06
**Status:** Deep-dive review complete → sprint plan → implementation.
**Scope:** `atx-vol` American-option pricing/greeks hot path. Make the per-option
reprice **state-of-the-art fast** for a backtest that reprices a book across a
corpus (runs millions of times), **without sacrificing fit quality or pricing
quality beyond tick-size epsilon** ($0.005 half-tick for listed equity options).
No changes to the surface FIT — this is the REPRICE path only.

Goal framing: *"Increasing core count does not make this the fastest."* Correct —
C1 (parallel-by-default) is orthogonal throughput; it does not touch the
per-call cost. This sprint attacks the per-call cost, which is where the wall time
actually lives.

---

## 1. The hot path (measured, from source)

Backtest step ⇒ `PortfolioPricer::pnl_explain(base, shifted)` ⇒ **per unique
contract**:

| Call | Cost |
|---|---|
| `surf->greeks(K,T,side)` on `base` → `PricedSurface::greeks` → `american_greeks_fd` | **17 cold `andersen_lake` solves** |
| `surf->fair_value` target reprice on `shifted` → `american_price` | **1 cold solve** |

So **~18 cold Andersen-Lake solves per contract per step.** A 50-name ×
200-strike × 500-day × 2-side dispersion backtest ≈ 10M reprices × 18 ≈ **180M AL
solves** — this is the "millions of times" the goal calls out.

### 1.1 Anatomy of one cold AL solve

`andersen_lake` → `al_solve_put` (puts direct; calls via McDonald-Schröder
`C(S,K,r,q)=P(K,S,q,r)`). Default scheme `al_default_opts() = {n_boundary=12,
n_quad_fp=24, n_iter_jn=8, tol=1e-10}`:

1. `al_init_nodes(bnd, 12, T, K, r, q)` — τ-grid + asymptotic `xmax`. **No S.**
2. `al_seed_boundary(bnd, σ, r, q)` — **12 nested Barone-Adesi-Whaley Newton
   root-finds.** Per the in-code comment (american.cpp:1000) this is *"the
   dominant cold cost."* **No S.**
3. up to `n_iter_jn=8` `al_jacobi_newton_sweep` (each 12×24 = 288 integrand evals,
   2 `norm_cdf` + `exp/log` each), then `al_fixed_point_sweep` fallback. **No S.**
4. `al_put_premium(bnd, ws, S, σ, r, q)` — one `n_quad_price`-node quadrature.
   **S enters ONLY here** (via `premium_integrand_put`'s
   `dp = log(S·e^{-qt}/(b_t·e^{-rt}))/v + v/2`).

**Load-bearing invariant (verified in source):** the exercise boundary `bnd`
depends on `(K, T, σ, r, q)` but **not on S**. Steps 1–3 (the entire expensive
part) are S-independent; only step 4 uses S.

### 1.2 Where `american_greeks_fd` wastes work

17 stencils, each a *full* fresh solve. Bumps are tiny (`hS=1e-3·S`, `hv=1e-3`,
`hr=1e-4`, `hT=1e-3`). Group the 17 by the boundary they need:

| Boundary (σ,r,T) | Stencils that use it (differ only in S) |
|---|---|
| base `(σ,r,T)` | p0, p_Sp, p_Sm |
| `(σ+hv,r,T)` | p_vp, p_SpVp, p_SmVp |
| `(σ−hv,r,T)` | p_vm, p_SpVm, p_SmVm |
| `(σ,r+hr,T)` | p_rp |
| `(σ,r−hr,T)` | p_rm |
| `(σ,r,T+hT)` | p_Tp, p_SpTp, p_SmTp |
| `(σ,r,T−hT)` | p_Tm, p_SpTm, p_SmTm |

**7 unique boundaries, 17 premium evals.** The current code solves the boundary
17 times — **10 of them are exact duplicates of a boundary already solved.**

---

## 2. Sprint plan (ordered by ROI × safety)

Each sprint is independently shippable, gated against the real-OPRA litmus, and
built subagent-driven with an independent rebuild + full-suite re-run.

### Sprint P1 — boundary-reuse in `american_greeks_fd` (puts: BIT-IDENTICAL)

**Idea:** solve each of the 7 unique boundaries once, evaluate the premium at the
S-values that boundary serves. For **puts** this is *bit-identical* to today:
same boundary object → same `al_put_premium` inputs → same doubles.

**Seam:** factor `al_solve_put` into
`al_solve_boundary(K,T,σ,r,q,sch) → AlSolved{bnd,ws,euro-args}` and
`al_eval_put(AlSolved, S) → price`. Add a puts-fast branch to
`american_greeks_fd`: build the 7 `AlSolved`, then evaluate the 17 stencils via
`al_eval_put`. Falls back to the existing 17-solve path for calls in P1.

**Win:** 17 → **7 boundary solves** per put (the boundary solve dominates), plus
17 cheap premium evals. Expected **~2.4× on greeks**, aggregate **~2×** on the
backtest step. Zero accuracy change for puts.

**Calls (P1b, optional):** McDonald-Schröder maps call-S to internal-put *strike*
`Kp=S`, which drives the boundary; but the boundary is homogeneous degree-1 in
`(Kp, xmax)`, so `bnd(Kp=S+hS)` is the base boundary rescaled by `(S+hS)/S`.
Reuse via scaling → 7 boundaries for calls too, within tol (not bit-identical).
Ship separately if the OPRA book is call-heavy; otherwise leave calls on the
17-path (puts dominate American early-exercise).

**Gate:** `PerfBoundaryReuse` — for a strike/T/σ/r/q grid over the OPRA chain,
`american_greeks_fd` (new) == (old) **bit-identical** for every put greek;
determinism gates (n_threads 1 vs N) unaffected; real-OPRA bench before/after.

### Sprint P2 — analytic Andersen-Lake greeks via the envelope theorem (SOTA)

**Idea:** the AL price is `euro(S) + ∫ premium_integrand(S,b(·)) `. The
exercise boundary `b(·)` is the stationary point of the pricing functional, so by
the **envelope theorem** `∂Price/∂θ` (θ ∈ {σ,r,T}) equals the *explicit* partial
of the integrand holding `b` fixed — the `∂b/∂θ` term vanishes to first order.
And S never enters `b` at all. Therefore **all first-order greeks come from a
SINGLE boundary solve**:

- **delta, gamma** — differentiate `euro` (closed form) + `premium_integrand`
  explicitly in S (integrand's S-dependence is elementary: `∂dp/∂S = 1/(S·v)`).
  Exact, no FD, no extra solve.
- **vega, rho, theta** — explicit ∂/∂θ of the integrand at frozen boundary
  (envelope theorem). One extra quadrature pass each; no solve.
- **vanna, volga, charm** — central FD of the analytic first-order greeks over
  the *already-solved* σ±/T± boundaries from P1 (reuse, no new solves).

**Win:** greeks from **1 boundary solve** instead of 7 (or 17). Expected
**~7–15×** on greeks. This is the Andersen-Lake-Offengenden (2016) result and is
the state-of-the-art path.

**Risk / accuracy:** analytic vs cold-FD differ at the FD-truncation level
(O(hS²) ≈ 1e-6 relative). Must validate the analytic greeks reproduce a
Richardson-extrapolated FD reference to **< tick epsilon on price round-trip** and
< 1e-6 on each greek across the full OPRA chain (deep ITM/OTM, near-expiry,
high-skew). This replaces `american_greeks_fd` as the default only after the gate
is green; the FD path stays as `american_greeks_fd_ref` for the gate.

**Gate:** `AnalyticGreeksVsFD` — analytic greeks vs Richardson-FD reference on the
OPRA grid, per-greek tolerance; PnL-attribution closure (`pnl_unexplained`) must
not regress in the backtest.

### Sprint P3 — cross-step boundary warm-start (temporal coherence)

**Idea:** a backtest holds the same contracts day to day. For a fixed `(uid, K,
side)` the boundary moves only by one day's `(dt, dσ, dS-irrelevant, dr)` — a tiny
perturbation. Cache each contract's converged `bnd.y[]` in the pricer keyed by
`(uid,K,side)`; next step **warm-start** the JN sweeps from it (skip the 12-BAW
re-seed — the dominant cold cost). `AloPricer` already proves warm-start
converges to the same fixed point (american.cpp:1003) for σ-moves; day-steps are
smaller.

**Win:** cold seed (12 BAW root-finds) → skipped on ~every repriced step after the
first. Expected **~3–5×** on the *sustained* backtest loop (the "millions of
times" regime), stacking on P2.

**Design:** `PortfolioPricer` gains an optional per-contract boundary cache
(opt-in via `PriceOptions`); invalidated when `|dσ| > 12%` (reseed guard, matching
AloPricer). Determinism preserved: warm-start is deterministic given the cache;
the cache is per-contract, thread-local scatter keeps bit-reproducibility across
thread counts (re-gate).

**Gate:** `WarmStartConverges` — warm vs cold price/greeks agree to tol across a
multi-step OPRA corpus; backtest determinism gate re-run.

### Sprint P4 — kernel micro-optimization (throughput)

Mechanical, measure-guided, no math change:

- **`norm_cdf` is the inner-loop cost** (2× per node × 288 nodes × 8 sweeps). Swap
  the `erfc`-based `norm_cdf` for a vectorizable rational/polynomial approx
  accurate to < 1e-12 (Cody / West), or hoist `σ√t`, `e^{-qt}`, `e^{-rt}`
  per-node precompute out of the sweep loop.
- **`al_put_premium` / sweep** — precompute per-node `z_i², sqrt(t_i)` tables once
  per boundary (currently recomputed each premium eval).
- **Batch by shared boundary across strikes** — `andersen_lake_call_slice` already
  reuses one boundary across a whole strike slice for calls; add the put analogue
  and route the book's same-(T,σ,r,q) contracts through it.
- Compiler: confirm `/O2 /fp:precise`, `__vectorcall` on the kernel, `restrict`
  on the workspace pointers.

**Gate:** `KernelApproxAccuracy` (approx `norm_cdf` vs `erfc` < 1e-12) + bench.

### Sprint P5 — per-surface correction cache route (large-book / dispersion)

**Idea:** the fit already builds Chebyshev early-exercise correction caches;
`PricedSurface` deliberately drops them to stay serializable. Add an **optional,
lazily-built in-memory `CorrectionCache`** on `PricedSurface` (derived data, not
serialized). Route `fair_value`/`greeks` through the closed-form cached path
(`american_price_cached` + `american_greeks_first_order`) — already implemented and
used by `VolaSession::served_cache`.

**Amortization:** cache build ≈ `NK·NT·NS` cold solves; it pays off when
contracts-served-per-surface ≫ grid size — i.e. **deep dispersion books**, not a
160-lot toy. Enable via a heuristic (book size / surface) or explicit config;
`log()` when it engages. Greeks become interpolations (≈ free).

**Gate:** `CachedVsColdWithinTick` — cached price/greeks vs cold AL < tick epsilon
across the OPRA chain (the cache accuracy is the whole ballgame here); build-cost
amortization bench proving net win at the enable threshold.

---

## 3. Accuracy contract (all sprints)

Every sprint holds these or does not ship:

1. **Price round-trip < half-tick ($0.005)** vs the current cold AL on the full
   real-OPRA chain (deep ITM/OTM, weeklies to LEAPS, full skew).
2. **Per-greek < 1e-6 relative** vs a Richardson-extrapolated FD reference.
3. **PnL-attribution closure** (`pnl_unexplained` residual) does not regress in the
   backtest identity `step_total = Σ Taylor + settlement + shares + financing −
   cost`.
4. **Determinism** (n_threads 1 vs N bit-identical) preserved — re-run the
   existing determinism gates.
5. `/W4 /permissive- /WX` clean; full suite green.

## 4. Expected cumulative speedup

| After | Greeks solves/contract | Rel. to today |
|---|---|---|
| today | 17 (+1 reprice) | 1× |
| P1 (puts) | 7 boundary solves | ~2× |
| P2 | 1 boundary solve | ~7–15× |
| P3 | ~1 warm solve (seed skipped) | ~15–40× sustained |
| P4 | same solves, faster kernel | ×1.3–2 on top |
| P5 (dispersion) | interpolation | ×10–100 for large books |

Target: **push the 180M-solve dispersion backtest from ~hours to minutes** with
zero fit-quality change and pricing held to tick epsilon.

## 5. Order of execution

P1a (puts, bit-identical) first — safest, immediate ~2×, establishes the
`AlSolved` solve/eval seam that P2/P3 build on. Then P2 (analytic, the headline
win), then P3 (temporal coherence for the backtest regime), then P4 (kernel), then
P5 (dispersion-scale). Each gated on the real-OPRA litmus before the next starts.

---

## 6. Progress log (implementation)

| Increment | Commit | Result |
|---|---|---|
| **P1a** boundary-reuse in `american_greeks_fd` (puts: 17→7 boundary solves) | `28e2dbe` | **Bit-identical** (75-pt put grid, all 9 greeks `==` FD reference). Controlled micro-bench **2.50×** on the isolated put greeks call. |
| **P1b** warm-start machinery (`al_solve_put_boundary_warm` + `warm_start` flag) | `e9e5715` | **2.91×** warm (only +1.16× over P1a). **Kept dormant** — `PricedSurface::greeks` stays cold to preserve greeks bit-reproducibility across a surface-archive round-trip (`LifecycleIntegration`). Warm belongs in the backtest-engine cross-step path (P3). |
| **P1c** elide duplicate `fair_value` solve in `PortfolioPricer` | *(pending)* | **Bit-identical.** `price()` 8→7, `pnl_explain()` 9→8 boundary solves/put. |

### Key measured finding — sweeps dominate, not the seed

The controlled micro-bench (`DISABLED_FdBoundaryReuse_Speedup`, default reprice
scheme `{n_boundary=12, n_quad_fp=24, n_iter_jn=2, n_iter_fp=4}`) shows the
Barone-Adesi-Whaley cold seed is only ~16% of a solve; the **Jacobi-Newton +
fixed-point sweeps dominate**. This revises the plan:

- **P1b warm-start is worth little on its own** (skipping the seed = ~1.16×) — it
  pays off only when a *cross-step* seed is close enough to also cut the sweep
  budget (P3), so it ships as dormant infrastructure.
- **P2 (analytic greeks) is promoted to the top remaining lever.** It removes 6 of
  7 boundary *solves* (each with its full sweep budget) per greeks call — the
  envelope theorem gives vega/rho/theta from the base boundary with no σ±/r±/T±
  re-solve, delta/gamma are exact from the S-independent boundary, and
  vanna/volga/charm stay as cheap FD on the base-boundary-reused prices. Target:
  greeks from **1 boundary solve** ⇒ `pnl_explain` 8→2 solves/put (~4×). Gated vs a
  Richardson-FD reference to tick epsilon (the FD path is retained as the oracle).
- **Revised order:** P2 next, then P3 (cross-step warm, switching on P1b), then P4,
  P5.

Note: SIMD of the sweep kernel is a dead end here — xsimd measured 6.6× slower and
SVML is unavailable under clang-cl (american.cpp:586). Fewer solves, not faster
transcendentals, is the path.
