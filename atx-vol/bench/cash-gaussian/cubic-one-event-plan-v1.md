# Smallest real cash-option validation after the full-domain cubic primitive

Prospective scope only, 2026-10-24. Native source remains frozen at
`fa21d6b4e50466ce62e2e4a7941be9577298fe13`. No new numerical run,
implementation, matrix, production policy or reference replacement is included.
This is a proposed narrow amendment to the earlier Stage A ordering: one
analytically reducible American call, rather than executing or declaring complete
the old repeated-polynomial/324-European/18-option gates. Those remain pending.

## What is actually missing

The reviewed full-domain operation already integrates a continuous represented
Hermite curve and its affine tail, including its first stock derivative. It
does not construct that curve from option continuation values, project decimal
endpoints to the binary64 owner, apply a general cash/exercise maximum, or
control error accumulated by repeated reconstruction. Those are the missing
capabilities between the primitive and an actual cash price.

The next smallest useful seam is a controlled real-curve builder for one cash
event. It needs no general polynomial root solver, general option tail model,
repeated semigroup owner, nested put exercise clock, Gamma API or new moment
arithmetic. It can produce an actual American price and Delta from the existing
primitive, with independently measurable representation error. This analytical
last-segment subsolve must be explicit; it is not a completed general rollback.

## Exact model and exercise reduction

Use spot-liquidator dynamics: between dates, dS=r*S*dt+sigma*S*dW; at the single
event t_d, S_after=max(S_before-D,0). Flat r>=0, zero continuous yield, positive
volatility, 0<t_d<T, K>0, and 0<D<K. No escrowed-price adjustment is used.

No exercise is needed inside either cash-free interval. After the dividend,
the non-dividend-paying American call equals its European call. Before it,
the pre-event value is convex, dominates intrinsic, and the discounted
cash-free stock is a martingale. Jensen and r>=0 imply that waiting until the
event has value at least current intrinsic. This argument is specific to the
declared call/rate/yield/process domain; it is not applied to puts or q!=0.

Write tau=T-t_d and c(y)=Black-Scholes call(y,K,tau,r,sigma), with c(0)=0.
The exact pre-event function is

    g(x) = max((x-K)+, c(max(x-D,0))).

Restrict the first implementation to D>K*(1-exp(-r*tau)). For x>K,
F(x)=c(x-D)-(x-K) has derivative Delta_BS(x-D)-1<0. F(K)>0 and
its limit is K*(1-exp(-r*tau))-D<0, so there is one finite B>K.
Thus g is zero on [0,D], c(x-D) on (D,B), and x-K on [B,infinity).
The true upper tail is exactly affine, rather than an asymptotic truncation.
The values agree at B; the one-sided slopes are Delta_BS(B-D) and 1.

Boundary construction: bracket from [K,2K], at most 32 upper doublings, then
at most 256 bisections at the requested decimal precision. Require finite
opposite signs and a final relative width <=2^-192. Keep every bracket/sign,
function count and refusal. Numerical transcendental signs are not certified
interval signs; the independent reference brackets the same root separately.
There is no hidden retry, fitted boundary, epsilon snapping or general-root
fallback. D<=K*(1-exp(-r*tau)) is explicitly unsupported for this first builder.
The exact D>=K case is a separate analytical unit control, not silently mixed
into the requested A1c population: g(x)=(x-K)+, giving a call expiring at t_d.

## Six fixed candidate outcomes, using an existing real fixture

Select original A1c solely: S=100, K=95, T=.5, sigma=.25, r=.04, q=0,
D=2, t_d=.25-.5/301 with the original binary64 event arithmetic (stored value
0.24833887043189368). Its finite-boundary condition follows from
K*(1-exp(-r*tau))<K*r*tau<.96<2. No observed new price selects this case.

The canonical all-50 manifest is
`calendar/cash-manifest-v1/export/manifest.json`, SHA256
`def79ede5481a1bc1761fa6e7e126794f5128f9f995905ef2f7d6b1bf95d9810`.
The source fixture is `atx-vol/tests/support/ddiv_matrix_cases.hpp`, group A.
Retain all 50 inputs, mark the other 49 uncomputed, and leave all earlier 150,
32, 18+5 and 24 outcome files immutable. The old reference price is read only
by the independent reader for a secondary comparison, never by the builder.

