# American Greek qualification review, 24 October 2026

The current pricing comparison does not qualify American Greeks. The code has
several distinct derivative maps, and some public paths expose known near-edge
limitations without per-field validity. The immediate work is to make those
limitations explicit and build independent derivative evidence, before making
an analytics speed claim. No Greek tolerance is inferred from the user-approved
SPY price-error budget.

Reviewed source: `148683537dc3f8337ba2a73bf72e1014b2d2a5e3`; the relevant
American pricing/header/record files match integrated root `9dd1fd08` exactly.
This is a source audit and a frozen corpus proposal, not a completed Greek
accuracy or latency measurement.

## Findings and concrete actions

### G1: sigma-only family APIs expose unqualified near-edge theta

The public `AlSigmaFamilyGreeks` bundle has whole-price `status`, carry status,
and exercise/floor flags, but no per-Greek validity mask. Its documentation
explicitly records near-edge PDE theta errors of 25--138% against converged
calendar differences, including a wrong sign. `family_greeks`,
`family_chain_greeks`, `family_shock_grid` and detached `greeks` can therefore
return an Ok price/status together with a requested theta that has no machine-
readable qualification. Sources: [bundle contract](../include/atx/vol/api/pricing/american.hpp),
[public entry implementations](../src/pricing/american_family_greeks.cpp).

Caller trace matters: the only in-tree external uses of this sigma-only bundle
are its public wrappers and `al_preset_ladder_bench.cpp`. The live
`SliceServeRecord` uses **AlSigmaQFamilyGreeks**, whose per-field reasons already
withhold theta and gamma in `NearExerciseEdge`. No evidence was found that the
current live record traverses the defective sigma-only route. The public
analytic APIs remain exposed; this finding must not be misreported as a proven
current ServeBook defect.

Fix sequence: reproduce the public sigma-only success with the existing
near-edge contract; retain price and unaffected derivatives, add per-field
supported/reason metadata and use the established normalized edge band for
theta/gamma. Charm differentiates that same questionable theta and must also
be withheld or independently validated. Propagate metadata through chain,
shock and detached outputs. Preserve allocation-free lookup and exercise-region
payoff derivatives. A fallback is acceptable only if it independently validates
the relevant derivative on the identical model; substituting another PDE
identity is not such validation. Add direct, chain, shock, detached, boundary-
band endpoint and price-only regression coverage.

### G2: the American 'adjoint' is a finite-difference forward tangent

[adjoint_greeks.hpp](../src/pricing/adjoint_greeks.hpp) correctly explains that
the American path propagates forward tangents through boundary iterations,
with finite-differenced Jacobian-vector products, plus cold volatility bumps
for volga. It is not reverse-mode automatic differentiation. The European
implementation has a genuine first-order reverse sweep.

The [American tests](../tests/adjoint_greeks_test.cpp) compare primarily with
the same Andersen-Lake mark or its Richardson differences. Some accepted grids
allow first-order errors of 3% plus an absolute floor, and second-order volga
errors of 2 plus 10% on selected smooth regions. Negative-borrow vega/vanna/volga
and some long-maturity second derivatives are not generally gated. These tests
are useful implementation consistency checks, not an independent uniform
Greek guarantee. Keep errors, claimed route and fallback counts visible for
every contract; never replace the omitted region by a successful zero Greek.

