# Options Market-Making System & Algorithm Design — State of the Art vs. `atx-vol`

**Date:** 2026-07-18
**Scope:** How top options market-making firms (Citadel Securities, Jump, SIG/Susquehanna, Jane Street, Optiver, IMC, Wolverine, DRW) design their pricing, surface, quoting, and risk stacks — and what it would take for `atx-vol` (C++20 American-equity options pricing/analytics library) to reach that frontier. Focus: **(1) performance, (2) features, (3) next steps.**
**Method:** Multi-agent deep-research harness — 5 search angles → 25 sources fetched → 106 claims extracted → 25 top claims adversarially verified (3-vote, ≥2/3 to kill) → synthesized. 23 confirmed, 2 refuted. Plus a codebase reconnaissance pass over `include/atx/vol`.
**Confidence policy:** Every quantitative claim below carries a source tag `[n]` and, where the verifier ran, a vote (`3-0`, `2-1`). Read the **Caveats** section before treating any single ns-figure as gospel.

---

## 0. TL;DR for the engineering roadmap

1. **`atx-vol`'s numerical core is already the industry-standard family.** Andersen–Lake–Offengenden (ALO) spectral collocation for American pricing and Choi-seeded Newton / Let's-Be-Rational for IV inversion are exactly what the top firms and QuantLib use. You are not behind on *algorithm choice*. You are competing on *implementation latency* and on *the layers above pricing that you have not built yet*.
2. **The two measurable performance gaps** are (a) IV-inversion latency (`atx-vol` ~218 ns scalar vs. Jäckel ~180 ns and a 2026 wave of faster methods) and (b) whether vol-surface fitting is fast enough to stream tick-by-tick. Both have published, adoptable fixes.
3. **The two biggest *feature* gaps** — a **quoting engine** and **unified cross-underlier portfolio risk** — are precisely the parts that turn a pricing library into a market-making *business*. Both have a mature literature (Avellaneda–Stoikov → GLFT → Guéant high-dimensional options MM; adjoint-AD portfolio greeks; grid revaluation VaR) that gives you a concrete design, not just inspiration.
4. **The firm-specific latency/architecture detail is mostly unpublished.** Citadel/Jane Street/Jump/SIG do not publish their pricing internals. What *is* public (Optiver CppCon talks, HFT C++ microbenchmark studies) is design discipline, not numbers you can copy. Treat every "firms do X" statement here as inference unless it cites a named talk.

---

## 1. Where `atx-vol` stands today (codebase reconnaissance)

Baseline for the gap analysis. `atx-vol` is a C++20 compiled library (`atx::vol` over `atx::core`), an idiomatic port of a ~65k-LOC C17 `ats-vol` plus a Vola-Dynamics-style American-equity parity layer.

| Layer | What exists | Maturity |
|---|---|---|
| **European pricing** | Black-76 price/vega/aux; 8 analytic greeks (delta, gamma, vega, theta, rho, vanna, volga, charm) | ✅ Solid |
| **American pricing** | **ALO spectral collocation** (Chebyshev-in-√t boundary, damped Jacobi–Newton + fixed-point, Gauss–Legendre premium quad, calls-from-puts symmetry, ~10–11 sig figs); **BAW** quadratic (seeds ALO); **QD+ seed** exists but opt-gated (task A6); Chebyshev American−European correction cache | ✅ Strong; QD+ underperforms BAW in A/B (obs 27240/27241) |
| **IV inversion** | European: Stefanica–Radoicic (2017) closed-form seed + Halley; American: safeguarded Newton (`rtsafe`) + correction-cache surrogate; AVX2 batch with **Choi-2023 seed** + 3× Halley | ✅ ~218 ns scalar (K2), AVX2 1.27× (K3) |
| **Vol surface** | SVI (Zeliade + Martini–Mingone LM), **eSSVI** (cube-space LM, analytic Mingone Jacobian, IRLS-Huber, global sequential driver), C8 (SVI-JW), CStar (C16M modal), S3/SSVI, dense linear-variance; calendar+butterfly arb checks + repair | ✅ Broad; **no SABR** |
| **Dividends** | Hybrid escrowed↔proportional cash-div forward; PCP-implied borrow (HTB) | ✅ Differentiator |
| **Performance eng.** | Per-file AVX2 kernels (black76/greeks/iv/essvi/pnl/american-boundary), runtime CPUID dispatch, SoA batches, **work-stealing `PricingExecutor`** (E1 nested budget, E2 help-first steal), header-only `parallel_for`, bit-identical for any thread count | ✅ Advanced |
| **Portfolio / risk** | Canonical `portfolio_pricer` (dedup + mark + Taylor PnL-explain). **Cross-underlier aggregation, scenario/stress engine, VaR, stock/cash legs live only in DEPRECATED `portfolio.hpp`/`portfolio_risk.hpp`** | ⚠️ Gap on canonical path |
| **Quoting** | **None.** All "quote" hits are `QuoteFrame` data structures. No edge/width/skew, no two-sided quote generation | ❌ Absent |
| **Streaming surface** | Snapshot/batch pipeline (chain→fit→archive); tick-to-quote is a single-expiry transactional refit, not a live feed handler; `vol_time` clock unwired | ❌ Not real-time |

