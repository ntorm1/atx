# rev-correct-core report — verbatim

## FINDINGS

**1. HIGH — Math error (analytic derivative sign) — `src/american.cpp:97` and `:113` (mirrored in `src/simd/american_boundary_avx2_kernel.hpp:218-222`)**
`put_residual_deriv` / `call_residual_deriv` — Newton derivatives of BAW smooth-pasting residuals — carry φ-term with wrong sign. Code: `-1.0 + dq*Nm + (1.0 - dq*Nm)/q1 - dq*phim/(q1*v)`; correct derivative of `put_residual = K - Sx - pE + Sx*bit/q1` is `-1 + dq*Nm + (1-dq*Nm)/q1 + dq*phim/(q1*v)` (d(bit)/dS = +e^{-qT}φ(d1)/(S·v); Haug/BAW slope b_i agrees). Numerically verified vs central FD: K=100,T=0.5,σ=0.25,r=0.05,q=0.02,S=70 → code +0.0033 vs true −0.0982 (wrong sign); call S=130 → −0.019 vs true +0.121. Flipped sign matches FD to 8 digits everywhere tested. Consequences (simulated `newton_critical_put` exactly): safeguarded loop never satisfies Newton test, degrades to bracket bisection — **16/16 iterations exhausted vs 5-7 correct** across 7-case grid, returns silently non-converged S* off ~1e-4·K–6e-4·K (tol 1e-10·K). Impact: (a) baw_american prices shift ~1e-5 abs (inside BAW envelope, PDE-oracle test can't see); (b) **every cold AL boundary seed** (`al_seed_boundary`, 12 root-finds per solve — documented dominant cold cost) burns ~2.5x necessary iterations, slightly worse seed for truncated 2JN+2FP fast tier; (c) A6 QD+-vs-BAW study and `BoundaryHoist.SeedSpike_SweepCount` measured atop this bug. AVX2 seed kernel replicates identical wrong expression for bit-parity — fix both together (bit-pinned tests repin). Fix: `- dq*phim/(q1*v)` → `+ dq*phim/(q1*v)` (put), `- dq*phip/(q2*v)` → `+ dq*phip/(q2*v)` (call), + AVX2 mirror.

**2. MEDIUM — Missing intrinsic floor on cached hot path — `american.cpp:2164-2191` (`american_price_cached`), `:2193-2217` (blend), `:1634` (greeks price)**
Every cold path clamps served price to `max(price, intrinsic, euro, 0)` (`al_put_price_from_boundary` :1326-1343, `AloPricer::price` :1826-1835); cached path returns raw `euro + F*corr` with no floor. Deep ITM puts r>0: European leg ~rTK below intrinsic, gap carried by Chebyshev correction; in-box cached marks can dip below intrinsic by interpolation error; **outside k_log box correction clamps to edge value** → shortfall grows ~linearly with moneyness — arbitrageable sub-intrinsic marks. No test asserts cached >= intrinsic out-of-box. Fix: floor cached return at `max(intrinsic, euro)` or hard-gate on `CorrectionCache::contains()`.

**3. MEDIUM — Unbracketed Newton "cold polish" — `american_iv.cpp:444-462`**
After rtsafe converges, ALO path runs up to 2 raw Newton steps on cold pricer with B76-leg vega, no bracket: `rts -= step` guarded only by `!(rts > 0)`. Negative step (cold price < quote) has no upper clamp — rts can leave [xl,xh], exceed kSigmaHiCap (40); if second iter vega collapses, loop breaks, wild iterate returned as IV. Needs large warm-cold gap + tiny vega (gap documented up to ~1e-3 in hard corners). No test covers polish leaving bracket. Fix: clamp polished iterate into [xl,xh] (or reject step > few×tol).

**4. MEDIUM — Non-scaled no-arb tolerance European IV — `implied_vol.cpp:31-44`, `:262-269`**
`no_arb_band` accepts `pn > intr - 1e-15` absolute on price/df; intrinsic clamp `price <= band.intrinsic + 1e-15`. Index scale (F≈5000): rounding of legit at-intrinsic quote ~1e-12 → rejected OutOfRange instead of clamp-to-kIvMin; clamp band effectively zero-width. American inverter uses notional-scaled `band_tol = 1e-9*upper + 1e-12` (`american_iv.cpp:179`) — inconsistent front doors. Fix: scale both by max(F,K) (like K1 residual noise floor :287-288).

**5. LOW — FD greeks rho stencil can cross into Unsupported regime — `american.cpp:2356-2364`, `:2494-2495`**
r-stencil bumps r±1e-4 without regime guard. Put with 0<r≤1e-4 and q<r−1e-4 (plausible with q_eff<0 HTB bridge near-zero rates): r−hr lands double-continuation → NotImplemented → whole bundle fails for priceable base contract. `american_greeks_al` r−hr≤0 guard (:2627) re-routes to same failing FD. Fix: one-sided forward rho stencil when r−hr exits regime (mirror near-expiry theta treatment).

**6. LOW — IV floor discontinuity — `american_iv.cpp:186-190`, `:303-305`, `:360-362`; `types.hpp:65`**
Roots at/below bracket floor kSigmaLo=1e-4 reported as kIvMin=0.005 (50x snap); roots found inside (1e-4, 0.005) returned as-is — returns values below documented floor + jump discontinuity as quote decays toward intrinsic. σ cliff for vega-weighted fitters. Fix: same constant for bracket floor and reported floor.

**7. LOW — QD+ exponent sign contradicts own doc — `american.cpp:226-233` vs `:273-274`**
Doc "q1⁺ = q1 + c"; code `q1_plus = q1 - c` with c>0. One wrong vs Li (2010). Measurement-only (A6 seam) but A6 verdict measured atop finding 1 — re-run A/B after fixing 1.

**8. LOW — Silent non-convergence of critical-price root-finds — `american.cpp:116-177`**
`newton_critical_put/call` return last iterate on max_iter exhaustion, no flag; `baw_american` validates range only. Masks finding 1. Fix: convergence bool or `|f| < tol·K` check in baw_american.

**9. LOW — `scheme_from_opts` ignores sub-minimum quadrature — `american.cpp:548-561`**
`AlOpts{n_quadrature < 8}` falls through ladder, keeps ACCURATE n_quad_fp=24 — caller asking cheaper silently gets more expensive. Floor to 8 or reject.

**10. LOW — `TimeConvention::Calendar365` divides by 365.25-day year — `vol_time.hpp:163` (`kCalendarYearNs = 365.25*86400e9`), `vol_time.cpp:233-236`**
Name promises ACT/365, constant is Julian 365.25: T ~0.07% small, IVs ~3bp shifted vs vendors at ACT/365. Internally self-consistent; external comparisons inherit bias. Rename or re-derive deliberately.

**11. INFO — Fixed-carry correction cache consistency unenforced at kernel — `american.cpp:2164-2191`, `correction.hpp:116-127`**
`american_price_cached`/vega/greeks evaluate correction baked at (r_,q_) using query's (r,q), no check vs baked_r()/baked_q() (C2 stale-gate separate opt-in). Cached rho/theta/charm omit ∂c/∂r,∂c/∂q (fixed-carry, documented). Add debug assert on carry mismatch.

**Verified-correct (no action):** AL fixed-point kernel matches Andersen-Lake Equation B exactly; premium quadrature standard put-EEP with s=z² substitution, correct boundary indexing; tip-only Jacobi-Newton derivative standard ALO/QuantLib scheme; classify_regime matches Healy/Battauz table both sides via McDonald-Schroder; black76_greeks conventions verified analytically; american_greeks_first_order chain rule exactly consistent; Roper butterfly g + division-free w²g in cstar.cpp correct; closed-form w/w′/w″ + ∂w/∂θ gradient correct; de-Am nested tolerance ladder static_assert-enforced; closed-form PCP borrow inversion exact; Golub-Welsch + DCT-II/Clenshaw pinned by tests. Coverage strong; gaps exactly where findings sit: no test observes BAW root-find convergence, cached out-of-box intrinsic domination, IV polish bracket escape.

## ARCHITECTURE NOTE

Two-tier American pricer under Black-76 dynamics with continuous carry (discrete divs folded upstream into hybrid Klassen forward re-expressed as single q_eff). Cold reference: Andersen-Lake-Offengenden spectral collocation (`andersen_lake`): put EE boundary as Chebyshev-Lobatto interpolant in sqrt-time of y=log²(B/B∞), seeded per node from BAW critical price, few damped Jacobi-Newton + fixed-point sweeps of AL integral equation (Gauss-Legendre), premium = second GL integral + closed-form European leg; calls priced as puts via McDonald-Schroder symmetry; negative-rate regimes routed by classifier (European = exact B76, double-continuation = NotImplemented). Throughput layers reuse boundary solve: per-strike slices (strike homogeneity), σ-Chebyshev boundary interpolant for fitted boards, warm-started AloPricer for IV inversion, FD/analytic greeks bundles sharing 1-7 solves. Hot path replaces solver with B76 + precomputed 3D Chebyshev American-minus-European correction cache (correction.cpp) with analytic derivatives for cached greeks; American IV = safeguarded Newton (rtsafe) over cached/warm-ALO/BAW forward map with European seed; de-Americanization inverts OTM legs to European-equivalent vols around robust multi-pair PCP carry solve.