Predeclare continuation-cell counts N=32,64,128 and both existing precisions
Decimal100/Decimal200: exactly six requested price/Delta results, including
every preparation, root, construction, resource or expectation refusal.
Use one zero cell [0,D], N continuation cells and the affine exercise tail.
No result-driven grid changes or extra refinement runs are authorized.

Choose the upper binary64 bracket endpoint B_hat whose independently evaluated
F is nonpositive. Retain the decimal bracket, projection residual, F(B_hat)
and distance to both bracket endpoints; a failure to preserve the sign refuses.
Uniform interior knots D+(B_hat-D)*j/N are formed at the selected precision
and projected once. Require strictly ordered distinct binary64 knots. Use the
exact projected knot values as the mathematical sampling inputs afterward.

At ordinary knots evaluate c(x-D) and its analytic stock Delta at the selected
precision. Project each shared endpoint value once, then reuse it in both
adjacent cells. Project each one-sided slope once. At B_hat use the common
value B_hat-K and two slopes (continuation Delta and exactly 1), thereby
preserving owner continuity; retain the discrepancy from c(B_hat-D) explicitly.
The tail uses the same shared value and slope 1. Any rounding of B_hat-K is
an explicit constant tail error, not an exact-intrinsic-tail claim.

The native builder takes only raw model fields, N and precision. A private
owned result contains the StockCubic, root record, endpoint projection records
and actual setup work. It calls no production pricing/PDE/old PWL owner or
baseline reference. A separate pilot call invokes cubic_expectation once at
(S,t_d,sigma,r,0). No second fit, full rollback, bumps or timing are involved.

## Independent full-domain comparison and separate errors

An offline Python reader computes g directly from the raw contract. It must
not reuse candidate knots, endpoint values, candidate B or represented prices
to produce the reference. Use independently implemented stable Black value
and Delta with SciPy normal functions and independently bracketed root.
Integrate the exact event continuation on the finite interval (D,B), and add
the complete analytic affine tail. No normal-probability cutoff is needed.

For the true pre-event stock density p and discounted tail moments M_k(B):

    price = exp(-r*t_d) * integral_D^B c(x-D)*p(x) dx
            + M_1(B)-K*M_0(B)
    Delta = exp(-r*t_d) * integral_D^B Delta_BS(x-D)*(x/S)*p(x) dx
            + M_1(B)/S.

Boundary terms cancel because g is continuous. Reference Delta is analytic
under the integral, not a finite difference of approximate prices. Gamma is
not returned by the current primitive; do not infer it from Delta agreement.
Its later reference would additionally require the derivative-jump term at B.

Predeclare two adaptive quadrature requests, absolute/relative tolerances
(1e-11,1e-11) and (1e-13,1e-13), each with at most 256 subdivisions per integral.
Retain both results, error estimates, actual evaluations and failures. Accept
reference stability only if price differs <=1e-9 USD and Delta <=1e-10, with
both estimates within those bounds. This is empirical numerical uncertainty,
not a rigorous enclosure. Do not retry with enlarged limits after refusal.

Root/projection and numerical ledgers remain distinct from interpolation:

1. Retain exact decimal endpoint values/slopes, binary64 bits and residuals.
   For a cell width h, endpoint perturbations imply the real-arithmetic bounds
   |dH| <= max(|ev0|,|ev1|)+(4*h/27)*(|em0|+|em1|),
   |dH'| <= (3/(2*h))*(|ev0|+|ev1|)+|em0|+|em1|.
   Count the boundary common-value adjustment in ev1. Include the tail's
   constant-value rounding separately. Discounted full-price/Delta projection
   bounds use these sup norms and the complete transition mass/first moment.
