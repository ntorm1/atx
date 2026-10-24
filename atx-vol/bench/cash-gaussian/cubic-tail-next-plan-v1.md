# Prospective cubic owner completion: exact zeros and full tails

The first representation component is frozen at09b5bb21. It supplies owned
endpoint tuples, direct evaluation and finite-cell contributions, with58
passing research controls. It does not supply a full-domain expectation or
complete Stage A. This document declares the next math seams before their
implementation. It declares no repetition matrix or option experiment.

## Exact zero coefficient proof, without a numerical tolerance

The current finite-cell derivative contraction forms
`middle=3*(v1-v0)/(b-a)-m0-m1`. Its condition rule refuses a zero sum of nonzero
terms. Consequently the valid tuple for `x^3` on `[0,1]` refuses because its
middle coefficient is exactly zero. The current refusal remains historical
evidence; no arbitrary nonzero increment will replace it.

A prospective separate proof can establish this coefficient as an exact
algebraic identity before any floating sum is formed. For positive width,
the coefficient is exactly zero if and only if

```
N = 3*(v1-v0) - (b-a)*(m0+m1) = 0.
```

Every finite binary64 number has an exact integer representation on the common
scale `2^-1074`. Decode its sign, exponent and fraction bits directly. A normal
number with exponent field E has integer magnitude
`(2^52+fraction) << (E-1)`; a subnormal has magnitude `fraction`. Zero maps to
integer zero, retaining its input bits separately if diagnostics need them.
NaN/infinity never enter this proof.

Let A/B/V0/V1/M0/M1 be these integers. The exact scaled numerator is

```
N_integer = (3*(V1-V0) << 1074) - (B-A)*(M0+M1).
```

The displayed shift denotes exact multiplication by a power of two. Actual
checked signed-integer code must shift nonnegative magnitudes and apply the
sign afterward; it must not rely on bitwise shifting a negative signed value.
Use explicit bounded temporaries, with expression-template allocation/order
behavior accounted for by the chosen fixed backend.

Each input magnitude uses fewer than2099 bits. All differences, sums, products
and final subtraction fit below4200 magnitude bits. A fixed, checked8192-bit
signed integer therefore leaves explicit margin without an unbounded integer
allocator. Installed Boost headers suffice; no dependency change is needed.
Implementation must reserve a fixed number of complete integer objects before
decoding, count actual decoded operands/shifts/products/sums and retain typed
resource/overflow refusals. Compute the bound using the actual type size,
rather than assuming its object is exactly1024 bytes.

Only an exact zero numerator permits symbolic elimination. The floating
coefficient record must identify the exact proof explicitly; it must not
claim that a floating `6/0` condition became finite. Retain the original input
bits and proof outcome so an independent rational reader can reconstruct it.
Any exact nonzero numerator, missing proof or exhausted budget leaves the
existing cancellation refusal unchanged. Other output cancellations remain
subject to the existing guard.

Required controls include exact cubic/negative cubic tuples, constants and
affine nonzero coefficients, mixed input exponents, subnormal operands,
signed zero, both input signs, resource exhaustion and checked-width limits.
The adversary `h=delta=1, m0=1e-300, m1=3` makes the100/200-digit sum round to
zero while its exact numerator is negative and nonzero; it must still refuse.
An independently constructed `Fraction.from_float` oracle can test the exact
identity, separately from native fixed-integer operation order.

This prospective proof is distinct from allowing a zero floating sum with
nonzero absolute terms. Source and contract review must approve that exact
identity distinction before the old refusal can change.

## Full-domain expectation and the affine tail

For positive stock, time and volatility, a new explicit whole-owner operation
will visit every finite cell and the infinite affine tail. It will use the
existing reviewed finite-cell weights without numerical changes. No positive
contribution is discarded by a probability cutoff. Keep both precision modes
explicit and output decimal strings.

For a tail starting at a>0 and terminal random stock X, define

```
variance = sigma^2*dt
z_k = (log(a/S) - (r-q-sigma^2/2)*dt - k*variance) / sqrt(variance)
M_k = S^k * exp((k*(r-q)+k*(k-1)*sigma^2/2-r)*dt) * survival(z_k).
```

The affine tail uses k=0/1. Its value contribution is
`v_a*M0 + m_a*(M1-a*M0)` and its interior derivative contribution is
`m_a*M1/S`. At a=0 use the complete moments directly. Use the appropriate
normal survival-function form, keeping tiny positive probabilities as decimal
values. The existing `abs(z)<=256` and exponent-domain checks remain explicit
numerical-domain restrictions. An unsupported positive tail refuses instead
of being replaced by zero. The excess `M1-a*M0` has a separate conditioning
ledger and must remain positive for a positive stochastic tail.

The whole derivative is the sum of interior derivative contributions only
because adjacent values and the tail value are exactly continuous in the
represented owner. Boundary terms then cancel mathematically. A standalone
finite-cell derivative contribution remains distinct from the derivative of
that cell's truncated price.

Use sequential per-cell scratch and bounded accumulators. Whole-call work
must add actual finite-cell and tail moment/function attempts, coefficient
construction and signed accumulation, including partial refusal. Do not count
one successful whole call as one cell. Preserve the8192-knot,65536-cell,
262144-moment and64MiB ceilings. Any optional per-cell trace must fit a separate
explicit reservation; no dense transition matrix is introduced.

Signed value/derivative sums retain actual absolute sums and the existing
precision-dependent condition guard. An all-zero sum stays valid. A zero
from nonzero unresolved terms refuses. Normal/transcendental evaluation and
rounding remain separate from any real-arithmetic truncation formula.

## Limits and deterministic transformations

Zero stock is absorbing: value is `exp(-r*dt)*f(0)`, and the supported right
derivative limit is `exp(-q*dt)*f'(0+)`. Zero time is the exact direct owner
query. Zero volatility propagates to `S*exp((r-q)*dt)` at the selected
precision, discounts the value and scales the derivative by `exp(-q*dt)`.
Never round that transformed stock to binary64 before locating/evaluating
the represented cell.

When r=q, the deterministic stock transformation is the exact identity and
retains existing exact knot semantics. For nonzero drift, a rounded exponential
that meets or becomes numerically indistinguishable from a knot cannot be
silently snapped into an exact-knot case. Retain a typed uncertain-location
refusal under the existing conditioning margin, including near-knot controls.
This is an arithmetic admission rule, not a rigorous transcendental error bound.
A strictly positive time/volatility never uses the deterministic branch.

Degree2/3 infinite tails belong only to an explicit polynomial-control type,
not the option owner. Their full moments and analytic derivatives must include
the complete tail. The control type must not let an arbitrary affine-tailed
option approximation advertise exact global quadratic/cubic behavior.

## Required gates before any matrix

First review the exact-zero proof and full-tail implementation independently.
Add constant/affine complete-partition controls, signed/nonconvex continuity,
zero/deterministic and near-zero-positive cases, both carry signs, small/large
stock, tail excess conditioning, resource/reset/alias controls and independent
full-moment identities at both100/200 digits. Primitive tests do not replace
the separately declared repeated constant/affine/quadratic/cubic matrix.

Before that matrix executes, freeze its actual models, partitions, step counts,
tolerances and complete outcome denominator. The original324 European
price/Delta controls and later18-option study remain gated afterward. Original
50 cash cases,150 PDE outcomes,32 Gaussian outcomes,18+5 diagnosis and both24
weight receipts remain unchanged. No production, reference or timing promotion
is implied by this prospective completion plan.
