# Prospective full-domain cubic expectation interface

Status: plan only, based on merged main b422fe45da3beae6eba31872ff9da06f595b7b64.
The previous dyadic-zero branch remains frozen at 8a722fe3. This follows
[cubic-tail-next-plan-v1.md](cubic-tail-next-plan-v1.md) and the endpoint
projection warning there. No new matrix, numerical result or production
dispatch is declared. Root review is required before implementation.

## Component boundary and owned results

Add a private full-owner expectation seam under bench/cash-gaussian. Keep the
existing StockCubic endpoint tuples, finite-cell contribution API, local weights
and exact dyadic identity semantics unchanged. The owner remains affine-tailed;
it does not silently become a global cubic or an American rollback engine.

Proposed interface, with declarations in a new cubic_expectation.hpp:

- CubicExpectationRequest: finite binary64 stock, dt, sigma, rate and yield;
  stock, dt and sigma are nonnegative. No interval bounds: the owner defines
  the complete partition.
- CubicExpectationBudget: the existing CubicOwnerBudget plus the cumulative
  work limits below. The precision remains an explicit WeightPrecision
  argument, with only Decimal100 and Decimal200 accepted.
- CubicExpectation: owned decimal strings value, left_delta and right_delta,
  CubicSlopeKind, global value/delta absolute-term sums, and maximum condition.
  A genuinely smoothed positive-stock stochastic result has equal left/right
  Delta strings. At stock zero the left string is empty and RightLimit is
  explicit. A deterministic derivative kink returns both sides and Kink,
  rather than inventing a smooth Delta.
- CubicExpectationWork: reset on entry; route, finite-cell attempts/completions,
  tail attempts/completions, aggregate existing weight and dyadic counters,
  owner conversion/coefficient/accumulation counts, work reservations and the
  refused phase/index. It also retains a single bounded last-cell work record
  and a tail ledger. No list of all cells is retained by default.
- Result<CubicExpectation> cubic_expectation(const StockCubic&,
  const CubicExpectationRequest&, WeightPrecision,
  const CubicExpectationBudget&, CubicExpectationWork&).

The input owner is borrowed synchronously and never mutated. The result owns
its strings. No span or callback escapes. Refusal yields no partial price or
Delta; completed and refused work remains in the caller-owned work record.
Invalid arguments, unavailable budget/allocation and numerical-domain failures
retain distinct existing ErrorCode categories.

A successful result describes the expectation of the represented curve. It is
not a general option-price error bound or an American exercise certificate.

## Positive stochastic route: every finite cell and the infinite tail

For stock S>0, dt>0 and sigma>0, visit finite cells in stored order, then the
tail exactly once. Use the unchanged finite-cell evaluator with its bounds
taken from the owner. Its returned decimal contribution is parsed at the same
selected precision and added to the full-curve accumulator; this conversion is
counted. Do not replace the reviewed contraction by rounded absolute Bernstein
controls or silently change its decimal boundary.

Let a be the tail's lower endpoint, with authoritative endpoint value v and
outgoing slope m. For a>0, k=0,1 and variance V=sigma^2*dt, use

    z_k = [log(a/S) - (r-q-sigma^2/2)*dt - k*V] / sqrt(V)
    M_k = S^k exp([k(r-q)+k(k-1)sigma^2/2-r]*dt) Q(z_k)

where Q(z)=erfc(z/sqrt(2))/2. M_k is a discounted moment over X>=a.
This direct survival expression avoids subtracting a tiny positive tail from
one. No probability cutoff is introduced. For a=0, use the complete moments
directly, with zero normal-function evaluations.

The tail contribution is

    value_tail = v*M_0 + m*E,       E=M_1-a*M_0
    delta_tail = m*M_1/S.

Compute E at the selected precision with its own signed result, absolute-term
sum and condition ratio. For positive stochastic inputs and a>0, E must be
strictly positive; a rounded zero, negative or ill-conditioned result refuses.
At a=0, E=M_1 algebraically and no cancelling difference is manufactured.
Retain both M_k, their standardized thresholds/prefactor exponents and E in
the bounded tail ledger.