2. If the true root lies between bracket endpoints, selecting the wrong branch
   only in that interval has pointwise price discrepancy at most its width
   (|F'|<=1); its Delta discrepancy is at most the discounted first interval
   moment divided by S. Add displacement from decimal root to B_hat. These
   are mathematical conditional bounds; rounded root signs retain their own
   empirical uncertainty rather than being advertised as rigorous enclosures.
3. Require the combined root/projection diagnostic allowance <=1e-8 USD and
   <=1e-8 Delta. This is an admission threshold, not a tunable extra time value.
4. Compare the full primitive result with independent integration of the
   represented Hermite polynomials (direct basis evaluation, not native moment
   weights) and complete tail. Require <=1e-8 USD and <=1e-9 Delta agreement.
   This separates operator arithmetic from the true-curve interpolation error.
5. Report actual error against the independent exact-event reference at all
   six resolutions. The prospective working-case gate is <=1e-4 USD and
   <=1e-6 Delta, including the stated empirical reference uncertainty, at both
   finest-resolution results. Also show all coarser errors and precision drift.
   These are pilot tolerances, not a new global Greek/risk policy.

The represented polynomial can violate a shape constraint even with correct
endpoints. Report its actual extrema of value, first and second derivative,
and its distance from intrinsic on every cell; no clamp or slope adjustment.
An accuracy-pass alone does not qualify an arbitrage-free production curve.

## Small controls, work limits, and review gates

Before the six outcomes, unit controls cover independent Black value/Delta,
the D>=K reduction to a call expiring at t_d, exact zero-stock behavior,
both sides of the known exercise kink, shared endpoint identity, rounded
boundary/projection accounting, invalid domain and every setup budget. The
zero/time/volatility primitive controls already retained are not reclassified.

Keep existing full expectation budgets unchanged. New builder caps: 32 bracket
expansions, 256 bisections, 8192 knots, 65536 endpoint projections, 64 MiB
conservative owned+live-scratch reservation. Record actual root/function
attempts, knot constructions, value/slope conversions, tuple bytes and refused
work. The fixed six outcomes use no cross-request cache and no extra retries.
Final implementation must give an explicit bounded allocation/lifetime count;
this plan does not claim measured RSS or make special-function work disappear.

TDD/source-review the small builder and independent reference/reader before
executing the fixed six. Freeze their exact schemas, hashes and controls; use
fresh output paths and retain incomplete outputs on failure. Existing numerical
source stays frozen until this scope is approved. No 324/18 matrix, original
32-option replay, general put clock, Gamma implementation or latency claim is
authorized by this document.

If this case succeeds, the next genuine generalization is a controlled cash
composition plus exercise/root geometry and repeated value/Delta projection.
It remains necessary for multiple dividends and nonconvex put continuation.
The exact zero proof and successful one-event result cannot replace those gates.

## Primary-method context and model distinction

[Cosma et al., section 2.2 pp.14-16 and section 4.1 p.31](https://arxiv.org/pdf/1612.03031)
describe event-date recursion and the comparison of intrinsic at x with the
post-cash continuation at x-D. This supports the event-only call reduction;
the detailed finite-boundary proof and single-case construction above are our
derivation under the explicitly narrower model assumptions.

[Whaley, 1981, assumption (c) p.207 and boundary equation p.208](https://www.whaley.info/_files/ugd/1362e1_375c694ca97743898ad62620db78b319.pdf)
uses GBM for the stock net of an escrowed dividend. Its RGW closed-form price
therefore cannot be imported as an exact liquidator/spot-GBM reference. The
same local exercise equality does not make the pre-event transition laws equal.

[Itkin v2, January 2026, introduction and equations 46-50](https://arxiv.org/pdf/2510.18159v2)
offers a longer-term boundary/integral-equation alternative. It does not remove
the need to validate our absorbing-zero convention and event model. Implementing
that general solver is larger than the direct one-event integral proposed here.
The SSRN landing page returned 403 on this read; the cited arXiv v2 was inspected.

[Luo and Shevchenko v3, equations 17-18](https://arxiv.org/pdf/1408.6938v3)
motivate recursive cubic reconstruction. Their spline/quadrature procedure is
not this offset-Hermite implementation, and its speed/convergence results do
not qualify the current primitive or this proposed single-case pilot.
