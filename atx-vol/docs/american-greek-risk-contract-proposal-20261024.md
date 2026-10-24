# Proposed engineering contract for cold American risk

This proposal supplies concrete gates for the next validation experiment. It
is not a public default, a currently implemented acceptance gate, an approved
user Greek tolerance, or an extension of the approved SPY price-fit budget.
The machine-readable proposal is
[`risk-contract-proposal-v1.json`](../bench/american-greeks/risk-contract-proposal-v1.json).
Its constants are frozen before broader candidate validation. Failures must
lead to explicit unresolved results or numerical work, not retrospective
threshold changes to make the current candidate pass.

The first scope is the eight fields represented by the cold `AmericanGreeks`
bundle: Delta, Gamma, Vega, Theta, Rho, Vanna, Volga and Charm. Units are dollars
per underlying share; volatility/rates are absolute annualized units, with
ContractSigma and other scalar model inputs held fixed. Theta and Charm use
calendar time, `-d/dT`, on the declared Calendar365 scalar model. Pricing views
with other clocks or changing effective carry need a separate chain-rule and
clock validation. Cash dividends, sticky-surface risk and derivatives of a
served interpolation map are distinct contracts. Any additional requested
axis, such as Phi, remains a failed full-request outcome until an appropriate
qualified producer supplies it; an eight-field label cannot hide that absence.

| Shock set | Spot | Volatility | Rate | Forward calendar roll |
|---|---:|---:|---:|---:|
| Operational | +/-0.1% | +/-0.001 | +/-0.0001 | 60 seconds |
| Stress | +/-1% | +/-0.01 | +/-0.0025 | 3,600 seconds |

Time rolls are capped at 10% of remaining maturity and use actual effective
seconds in every reported coefficient. All sign combinations are considered;
there is no favorable cancellation assumption. These are explicit engineering
scenarios, not a claim about market moves or a recommended trading horizon.
The resulting Taylor terms are `Delta*dS`, `Gamma*dS^2/2`, `Vega*dsigma`,
`Volga*dsigma^2/2`, `Theta*dt`, `Rho*dr`, `Vanna*dS*dsigma`, and `Charm*dS*dt`.
Mixed terms have no extra factor of one half. For a larger joint scenario,
unrepresented cross derivatives must be reported; this eight-term expansion
does not pretend to be a full Hessian.

The provisional limit is **$0.0001/share per component**, including empirical
reference uncertainty, for both shock sets. The sum of absolute component
differences must also be at most **$0.0005/share**. For a 100-share multiplier,
these correspond to one cent per component and five cents for the sum.
These independently chosen engineering constants are intentionally distinct
from the option quote price-fit tolerance. They may be revised only through a
new version with the rationale and prior failures retained.

Reference uncertainty may consume at most one quarter of each component limit.
For a candidate derivative `g`, stabilized reference center `m`, empirical
reference radius `u`, and absolute Taylor coefficient `a`, the diagnostic gate
is `a*(abs(g-m)+u) <= 0.0001`, with `a*u <= 0.000025`. The absolute-component
sum uses those full expressions. A missing candidate or reference never
contributes zero. Preserve signs when the stabilized reference interval
excludes zero and the reference shock contribution exceeds the uncertainty
reserve. Report native-unit derivative discrepancies alongside shock dollars;
economically small differences do not become claims of uniformly accurate
raw Greeks.

Reference construction is independent of which window best matches the
candidate. Starting coarse-to-fine, inspect adjacent three-step windows at
both refined24 and refined32. Require finite operands, complete actual exercise
evidence and the same homogeneous regime across every relevant operand in both
profiles. An exact exercised payoff branch is handled explicitly. For
continuation, successive difference magnitudes must contract by at least a
factor of two for a central stencil, or by at least 25% for a one-sided stencil,
with a separately reported allowance of 32 times the arithmetic cancellation
indicator. The scheme/step and one-sided rules must be identical across the
comparison. The first window satisfying these conditions and the uncertainty
reserve is used; choosing the smallest step or the minimum observed candidate
error is prohibited.

Use the midrange of the six admitted profile/step estimates as the reference
center. An empirical radius must cover their whole range and be at least the
largest adjacent-step drift, cross-profile drift and cancellation allowance.
This is a reproducible stability envelope, **not a rigorous numerical error
bound**. Shared Andersen-Lake bias can survive every such check. An American
promotion therefore still requires agreement with an independent reference
method whose own derivative refinement has passed. The existing independent
CN price calculation does not automatically qualify Greeks; its space/time
and free-boundary convergence must be established separately. Closed-form
European controls are already an independent check for their restricted domain.
This distinction is supported by in 't Hout's 2024 study: American Delta/Gamma
can converge poorly under Crank-Nicolson even with Rannacher damping; selected
DIRK schemes exhibit more regular second-order convergence in the paper's
experiments. That is a direction for the independent oracle, not a benchmark
claim about our implementation. [Primary paper](https://arxiv.org/abs/2401.13361).

Each output receives one of: supplied estimate awaiting reference, empirical
gate pass, empirical gate failure, unavailable field, or unresolved reference.
Only a complete requested bundle with independent reference evidence can be
considered for a qualified tier. Retain the input/field denominator, actual
boundary/refusal reasons, derivative and shock errors, reference radius,
selected window, rejected coarser windows, profile settings and cost counts.
Run the full frozen corpus before selecting an implementation default, then
validate out-of-corpus strikes, maturities, exercise distances and rates on a
separately frozen set. Price bits and existing typed admission must remain
covered by regressions.

The initial twelve-case study argues for separating three problems. Relative
time and diffusion-scaled spot steps fix obvious truncation error cheaply.
Near-boundary quadrature bias requires a better price map or a stronger
reference/profile, while unstable smaller-step Gamma/Charm require a stability
policy rather than unconditional acceptance. A two-level refinement can share
base/vol/rate boundaries and add only the distinct time models; that is a
performance direction to measure after the diagnostic contract is implemented.
Price success must remain independent of refused risk fields. Neither a
fallback FD estimate nor a passing dollar proxy authorizes hiding unresolved
derivative behavior.
