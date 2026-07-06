# Arbitrage-constrained dense volatility surface — design

Status: **Phase 1 in progress** (2026-07-05). Goal: match SOTA `% within bid-ask`
on a dense, highly-parameterized index surface (SPY/SPX) — on par with Vola
Dynamics — by fitting a genuinely dense, near-interpolating **arbitrage-free**
per-slice curve, with the no-arbitrage constraints enforced *inside* the
optimizer rather than repaired after the fact.

## Why (measured motivation)

On the real cached SPY OPRA board (spot 739, ~5.7k liquid quotes), measured with
the `spy_bidask_bench` harness:

| representation | price-in-band (clean) | vwIV ×1 | vwIV ×3 | bias | ms/slice |
|---|---|---|---|---|---|
| eSSVI backbone (3 DoF/slice) | 11% | 10% | 34% | −0.011 | — |
| eSSVI + dense C2 residual, arb-safe (14 DoF, post-hoc butterfly projection) | 24% | 21% | 62% | −0.009 | — |
| **Phase 1: arb-constrained convex QP (40 ATM-clustered nodes)** | **66%** | **39%** | **50%** | **+0.001** | ~24 (debug) / ~2 (release) |

**Phase 1 DELIVERED (2026-07-05):** the convex-QP fit lifts price-in-band 11% → 66%
(6×) and vega-weighted IV-in-band 10% → 39% (4×) on the clean SPY board, ELIMINATES
the ~1-vol-pt systematic bias (−0.011 → 0), and is arbitrage-free by construction.
ATM-node clustering was the unlock (uniform-log nodes are too coarse at the money,
where vega is largest and the penny band tightest). 40 nodes is the sweet spot (56
adds nothing at 3× the cost). The residual gap to SOTA ~97-99% is the measured
24.8% raw-board butterfly-arb floor + penny-band tightness at the highest-vega ATM
strikes + a looser "clean" filter than the SOTA papers apply.

The dense-residual + *post-hoc* local butterfly projection doubled coverage but
plateaus (~24% price-in-band): a smoothed residual over-bends to chase the smile,
craters density `g(k)`, and the projection then strips the correction. The fix,
per the research (Fengler 2009; SANOS arXiv 2601.11209; Vola C-curves), is to make
no-arbitrage a **constraint of the fit**, so the optimizer finds the tightest
curve that is arb-free — never one that must be projected back out.

Two hard ceilings are already quantified and must be reported honestly:
- **24.8%** of the raw penny board is itself butterfly-arb (stale / one-sided /
  non-synchronous) — un-fittable by *any* arb-free surface. Coverage is therefore
  reported over the locally-convex **fittable** subset (the research protocol).
- The vega²-weighted, raw-penny-band metric is near-unachievable by construction
  (weight ∝ vega², band width ∝ 1/vega). Report a **band_k ∈ {1,2,3}** family and
  headline price-in-band + vega-weighted IV-in-band with a stated band.

## Key idea — butterfly no-arbitrage is LINEAR in price space

Total-variance space makes butterfly (`g(k) ≥ 0`) a nonlinear constraint → SQP,
fragile. In **call-price space** the risk-neutral density is `∂²C/∂K² ≥ 0`, i.e.
**convexity of the call price in strike is exactly the butterfly no-arb condition**
— a *linear* inequality on the fitted call prices. Monotonicity
(`-df ≤ ∂C/∂K ≤ 0`) and the price bounds are likewise linear. So the per-slice fit
is a **convex quadratic program**:

```
minimize    Σ_i w_i (C(K_i) − c_i)²  +  λ · roughness(C)
subject to  C convex in K              (butterfly)
            C non-increasing, bounded   (monotonicity / no calendar-free lunch)
```

with `c_i` the de-Americanized European call price at strike `K_i` and
`w_i ≈ vega²/spread²`. λ tunes near-interpolation (small) vs smoothing (large);
because the feasible set is exactly the arb-free cone, even λ→0 stays arb-free.

## Phases

**Phase 1 (this session) — per-slice convex-QP fit + measurement.**
- `qp_convex_smooth`: an active-set solver for the above QP built on atx-core
  `solve_spd` (each active-set iterate is an equality-constrained KKT solve).
  Unit-tested: recovers a known convex curve; output is convex & feasible;
  near-interpolates clean data; degrades gracefully on infeasible input.
- `fit_convex_slice(obs, F, T, df, λ) → ConvexSliceFit` returning `iv(k)` (invert
  the fitted convex call price back to Black-76 IV).
- Bench: measure clean-board price-in-band + vwIV(×1/×2/×3) vs eSSVI baseline and
  the dense-residual. Proves (or refutes) the SOTA path on this board.

**Phase 2 — surface assembly + calendar.** Assemble per-slice convex fits into a
surface; enforce calendar `∂w/∂T ≥ 0` (total-variance monotone in T) across
slices — either as a coupled QP or the existing sequential floor. Hot-path `iv/w`
evaluator over the dense nodes.

**Phase 3 — session integration.** A `FitPreset` (or surface-kind switch) that
routes `VolaSession` through the dense arb-constrained fit; re-Americanized parity
+ Greeks unchanged. Full `spy_real_test` acceptance at the new coverage.

**Phase 4 — performance.** The QP is per-slice and small (nodes ≈ liquid strikes);
warm-start the active set across ticks; benchmark build/query latency for the HFT
path.

## Non-goals / honesty

- Not chasing 100% — that certifies fitting arbitrage. Target is SOTA coverage
  over the *fittable* board, reported with the arb-floor and band_k family.
- Phase 1 does not touch the shipped eSSVI/session path; it is additive and
  measured side-by-side before any integration.
```
