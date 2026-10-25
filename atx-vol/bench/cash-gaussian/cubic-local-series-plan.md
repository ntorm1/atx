# Prospective Stage A: direct positive local weights

This declaration precedes the new numerical implementation and successor
fixed-24 export. It follows the negative raw-moment control documented in
`../../docs/cash-cubic-weight-review-20261024.md`. Keep that receipt and all
original 50/150/32/18+5 outcomes. Do not change the fixed inputs, 100/200-digit
precisions, `1e-40` comparison threshold, quadrature thresholds or any option
price budget. No production dispatch or 18-request cubic progression is part
of this component.

## Definition and stable coordinates

For a finite cell `0<a<b`, let `h=b-a`, `c=a+h/2`, `epsilon=h/(2*c)` and
`u=(x-a)/h`, `w=2*u-1`. Thus `x=c*(1+epsilon*w)` with `w` in `[-1,1]`.
Convert the original binary64 inputs exactly to the chosen multiprecision
type before forming widths, midpoints, ratios or drift. Do not form `a+b` or
the cell midpoint in binary64. Use `log1p((c-S)/S)` when its argument is well
conditioned (`abs((c-S)/S)<=1/2`), otherwise `log(c/S)`; record the route.

Set `s=sigma*sqrt(dt)`, `m=(r-q-sigma*sigma/2)*dt`, and
`z_c=(log(c/S)-m)/s`. The discounted lognormal density after transformation
to `u` is `g0*F(w)`, where

```
g0 = exp(-r*dt - z_c*z_c/2) * h / (s*c*sqrt(2*pi))
A  = 1 + z_c/s
B  = 1/(2*s*s)
L(w) = log(1+epsilon*w)
F(w) = exp(-A*L(w) - B*L(w)*L(w)).
```

This follows by dividing the density at `x` by its value at `c`: the log
Jacobian contributes `-L`, and the Gaussian exponent contributes
`-(z_c/s)*L-L*L/(2*s*s)`. The positive prefactor must remain a finite positive
multiprecision number; no tiny value is rounded to zero to obtain admission.

Using Bernstein polynomials `B_j^d(u)=choose(d,j)*u^j*(1-u)^(d-j)`, the desired
primitive weights are directly

```
W_j = g0 * integral_0^1 B_j^3(u) F(2*u-1) du,                 j=0..3
D_j = g0*(c/S) * integral_0^1 B_j^2(u)
                         (1+epsilon*(2*u-1)) F(2*u-1) du,   j=0..2.
```

Each exact integrand is positive. This formulation contains no subtraction
of nearly equal shifted raw moments and no inverse-width cubic coefficients.
The `D_j` are derivative weights, not final option Delta.

