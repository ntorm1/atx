# Prospective Stage A owner and full-domain controls

The reviewed fixed24 successor at a8009c93 passes its finite-cell weight gates;
the result is frozen in823bd0a1. This next component addresses representation,
analytic Delta and omitted tails. It does not run the18-option study or add
American exercise. Original primitives, receipts and production defaults stay
unchanged. Implement and review the bounded owner/math controls first.

## Preserve local increments and endpoint meaning

Stable weights do not make large absolute Bernstein controls safe. For the
affine function f(S)=S on `[1e12,nextafter(1e12,+infinity)]`, width is
0.0001220703125. Forming absolute `b1=left+width/3` rounds back to left, while
`b2=right-width/3` rounds to right. The derivative reconstructed from these
rounded controls is0 at either endpoint and1.5 at the midpoint, although the
correct slope is1. This is a mandatory failing control before implementation.

The private owner stores each cell's endpoints and endpoint value/slope tuple,
with a shared authoritative value at each knot. It represents an explicit
value offset and local increments. It never stores rounded large absolute
Bernstein controls as the source of derivative differences.

Let h=b-a, u=(x-a)/h, delta=v1-v0. In real arithmetic the represented cell is

```
f(u) = v0 + delta*(3*u^2-2*u^3)
       + h*m0*(u-2*u^2+u^3) + h*m1*(-u^2+u^3).
```

Its four local increment controls are `0,h*m0/3,delta-h*m1/3,delta`.
Store the original endpoint tuple, not a lossy `v0+increment` conversion.
Form delta and width from the exact binary64 inputs at the requested100/200
decimal precision when evaluating. The mathematical represented function is
defined by the endpoint tuple; finite-precision construction/evaluation error
is separate and must be checked. A wider exponent gap cannot be advertised as
an exact subtraction merely because multiprecision was used.

The exact weighted value contribution for the existing W0..W3 is

```
v0*mass + delta*(W2+W3) + (h/3)*(m0*W1-m1*W2).
```

For the existing D0..D2, the analytic Delta contribution is

```
m0*D0 + (3*delta/h-m0-m1)*D1 + m1*D2.
```

There is no subtraction of large absolute control points in the second formula.
Keep actual absolute-term sums and signed results for both outputs, including
the coefficient construction and final accumulation. A condition exceeding
the existing precision-dependent guard `10^(digits-30)` refuses; finite output
alone is insufficient. Require independently tested endpoint value and
one-sided derivative reconstruction. Use explicit endpoint access rather than
silently snapping an interior point to a knot.

Adjacent cells share the same authoritative endpoint value bit pattern.
Left/right derivatives may differ at a genuine kink and are stored separately.
The Gaussian derivative formula relies on continuity of the represented value:
moving-boundary value terms cancel, while derivative kinks have measure zero.
A value discontinuity is rejected; the owner does not silently discard its
distributional derivative. This is an algebraic property of the represented
tuple, distinct from rounding error in evaluating it.

## Domain, limits and tails

The stock domain is `[0,infinity)`. Own all knot/value/one-sided-slope storage.
Knots are finite, strictly increasing and start at0. Values/slopes are finite;
signed/nonconvex cells are allowed for controls. The right option tail is
affine, anchored to the final knot's shared value and outgoing slope. No
positive Gaussian contribution may be skipped by a probability cutoff.

Use explicit full/truncated lognormal moments for the affine tail. For k=0..3,
the full discounted moment is

```
S^k * exp((k*(r-q) + k*(k-1)*sigma^2/2 - r)*dt).
```

Truncated moments use the corresponding shifted normal interval; use the
appropriate survival-function form for the infinite upper tail. Subtraction
in a tail excess such as `M1-a*M0` has its own conditioning check. The existing
finite-cell numerical-domain guards remain; unsupported positive stochastic
inputs return typed refusal. An underflowed tail is not implicitly zero.

Degree2/3 tails are permitted only in an explicit polynomial-control descriptor
used to test full and repeated moments. They cannot enter an option-owner
request. This avoids falsely treating an affine-tailed approximation as an
exact global quadratic/cubic.

At S=0, the state stays0: value is `exp(-r*dt)*f(0)` and the right-derivative
limit is `exp(-q*dt)*f'(0+)` when that limit is supported. At dt=0, evaluate the
represented curve directly. At sigma=0, evaluate at `S*exp((r-q)*dt)`, discount
the value and scale the derivative by `exp(-q*dt)`. If the transformed point is
a derivative kink, retain a typed nondifferentiable result or explicit
one-sided diagnostic; do not invent a smooth Delta. A strictly positive time
or volatility must not silently use the deterministic branch.

## Bounded component and controls

This remains private research with installed Boost headers and no production
dependency change. Keep100/200-digit selection explicit. Per-call caps are
8192 owned knots,65536 visited finite cells,262144 scalar moment attempts and
64MiB reserved workspace. Each finite-cell request retains the approved local
J/N16,262144-product,64-function and1MiB limits. Reuse bounded per-cell workspace
sequentially; do not allocate a dense transition matrix. Charge tail moments,
coefficient construction, conversions and every attempted/refused operation.
All output ownership is transactional; failure supplies no partial value/Delta.

Before any owner semigroup or option run, retain runtime-red controls for:

1. Adjacent high/low/unit affine cells, at1e12,1e-12 and1, including endpoint
   and interior analytic derivatives. Also retain ordinary width, negative
   value offset and near-cancelling signed Hermite pieces.
2. Constant/affine continuity across knots with both smooth and genuine
   derivative-kink joins; deliberate value discontinuities must refuse.
   Independently differentiate the Hermite polynomial rather than calling the
   implementation's derivative helper as the oracle.
3. Full-domain partition identities for k=0..3, with both finite cells and the
   infinite tail included. Compare100/200 digits at the unchanged1e-40 relative
   gate and use independent high-precision moment formulas. Include negative
   rate/carry, near-zero positive time, large/small stock and supported tails.
4. One-step and repeated constant/affine/quadratic/cubic propagation against
   the analytic full moment and Delta. Use only the explicitly marked
   polynomial-tail control for degrees2/3. Predeclare the actual finite models,
   partitions, repetition counts and numerical tolerances before executing
   that repetition matrix; primitive algebra tests alone are insufficient.
5. Zero stock/time/volatility, near-zero positive stochastic inputs and exact
   derivative kinks. Include malformed/nonfinite inputs, unsupported overflow,
   low resource budgets, reset/partial-work accounting and owned snapshots.
6. Borrowed-grid/consumed-value aliasing: copy necessary storage before any
   replacement, including any future repeated-rollback helper.

Freeze exact component source for independent review. A later separate set of
the original324 no-cash European price/Delta controls must still pass before
the original18-option semigroup experiment. No subset, adjusted grid,
probability cutoff or epsilon knot merge is introduced by this plan.

## Limits of the result

Endpoint continuity and moment identities do not certify general option error.
Hermite reconstruction stability, tiny tail arithmetic, approximate derivatives,
cash-kink ownership and exercise ambiguity retain distinct obligations. Even a
passing full-domain primitive would not resolve continuous-exercise error or
the original uncertain DSPYp reference. No timing or HFT improvement is predicted.