The tail uses the existing abs(z)<=256 and abs(prefactor exponent)<=32768
numerical domain. For a=0 the threshold restriction is inapplicable. Validate
all intermediate results; underflow to zero is a refusal for an expected
positive moment. Special-function convergence exceptions and overflow refuse.
These are explicit supported-domain restrictions, not rigorous error bounds.

Continuity of authoritative values across every cell and the tail is already
a StockCubic invariant. Consequently moving-boundary terms cancel in the sum
of the interior derivative contributions. One-sided slope jumps are permitted
and have zero mass under a positive lognormal density. This argument would
not hold for a discontinuous value owner, which remains rejected.

Global signed accumulation retains the sum of all underlying absolute terms,
not merely abs(each net cell contribution). Combine each returned cell's
value_absolute_sum/delta_absolute_sum and the tail's corresponding ledger.
A second cancellation among net cell contributions is therefore visible.
The condition limit remains 10^(digits-30). An exact zero with zero absolute
sum is valid; a zero with a nonzero unresolved absolute sum refuses.
Only the existing input-bound exact dyadic middle-coefficient proof can remove
its particular algebraic zero; there is no new general cancellation exemption.

## Empty all-affine curve and actual limits

An empty finite-cell vector with tail.lower=0 is a first-class valid owner:
f(x)=v+m*x for the whole half-line. Its full expectation is
v*exp(-r*dt)+m*S*exp(-q*dt), and Delta=m*exp(-q*dt).
This follows the same two full-tail moments for positive stochastic inputs.
It does not require dummy finite cells, a sigma threshold or a special
approximately-affine detector. All-zero and negative-affine controls remain
subject to the same signed-sum policy.

Dispatch after validating the full request, in this order:

1. dt=0: preserve direct owner evaluation and exact endpoint semantics.
2. S=0: value=exp(-r*dt)*f(0); right Delta=exp(-q*dt)*f'(0+).
3. sigma=0: propagate X=S*exp((r-q)*dt) at the selected precision, evaluate
   the owner there, discount value and multiply each derivative by exp(-q*dt).
4. Otherwise use the positive stochastic route above, however small dt/sigma.

The deterministic route must not convert X to binary64 before locating a cell.
Introduce only a private selected-precision point helper, preserving the existing
binary64 direct API's output/arithmetic where it is called today. If r=q, the
transformation is an exact identity and existing knot semantics apply.
For nonzero drift, equality with a knot after rounded exponential evaluation,
or a location difference whose ratio (abs(X)+abs(knot))/abs(X-knot) exceeds the
unchanged condition limit, is a typed uncertain-location refusal. No epsilon
snap or derivative-side guess is allowed. Exact-zero all-zero location handling
must not bypass this nonzero-drift uncertainty rule.

A positive stochastic request can remain outside the existing finite-cell/tail
numeric domain when variance is extremely small. Retain that typed refusal,
not a deterministic substitution. The empty all-affine owner supplies a
supported near-zero-positive control without finite-tail threshold division.

## Cumulative work and sequential storage

Retain hard ceilings of 8192 owned knots, 65536 attempted finite-cell visits,
262144 scalar moment attempts and 64 MiB conservative workspace. A single owner
currently has at most 8191 finite cells; no larger owner or dense transition
matrix is added. These limits apply to a whole call, not anew to every cell.

Additional whole-call caps, declared before tests, are:

| Count | Default/hard cap |
|---|---:|
| weight selection checks | 262144 |
| local-series coefficient products | 262144 |
| local-series function attempts | 262144 |
| normal-function attempts | 524288 |
| owner conversions | 1048576 |
| owner coefficient terms | 524288 |
| owner accumulation terms | 524288 |
| deterministic/tail elementary function attempts | 64 |
| tail moment attempts | 2 |

Every per-cell invocation receives the lesser of its existing per-cell cap and
the remaining whole-call allowance. Existing per-cell J/N<=16, product/function
limits and 1 MiB workspace remain unchanged. Sum actual work using checked
integer arithmetic, including partial refused work. The exact proof retains
its per-call fixed caps (six decodes, thirteen shifts, eight sums, five products,
two masks, 32 scans); aggregate every proof attempt, including optional-proof
refusals that fall through to the original coefficient calculation.