For a future reverse implementation, first select the target: derivative of
the finite-iteration served mark, or derivative of a converged obstacle model.
Differentiate the corresponding algorithm, and report any mark-map difference.
A reverse sweep is most attractive for few outputs and many input parameters;
it is not automatically superior for a small fixed parameter set. The primary
[Giles--Glasserman paper](https://people.maths.ox.ac.uk/gilesm/files/NA-05-15.pdf)
establishes that distinction for Monte Carlo sensitivities; applying it to this
boundary solver is an engineering proposal, not a measured speed result.

### G3: existing timing loops are not correctness-qualified throughput

[american_greeks_reuse_bench.cpp](../bench/american_greeks_reuse_bench.cpp)
uses 40 puts at fixed positive rates/yields. Several paths add zero to the sink
when a pricing result fails and still count every attempted contract as an item.
The current corpus is intended to be valid, but the receipt does not independently
prove per-contract success, finiteness, model identity or derivative accuracy.
The SIMD path likewise records timing rather than a complete accuracy receipt.

Keep these microbenchmarks, but add a separate frozen correctness export and
qualified throughput runner. Report attempted, supported, accurate, refused and
reference-unresolved denominators per Greek; include typed fallbacks. Never
divide by only successful contracts while omitting failed latency. Charge model
and family preparation in cold clocks; show setup and query cost separately for
declared reuse groups.

### G4: similar names differentiate different maps

* `american_greeks_fd`: differences of the declared price implementation;
  sigma/r/time bumps can re-solve the boundary. Its call path now uses homogeneity
  reuse as well as its put path.
* `american_greeks_al`: spot differences and sigma/r re-solves, with PDE
  theta/charm; current core admits genuine early exercise on both sides.
  Its nearby puts-only documentation is stale and needs a scoped correction.
* Cached `american_greeks`: derivatives of the Black-76 plus correction
  interpolant. Null/invalid/opposite-side caches deliberately fall back to
  the European leg. The harness must label that model, not count it as a valid
  American result merely because numbers are finite.
* Sigma and sigma/q family Greeks: derivatives of an interpolated boundary
  map, with separate carry bumps or missing-axis refusals. The price and
  derivative map may differ from cold Reference by more than roundoff.
* Record/served analytics: explicit model, version, held-fixed convention,
  price-map difference and per-field reasons. The [served contract](../include/atx/vol/api/analytics/served_analytics.hpp)
  already distinguishes ContractSigma, StickyStrike, StickyDeltaMoneyness and
  RefittedSurface. Those are separate benchmark answers.

All new reports must preserve these distinctions. Unrequested zero-valued
fields are not evidence of a supported derivative. PriceOnly IV nodes cannot
be manufactured into Greek-capable contracts.

### G5: cash-dividend Greeks need their own model and event corpus

The [production cash kernel](../include/atx/vol/api/pricing/american_discrete_div_kernel.hpp)
has explicit process, cash-excess, exercise and ex-date-grid conventions.
Its Greek route holds the node set/pre-steps fixed during bumps, uses per-field
unsupported bits, withholds theta/charm when an ex-date enters the time stencil,
and withholds exercise-dominated call vega. Existing independent-reference pins
are a useful starting point in
[american_discrete_div_production_test.cpp](../tests/american_discrete_div_production_test.cpp).

Do not compare this spot-shift/liquidator model with an escrowed or proportional-
yield approximation under a common 'American' label. A time roll crossing an
ex-date is a priced event with stock/schedule updates, not a theta difference.
Cash amount sensitivities, event timing, dividend exhaustion and call exercise
need separate coverage and denominators.

## Frozen continuous-carry corpus and reference contract

[corpus-v1.json](../bench/american-greeks/corpus-v1.json) freezes 715 contracts:
the 357 pricing contracts on both call and put sides, plus the existing near-edge
SPY-style regression. Its source pricing corpus hash is pinned. All rates,
yields, strikes, maturities and volatilities are explicit; no vendor labels or
fitted IVs are used. This is the first fixed corpus, not a full market-domain
claim. Refusals and non-differentiable/reference-unresolved cases stay in its
715-row denominator. Extension corpora for minutes-to-expiry, regime thresholds
and real raw-snapshot rows must be separately versioned before measuring.

Use scalar continuous-carry Black-Scholes model inputs with ContractSigma held
fixed. Vega/rho/phi are per unit annualized volatility/rate/yield; theta is
calendar `-dP/dT` on the declared scalar model clock. A variance-time derivative
cannot be relabelled as an actual calendar roll when the funding and variance
clocks differ. Cash schedules and fitted-smile dynamics are excluded here.

For each requested derivative, preserve two separate comparisons:

1. **Map consistency:** central differences/Richardson at several bump sizes
   of that route's own declared price map, with identical model and immutable
   preparation inputs. This catches derivative implementation errors.
2. **Model accuracy:** a separately implemented converged obstacle reference,
   with independently refined time, space, domain, interpolation and bump sizes.
   This catches shared pricing-map errors. European closed forms and exact
   interior exercise derivatives anchor regimes where they apply.

Never differentiate a price error tolerance as if it were a Greek bound. With
price uncertainty epsilon, a central first derivative can inherit epsilon/h,
and a second derivative up to 4*epsilon/h^2 before truncation. For epsilon=.001
and a .1-dollar spot bump these are .01 delta and .4 gamma; a small price residual
alone is insufficient. Report actual errors and reference uncertainty first.
The corpus deliberately sets `derivative_tolerances` to null: a future economic
P&L tier can be proposed separately, but no existing price contract authorizes
loosening derivative accuracy.

PDE price convergence also does not establish Greek convergence. In 't Hout's
[American Greek study](https://arxiv.org/abs/2401.13361) reports problematic
Crank--Nicolson derivative behavior even with initial backward-Euler damping
and studies alternatives with more regular convergence. The independent
reference should compare suitable strongly damped time methods and expose
near-boundary uncertainty rather than assigning a zero error bar.

## Accuracy-qualified latency experiment

Run scalar FD cold/warm, scalar AL Reference/Fast/Screen, private forward tangent,
scalar/AVX2 bundle, sigma-family, sigma/q-family, and actual record query as
distinct routes. Request masks are separate workloads: price/delta/gamma/theta;
add vega; full carry/second-order bundle where supported. Do not rank a partial
bundle against a full bundle without its output mask.

Cold process clocks include setup and one complete corpus. Prepared runs report
setup, query and inclusive totals, with amortization at 1,7,32,256 queries; freeze
which inputs remain constant for each group. Family construction and carry-bump
families must be charged. Scalar/AVX2 must share exact inputs and profile. Keep
allocation counts and fallback counts beside latency. Record latency measures
an already-built immutable record and is not cold fit latency.

Use archived ThinLTO executables, one physical core with idle SMT sibling,
multiple fresh processes and balanced engine order. Retain all trials; report
median/p95/p99, total attempts, per-field accurate coverage and reference-
unresolved counts. The price comparator's timing-order reversal is a reason to
require this stronger experiment before publishing a close ranking. Do not
start a Greek performance promotion until its independent accuracy gate exists.
