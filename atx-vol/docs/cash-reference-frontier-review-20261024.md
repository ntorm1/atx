# Cash-dividend accuracy and performance: next qualification work

The repository already contains a substantial dated-cash implementation. D0
provides event-aligned PDE/obstacle and trinomial references; D1 corrects the
linear splice's accumulating bias, adds event-aware production routes, and
prepares allocation-free price, inversion and Greek batches. Continue from those
implementations. The older statement that every cash price comes only from the
301-step linear splice describes the pre-D1 baseline.

The outstanding issue is an equal-model, equal-accuracy performance frontier.
The Standard route's small IV-basis-point errors do not imply a $0.001 price
error. For example, the existing D1 twelve-event put/call results have Standard
price errors around $0.0112/$0.0115, while Refined errors are approximately
$0.0000443/$0.0000652. Neither a historical speed nor an IV-error column can
substitute for the user's monetary-price acceptance rule. These are examples
from the existing matrix, not new measurements.

See [the complete D0/D1 report](american-cash-dividend-reference.md), especially
sections 11.4–11.9, and [the HFT support matrix](hft-support-matrix.md).

## Finding: reference uncertainty is empirical

`ddiv_ref::richardson_axis` calls a three-level sequence asymptotic when the last
differences have an admissible ratio. Its `uncertainty` is the absolute extrapolation
correction, or a difference envelope when that ratio fails. The source currently
describes this as a conservative bound. That description is stronger than the
implemented evidence: an observed order alone does not establish an asymptotic
error expansion or bound its remainder. Richardson error estimates depend on
being in the asymptotic range. [Phillips and Roy's uncertainty study](https://doi.org/10.1115/1.4027353).

A direct counterexample to the generic claim is
`v(h) = h^2 + 100*(h^2-1)*(h^2-1/4)*(h^2-1/16)`.
At `h = 1, 1/2, 1/4`, its values are `1, 1/4, 1/16`. The implemented rule observes
order two, extrapolates to zero, and reports uncertainty `1/16`. The actual limit
is `-25/16`. This does not demonstrate a cash-pricing error; it demonstrates why
three observed levels cannot be called a mathematical error bound. Preserve
historical `RESOLVED` results as results under their declared empirical rule,
with their raw refinements and independent checks. Do not turn them into a
rigorous or whole-domain certificate.

Before a new frontier claim, retain separate time, space, domain, interpolation
and event-alignment evidence. A second discretization sharing the same jump
operator cannot independently expose a defect in that operator. Existing exact
one-event quadratures and primitive tests are valuable controls; retain them.

## Required experiment order

1. Freeze a machine-readable copy of all 50 D0/D1 contracts, including original
   reference values, uncertainty indicators, model conventions and refusal labels.
   Re-evaluate Standard and Refined on the current source. Export unrounded prices,
   IV residuals, route decisions, event counts and per-field Greek validity.
   Synthetic prices use an explicitly declared dollar budget; market rows use
   their original spread-scaled budget. Keep price-only, no-market, convention-gap,
   inconsistent-range and unresolved-reference denominators distinct.
2. Add a pinned QuantLib dated-cash comparison with the same process and event
   rights. Its API distinguishes Spot and Escrowed models; these cannot be mixed
   in one error column. Join actual event times and model year fractions, with
   explicit treatment of date quantization, simultaneous events, cash at expiry,
   cash before the first lattice step and cash exceeding spot. Inspect the
   comparator's jump behavior before accepting a model match.
   [QuantLib engine implementation](https://github.com/lballabio/QuantLib/blob/e96579fd471b2e9f6bc974c96674d1ccfff248fa/ql/pricingengines/vanilla/fdblackscholesvanillaengine.cpp).
3. Attribute production work before optimizing. The existing prepared batch
   shares routes/step maps, yet historical prepared price latency is nearly the
   standalone latency. Identify repeated transition weights, underlying grids,
   jump interpolation locations and payoff-independent rollback operations across
   same-model requests. Charge preparation and every scalar fallback. SIMD across
   strikes or scenarios must preserve each contract's cash schedule, volatility,
   exercise decisions and error evidence; grouping by expiry alone is insufficient.
4. Compare a shared-grid multi-right-hand-side obstacle solve with the corrected
   lattice on the same corpus, including first-event and multi-event corners.
   Event-local damping and independent Greek convergence are mandatory. An explicit
   stabilized Runge–Kutta–Legendre method is a candidate for a measured comparison,
   not an assumed upgrade; its published study covers American price convergence
   and Greek stability. [Le Floc'h, 2021](https://arxiv.org/abs/2106.12049).
5. Keep a separate research branch for event-segment integral-equation methods.
   Itkin's hybrid-dividend method computes boundaries and prices through Volterra
   equations; its examples do not establish parity with this library's liquidation,
   event-rights or full-domain conventions. Reproduce those before implementation
   promotion. [Itkin's paper](https://arxiv.org/abs/2510.18159).
6. Measure cold, inclusive prepared, update and query latency independently on
   a quiet single core. Report percentiles with accuracy-qualified coverage,
   allocation counts, preparation memory and break-even batch size. A nominally
   faster route with reduced coverage or unresolved reference errors cannot win.

The continuous-carry SPY candidate does not qualify dated-cash pricing. Its
subsecond fitting work proceeds independently; cash work can reuse the audit,
provenance and measurement infrastructure while retaining its own model contract.
