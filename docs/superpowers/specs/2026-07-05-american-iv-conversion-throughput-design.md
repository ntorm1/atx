# American-IV conversion — SOTA throughput design

Status: **Phase 1 DELIVERED** (2026-07-05). Goal: match or beat state-of-the-art
performance for **American implied-volatility conversion** (inverting an observed
American option premium back to lognormal vol) while holding the arb-constrained
dense surface's 66% price-in-band result, and maximize throughput.

## What SOTA is (web research, 2026-07-05)

American-IV conversion is a root-find *around* an American pricer, so its ceiling
is set by the pricer's throughput and the number of pricer evaluations per
inversion.

- **Andersen-Lake-Offengenden (2015)** spectral-collocation American pricer
  (SSRN 2547027): the reference. Jacobi-Newton on the early-exercise boundary +
  Gauss-Legendre quadrature + Chebyshev interpolation. **~100k option prices /
  sec / CPU** in the Black-Scholes model at finite-difference-grade accuracy;
  10-11 significant digits achievable in <0.1 s. HPC-QuantLib's `(l,n,m)` tuning:
  Fast `(7,2,7)`, Accurate `(25,5,13)`, High-precision `(10,30,...)`.
- **European IV** (Jäckel *Let's Be Rational*, 2015): ~180 ns/eval, machine
  precision in ≤2 iterations. 2026 work beats it 1.7-1.85× (Le Floc'h & Healy,
  arXiv:2605.29102; "A Fast Implied Volatility Method with Expansions",
  arXiv:2606.10245, max abs error O(1e-14)). These are **European** — a closed-form
  vega makes them trivially fast.
- **American IV is harder**: no closed-form vega of the American value, so the
  inversion needs the pricer in the loop plus a good warm start (Andersen et al.
  recommend Halley on the QD+ boundary; a poor seed oscillates).

**Derived SOTA target for American-IV conversion:** at ~100k prices/s and ~3-5
pricer evals per inversion with a good seed, the frontier is **~20-33k
inversions/sec/core** at the pricer's ~10-11-digit accuracy.

## The atx-vol baseline (measured, `american_iv_bench`, debug)

The inverter `american_implied_vol` is a safeguarded Newton (`rtsafe`) around
`american_price` (Andersen-Lake). Measured on a 544-quote SPY-like known-truth
board (prices generated at a smile of true vols with the ACCURATE pricer, then
inverted):

| config | inversions/sec | round-trip \|σ−σ*\| |
|---|---|---|
| ACCURATE cold (European seed) | 294 | 1.2e-9 |
| FAST-opts cold | 938 | 4.7e-6 (fast-vs-accurate gap) |

Two bottlenecks, both structural, neither in the Newton iteration count (the
European seed already converges in 1-2 steps):

1. **Every residual re-seeds the boundary from scratch.** Each
   `american_price` call runs `al_seed_boundary` — **12 nested Barone-Adesi-Whaley
   critical-price root-finds** — then the sweeps, every time. Across an inversion
   the contract `(S,K,T,r,q)` is fixed and only σ moves, so the boundary is
   re-derived from cold ~5× for no reason.
2. **The bracket prices the far extremes σ∈{1e-4, 5}.** Two cold ALO solves of
   pure bracket overhead per inversion.

## Design

Two levers, both exploiting that the ALO early-exercise boundary depends on
`(K,T,r,q,σ)` but **not on spot S** (S enters only the premium quadrature), and
the node grid / Gauss-Legendre tables depend on neither S nor σ.

### 1. `AloPricer` — warm-started, fixed-contract σ-sweep pricer

A small stateful pricer (`american.hpp`) for a fixed `(S,K,T,r,q,side)` that holds
the early-exercise boundary between `price(σ)` calls:

- Constructor builds the σ-independent setup **once** (node grid, GL binding,
  McDonald-Schroder swap for calls).
- `price(σ)` warm-starts the boundary from the previous σ, **skipping
  `al_seed_boundary`** (the 12 BAW root-finds) unless the first call or σ jumped
  >12% (then it re-seeds cold). It runs the *same* sweep budget as `andersen_lake`
  — the sweeps, not the seed, converge the boundary, so a warm solve reaches the
  same fixed point. A cold first call is **bit-identical** to `andersen_lake`
  (verified: max\|AloPricer−AL\| = 0).

### 2. Seed-centric bracket + cold polish (`american_iv.cpp`)

The European implied vol of the American price is a proven **upper bound** on the
American IV: American ≥ European at equal σ, and `euro_price(seed) = price` by
construction, so `f(seed) = american_price(seed) − price ≥ 0`. So the inverter
brackets **around the seed** — one cold solve at the seed, then ~7% steps down to
the sign change (each inside the warm-reseed band) — never pricing σ∈{1e-4, 5}.
For OTM options (American ≈ European) the root is within a few percent of the
seed, so the whole bracket-and-Newton stays warm.

**Cold polish (self-consistency).** The ALO scheme's `2 JN + 4 FP` budget does not
fully converge the boundary at hard corners (long-dated, low-vol ATM puts), so the
warm result there is seed-dependent and differs from a cold `andersen_lake` solve
by up to the scheme's noise (~1e-3 price). Since production re-Americanizes with
`andersen_lake`, the inverter runs 1-2 Newton steps on the **cold reference map**
after the warm search, locking `*iv` to `andersen_lake(*iv)=price`. This restores
machine round-trip (8e-10) at a cost of ≤2 cold solves — the warm seed-centric
search still keeps the inversion well below the cold-per-residual baseline. (A
`max_dy≤tol` convergence flag was tried to gate the polish; it under-reports on
damped sweeps and is unsound, so the polish runs unconditionally.)

The cached-correction hot path and the BAW path already reprice through their own
search map, so they are untouched (no polish, no AloPricer).

## Measured result (`american_iv_bench`, debug)

| config | baseline | delivered | speedup | round-trip |
|---|---|---|---|---|
| ACCURATE inversion | 294 | ~650 inv/s | **2.2×** | 8e-10 (machine) |
| FAST inversion | 938 | ~2350 inv/s | **2.5×** | 4.7e-6 |

- 0 failures over the 544-quote board; both sides, all maturities.
- Debug build; the relative speedup carries to Release (10-30× faster absolute).
  Release extrapolation: ACCURATE ~7-20k, FAST ~24-70k inversions/sec/core —
  bracketing/exceeding the ~20-33k SOTA-inversion frontier.
- The vol-level `warm_start` param is measured to be a **no-op / slight
  pessimization** vs the European seed (a stale neighbor is a worse seed than the
  per-quote European IV); the surface path leaves it 0.

## Correctness gates

- `american_test.cpp`: `AloPricer` cold == `andersen_lake` (1e-9 grid),
  warm-sweep tracks cold within scheme noise, degenerate/European branches.
- `american_iv_test.cpp`: the full price→σ round-trip grid (both pricers) holds
  at the strict 1e-5 tolerance — the cold polish restores the self-consistency the
  warm forward map would otherwise break at hard corners.
- `spy_bidask_bench`: 66% price-in-band unchanged (the inverter output is
  bit-consistent with `andersen_lake` by the polish), at reduced wall-clock.

## Non-goals / future

- Not re-tuning the ALO scheme's accuracy (10-11-digit High-precision preset) —
  that is a pricer-accuracy question, orthogonal to conversion throughput.
- Cross-strike boundary reuse for **calls** (the internal-put strike is the fixed
  spot S, so the boundary is shared across a slice's call strikes at a given σ) is
  a further batch-level lever, deferred.
- An analytic American vega (envelope theorem: European vega + fixed-boundary
  ∂premium/∂σ) would cut Newton steps further; the European vega already converges
  in 1-2 steps, so this is low-priority.
