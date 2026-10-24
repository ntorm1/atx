# Single-cash call curve builder: component checkpoint

This implements the real-curve builder in the parent-approved prospective
`cubic-one-event-plan-v1.md` (original evidence file SHA256
`b7e7c2928e903722246d2ca0b1463f04750a6821c9fb6f028e9b4c79d993f590`).
It remains in the existing default-OFF cash-Gaussian research target. The
independent reference/reader and six A1c outcomes have not run. No production
pricing, generic rollback, full-price accuracy, Gamma or timing claim follows.

The private builder accepts one interior spot-liquidator dividend, positive
volatility, nonnegative flat rate and exactly zero continuous yield. It owns
all endpoint tuples and diagnostics. For D<K it requires a finite exercise
boundary, uses a bounded decimal bisection and constructs the continuation
Hermite cells with shared rounded endpoint values, one-sided boundary slopes
and the complete affine exercise tail. D>=K is the exact analytical
exercise-at-dividend control; it does not evaluate a post-event Black price.

The true continuation value and its analytic Delta are retained separately
from the common intrinsic boundary value. Each decimal-to-binary64 projection
retains its decimal input, actual output bits through the double field and
signed residual. Ordinary left/right slopes share one projection. Root
records live in caller-owned work, so completed brackets survive a later
numerical/resource refusal. Failure returns no partially usable curve.

The approximation allowances separate endpoint projection from root placement
and from the still-unmeasured Hermite interpolation error. In the root-region
Delta decomposition, the moment allowance bounds the interior slope mismatch
jointly with the endpoint continuity correction. A hypothetical discontinuous
branch switch has boundary terms; its interior-only integral is not claimed
as its standalone Delta. The actual final owner is continuous, and its full
expectation therefore uses the reviewed cancellation of boundary terms.
These are real-arithmetic conditional/empirical diagnostics; decimal root signs
and special functions are not promoted to rigorous interval proofs.

## Bounded work and controls

The existing primitive, owner and exact-zero implementation are unchanged.
The builder preflights at most 8192 endpoints, 65536 projections and 64 MiB
of conservative owned/live storage. Its reservation includes two simultaneous
cell arrays during owner construction, every endpoint and root-record string,
and simultaneous builder/owner scalar scratch. Allocator metadata and RSS are
not measured. The owner retains its separate, possibly smaller caller budget.

Actual root attempts are capped at32 expansions and256 bisections. There are
at most290 saved brackets and8490 Black evaluations. The latter implies a
bound on normal/elementary requests; the work fields count their actual logical
calls, not special-function internal iterations. Completed projected endpoints,
projection attempts and actual partial root/function work remain visible on
failure. No global or cross-request cache exists.

Two unavailable-stub controls failed at runtime before implementation. A later
valid D>=K control with large irrelevant post-event rate-time failed before
the analytical route stopped evaluating those irrelevant Black parameters.
Both RED receipts remain retained. The final eight focused controls cover:

- Owned continuous finite-boundary construction at both precisions.
- The exact D>=K Black price/Delta reduction at both precisions.
- Actual continuation values/slopes and all recorded projection residuals.
- Exercise-boundary one-sided slopes and absorbing-zero value/right Delta.
- Unsupported domains and reset of previous work.
- Low knot/workspace/projection budgets before local arithmetic.
- Partial bisection records and paired actual function counts on refusal.
- The analytical route's irrelevant-parameter independence.

The unchanged85 controls plus eight new controls pass:93 total. The synthetic
component model is different from A1c; no requested A1c result or broad matrix
was executed. The direct continuation identity test compares the formula at
independent higher precision; it does not replace the forthcoming independent
event-payoff quadrature/reference implementation.

The two new owning translation units also pass PCH-off before this freeze.
Evidence is under `calendar/single-cash-call-component-v1/` and its associated
`calendar/single-cash-call-*.log/.xml` files in the r7 evidence directory.
