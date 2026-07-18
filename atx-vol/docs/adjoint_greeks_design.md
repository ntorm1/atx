# Adjoint (AAD) American Greeks — design note (WS-P, P1)

Sprint: `2026-07-18-atx-vol-backtest-hotpath-throughput-sprint.md` §4 WS-P (P1/P2).
Status: design + first-order/second-order kernel (P2). Wave-2 wires it into
`PortfolioPricer` (P3) and the batched strike resolve (P4).

Goal: **all 8 greeks (delta, gamma, vega, theta, rho, vanna, volga, charm) of the
Andersen-Lake American pricer at ~constant cost — one forward evaluation plus one
adjoint sweep — machine-precise vs a high-quality central-difference reference**,
replacing the `american_greeks/fd_warm` cost (baseline ~682 items/s, ~1.5 ms/solve;
≥5× target). Scalar-first; SIMD is explicitly out of scope this sprint.

The kernel differentiates the American pricer w.r.t. its **direct inputs**
(S, K, T, σ, r, q). The eSSVI surface chain-rule (σ = σ_surface(k, T)) is composed
*later* (P3/P4); this note makes the **input-Jacobian shape** explicit so that
composition is a single vector-Jacobian product (§6).

---

## 1. Research basis (primary sources — cited in-code at point of use)