**Measured numbers already in-tree** (for calibrating "how far to go"): `value_chain` 21,395 inv/s/core → 93,612 on 4 cores; `fair_value` cached 6.5 µs (15–75×); IV ~218 ns/op scalar (K2, vs Jäckel 180); AVX2 IV 1.27×; American fast/accurate 37–47 / 158 µs; SPY e2e 347→139 ms @ 4 workers; SIMD kernel wins eSSVI 2.59×, greeks 1.95×, PnL 2.25×, Black-76 1.62×.

**Bottom line:** `atx-vol` is a *research-grade pricing + fitting engine*, not yet a *market-making system*. The three missing pieces for the latter: a quoter, a streaming surface, and unified cross-name risk.

---

## 2. Pricing engines & numerics — the algorithm layer (`atx-vol` is at frontier)

### 2.1 American pricing: ALO is the production standard — you already have it

**Verified `[3-0]`:** The production standard for American-equity pricing is the **Andersen–Lake–Offengenden (ALO)** spectral-collocation method — a Jacobi–Newton iteration for the optimal early-exercise boundary, aided by Gauss–Legendre quadrature and Chebyshev interpolation of a boundary transformation — reaching **~100,000 prices/sec/CPU (~10 µs/price)** at accuracy comparable to a several-hundred-step finite-difference grid. Independently reproduced in QuantLib (HPC-QuantLib) as dominating PDE/tree methods on the accuracy-vs-runtime frontier. `[1]`

> *"typically close to 100 000 option prices per second per CPU"*; algorithm *"involves a carefully posed Jacobi-Newton iteration for the optimal exercise boundary, aided by Gauss-Legendre quadrature and Chebyshev polynomial interpolation on a certain transformation of the boundary."* — Andersen, Lake, Offengenden (2015), J. Computational Finance 20(1) `[1]`

**Implication for `atx-vol`:** This *is* your `andersen_lake_call_slice` / `al_fast_opts()` path. Your measured 37–47 µs "fast" and 158 µs "accurate" are in the right ballpark (the 10 µs figure is 2015-era, single-price, minimal-node). **No algorithm change needed here — the headroom is node-count tuning, boundary-reuse across strikes (you have `andersen_lake_call_slice`), and SIMD of the boundary batch** (currently below the 2.0× gate at 1.87×, not shipped). The QD+ seed you spiked (A6) *underperformed BAW* in your own A/B — consistent with the literature treating BAW/QD+ as *seeds*, not final pricers. Keep BAW as the seed.

**What you're *not* missing:** binomial/Leisen-Reimer trees and production finite-difference PDE are *slower* on the accuracy-vs-runtime frontier than ALO for vanilla American equity options; your PDE-as-test-oracle-only decision is correct. The place a PDE pricer earns its keep is **discrete cash dividends** (early-exercise around ex-div dates), which ALO handles only approximately — see §6 roadmap.

### 2.2 IV inversion: this is your clearest measurable performance gap

**Verified `[3-0]`:** **Jäckel's "Let's Be Rational" (LBR)** is the reference IV-inversion algorithm and the speed baseline to beat: a four-branch rational initial guess + three-branch objective + third-order Householder step (convergence order 4) reaches **maximum IEEE-754 double precision in exactly two iterations** for all valid inputs. Speed: *"just under one microsecond"* in the 2015 paper, **~180 ns** in current implementations. `[2]`

> *"the presented method evaluates a single implied volatility with two iterations on a standard computer in just under one microsecond, most of which is spent in the normalized Black function."* — Jäckel (2015) `[2]`

**Verified `[3-0]`:** **Choi, Huh & Su (2023/2024)** — the seed `atx-vol` already uses — formulate Newton–Raphson **on the log price** with a new tighter lower bound as the initial guess, giving rapid convergence *across all price ranges* including deep-OTM where naive Newton stalls. Peer-reviewed (Operations Research Letters 57, 2024). `[3]`

**The 2026 frontier (adopt-with-care):**

