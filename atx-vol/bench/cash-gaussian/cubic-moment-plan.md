# Stage A: bounded high-precision local-weight control

Declared before implementation/results. This is a control oracle for local
cubic weights, not a production price path or permission to run the 18-request
cubic semigroup study. The reviewed `cubic-plan.md` remains the progression
contract. No existing PWL arithmetic changes.

For a finite cell [a,b], use Y=X/b, A=a/b, H=(b-a)/b and u=(Y-A)/H. The four
discounted cubic Bernstein weights are

```
W_j = exp(-r*dt) E[choose(3,j) u^j (1-u)^(3-j) 1(a<X<b)].
```

The three derivative weights are

```
D_j = exp(-r*dt) E[(X/S) choose(2,j) u^j (1-u)^(2-j) 1(a<X<b)].
```

Price is the dot product of W with the four Bernstein coefficients. The
derivative integral is the dot product of D with `3*(c[j+1]-c[j])/(b-a)`.
This becomes the derivative of a complete represented price only when values
are continuous across cell boundaries, as required by the parent plan.

Compute shifted truncated lognormal moments of Y at fixed 100 or 200 decimal
digits using the installed header-only Boost.Multiprecision/Math code. Input
binary64 values are converted exactly before the scaled-cell arithmetic.
Form the degree-at-most-three polynomial weight combinations at that precision.
Serialize weights and component moments as decimal strings, retaining tiny
positive values; no conversion to binary64 is used as an admission proof.

The raw formula still has a conditioning cost: high precision is an explicit
reference-control choice, not evidence that ordinary binary64 subtraction is
stable. Record cancellation estimates, moment/special-function calls and the
chosen precision. Compare the two precisions and a separate positive-integrand
quadrature in the existing Python312/SciPy1.12.0 environment. No package is
installed and no production target links this helper.

Every positive-stock Gaussian request requires finite 0<=a<b, positive S/dt/
sigma and finite rates. Zero/deterministic cases belong to a later explicit
owner and are typed unsupported by this Gaussian-only helper. Admit finite
standardized endpoints only within [-256,256], except the exact lower-zero
limit -infinity. Outside this declared numerical domain, refuse instead of
dropping a tail. Also bound absolute log prefactors at 32768 and the special
function series budget. These are research numerical-domain limits, not model
infeasibility claims.

The first numerical export will contain the following fixed 12 cells, each at
both precisions, plus separately labeled invalid/resource controls. `next(x)`
means the next binary64 number toward positive infinity.

| ID | a | b | S | dt | sigma | r | q |
|---|---:|---:|---:|---:|---:|---:|---:|
| central | 80 | 120 | 100 | .7 | .35 | .03 | .01 |
| zero-lower | 0 | 100 | 80 | .5 | .2 | -.03 | .02 |
| adjacent-high | 1e12 | next(1e12) | 1e12 | 1 | .2 | 0 | 0 |
| adjacent-low | 1e-12 | next(1e-12) | 1e-12 | 1 | .2 | 0 | 0 |
| adjacent-unit | 1 | next(1) | 1 | 1 | .2 | 0 | 0 |
| lower-tail | 1 | 2 | 100 | 1 | .25 | .01 | .03 |
| upper-tail | 400 | 450 | 100 | 1 | .25 | .01 | .03 |
| positive-near-zero-time | 100 | next(100) | 100 | 1e-16 | .2 | 0 | 0 |
| negative-rate | 40 | 180 | 100 | 3 | .4 | -.1 | .07 |
| negative-yield | 70 | 140 | 100 | 2 | .5 | .02 | -.05 |
| small-variance | 99.99 | 100.01 | 100 | .01 | .001 | 0 | 0 |
| wide | 1 | 1000 | 100 | 5 | 1 | 0 | 0 |

For these cells, require nonnegative weights, partition-of-unity mass and
first-moment derivative identities, precision agreement and independent
positive-integrand quadrature. Retain every mismatch/refusal. Include constant,
affine and degree-2/3 coefficient identities; add separate full-domain/tail and
repeated-polynomial controls before calling Stage A complete. The 12-cell
weight study alone cannot satisfy the broader Stage A gate.

Before the first 24 weight outputs, fix the diagnostic reader thresholds:
100/200-digit results must agree to 1e-40 relative for every reported moment
and weight. Positive-integrand quadrature uses SciPy `quad` on the full local
coordinate interval [0,1], with fixed epsabs=1e-14, epsrel=3e-13 and limit=200.
Rescale by the peak log density only to avoid numerical underflow, without
dropping any interval. Compare each scaled weight with tolerance
`5e-11*abs(quadrature_value) + 8*reported_quadrature_error`; retain estimated
errors, evaluation counts and every failed comparison. The quadrature error
estimate and two-precision agreement remain empirical controls.

The helper owns bounded fixed-precision workspace and returns no partial
weights on failure. Limits are four moment attempts and eight normal-function
calls per request by default; lower caller budgets exercise partial-work
accounting. Internal special-function series exhaustion is typed numerical
refusal. Count actual attempted work and distinguish it from the fixed maximum.
The fixed special-function limits are 4096 series iterations and 256 root
iterations. Additional admission requires positive finite weight combinations,
absolute-term/signed-sum condition at most 10^(digits-30), and mass/first-moment
partition residuals at most 1e-25 relative. These arithmetic guards are not
certified transcendental error bounds or a monetary tolerance.

Boost documents correctly rounded basic arithmetic/sqrt but not transcendental
functions. Therefore agreement at two precisions and independent quadrature is
empirical validation, not a certified enclosure or full-price error bound.
[Boost cpp_bin_float documentation](https://www.boost.org/doc/libs/latest/libs/multiprecision/doc/html/boost_multiprecision/tut/floats/cpp_bin_float.html).