1. **Giles & Glasserman, "Smoking Adjoints: fast Monte Carlo Greeks", RISK, Jan 2006**
   (`people.maths.ox.ac.uk/~gilesm/files/NA-05-15.pdf`). The founding result: for a
   scalar output and many inputs, the **adjoint (reverse) sweep** computes *all* input
   sensitivities at a cost comparable to *one* forward evaluation — the opposite
   scaling to bump-and-reprice (which costs O(#inputs)). This is exactly the
   "few outputs (price), many inputs (S,K,T,σ,r,q, later the whole surface)" regime.

2. **Savine, "Modern Computational Finance: AAD and Parallel Simulations" (Wiley 2018),
   ch. 9 (Algorithmic Adjoint Differentiation).** Establishes the tape / Wengert-list
   operator-overload machinery **and** its cost: a tape must be recorded and *replayed
   (interpreted)* on every evaluation, which adds allocation + interpretation overhead
   vs a hand-coded / source-transformed adjoint. On a **fixed, static** computational
   graph (our hot path), the hand-coded adjoint wins — no tape, no per-node dispatch,
   cache-friendly. **Decision: hand-coded adjoint, not a tape** (§2).

3. **Henrard (OpenGamma), "Adjoint Algorithmic Differentiation: Calibration and the
   Implicit Function Theorem" (2011)** (`quant.opengamma.io/Adjoint-Algorithmic-
   Differentiation-OpenGamma.pdf`). The load-bearing recipe for **differentiating
   *through* a solver without differentiating the iteration.** For a value `c` defined
   implicitly by `g2(b, c) = 0`, the IFT gives `D_b g4 = -(D_c g2)^{-1} D_b g2`, and its
   **adjoint** is:

   ```
   c̄  = (D_c g3)^T · z̄                          (adjoint of the boundary)
   b̄  = -(D_b g2)^T (D_c g2)^{-T} c̄             (one transposed solve, then products)
   ```

   i.e. **solve `(D_c g2)^T λ = c̄` ONCE, then every parameter sensitivity is a cheap
   dot product `-(D_b g2)^T λ`.** Henrard measures "price + 40–70 derivatives in < 2×
   the price time." Griewank & Walther (2008) §4.6 bounds the full-gradient relative
   cost `Cost(P+D)/Cost(P) = A`, `A ∈ [3,4]` (the "cheap gradient principle").
   Predecessors: **Christianson (1998)** (adjoint of iterative solvers),
   **Giles & Pierce (2000)** (adjoint design / IFT in engineering).

4. **Battauz–De Donno–Sbuelz (2015)** / **Andersen–Lake (2021)** — the regime table
   already encoded in `detail::classify_regime` (European / Unsupported / American).
   Governs where the boundary exists at all.

5. **Deussen/Naumann (STCE/RWTH), "Fast Delta-Estimates for American Options by AAD"
   (EuroAD 2015)** and the **envelope observation** repeated across the AAD-American
   literature: *"from the viewpoint of AD the option price is independent of the
   [optimal] boundary."* At the **exact** optimal boundary, smooth-pasting makes
   `∂Price/∂boundary = 0` (the envelope theorem), so **first-order** greeks are correct
   even with the exercise decision frozen. **Trap (Trap 2 of the sprint):** the *same*
   paper warns that naively freezing the boundary makes AD **compute second-order
   greeks as zero (Γ = 0)** where the boundary depends on the differentiation variable.
   Our kernel confronts this head-on (§4).

---

## 2. Tape vs hand-coded adjoint — decision: **hand-coded**

The hot path is a *fixed* graph: Black-76 leg + Gauss-Legendre early-exercise-premium
quadrature + a boundary fixed point `R(y; θ) = 0`. There is no data-dependent control
flow whose structure changes per call (regimes are branched *once*, up front). Per
Savine ch. 9, a tape buys generality we do not need and pays interpretation +
allocation overhead we cannot afford on a per-tick path. We hand-code the adjoint of
the *closed-form* legs and use the **IFT** (not backprop-through-iterations) for the
boundary. This also keeps the kernel a **pure function** (no globals/statics
mutation) — required for live/backtest determinism (sprint §3).

---

## 3. The pricer we differentiate, and the seam we reuse

`andersen_lake` (put path) computes, for a genuine-early-exercise put (r > 0):

```
P(S,K,T,σ,r,q) = euro_put_sk(S,K,T,σ,r,q) + premium(S,K,T,σ,r,q; y*)
```

where `y*` is the converged dimensionless early-exercise boundary — the fixed point
of the spectral collocation `R(y*; K,T,σ,r,q) = 0` (node grid Chebyshev-in-√time; the
premium is a Gauss-Legendre quadrature). Crucially, **`y*` is spot-independent** — S
enters only the premium integrand and the European leg, not the boundary equation.

Reused (already-linkable) seams — `atx/vol/src/american_boundary.hpp`, namespace `amer`:
- `al_solve_put_boundary(K,T,σ,r,q, sch, bnd, ws)` → converged `bnd.y*` + bound workspace.
- `al_put_price_from_boundary(bnd, ws, S,K,T,σ,r,q)` → `P` from a *supplied* boundary
  (identical euro+premium+clamp path as a cold solve). This is our `g3` and the
  vehicle for every frozen-boundary finite difference.

**New seam added by P2 (the minimal internals exposure permitted by the ownership
rule):** a *linkable pure residual* `al_put_boundary_residual(bnd, ws, y, σ, r, R_out)`
— today the residual `R(y; σ, r)` exists only as a *private lambda* inside
`detail::al_implicit_diff_put_greeks` over the file-static kernel `eqn_b_ND_impl<0,0>`.
The adjoint needs `∂R/∂y` (the Jacobian `J`) and `∂R/∂θ`, so `R` must be callable from
the new TU. The seam mirrors that lambda verbatim (pure function of `(y, σ, r)` given
`bnd/ws`; node 0 pinned, interior nodes give `R_i = y_i - y_from_b(clamp(α N/D))`).
No behavior change to any existing entry point.

Prior art in the same file (a **forward-mode** IFT spike): `detail::
al_implicit_diff_put_greeks` (P2.4). It solves `J y_θ = -R_θ` for `θ ∈ {σ, r}` (two
solves) and propagates `y_σ, y_r` through a *moving-boundary* central difference of the
quadrature. **Our kernel is the reverse-mode (adjoint) dual of it** — see §5 for why
that is the right architecture even though for two parameters the flop counts are close.

---

## 4. Boundary IFT treatment (Trap 2 — the highest-severity correctness risk)

Two facts must both be respected:

**(a) First order — do NOT freeze the boundary for σ/r/T greeks; differentiate through
it via IFT.** The envelope theorem gives `∂P/∂y = 0` only for the *exact* continuous
optimal boundary. The Andersen-Lake scheme uses a *finite* collocation (n nodes) and a
*finite* Gauss-Legendre premium, so the discrete `∂P/∂y*` is **small but not exactly
zero** — the P2.4 spike comment confirms "the frozen-boundary/envelope shortcut is NOT
valid for the AL premium decomposition." The IFT-adjoint captures the residual exactly:

```
delta = ∂P/∂S            (∂R/∂S = 0  ⇒  no boundary term; boundary is spot-independent)
vega  = ∂P/∂σ|_y  -  λ^T R_σ
rho   = ∂P/∂r|_y  -  λ^T R_r          where   J^T λ = (∂P/∂y)^T   (ONE transposed solve)
```

If `∂P/∂y` really were machine-zero (deep in a European-optimal region), then `λ → 0`
and the greeks collapse cleanly to the frozen-boundary values — **the boundary adjoint
vanishes on its own, no special-casing** (the "European-optimal region" gate the sprint
asks for is automatic).

**(b) Second order — freezing the (σ/T-dependent) boundary gives the Γ = 0 trap.** Any
second derivative that involves a variable which *moves the boundary* (σ, r, T) must
include the boundary motion, or it comes out wrong (the EuroAD `Γ=0` failure). But note
the asymmetry that makes our life tractable:

| greek | 2nd-deriv variables | boundary moves? | treatment |
|---|---|---|---|
| **gamma** = ∂²P/∂S² | S, S | **no** (y spot-independent) | frozen-base-boundary spot stencil — *exact*, NOT the Γ=0 trap |
| **vanna** = ∂²P/∂S∂σ | S, σ | yes (σ) | mixed 2nd deriv; needs only **first-order** `y_σ` (from IFT) |
| **charm** = ∂delta/∂T | S, T | yes (T) | continuation-region **PDE identity** (no T-boundary-grid deriv) |
| **theta** = -∂P/∂T | T | yes (T) | continuation-region **PDE identity** |
| **volga** = ∂²P/∂σ² | σ, σ | yes (σ) | full 2nd deriv; needs `y_σ` **and** `y_σσ` (second-order IFT) |

- **gamma**: exact on the frozen base boundary because `∂R/∂S = 0`. This is the crucial
  distinction from the MC-LSM `Γ=0` trap: there the exercise *time* depends on the spot
  path; here the boundary is genuinely spot-independent, so freezing it is *exact*, not
  an approximation.
- **theta, charm** via the **Black-Scholes continuation-region PDE** the AL price
  satisfies: `θ = rV - (r-q)S·Δ - ½σ²S²·Γ`; `charm = ∂θ/∂S = r·Δ - (r-q)(Δ + S·Γ) -
  ½σ²(2S·Γ + S²·speed)` (speed = ∂³P/∂S³, a 5-pt spot stencil on the base boundary).
  This avoids differentiating the T-dependent node grid, is *more accurate* than a
  time-bump (no truncation), and matches `american_greeks_al` / the P2.4 spike. In the
  *exercised* region (V ≤ intrinsic) θ = charm = 0.
- **vanna**: mixed derivative — only the *first-order* boundary motion `y_σ` enters
  (`vanna = ∂²P/∂S∂σ|_y + ∂²P/∂S∂y · y_σ`), computed as a spot-difference of delta over
  the σ-moved boundary `y* ± h·y_σ`. No `y_σσ`.
- **volga** is the one greek needing the **second-order** boundary sensitivity `y_σσ`.

### Second-order strategy — **as implemented (P2): cold σ± re-solve for volga**

`volga = ∂²P/∂σ²` is computed by **two COLD boundary re-solves at σ ± h** (h = 5e-3) and
a second price difference `(P(σ+h) − 2P₀ + P(σ−h))/h²` — the proven `american_greeks_al`
route, which captures `y_σσ` exactly (each re-solve lands the full boundary at its σ).
The re-solves must be **cold**, not warm: a warm re-solve stops at a larger residual and
that residual is amplified by `1/h²` in the second difference, so volga blows up.
gamma/vanna do NOT re-solve (gamma frozen spot stencil; vanna the first-order `y_σ`
tangent), so only volga carries the two extra solves. Because a raw 2nd σ-derivative of a
budget-limited quadrature price is inherently noisy, **volga is 2nd-order-accurate only,
not machine-precise** — its parity tolerance is documented separately (§7) and gated
loosely; it is NOT claimed as a tight greek.

*(Wave-2 candidate, NOT built in P2:* a forward-over-adjoint *tangent-bundle* that gets
`y_σσ` from a second IFT solve `J y_σσ = −(R_σσ + 2R_σy·y_σ + R_yy[y_σ,y_σ])`, reusing the
factored `J` — no re-solve. It needs its own parity gate before it can replace the cold
re-solve; see §8.)

**Domain of the adjoint path — a NARROW well-converged fast-lane, not the whole grid.**
The IFT computes the derivative of the *exact fixed point* `y_fp(θ)`. The production
Andersen-Lake pricer runs a **budget-limited** sweep schedule (ACCURATE preset: 2 JN + 4
FP sweeps, early-exit at tol=1e-10), so for the majority of contracts it stops
*under-converged* — the solver output `y*(θ)` carries residual `‖R(y*)‖ ~ 1e-6…1e-4`, and
its actual derivative (what `andersen_lake`/`fd` compute) is `dy*/dθ`, **not** `dy_fp/dθ`.
Where the boundary is well-converged (`‖R(y*)‖ ≤ 1e-7`), `y* ≈ y_fp` and the IFT matches
the pricer's true derivative; elsewhere they diverge (measured up to ~20% on vega). The
kernel therefore **guards on `‖R(y0)‖ ≤ 1e-7` and only claims the well-converged subset**
(~1/12 of a representative grid, concentrated at short maturities), handing every other
point to the exact `fd` bundle. The reverse-IFT result was cross-validated to agree with
the already-validated *forward*-IFT spike `al_implicit_diff_put_greeks` to < 1e-3, so the
narrow domain is a property of the loose pricer, **not** an implementation bug. Widening
it (wave 2) requires either tightening the pricer's boundary convergence — which costs
pricing throughput and changes the mark — or adopting the well-converged IFT greek as the
canonical greek and re-baselining `fd` to match. That is a P3/P5 design decision.

**Fallbacks (kept intact).** Calls, the European-exact regime (r ≤ 0, American ==
European), degenerate T~0 / σ~0, the negative-carry / double-continuation corners, the
exercised/straddle region, an unconverged base fixed point, an ill-conditioned `J`, and
an IFT-vs-cold-re-solve vega self-consistency failure all route to the untouched
`american_greeks_fd`. P3 will add the user-facing `--force-fd` switch. The adjoint path
claims only genuine-early-exercise **puts** (r > 0) this wave — mirroring
`american_greeks_al`, which is also put-only for its native route. (Call adjoint via the
McDonald-Schroder dual-greek map is a wave-2 extension.)

---

## 5. Why adjoint (reverse) rather than the existing forward IFT spike

For the *direct* American inputs alone, forward IFT (P2.4 spike: solve `J y_σ`, `J y_r`)
and reverse IFT (this kernel: solve `J^T λ` once) have comparable flop counts — two
RHS vs one RHS plus the `∂P/∂y` assembly. The adjoint is the right architecture because
of **what comes next**:

- **Composability with the surface chain rule (P3/P4).** In the backtest the vol is not
  an input — it is `σ = σ_surface(k, T; ψ)` for eSSVI parameters ψ (5 per slice) plus
  the forward/carry curve. The adjoint produces `σ̄ ≡ ∂P/∂σ` as a *single scalar seed*;
  the surface sensitivities are then **one vector-Jacobian product** `∂P/∂ψ = σ̄ ·
  ∂σ/∂ψ` — no re-solve, no per-parameter boundary work. Forward mode would need one
  boundary-tangent per ψ. This is precisely the Giles–Glasserman "few outputs, many
  inputs" argument, and it is why the *portfolio* greeks (P3, book-wide) scale.
- **Constant cost in the number of upstream inputs** — the merge-gate promise
  ("all 8 greeks ~constant cost, one adjoint sweep").

---

## 6. Input-Jacobian shape (the contract P3/P4 compose with)

The kernel's first-order output is the **price gradient** in direct-input space:

```
g = ∇P = [ ∂P/∂S,  ∂P/∂K,  ∂P/∂T,  ∂P/∂σ,  ∂P/∂r,  ∂P/∂q ]   (row vector)
```

exposed through `AmericanGreeks` (delta = g_S, vega = g_σ, rho = g_r, theta = -g_T via
PDE) and, for the surface composition, the **adjoint seed** `σ̄ = ∂P/∂σ` (= vega).
The second-order block exposes the Hessian rows the 8 greeks need
(`H_SS = gamma`, `H_Sσ = vanna`, `H_σσ = volga`, `H_ST → charm` via PDE).

Surface chain rule (P3/P4, not this wave):
```
σ = σ_surface(k=ln(K/F), T; ψ)        F = S·e^{(r-q)T}
∂P/∂ψ   = σ̄ · ∂σ/∂ψ                                        (VJP, adjoint seed = vega)
∂P/∂S|_total = delta + σ̄ · ∂σ/∂k · ∂k/∂S  (+ forward/carry terms)   (surface-aware delta)
```
The kernel returns the **direct-input** Jacobian; the surface Jacobian `∂σ/∂ψ`,
`∂σ/∂k`, `∂k/∂S` is owned by the surface layer and multiplied in at P3/P4. Keeping the
seam at `σ̄` means the American adjoint never needs to know the surface parameterization.

---

## 7. Parity gate & tolerances (target: machine-precise vs the FD convergence plateau)

Reference: high-quality **central differences** of the *same* priced function
(`andersen_lake` / `al_put_price_from_boundary`), Richardson-extrapolated where a single
step cannot reach the plateau, **plus** an independent cross-check vs the already-validated
*forward*-IFT spike `al_implicit_diff_put_greeks`. Grid: moneyness × maturity × vol ×
rate/borrow sign flips (**negative borrow**), deep ITM/OTM, near-expiry, near the exercise
boundary, and European-optimal regions.

Gating is **per-region**, because the achievable accuracy is not uniform:

| greek | full grid | adjoint-path subset (‖R(y0)‖≤1e-7) | reference |
|---|---|---|---|
| price | ≤ 1e-9 | — | andersen_lake |
| delta, gamma | **BIT-IDENTICAL to fd** (incl. neg borrow) | — | fd (same spot-independent boundary + stencil) |
| vega, rho | (mostly fallback→fd) | **≤ 5e-3 vs Richardson AND vs the forward-IFT spike** | Richardson / spike |
| vanna | — | 2nd-order plateau (~5e-2 vs fd) | fd |
| volga | — | **2nd-order only, loose (~2·|·|); NOT machine-precise** | Richardson |
| theta, charm | ~1–4% vs fd (PDE identity, near-expiry ITM worst) | — | fd |

Because the IFT-adjoint claims only the **well-converged subset** (§4), the tight vega/rho
gate runs only on `took_adjoint_path == true` points (a `bool*` out-param the test asserts,
so a silent fd fallback cannot make the parity vacuous — the reviewer's ⚠️-1). The material
`−λ^T R_σ` correction is separately pinned where the American vega departs from the European
vega, so a wrong-sign/scaled correction cannot hide. The existing `american_greeks_fd` path
stays the untouched fallback everywhere the adjoint does not claim (Trap 2 discipline).

**"Machine-precise" is scoped, not blanket:** first-order vega/rho are machine-precise vs
Richardson *on the well-converged adjoint subset*; delta/gamma are bit-identical to fd
*everywhere*; volga and (near-expiry) theta/charm are only 2nd-order/PDE-accurate.

---

## 8. Implementation status & the throughput path (P2 → wave 2)

**Landed (P2):** correctness. delta/gamma are bit-identical to `american_greeks_fd`
*everywhere*; vega/rho are machine-precise vs Richardson **and** vs the validated
forward-IFT spike **on the well-converged adjoint subset**; robust guards
(exercise-region/straddle, unconverged fixed point `‖R(y0)‖>1e-7`, ill-conditioned J,
IFT-vs-re-solve self-consistency) hand every other point — including the narrow
under-converged majority and the neg-carry corners — to the FD bundle. The domain is
narrow by design (§4): the IFT is only consistent with the loose pricer where the loose
boundary is well-converged.

**NOT yet landed (the ≥5× lever — wave 2):** *the current kernel builds the boundary
Jacobian `J = ∂R/∂y` and `∂P/∂y` by FINITE DIFFERENCES* (~2m residual + 2m price
evals, m≈11 on the ACCURATE preset) and computes volga by 2 cold σ± re-solves. An
informal rel-avx2 best-of-3 measured **0.81× vs fd_warm on the full 8-greek bundle**
(618 vs 764 items/s) — the FD-Jacobian assembly costs more than the 4 warm boundary
re-solves it replaces. This is a correct-but-unoptimized prototype: it is *not yet*
the "one forward + one adjoint sweep" the target assumes.

The ≥5× requires replacing the 44 FD evals with a **hand-coded analytic reverse
sweep**:
- **Analytic `∂P/∂y`**: differentiate the Gauss-Legendre premium quadrature w.r.t.
  the boundary nodes in closed form (the integrand's `al_boundary_at` barycentric
  weights give `∂b(u)/∂y_j` analytically). One reverse pass over the quadrature.
- **Analytic `J = ∂R/∂y`**: the residual's `N,D` integrals depend on the boundary at
  all nodes through the same barycentric interpolation; `eqn_b_NDd` already gives the
  self-term `∂N/∂b_i, ∂D/∂b_i`, and the cross-node coupling is the interpolation
  weight matrix — a closed-form dense `J` with no residual re-evaluation.
- **Skip volga on the first-order-only path** (P4 strike-resolve / hedge delta+vega):
  no cold re-solves; pure IFT.

With those, the adjoint cost collapses to ~1 forward price + 1 reverse sweep + one
`m×m` solve — the Giles–Glasserman constant-cost regime. This is a wave-2 task
(P3 wires it into `PortfolioPricer`; P5 measures it against the WS-M baseline). The
**architecture is already correct** for it: the surface chain-rule composes through
the single adjoint seed `σ̄` (§6) regardless of how `J`/`∂P/∂y` are computed.

### Wave-2 ≥5× preface (reviewer guidance — gate ownership)

Two wave-2 gates MUST precede P3 wiring, and the PM holds the ≥5× decision:

1. **The analytic `∂R/∂y` cross-node Jacobian is NEW numerical code** (only the diagonal
   `eqn_b_NDd` self-term exists today). A bug there re-opens the exact Trap-2 boundary-
   differentiation risk this wave closed. It **must ship with its own parity gate** (vs the
   FD Jacobian this wave used, and vs the forward-IFT spike) before any consumer trusts it.
2. **Analytic volga (the tangent-bundle `y_σσ`)** likewise needs its own parity gate before
   it replaces the cold re-solve; volga is the noisiest greek and is currently loose.
3. **The ≥5× gate is scoped, not blanket.** The first-order **delta+vega hedge/resolve
   path** (no volga, re-solve-free) is the credible ≥5× target and the real backtest hot
   cost; scalar-only, the realistic win is ~2.5–4× (Giles/Griewank bound the full gradient
   at 3–4× one price vs `fd_warm`'s ~7 solve-equivalents), reaching 5× only if the reverse
   sweep is genuinely ~1× price. The **full 8-greek bundle** keeps volga's 2 cold re-solves
   unless the analytic `y_σσ` lands, so "all 8 greeks ≥5×" is optimistic; the PM should
   decide between a first-order-scoped ≥5× gate and a looser full-bundle target once P5 has
   the WS-M baseline. Do **not** treat the P2 0.81× as the throughput verdict — it is a
   disclosed prototype and the FD Jacobian is the known bottleneck.

Note the domain narrowness (§4) is a *separate* wave-2 lever from the FD-Jacobian cost:
even a fully analytic sweep only fires where the loose boundary is well-converged unless
the pricer's convergence is also revisited.