Gaussian definitions and the logarithm series are standard identities in
[DLMF 7.2](https://dlmf.nist.gov/7.2) and
[DLMF 4.6.1](https://dlmf.nist.gov/4.6.E1). The translated density formula,
specific truncation bound and algorithm below are derived here. DLMF does not
certify this implementation or its floating-point error.

## Analytic domain and deterministic truncation

The new route is limited to `epsilon<=1/16` and
`C=abs(A)*ell+B*ell*ell<=1/8`, where `ell=-log1p(-epsilon)`.
These fixed limits bound variation and coefficient conditioning; they are
not selected from a successful option-price subset. Preserve the earlier
finite-endpoint domain `abs(z)<=256` and log-prefactor limit 32768. Zero lower
edges, wide cells and unsupported tiny-time/tail domains retain a labeled
original-moment route or its original typed refusal, as specified below.

For `J>=1`, truncate the local logarithm:

```
L_J(w) = sum(k=1..J) (-1)^(k+1) epsilon^k w^k/k
Rlog   = epsilon^(J+1)/((J+1)*(1-epsilon))
E      = Rlog * (abs(A) + B*(2*ell+Rlog))
P_J(w) = -A*L_J(w) - B*L_J(w)^2.
```

Uniformly on `[-1,1]`, `abs(L)<=ell`, `abs(L-L_J)<=Rlog` and the true
exponent differs from `P_J` by at most `E`. Both exponents are bounded by
`C+E`; using the slightly looser common bound avoids relying on cancellation.
Truncate the exponential as

```
Q_N(w) = sum(n=0..N) P_J(w)^n/n!
RF = exp(C+E) * (E + (C+E)^(N+1)/(N+1)!).
```

The exponential mean-value bound controls the logarithm truncation, and the
Taylor remainder controls the second truncation. Therefore in real arithmetic
`abs(F(w)-Q_N(w))<=RF` throughout the cell. This bounds only the two series
truncations. It is not a multiprecision transcendental/rounding certificate.

Use fixed maxima `J=16`, `N=16`. Inspect J in increasing order and, for each J,
N in increasing order. Select the first pair satisfying both relative
primitive remainder checks below at
`tau=10^(-(Digits-20))`, with Digits fixed at 100 or 200. This leaves 20 decimal
digits between the arithmetic precision and the series target; it does not
change the external `1e-40` comparison gate. Evaluate scalar selection bounds
with recurrences and cached exponentials; build polynomial coefficients only
once after a pair is selected. At most 256 pair checks are permitted.

If no pair satisfies the fixed cap, report that fact and select the original
bounded moment route before polynomial construction. Do not increase J, N or
precision until a new prospective experiment is declared. A resource or
numerical failure after selecting the series returns a typed refusal; it must
not trigger an unrecorded second construction.

## Primitive and final-output error scopes

Because the integrals of cubic and quadratic Bernstein functions are `1/4`
and `1/3`, respectively,

```
abs(delta_W_j) <= g0*RF/4
abs(delta_D_j) <= g0*(c/S)*(1+epsilon)*RF/3.
```

The exact weights have lower bounds
`W_j>=g0*exp(-C)/4` and
`D_j>=g0*(c/S)*(1-epsilon)*exp(-C)/3`. Selection therefore requires

```
RF*exp(C) <= tau
RF*exp(C)*(1+epsilon)/(1-epsilon) <= tau.
```

Retain absolute bounds for all seven weights as decimal strings, as well as
the relative tests and selected J/N. The factors involving `c/S` and epsilon
cannot be omitted for derivative weights.

For a represented local cubic with Bernstein coefficients `v_0..v_3`, price
contribution is `sum(v_j*W_j)` and its differentiated integral contribution
is `sum(3*(v_(j+1)-v_j)/h * D_j)`. The resulting truncation-only estimates are

```
price_error <= sum(abs(v_j)*abs_bound_W_j)
Delta_error <= (3/h)*sum(abs(v_(j+1)-v_j)*abs_bound_D_j).
```

Coefficient reconstruction, subtraction, summation, prefactors and
transcendental rounding need additional ledgers. A narrow cell can amplify
coefficient-difference error through `3/h` even when price weights are stable.
Boundary-value cancellation in complete-price differentiation requires actual
continuity of the represented function. At a cash kink retain the two
one-sided derivatives; do not average them to manufacture continuity. These
local estimates do not bound cumulative interpolation, exercise projection,
domain truncation or an American option's total error.

## Integrate the polynomial without large binomial cancellation

Write `Q_N(w)=sum(q_k*w^k)`. Precompute only the low-degree integer coefficient
vectors `p_l` of `(1+w)^j*(1-w)^(d-j)` for d=2,3. Then

```
I(d,j,k) = integral_0^1 B_j^d(u)*(2*u-1)^k du
         = choose(d,j)/2^d *
           sum(l=0..d, k+l even) p_l/(k+l+1).
```

There are at most two nonzero rational terms for each parity. Combine their
integer numerators/denominators exactly in checked signed 64-bit arithmetic
before converting and dividing once in multiprecision. With maximum exponent
513 and d<=3, the small denominator products fit comfortably in 64 bits;
the implementation must still check rather than assume unchecked products.
Do not expand `(2*u-1)^k` in powers of u: that would introduce coefficients
growing like `2^k` and reintroduce unnecessary cancellation.

Compute W with the four degree-three integral vectors. Multiply Q by the
linear factor `(1+epsilon*w)` for D and use the three degree-two vectors.
All polynomial products follow a declared order, with owned buffers and
actual multiplication counts. No dense quadrature matrix is introduced.

The logarithm coefficient absolute sum is at most ell. Thus the exponent's
coefficient absolute sum is at most C and the exponential polynomial's at
most `exp(C)`. Since `abs(I(d,j,k))<=1/(d+1)`, the sums are well conditioned
in this admitted domain. Record actual absolute-term/signed-result ratios
for all seven integrals and require each at most 4. This conservative guard
is specific to the new route; it does not replace the old route's guard.
An unexpected negative/nonfinite weight or condition excess refuses.

## Bounded implementation and honest work

The new route is a private explicit research mode with a successor schema.
The original helper and v1 receipts remain callable and unmodified in meaning.
The fixed-24 successor records each route, admission/selection reason, selected
orders, bounds and all attempted work, including a failed selection before
the original-moment fallback. Keep all four scaled raw moments as independent
diagnostic/partition controls, with their original 4/8 moment/normal budgets.
They must not supply the seven local-series weights.

| Resource | Fixed maximum per request |
|---|---:|
| Log order J / exponential order N | 16 / 16 |
| Selection pair checks | 256 |
| Q degree / derivative degree | 512 / 513 |
| Owned polynomial coefficient buffers | four buffers, 514 entries each |
| Polynomial coefficient products | 262,144 |
| Extra local-route special-function calls | 64 |
| Extra local-route owned workspace | 1 MiB |
| Original raw-moment attempts / normal calls | 4 / 8 |

The special-function counter covers actual additional sqrt/log/log1p/exp calls
for this route and selection; original moment work remains separately counted.
Use a compile-time object-size check plus checked byte arithmetic for the 1 MiB
workspace. Temporary objects outside these buffers must also fit that declared
owned workspace. Every low caller budget returns a typed resource refusal and
retains completed work; no partial weights are published.

For naive dense exponent convolution, the worst product count is
`sum(n=1..16) (32*(n-1)+1)*33 = 127248`, before the at-most 289 products for
the log square, final linear multiplication and small integral dot products.
The fixed 262144 cap leaves room for those explicitly counted operations.
Count actual products, including products by zero if executed. Scalar
selection work is also bounded and counted rather than hidden in setup.
Allocation failure returns typed Unavailable, with no partially returned
weight object and no global cache.

## Review and validation order

First review this derivation and freeze the implementation's exact API and
resource contract. Then use runtime-red controls before numerical code:

1. Exact rational integrals for constants and cubic basis vectors, odd/even
   parity and high degree; independent direct polynomial integration at small
   degrees. Check integer overflow paths and signed-zero handling.
2. Series remainder and selected-order reconstruction using independent
   higher-precision positive quadrature, both precisions, adjacent high/low
   stock, very small positive time and admitted negative carry/rates.
3. Primitive mass and first-moment conservation, affine/cubic propagation and
   analytic derivative identities. Compare complete polynomial identities as
   well as the seven separate positive weights.
4. Admission edge, unavailable J/N, low product/function/workspace budgets,
   allocation/overflow/nonfinite failures and reset/partial-work accounting.
   No output can silently switch to a zero-time or tail-cutoff approximation.

After exact-source review, run the same fixed 12 cells at both precisions in
a new directory/schema. Require the unchanged complete24, all12 precision
and all168 quadrature gates; show both historical and successor outcomes.
Do not call this Stage A complete. The larger parent plan still requires
full-tail partitions, repeated polynomials, no-cash European/analytic Delta,
cash-kink ownership and stable exercise controls before the fixed18 option
semigroup run. No value from the five rounded crossing diagnostics can serve
as a complete-price error bound.

An alternative translated normal-interval series follows by integrating
`phi(m)*exp(-m*t-t*t/2)` on `[-h,h]`. Its exponential truncation is also
bounded, but a stable normal interval alone would not remove subsequent
raw-moment cancellation. The direct Bernstein construction above addresses
both stages. The entire error-function series is documented in
[DLMF 7.6.1](https://dlmf.nist.gov/7.6.E1); neither that source nor this
real-arithmetic remainder establishes correctly rounded transcendental calls
or a rigorous global pricing enclosure.