For owner conversion/term counters, reserve a conservative fixed per-cell
allowance before invoking the existing complete evaluator, then debit its actual
counts and release the unused allowance. The initial design allowance is
128 conversions, 16 coefficient terms and 16 accumulation terms per finite-cell
entry, derived conservatively from the existing source; verify it against the
frozen implementation in unit controls. The tail has the same allowance.
Insufficient reservation refuses before that component starts. Counter meanings
remain logical owner conversions/terms, not every internal machine operation
or every special-function iteration.

Reserve owner storage plus 256 selected-precision scalar objects and 16384 bytes
of bounded global string/ledger scratch, then add the maximum sequential cell,
exact-proof or tail scratch reservation. Cell and proof numeric workspaces are
not summed as if live concurrently after the existing cell helper returns.
The last-cell diagnostic/string record and aggregate ledger remain live and
are charged separately. Compute requirements using actual sizeof types and
checked arithmetic; caller and hard caps must pass before local arithmetic.
Allocator metadata/resident memory are not claimed as measured. The exact
source review must verify the reservation against the actual chosen call
structure before treating these counts as established.

A new result/work record is not silently cumulative across separate calls:
each call resets. Any later repeated propagation owner must own and charge its
aggregate budget explicitly. No repeated owner is introduced in this seam.

## Endpoint projection is a separate operation

The full expectation returns decimal strings only. It neither constructs nor
replaces a StockCubic owner and performs zero decimal-to-binary64 endpoint
projections. Report endpoint_projection_count=0 as an explicit current contract.

A future rebuilding step must record each exact decimal value/Delta, its rounded
binary64 tuple, conversion residual and actual reconstructions. Shared endpoint
values must be projected once and reused bit-identically; one-sided slopes
retain ownership at a kink. Rounding an expectation endpoint is an additional
representation error, not an integration error and not resolved by agreement
between the two precisions. The full semigroup must be compared with independent
analytic propagation after these real projections. A high-precision owner, if
needed, requires its own type and resource review; none is implied here.

## TDD controls and review boundary

Before implementation, retain refusal-stub runtime red evidence for the new API.
Keep the existing 65 component controls and original negative receipts intact.
Add bounded unit controls at both precisions:

- Empty all-affine constant, affine, negative-affine and all-zero owners,
  checked against independent full discounted moments and analytic Delta.
- Partitioned constant/affine owners with ordinary cells, signed offsets and
  adjacent high/low/unit endpoints; every cell and the tail appears in work.
- A genuine continuous kink: piecewise-affine positive-part payoff with its
  analytic Black value/Delta, plus exact deterministic left/right slopes.
- The finite cubic x^3 on [0,1] with affine continuation 1+3*(x-1) above 1.
  Compare to independently expanded truncated Gaussian moments. This is not
  mislabeled a global cubic; the dyadic-zero proof is charged.
- Tail a=0, ordinary positive tail and extreme supported positive tail;
  no omitted contribution. Unsupported threshold/overflow, rounded tail excess,
  and cancelling global outputs refuse with retained work.
- Zero stock/time/volatility, exact r=q identities, nonzero-drift near-knot
  uncertainty, and strictly positive tiny variance with no deterministic route.
- Low global/per-cell limits, whole-call exhaustion after a completed cell,
  scalar-reservation refusal before arithmetic, counter overflow protection,
  work reset and transactional unchanged input ownership.

Independent test formulas must not call the implementation's moment or
contraction helper. Use separately expressed high-precision Gaussian integrals
and full-moment/Black identities, including both carry signs. Compare supported
100/200-digit results at the unchanged 1e-40 relative gate with a declared
absolute treatment only for analytically exact zero, not a general tolerance
escape. No claim of a rigorous transcendental/rounding or full-price bound.

Global quadratic/cubic tails still require the separately declared explicit
polynomial-control descriptor before the repeated polynomial matrix. The affine
owner cannot pretend its extrapolation is a global cubic. This seam does not
run that matrix, the 324 European controls, the 18-option study, an option
rollback, a PDE comparison or a latency experiment. Freeze numerical source,
focused controls and owning PCH-off evidence for root and peer review first.

