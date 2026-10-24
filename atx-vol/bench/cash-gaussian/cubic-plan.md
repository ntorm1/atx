# Prospective cubic stock-state reference experiment

Status: design frozen after the negative PWL semigroup study, before any cubic
implementation or result. This proposes a new private, default-off research
representation. It changes no production price, fit, reference, exercise clock
or accuracy policy. The original 50 inputs, 150 PDE outcomes, 32 American PWL
outcomes and 18+5 follow-up outcomes remain immutable evidence.

## Why change the representation

The [frozen follow-up](../../docs/cash-gaussian-followup-review-20261024.md)
isolates large drift without cash or exercise. At N1024/M128, repeated PWL adds
0.339374 USD to the put and 0.398951 USD to the call, while one-step Gaussian
rollback matches Black76 within 1.43e-13 USD. Repetition and spatial scaling
identify a concrete representation problem. Another M/N search or a fitted
Richardson adjustment would leave that mechanism in place.

The proposed change retains local curvature by carrying both value and first
derivative through each Gaussian rollback. It integrates local stock cubics
using moments instead of replacing every smooth continuation by chords. It
does not infer an American accuracy guarantee from polynomial degree.

## Primary-method context and boundaries of attribution

Luo and Shevchenko's version 3 uses cubic splines with Gauss-Hermite quadrature
for recursive expectations. Equations 17-18 describe spline reconstruction;
their implementation uses a log-stock grid, with a separate nodal exercise
maximum. This supports investigating a higher-order representation, but it
does not validate our liquidator-zero model, proposed stock-coordinate Hermite
variant or full-interval moment arithmetic. Its reported speed comparisons do
not transfer to this implementation. [Luo and Shevchenko, v3, 2014](https://arxiv.org/pdf/1408.6938v3).

Cosma et al. formulate recursive expectations using localized projections and
matrix operations. Their discussion emphasizes local treatment of payoff
features and discrete events. This motivates a local representation and later
exact-key operator reuse; it does not imply that the already-failed stock-PWL
implementation has the paper's convergence properties. [Cosma et al., 2016](https://access.archive-ouverte.unige.ch/access/metadata/3cd39fab-57cb-4036-8950-da5211f36c1f/download).

Itkin's version 2 instead derives coupled integral equations for value and
exercise boundary, including dividend effects. Equations 46-50 expose the
boundary/source dependence. This remains a longer-term alternative that can
avoid a dense exercise-time rollback grid, but requires independent derivation
and validation of our absorbing-zero liquidator domain and event conventions.
It is not a ready-made boundary or price oracle for these fixtures. [Itkin, v2,
January 2026](https://arxiv.org/pdf/2510.18159v2).

The proposed Hermite value/derivative method below is our design, not code or
an error theorem copied from those papers. Primary PDFs are pinned in
`calendar/cash-cubic-design-v1/primary-sources.json`: Luo/Shevchenko SHA256
`114c439f5f78db5ed1cc1997b2fd695e4f3810ad0adba6763d55827cc606a5df`;
the previously pinned Cosma and Itkin PDFs remain in `cash-dspy-axis-v1`.

## Proposed representation and Gaussian operator

Own a sequence of stock intervals. Each finite interval stores a cubic Hermite
polynomial in normalized local stock coordinates, with its endpoint values and
one-sided endpoint derivatives. Adjacent cells share an endpoint value but may
have different derivatives at a known payoff/cash/exercise kink. Option tails
remain explicit affine pieces over the entire omitted half-line, with zero
included as an exact state. General nonconvex finite pieces are supported.

Do not fit a global smooth spline across a cash or exercise kink, estimate
derivatives from a neighboring-price finite difference, impose global
convexity on post-cash puts or add a positivity clamp. Polynomial extrema and
shape diagnostics must be observable; an unsupported representation can refuse.

For `X = S exp((r-q-sigma^2/2)dt + sigma sqrt(dt) Z)`, define the discounted
truncated moments, for k=0,1,2,3,

```
M_k(a,b;S) = exp(-r dt) E[X^k 1(a < X < b)].
```

Completing the square gives a shifted normal-mass expression for each moment.
Use all intervals, including zero/infinity limits, with no probability cutoff.
For an interval expressed algebraically as `p(x)=sum c_k*x^k`, its price
contribution is `sum c_k*M_k`. Local-coordinate coefficients and moment
combinations must be evaluated in a numerically stable form; blindly expanding
small cells around a large stock into global powers can cause catastrophic
cancellation and is a named implementation risk.

For a continuous piecewise polynomial, differentiation under the expectation
gives, for S>0,

```
Delta = exp(-r dt) E[(X/S) f'(X)]
      = (1/S) sum_over_cells sum_k k*c_k*M_k.
```

The same moment evaluations therefore supply price and Delta. Continuity makes
the moving cell-boundary value terms cancel; a discontinuous represented value
requires explicit jump terms or typed refusal, not this formula. Derivative
kinks are allowed. At zero, zero time and zero volatility, implement explicit
limits with declared one-sided derivative semantics. Undefined derivatives
must be tagged/refused rather than quietly assigned a smooth value.

After each smooth rollback, build Hermite cells from the computed value and
Delta at the fixed output nodes. For exact derivatives of a C4 function, the
local Hermite remainder has the usual fourth-power spatial factor. Our inputs
and moments are approximate, tails are represented and cash/exercise functions
have kinks, so that local statement is not a global error bound.

## Stage A: primitive controls before any new owner or American pilot

Implement only a new private polynomial owner and price/Delta moment primitive.
Keep the old PWL code, results and failure policies intact. Freeze this component
and obtain independent source review before the multi-step run.

Required controls, with actual failed executions retained:

1. Constants and linear functions, plus degree-2/3 monomials and signed finite
   polynomial pieces. Compare full and truncated moments with an independent
   high-precision calculation. Global polynomial tails may be used only in
   primitive moment tests; option-owner tails remain affine. Check both one-step
   and repeated propagation against
   `S^k*exp((k*(r-q)+k*(k-1)*sigma^2/2-r)*T)` and its analytic Delta for k=0..3.
   Sum a partition's moments over the full domain and compare with these known
   full moments; price/Delta conservation cannot omit either tail.
2. Independent one-step European call/put price and Delta across the existing
   324 model controls, including tails, both carry signs and negative rates.
   Include the exact initial payoff kink rather than smoothing it.
3. Narrow intervals, large stock offsets, near-cancelling polynomial terms,
   subnormal tails and the actual A1p tail operands. Retain absolute component
   sums/conditioning diagnostics; loss of supported arithmetic returns a typed
   numerical refusal. Finite output alone is not an accuracy test.
4. Zero stock/time/volatility and strictly positive near-zero dt, including
   subnormal inputs, derivative-kink conventions, malformed intervals,
   nonfinite coefficients and overflow. A positive stochastic interval that
   cannot be resolved numerically must refuse rather than silently become a
   deterministic step. Own input/grid storage; exercise the
   consumed-value/borrowed-grid alias case that affected the prior helper.
5. Exact work and reset behavior, caller resource exhaustion and no partial
   public price/Delta on failure. Check that omitted diagnostic observers cause
   no extra traversal or allocation.

Where high-precision libraries already available in the environment are used
by a control, record their version and numerical settings. Do not add a new
production dependency or use the existing PDE/jump code as the independent
moment oracle. Stop for review if the stable moment formula cannot meet its
independent controls; do not hide cancellation behind a monetary tolerance.
Before running the 18 controls, freeze and review the actual local-coordinate
or equivalent stable basis/moment construction and its numerical-admission
criteria. Merely evaluating global-power coefficients with a compensated final
sum does not recover digits already lost inside a narrow-cell moment difference.

## Stage B: fixed multi-step and cash representation controls

After Stage A review, repeat exactly the existing 18 no-cash European requests:
DSPYp/DSPYc, N256/512/1024, M1/32/128, original fixed grid and constant T/M. Native
inputs contain no Black price, baseline price or fitted target. Compare value
and Delta with independent analytic formulas, preserving every output/refusal.

Predeclare these diagnostic progression gates:

- All 18 requests complete within their fixed budgets.
- All six one-step price errors are at most 5e-10 USD, and Delta errors at most
  5e-11, against independent formulas.
- The four repeated N1024 requests each have price error at most 1e-4 USD and
  Delta error at most 1e-5. This is a control margin, not a new production policy.
- All coarser-grid errors remain reported; no favorable subset or averaged
  value can replace a failed finest request. Record spatial and repetition axes
  without extrapolating a correction into the output.

These are prospective gates, not predicted results. Failure blocks the next
American pilot and yields another negative report without another resolution
search. Passing does not establish a general reference error bound.

Next use the existing 54 single-cash call-spread identities and both sides of
the original interior-cash European controls. In exact arithmetic,
`f(max(S-D,0))` maps stock cubics to stock cubics plus a constant plateau. Preserve
the cash kink and the derivative jump. A liquidator put can be nonconvex near D;
that is part of the model, not a defect to project away. Rounded shifted knots
and coefficient transforms require independent pointwise/moment controls and
explicit error scope; do not label their floating arithmetic exact merely
because the symbolic map is affine.

## Stable exercise is a separate required component

The five prior crossing receipts remain refused. Their tiny captured-line
chord excesses do not authorize epsilon merging, a nearest-interior knot or
automatic acceptance of a changed curve.

For the new representation, propose retaining the obstacle maximum as an
explicit operation over a polynomial cell, rather than requiring every real
intersection to become a distinct stored binary64 knot. For `p-g`, use
Bernstein/interval sign enclosures to separate cells where one branch wins.
Subdivide ambiguous cells deterministically. Tangencies, multiple roots and
endpoint/subnormal cases must remain explicit; no single-boundary assumption
is valid for a general nonconvex post-cash function.

Integrate definite branches with their polynomial moments. On a remaining root
bracket, carry upper/lower contributions derived from polynomial enclosures and
the positive transition measure. Preserve that uncertainty in the returned
value. A Delta calculation needs its own derivative/moment enclosure there:
a small price gap does not bound a derivative jump. If enclosures or resource
limits cannot resolve the requested computation, return a typed refusal.

This is a proposed geometric/integration mechanism, not an implemented proof.
It must first pass independent synthetic controls with one/two/three roots,
tangencies, exact endpoint roots, all five captured PWL cases and nonconvex cash
pieces. Rounded arithmetic and normal-function uncertainty must remain separate
from the exact-polynomial root enclosure. A local sign bound alone cannot turn
ordinary libm evaluations into a rigorous whole-price interval.

For later composition, discounting has a known positive operator norm, cash
composition and the exact maximum are nonexpansive in the value sup norm.
However, Hermite reconstruction with approximate derivatives is not automatically
nonexpansive. Any cumulative error ledger must charge its actual reconstruction
stability as well as price/Delta arithmetic and tails. Until that is established,
interval claims must remain scoped to the represented input operation.

## Bounded work and cost accounting

The first semigroup experiment keeps the original 8192-node limit,
2^24 constructed-node limit and 2^20-query limit. Record both geometric cell
visits and scalar moment evaluations; cap them at 2^28 and 2^30 respectively.
A price/Delta pair may share four moments, but both outputs and every retry
remain counted. No larger fallback budget is supplied after a refusal.

Allocate no dense global transition matrix in this first experiment. Own
workspace with a fixed 64 MiB per-request ceiling, checked before allocation.
The later obstacle component requires a separately frozen subdivision/bracket
budget before its controls run; it is not authorized to recurse without a cap.

Compared with PWL, a cubic cell requires up to four shifted mass evaluations
instead of two, stores more coefficients and supplies a second output. Reduced
representation error may permit fewer cells later, but no speedup is predicted.
Direct all-cell work remains quadratic in the fixed grid size per rollback.
If correctness succeeds, exact-key transition weights may share model/grid/dt
work across repeated rollbacks. All cold construction, storage, event changes,
refusals and destruction must then be charged. No coefficient/date quantization
or reuse of fitted/reference outputs is allowed.

## Later owner and reference qualification

Only after the independent controls and exercise component are reviewed should
a new event owner run the same original 32 American requests. Preserve event
normalization, liquidator/zero behavior, event-only call domain r>=0/q=0 and
nested put exercise times. Record all refusals, representation/rounding
diagnostics and full input hashes. Continuous-exercise discretization and tail
errors remain distinct from the polynomial integration error.

Passing the fixed pilot still cannot qualify all 44 liquidator contracts or the
six other-model cases. A later full-matrix study must keep all 50 input/model
records and every original uncertainty, compare against independently assessed
reference convergence and make no all-case claim while DSPYp or any other case
remains unresolved. No timing begins before the complete numerical denominator
and the normal Release/ThinLTO/host-quiet controls are satisfied.