- **Verified `[3-0]`:** **FlashIV** (Le Floc'h & Healy, 2026) — built for *sub-LBR* latency via a **fixed-cost, branch-minimizing** design instead of iterate-to-convergence: cheap Li/asymptotic seed + a *fixed* branch-light Householder refinement + guarded boundary handling; each input normalized to an OTM price and solved as a **tail-stable erfcx/log-price residual** (numerically stable in the wings). Runs *materially faster than a normalized Java port of LBR* at near-reference accuracy. `[4]` **Caveat:** the comparison is vs a *Java* port, not Jäckel's ~180 ns C++, and the benchmark is self-reported.

- **Verified `[2-1]` (medium confidence):** **Schadner (2026)** derives an *analytically exact, explicit* IV formula (inverse-Gaussian-quantile identity), benchmarked at **~59.5 ns median vs LBR 180 ns** at comparable machine precision. `[5]` **Heavy caveat:** single-author non-peer-reviewed preprint; an independent reproduction found the "explicit" formula still runs **~4–5 internal Halley/GIG-quantile iterations**, giving a real-world **~2.31× speedup, not the 3.4× some summaries cite**. Treat as "there is ~2× headroom to be had," not "59 ns is free."

**Implication for `atx-vol` (high-value, concrete):**
Your ~218 ns scalar trails LBR's ~180 ns by ~20%. Three things to try, in order:
1. **Profile the residual, not the solver.** LBR spends *most of its time in the normalized Black function*, i.e. in `erfc`/`exp`. You already ship a Cody CALERF erfc (~1.1e-16) — verify it, not the Householder iteration, is the bottleneck. If `erfc` dominates, the win is in the special-function kernel, not the root-finder.
2. **Adopt FlashIV's branch-light *fixed-cost* structure for the SIMD path.** Your AVX2 IV path patches degenerate/deep-wing lanes to scalar (parity-driven). FlashIV's whole thesis is *fixed cost + tail-stable erfcx residual so no branch/scalar-fallback is needed* — that directly attacks your 1.27× (should be closer to lane-count) by eliminating the scalar patch lanes. This is the single most actionable IV item.
3. **Bench Choi-seed vs FlashIV-style vs explicit-formula on your own SIMD harness** (this is an open question from the research — nobody has published a head-to-head on identical hardware). You have the harness (`simd/iv_batch.hpp`, K2/K3 benches). This is a 1–2 day experiment with a clear, publishable-internally result.

### 2.3 Greeks at scale: adjoint AD is the missing efficiency tool

**Verified `[3-0]`:** **Adjoint algorithmic differentiation (AAD)** computes the *full gradient* (all greeks) of a payout at a cost **bounded by ~4× a single price evaluation, independent of the number of sensitivities** — versus "bumping" (finite-difference), whose cost grows *linearly* with the number of greeks. Adjoint mode is the efficient choice when inputs ≫ outputs, e.g. **aggregated risk of a whole portfolio**. `[16]`

> *"the adjoint mode of AD allows the gradient of the payout function to be obtained at a computational cost that is bounded by approximately four times the cost of evaluating the payout itself"* … *"such relative cost is independent of the number of components of the gradient."* — Capriotti (2011), J. Computational Finance 14(3) `[16]`

**Implication for `atx-vol`:** You currently compute American greeks by **cold finite-difference on the cached mark** (`adjusted_greeks.hpp`) — the exact "bumping" method AAD beats. For per-contract analytic B76 greeks this is fine (you have closed forms). But for **portfolio-level risk of millions of contracts w.r.t. many surface/curve parameters**, bumping is quadratic-ish and will not scale. **AAD over the pricing graph is the industry answer for portfolio greeks.** This is a large but well-defined investment (see §6, tier 3) — and it is the correct foundation for the missing cross-name risk layer, not a bolt-on.

---

## 3. Vol surface construction — fast enough to *stream* is the goal

### 3.1 The arbitrage-free foundation you already build on

**Verified `[3-0]`:** **Gatheral–Jacquier SVI/SSVI (2014)** is the canonical arbitrage-free surface: calibrate the SVI parametrization to *guarantee absence of static arbitrage*, exhibiting a large class of arbitrage-free surfaces (SSVI / Surface SVI) with a *simple closed-form representation* enabling cheap full-surface parametrization. `[6]` **Caveat verified:** the guarantee is realized via the SSVI sub-family — *raw per-slice SVI is not automatically arbitrage-free.*

### 3.2 The streaming-surface unlock: unconstrained global eSSVI

**Verified `[3-0]`:** **eSSVI surfaces can be made provably arbitrage-free *by construction*, enabling unconstrained (rather than sequential slice-by-slice constrained) calibration — the key to a real-time streaming surface.** Explicit closed-form conditions exist (SSVI calendar-arb-free iff ∂ₜθₜ ≥ 0 and 0 ≤ ∂_θ(θφ(θ)) ≤ (1/ρ²)(1+√(1−ρ²))φ(θ); butterfly-arb-free when θφ(θ)(1+|ρ|)<4 and θφ(θ)²(1+|ρ|)≤4). **Mingone (2022)** reparametrizes the *entire* no-arbitrage domain onto a box (−1,1)ⁿ × (0,∞)ⁿ × (0,1)ⁿ so **every parameter point yields a butterfly- and calendar-arbitrage-free surface, calibrated globally over all slices at once.** `[9][10]`

> *"a global and arbitrage-free parametrization of the eSSVI surfaces"* … *"Parameters calibration is no more performed sequentially slice by slice but globally on all slices."* — Mingone (2022), Quant Finance 22(12) `[9]`

**Verified `[mixed: guarantee 2-1, throughput 3-0]`:** **Corbetta, Cohort, Laachir & Martini (2019)** give a robust SSVI/eSSVI slice calibration guaranteeing each slice is free of *both* butterfly and calendar arbitrage, and *prove that linear interpolation of the calibrated (θ, ρ, ρ·φ) parameters preserves* static-arb-freedom across the full surface. **Concrete throughput:** a *Python* implementation calibrates 12 maturities (avg 98 options each) in **1.2 s** on an Intel E5-2673 v3; projected **~0.1 s parallelized** and **~0.01 s in C**. `[7]`

**Implication for `atx-vol` (high-value):**
- You already have eSSVI with an analytic Mingone Jacobian and a *global sequential* driver. **Adopt Mingone's box→unconstrained reparametrization** (box maps trivially to unconstrained via sigmoid) so calibration becomes an unconstrained optimization where *every* iterate is arbitrage-free. This is the difference between "fit then repair/reject" (your current `arb.hpp` projection/repair) and "cannot leave the no-arb domain" — the latter is what makes a **tick-by-tick streaming refit** safe to ship.
- The Corbetta ~0.01 s-in-C figure is your feasibility proof: a full SPY-style surface refit in **~10 ms in C** is achievable. Your SPY e2e is already 139 ms @ 4 workers for the whole pipeline; a *surface-only* incremental refit at ~10 ms is the streaming target.

### 3.3 Non-iterative SVI fit for the hot path

**Verified `[3-0]`:** **Schadner's "direct least-squares" SVI fit** replaces non-linear SVI optimization with a **non-iterative closed-form solve**: it fully linearizes SVI by rewriting it as a **conic section (hyperbola)** and solves a **generalized eigenvalue problem** — *insensitive to initial guess* (finds the global optimum). **~25× faster than the standard quasi-explicit routine (mean 0.087 ms vs 2.131 ms/fit)** at comparable SSE, validated across 7 asset classes. `[8]` **Caveat:** single-author self-benchmark vs the author's own quasi-explicit baseline; absolute ns are hardware-sensitive.

**Implication for `atx-vol`:** For **per-slice** hot-path refits (your `dense_slice.hpp` HFT linear-variance path is the analog), a direct-conic SVI solve gives a **global-optimum fit with zero iteration and zero seed sensitivity** — ideal for a quoter that must refit a single expiry on every tick without divergence risk. Consider it as a fast-tier alternative to the Martini–Mingone LM for single-slice streaming.

### 3.4 What the practitioners say (lower verification, directional)

- Standard parametric models used by MMs: **SABR and SVI, plus proprietary in-house models.** `[22]` **You have SVI/eSSVI but no SABR** — SABR remains the lingua franca for interest-rate/FX and some equity desks; a SABR module would close a recognized gap, though eSSVI is arguably superior for arbitrage-free equity surfaces.
- Auto-quoters send *"millions of quote updates per second across thousands of option series."* `[22]` — sets the throughput bar for the *surface → quote* pipeline, not just the fit.

---

## 4. Quoting & hedging — the biggest feature gap, with a clear literature

`atx-vol` has **no quoting engine.** This section is the design spec for building one. The theory is a clean lineage: Avellaneda–Stoikov (2008) → Guéant–Lehalle–Fernandez-Tapia closed form → Guéant high-dimensional *options* MM → hedging with market impact → RL that closes the loop with your surface.

### 4.1 Avellaneda–Stoikov (the primitive: inventory-skewed fair value + spread)

The MM's fair value is **not the mid** — it is an inventory-dependent **reservation price**: `[14]`

```
r_t = S_t − q_t · γ · σ² · (T − t)          # long inventory (q>0) pushes quote center below mid
δ*  = γ · σ² · (T − t) + (2/γ) · ln(1 + γ/κ) # optimal total spread: risk term + liquidity term
S_t^bid = r_t − δ*/2 ,  S_t^ask = r_t + δ*/2  # symmetric around r_t, NOT around mid
```

- **γ** (risk aversion): larger → wider spread **and** faster inventory mean-reversion to zero.
- **κ** (order-book liquidity / fill-intensity decay): higher → tighter spread; governs the adverse-selection component.

### 4.2 GLFT (the runnable refinement — closed-form depths)

**Guéant–Lehalle–Fernandez-Tapia** gives explicit optimal bid/ask *depths* as constant half-spread + linear inventory skew, adjusting to volatility and order-arrival intensity (A, k): `[15]`

```
δ_bid(q) = c₁ + (Δ/2)·σ·c₂ + q·σ·c₂
δ_ask(q) = c₁ + (Δ/2)·σ·c₂ − q·σ·c₂
```

This is the template for `atx-vol`'s missing quote math: **edge, width, and skew as closed-form functions of inventory and vol**, with the intensity params (A, k) calibrated from your own fill data. It is a few hundred lines on top of the pricer.

### 4.3 High-dimensional *options* MM (the part that matters for an options book)

**Verified `[3-0]`:** Optimal *options* market-making is a **high-dimensional stochastic control (HJB) problem over "several thousands" of option positions.** Bergault & Guéant (2020) / the Guéant optimal-MM line make it tractable with a **value function quadratic in the inventory vector** (an ansatz), claimed more flexible than prior options-MM algorithms. **Key risk consequence:** *short-dated options have distinct dynamics and cannot be folded into an aggregated inventory* — which invalidates naive first-order-greek / factor dimensionality reduction for them. `[11]`

> *"controlling positions of several thousands of financial assets"* … *"short-dated options inventories cannot be managed as a part of an aggregated inventory, which prevents the use of dimensionality reduction techniques such as a factorial approach or first-order Greeks approximation."* — arXiv:2009.00907 `[11]`

**Caveat verified:** in practice firms *do* aggregate short-dated options — via **higher-order greeks and full-scenario revaluation**, not first-order shortcuts. The "cannot aggregate" claim is specific to *first-order/factor* methods. This directly motivates the scenario-grid risk engine in §5.

**Hedging with impact:** Barzykin, Bergault & Guéant (2021) extend the theory to the realistic case where the maker **hedges accumulated delta in the underlying with temporary/permanent market impact**, deriving quote skew *and* an optimal hedging schedule jointly — the exact "inventory + delta-hedging + edge/width" coupling a quoting engine needs. `[12]`

### 4.4 The 2025 blueprint that couples your surface to the quoter

**Verified `[3-0]`:** **Zhang (2025)** embeds a **fully differentiable, arbitrage-free eSSVI surface** (butterfly/calendar no-arb enforced via smoothed softplus/C¹ lattice penalties) *inside* a constrained-RL market-making loop. The policy has **five economically-semantic control heads**: half-spread (width), delta-hedge intensity, and two structured surface deformations (state-dependent ρ-shift and ψ-scale), plus a learnable dual head acting as a state-dependent Lagrange multiplier — a **"white-box" learner** unifying pricing consistency and execution control. `[13]`

**Implication for `atx-vol`:** This is the *architecture diagram* for wiring your existing eSSVI/CStar calibration to a quoter. The five control heads map onto concrete outputs a quoter must emit; the differentiable-surface-with-smoothed-arb-penalty is directly buildable on your `arb.hpp` (replace hard projection with a C¹ softplus penalty so gradients flow). You do not need the RL to get value — the *parametrization* (quote = f(surface deformation + inventory skew + hedge intensity)) is the reusable part.

### 4.5 Desk-level heuristics (practitioner, directional)

- Skew quotes on inventory: **lower both bid and offer when long gamma and/or vega**; raise the ask / lower the bid when already heavily short a strike, to discourage further accumulation. `[21][22]`
- **Manage aggregate book-level greek exposure, not option-by-option.** `[22]` — this is the design principle that forces the §5 risk layer.
- Full-cycle quoting latency: *"low single-digit milliseconds for the full cycle; the fastest firms operate in the high-microsecond range."* `[22]` (secondary/blog — directional only.)

---

## 5. Risk & portfolio management at scale — the second feature gap

`atx-vol`'s cross-underlier aggregation, scenario engine, and VaR live only in **deprecated** modules. To manage "portfolios of millions of contracts," this must move to the canonical path and scale.

### 5.1 Full revaluation vs. delta-gamma approximation

The scenario/stress engine `atx-vol` is missing is **grid-based full portfolio revaluation**: reprice the *entire* book across a grid of market scenarios (spot × vol shocks) rather than trusting a delta-gamma Taylor approximation. `[23]` (Source quality: *unreliable*; treat the *idea* as sound — it is standard risk practice — but not the specific claims.) This is exactly why the §4.3 "short-dated options can't be first-order aggregated" caveat matters: for short-dated/high-gamma positions you **must** full-revalue, not Taylor-expand. Your `scenario_grid.hpp` (currently deprecated-path) is the right primitive; the work is making it (a) canonical, (b) cross-underlier, (c) fast.

### 5.2 Making it fast: adjoint AD + SIMD + the executor you already have

- **AAD (§2.3)** gives full portfolio greeks at ~4× one revaluation, independent of the number of risk factors `[16]` — the right tool for real-time cross-name greek aggregation w.r.t. many surface parameters.
- **SIMD on the risk scan:** a risk engine scanning **50,000 positions** cut per-tick latency from **~200 µs to ~30 µs (~6.7×)** via SIMD; scalar code leaves 60–70% of the hardware idle; AVX-512 processes 8 doubles/op. `[19][20]` Your AVX2 kernels + SoA batches are already the correct substrate — extend them to the aggregation scan.
- **Your `PricingExecutor` (work-stealing, bit-identical for any thread count) is exactly the engine to fan a full-book revaluation across cores.** The missing piece is the *cross-underlier aggregation tree* (by-uid / by-expiry / by-group), which exists in deprecated `portfolio_risk.hpp` — port it onto `portfolio_pricer` + the executor.

### 5.3 Throughput target

The research surfaced **no verified per-firm throughput number** for millions-of-contracts risk. The usable anchors: `[19]` 50k positions revalued per tick at ~30 µs (SIMD); `[22]` millions of quote updates/sec at the quoting layer. A defensible internal target: **full-book greek re-aggregation across ~1M contracts in ≤ single-digit ms** on a multi-core node, using AAD + SIMD + the executor. This is an **open question** worth its own benchmark sprint.

---

## 6. Low-latency systems architecture — mostly design discipline, few public numbers

**Important honesty flag:** the research found **essentially no firm-specific latency or architecture detail** (Citadel/Jump/Jane Street/SIG/IMC/DRW publish little). Everything below is either a named public talk or a generic HFT-C++ microbenchmark study — *not* a leaked internal spec. Treat as engineering discipline, not a target to match numerically.

### 6.1 What's public from the firms

- **Optiver** — "Low Latency C++ Systems for Trading" (David Gross, Options Tech Lead, CppCon) and "When Nanoseconds Matter: Ultrafast Trading Systems in C++": the stated principle is **ultra-low latency is a design-level concern, not post-hoc micro-optimization**; hot path kept **branch-free and allocation-free**, **cache-aware/data-oriented (SoA)**, using **concurrent/lock-free data structures**. `[17]` These are *exactly* the disciplines `atx-vol` already follows (no-exceptions `Result<T>`, Rule-of-Zero, SoA batches, `/W4 /permissive- /WX`).
- **"When a Microsecond Is an Eternity"** (Carl Cook, CppCon 2017): the canonical talk on keeping the trading hot path fast (avoid the slow path entirely; template/compile-time dispatch; cache warming). `[18]`

### 6.2 Quantified HFT-C++ technique study (peer-reviewed microbenchmarks)

A 2023 study `[19]` measured, on identical hardware:

| Technique | Measured effect |
|---|---|
| **Cache warming** | ~90% speedup (cold 267.7M ns → warm 25.6M ns) — *largest single lever* |
| **`constexpr` / compile-time eval** | ~90.9% (0.245 ns vs 2.69 ns/iter) |
| **Loop unrolling** | ~72% |
| **Lock-free atomic vs mutex** | ~63% faster (65,369 ns vs 175,904 ns) |
| **SIMD (SSE2, 4×f64)** | ~49% (21,447 → 10,929 ns) |
| **LMAX Disruptor (lock-free ring buffer)** | ~3 orders of magnitude lower mean latency, ~8× throughput vs lock/queue; ~38% vs traditional queuing |
| **Compile-time vs virtual dispatch** | ~0.68 ns / 0.23 ns saved per call |
| **All combined (pairs-trading algo)** | **87.4% end-to-end latency cut (517,559 → 65,588 ns), std dev 4233 → 400 ns (determinism)** |

**Implication for `atx-vol`:** you already exploit SIMD, SoA, compile-time dispatch, and cache-residency (L1-resident correction tensors). The two levers you may not be fully using:
1. **LMAX-Disruptor-style lock-free ring buffer** for the eventual tick→fit→quote pipeline (the streaming surface + quoter need an inter-thread messaging bus; the Disruptor pattern is the proven choice — ~3 orders of magnitude over lock/queue).
2. **Determinism as a first-class metric** — the combined-technique result cut std-dev 10×. Your "bit-identical for any thread count" property is already a determinism win; extend the discipline to *latency* determinism (tail control), which is what "a microsecond is an eternity" is really about.

### 6.3 SIMD width

C++26 `std::simd` targets quant workloads (Black-Scholes grids, MC aggregation); AVX-512 = 8 doubles/op vs your AVX2 = 4. `[20]` **An AVX-512 path is a plausible ~2× on your throughput-bound kernels** where AVX2 already wins (eSSVI 2.59×, greeks 1.95×) — gated on target hardware having AVX-512 (your runtime CPUID dispatch already supports adding an ISA tier).

---

## 7. Gap analysis — `atx-vol` vs. the frontier

| Capability | Frontier / firms | `atx-vol` today | Gap | Priority |
|---|---|---|---|---|
| American pricing algorithm | ALO spectral collocation `[1]` | ✅ Has ALO + BAW seed | **None** (algorithm) | — |
| IV inversion latency | LBR ~180 ns `[2]`; FlashIV/Schadner sub-180 `[4][5]` | ~218 ns scalar, AVX2 1.27× | ~20% scalar; SIMD lane efficiency | **P1** |
| Arbitrage-free surface | Gatheral-Jacquier SSVI `[6]`; Mingone global eSSVI `[9]` | ✅ eSSVI, but fit-then-repair | Unconstrained box reparam for streaming | **P1** |
| Streaming/tick surface | ~0.01 s C surface refit `[7]`; direct-conic SVI 0.087 ms `[8]` | ❌ snapshot/batch only | Real-time incremental refit | **P1** |
| Quoting engine | AS → GLFT → Guéant hi-dim `[11][14][15]` | ❌ none | Two-sided quote gen, skew, width | **P0 (feature)** |
| Delta hedging w/ impact | Barzykin-Bergault-Guéant `[12]` | ❌ none | Joint quote+hedge schedule | **P0 (feature)** |
| Cross-underlier portfolio greeks | AAD 4× cost `[16]`; SIMD scan `[19]` | ⚠️ deprecated path only | Canonical + AAD + scale | **P0 (feature)** |
| Scenario/stress/VaR engine | Grid full revaluation `[23]` | ⚠️ deprecated `scenario_grid` | Canonical, cross-name, fast | **P1** |
| SABR | Industry-standard alt `[22]` | ❌ none | Optional (eSSVI arguably better) | **P3** |
| Discrete-cash-div American PDE | (early exercise around ex-div) | ❌ PDE is test-oracle only | Correctness for div names | **P2** |
| Low-latency messaging bus | LMAX Disruptor `[19]` | ❌ (batch pipeline) | Needed for streaming/quoter | **P1 (with quoter)** |
| AVX-512 kernels | 8×f64 `[20]` | AVX2 4×f64 | ~2× on throughput kernels | **P2** |

---

## 8. Prioritized next-steps roadmap

Ordered by (value to "SOTA MM system") × (evidence that it works) ÷ (effort). "P0" = turns the library into a market-making system; "P1" = closes a measured performance/feature gap with a published fix.

### Tier 0 — turn the pricing library into a market-making system (features)
1. **Quoting engine (`quoter.hpp`)** — implement GLFT closed-form depths `[15]` (reservation price + inventory skew + vol/intensity-adjusted width) on top of `portfolio_pricer`. Calibrate fill-intensity (A, k) from your own OPRA data. **~weeks; highest strategic value.** The AS/GLFT math is small; the value is enormous (it's the missing product).
2. **Unified cross-underlier portfolio risk on the canonical path** — port the by-uid/by-expiry/by-group aggregation + scenario grid from deprecated `portfolio_risk.hpp` onto `portfolio_pricer` + `PricingExecutor`. This is the "manage aggregate book greeks, not option-by-option" principle `[22]` made real.
3. **Delta-hedging schedule** — Barzykin-Bergault-Guéant joint quote+hedge with market impact `[12]`; couples to (1).

### Tier 1 — close measured performance/feature gaps (published fixes)
4. **IV latency sprint** — bench Choi-seed vs FlashIV-style branch-light vs explicit-formula on your K2/K3 harness `[4][5]`; adopt FlashIV's **fixed-cost, tail-stable erfcx residual** to kill the AVX2 scalar-patch lanes (should push 1.27× toward true lane-count). Profile `erfc`, not the Householder loop, first. **~days, clear result.**
5. **Streaming surface** — adopt **Mingone box→unconstrained eSSVI reparam** `[9]` so every calibration iterate is arbitrage-free (replace fit-then-repair), targeting **~10 ms incremental refit** `[7]`. Add a **lock-free ring-buffer (Disruptor) bus** `[19]` for tick→fit→quote.
6. **Direct-conic SVI** `[8]` as the zero-iteration fast-tier single-slice fit for the quoter's per-tick refit (no seed sensitivity, global optimum).

### Tier 2 — correctness & throughput hardening
7. **AVX-512 kernel tier** `[20]` — add an ISA tier (dispatch already supports it); ~2× on eSSVI/greeks/PnL throughput-bound kernels where you already win on AVX2.
8. **Discrete-cash-dividend American pricer** — a small finite-difference/PDE pricer *specifically* for early exercise around ex-dividend dates (where ALO is only approximate); ships correctness for dividend-paying single names.
9. **Ship the AVX2 boundary batch** — currently 1.87× below your 2.0× gate; FlashIV-style branch elimination may be what gets it over.

### Tier 3 — foundational, larger bets
10. **AAD over the pricing graph** `[16]` — full portfolio greeks at ~4× one revaluation, independent of factor count. The correct long-term foundation for real-time cross-name risk at millions-of-contracts scale. Large but well-defined.
11. **Differentiable-surface quoter coupling** (Zhang 2025 architecture) `[13]` — replace hard arb projection with C¹ softplus penalties so the surface is differentiable end-to-end; enables the quote = f(surface deformation + inventory + hedge) parametrization even without the RL.
12. **(Optional) SABR module** `[22]` — for completeness / non-equity surfaces; low priority given eSSVI's arbitrage-free superiority for equity.

---

## 9. Caveats & confidence (read before acting on numbers)

- **Scope bias.** The verified evidence is strong on **published algorithms/numerics** (pricing, IV, vol-surface) and reasonable on **quoting theory** (academic optimal-MM). It is **essentially silent on firm-specific systems architecture** (AVX-512/lock-free/kernel-bypass/FPGA/colocation) and largely silent on the **engineering** of real-time cross-name risk. **Not one verified claim attributes a specific latency figure to a named firm** — those firms publish little. Every "firms do X" conclusion is inferential.
- **Self-reported benchmarks.** Schadner's ~59 ns explicit IV (independent repro found ~2.31×, not 3.4×, and it still runs ~4–5 internal iterations), FlashIV (compared only to a *Java* LBR port), and the direct-least-squares 25× / 0.087 ms (vs the author's own baseline) are single-author or self-benchmarked on varying hardware. **ns-level figures are hardware- and implementation-sensitive; treat all rankings as directional.**
- **Provenance correction (refuted `[0-3]`).** The hypothesis that `atx-vol`'s **"CStar calibration" equals the "star-calibration" reparametrization of Corbetta et al (1804.04924) was REFUTED.** That provenance is unconfirmed — the actual definition/origin of CStar (C16M modal) is an internal open question.
- **Refuted `[1-2]`.** The claim that Mingone global eSSVI is "~10× faster than sequential" did **not** survive — adopt it for the *arbitrage-free-by-construction* property, not for a specific speedup.
- **Confidence-mixed claims.** Corbetta calibration guarantee `[2-1]` (holds under 4 explicit conditions, not unconditionally); Schadner explicit-IV `[2-1]`; the eSSVI butterfly conditions used are *sufficient, not necessary*; "short-dated options cannot be aggregated" overstates practice (firms aggregate via higher-order greeks / full revaluation).
- **Time sensitivity.** FlashIV and Schadner are **2026 preprints** — recent, not peer-reviewed; numbers may shift. The ALO ~10 µs figure is a 2015-era self-report.

---

## 10. Open questions (worth their own research/benchmark passes)

1. **Firm architecture.** What concrete latency/throughput/hardware do Citadel/Jump/SIG/Jane Street/Optiver/IMC/DRW actually run for options pricing/quoting? Needs an **engineering-talk-focused pass** (Jane Street tech talks, Optiver/Jump conference decks, CppCon back-catalog, QuantLib/HPC-QuantLib maintainers).
2. **Cross-underlier risk design & throughput target.** How exactly should the millions-of-contracts real-time greek-aggregation + scenario + VaR layer be built, and what throughput must it hit? Literature gives only the modeling caveat, no systems design.
3. **IV latency head-to-head.** Can `atx-vol` close ~218 ns → ~180 ns (LBR) or lower under AVX2/AVX-512, given the "explicit" formula still hides ~4–5 iterations and FlashIV was only benched vs a Java port? **Direct in-repo bench needed** — you have the harness.
4. **CStar provenance.** True definition/origin of `atx-vol`'s CStar now that the star-calibration hypothesis is refuted, and how it relates to Mingone global eSSVI (which you should adopt for streaming).

---

## Appendix A — Source bibliography (with quality & verification)

*Quality: **primary** = peer-reviewed/canonical paper or firm talk; **secondary** = practitioner doc; **blog** = individual/company blog; **unreliable** = low-confidence.*

| # | Source | Quality | Angle | Key claim / verification |
|---|---|---|---|---|
| [1] | Andersen, Lake, Offengenden (2015/16), *High-Performance American Option Pricing*, J. Comp. Finance 20(1). SSRN 2547027 | primary | Pricing | ALO ~100k prices/s/CPU. **3-0** |
| [2] | Jäckel (2015), *Let's Be Rational*, Wilmott. jaeckel.org/LetsBeRational.pdf | primary | Pricing/IV | LBR ~180 ns, 2 iters to machine precision. **3-0** |
| [3] | Choi, Huh, Su (2023/24), ORL 57. arXiv:2302.08758 | primary | IV | Log-price Newton + tighter lower bound (atx-vol's seed). **3-0** |
| [4] | Le Floc'h & Healy (2026), *FlashIV*. arXiv:2605.29102 | primary | IV | Branch-light fixed-cost, tail-stable erfcx; sub-LBR (vs Java port). **3-0** |
| [5] | Schadner (2026), explicit IV formula. arXiv:2604.24480 | primary | IV | ~59 ns vs LBR 180 (contested; ~2.31× real). **2-1** |
| [6] | Gatheral & Jacquier (2014), *Arbitrage-free SVI*, Quant Finance 14(1). arXiv:1204.0646 | primary | Surface | SVI/SSVI arb-free foundation. **3-0** |
| [7] | Corbetta, Cohort, Laachir, Martini (2019), Decisions in Econ & Finance 42(2). arXiv:1804.04924 | primary | Surface | eSSVI slice calib + linear-interp preserves no-arb; ~0.01 s in C. **guarantee 2-1, throughput 3-0** |
| [8] | Schadner (2024), *Direct Fit for SVI*, J. Derivatives 31(3):38 | primary | Surface | Non-iterative conic-section SVI; ~25× (0.087 ms). **3-0** |
| [9] | Mingone (2022), *Global arb-free eSSVI*, Quant Finance 22(12). arXiv:2204.00312 | primary | Surface | Box reparam → unconstrained global arb-free calib. **3-0** |
| [10] | Pasquazzi (2023). arXiv:2304.02106 | primary | Surface | Restates Gatheral-Jacquier conditions + Mingone box. **3-0** |
| [11] | *An approximate solution for options MM in high dimension* (Bergault & Guéant, 2020). arXiv:2009.00907 | primary | Quoting | Hi-dim HJB, quadratic-in-inventory ansatz; short-dated ≠ first-order aggregable. **3-0** |
| [12] | Barzykin, Bergault & Guéant (2021), *Algorithmic MM in dealer markets with hedging & impact*. arXiv:2106.06974 | primary | Quoting/Hedging | Joint quote skew + optimal hedge schedule w/ impact |
| [13] | Zhang (2025), *Risk-Sensitive Option MM with Arb-Free eSSVI Surfaces*. arXiv:2510.04569 | primary | Quoting/Surface | Differentiable eSSVI inside RL loop; 5 control heads. **3-0** |
| [14] | Avellaneda & Stoikov (2008), via uditsamani.com | blog | Quoting | Reservation price + optimal spread closed forms |
| [15] | Guéant–Lehalle–Fernandez-Tapia (GLFT), via hftbacktest docs | secondary | Quoting | Closed-form bid/ask depths = half-spread + inventory skew |
| [16] | Capriotti (2011), *Fast Greeks by AAD*, J. Comp. Finance 14(3) | primary | Risk/Greeks | Full gradient ~4× one eval, factor-count-independent. **3-0** |
| [17] | Optiver tech blog, *Designing Low-Latency C++ Systems* (D. Gross) | primary | Systems | Design-level latency; branch/alloc-free; lock-free |
| [18] | *When a Microsecond Is an Eternity* (C. Cook, CppCon 2017), isocpp.org | secondary | Systems | Hot-path discipline; avoid slow path |
| [19] | Low-latency C++ techniques microbenchmark study (2023). arXiv:2309.04259 | primary | Systems | Cache-warm ~90%, atomic vs mutex 63%, SIMD 49%, Disruptor 3-orders; combined 87% + 10× determinism |
| [20] | cppforquants.com — C++26 `std::simd` | blog | Systems | AVX-512 8×f64; risk scan 50k pos 200→30 µs |
| [21] | Paradigm, *The Art of Options Market Making* | blog | Quoting | Inventory skew heuristics (long gamma/vega → lower) |
| [22] | Quantt, *Options Market Making Guide* | blog | Quoting/Risk | Aggregate book greeks; SABR+SVI+proprietary; latency low-ms/high-µs; millions quotes/s |
| [23] | *Grid-Based Full Portfolio Revaluation for VaR*, academia.edu | unreliable | Risk | Full revaluation vs delta-gamma (idea sound; specific claims unverified) |

---

*Generated by the deep-research harness (5 angles, 25 sources, 25 claims adversarially verified — 23 confirmed / 2 refuted) + `atx-vol` codebase reconnaissance. Cross-checked against in-tree numbers from the 2026-07-17 north-star sprint. Numbers are directional; run the in-repo benches (Tier-1 items) before committing to targets.*
