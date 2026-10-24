# Cold American Greek stencil admission

This change addresses incorrect successful derivatives found by the frozen
[715-contract audit](american-greek-full-corpus-20261024.md). It preserves option
prices and reports availability separately for every requested derivative.
Availability is not an accuracy certificate; derivative tolerances remain unset.

## Reproduced defects

For the put `S=772.99, K=811.6395, T=.0682, sigma=.12, r=.04496,
q=.00162193`, the prior cold AL theta was `+8.7999725020`. Three refined time
differences converge toward `-.3373631081`, with no time-stencil exercise crossing.
The production spot stencil includes an exercised lower point. Using its gamma in
the continuation PDE produced the wrong sign. The scalar, prepared and explicit
AVX2 routes all exposed that value as a successful derivative. The reflected call
exercises the same defect through the other internal-put orientation.

For the call `S=200, K=100, T=3, sigma=.2, r=.03, q=.08`, the default numerical
price is `100.0000062294`, just above intrinsic. A center-only price-proximity
test missed exercise and produced theta near `13` although the solved boundary
and refined price stencils classify exercise. The reflected put is also tested.

## Admission contract

Each evaluated sample records its price and exercise classification. The actual
converged boundary is read before a later bumped solve can overwrite it. Put
boundaries use their original strike; call boundaries use the same strike
homogeneity as that sample's price. A positive intrinsic value is classified as
exercised only when the internal spot lies within the solved exercise boundary or
the numerical price is at its active intrinsic clamp. Mere proximity within
`1e-9*K` does not prove exercise. Required missing/nonfinite boundary evidence
remains unavailable.

Every bundle field includes the center in its regime evidence, including first
and mixed differences whose formula does not explicitly consume the center.

| Field | FD evidence beyond center | AL evidence beyond center |
|---|---|---|
| Delta, gamma | `S +/- hS` | `S +/- hS` |
| Vega, volga | `sigma +/- hv` | `sigma +/- hv` |
| Theta | time endpoints | `S +/- hS` used by the PDE |
| Rho | rate endpoints | rate endpoints |
| Vanna | four spot/volatility corners | four spot/volatility corners |
| Charm | four spot/time corners | `S +/- hS`, `S +/- 2*hS` |

Homogeneous continuation retains the existing numerical estimate. Homogeneous
exercise supplies the payoff derivatives: call delta `+1`, put delta `-1`, all
seven remaining derivatives zero. Mixed exercise/continuation samples refuse
only that derivative with `StencilCrossing`; missing/nonfinite evidence refuses
it with `Nonfinite`. Refused numeric slots are NaN, and their support bits are
clear. Price is unchanged and independently available. This admission occurs
after combine arithmetic, so a refused gamma cannot poison another field before
that field's own evidence is classified.

The scalar FD/AL, prepared AL and direct AVX2 paths use this same rule. SIMD
`handled` means a priced bundle was produced; its per-field support must still be
checked. Existing numerical-solve failure fallbacks remain. The FD API's existing
whole-result pricing-error propagation is not converted into partial recovery by
this change.

## Vega-only scope

`american_vega_al` retains its two sigma-endpoint solves. Both exercised endpoints
supply zero; mixed/nonfinite endpoint evidence returns `Unavailable`. Uniform
continuation retains the centered estimate. No center is added, so this admission
is deliberately distinct from the full bundle's center-inclusive evidence.
A synthetic evidence regression pins that distinction without asserting that the
synthetic pattern is an observed model case. The FD fallback must check Vega's
support before extraction; it cannot return successful NaN from a refused bundle.

## Validation and remaining work

Regression tests cover the actual wrong-sign theta and false-continuation cases
on both sides, prepared/scalar/direct AVX2 routes, price preservation, typed
missing evidence, active-clamp versus price-proximity classification, and Vega
endpoint/fallback refusals. Existing 75-put and 75-call FD reuse tests preserve
all 1,200 requested derivative outcomes: admitted continuation arithmetic keeps
its original comparisons, exact exercised values are checked explicitly, and
refusals are counted and validated rather than disappearing into NaN reductions.
An isolated allocation-counter test covers cold FD/AL, prepared AL, public scalar
and AVX2 wrappers, and the Vega refusal path for both reflected crossing cases.

Native audit schema v2 exports actual cold request/support masks and all reasons;
its verifier joins them to every field and retains partial usable prices. It
rejects boolean masks, unknown bits/reasons, impossible support/reason pairs,
finite refused slots and inconsistent price availability. Historical v1 receipts
retain their original finite-only interpretation. No historical receipt is
rewritten as if it had exported this new evidence.

This is a bounded correctness fix. The frozen audit also found short-expiry
fixed-step theta/gamma error and unresolved carry-boundary reference convergence.
Uniform continuation does not settle these errors. Adaptive/refined field
estimation, independent-method reference convergence, carry/Greek conventions and
an explicit derivative error contract are still needed before Greek accuracy or
latency qualification. In particular, replacing PDE theta with one fixed-step
time difference would not establish such qualification.

The next bounded refinement work should retain these exact unresolved controls:

| Frozen case | Existing estimate | Finest refined same-model estimate | Meaning |
|---|---:|---:|---|
| `g010-put-03`, ATM, `T=1/365`, sigma `.8` | FD theta `-312.57048115` | `-307.17081527` | Absolute fixed time step is too large relative to maturity |
| `g001-call-03`, ATM, `T=1/365`, sigma `.05` | FD/AL gamma `1.50567564` | `1.52393428` | Fixed spot step needs refinement even without exercise crossing |
| Near-edge put, first smooth refined spot level | PDE theta about `-.352375` | time theta about `-.337363` | Absence of crossing does not establish PDE/FD consistency |

A candidate refinement must preserve model clocks and carry, keep center and
endpoint exercise evidence, report the original and refined field values, and
retain all requests when convergence is unresolved. Step/profile agreement is
empirical evidence only. A user-selected derivative or economic P&L budget and
independent-method controls remain prerequisites for a qualification gate.

## Frozen component checks

The final focused run passes 29 tests, with one counter-dependent test skipped
and one pre-existing disabled test not executed. The separate allocation test
passes with zero allocations for both sides across all five cold/prepared/SIMD
routes and the Vega refusal. Owning PCH-off compilation passes for both production
sources, the pricing test, the allocation test and the audit exporter.

Receipts are under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/`:
`cold-greek-admission-final-green.{log,xml}`,
`cold-greek-admission-alloc-green.{log,xml}`,
`cold-greek-admission-final-hygiene.log`, and
`cold-greek-admission-final-hygiene-refresh.log`. The original wrong-output,
Vega and proximity RED receipts remain alongside them. Downstream consumer
fixtures that formerly relied on tiny-spot arithmetic overflow require the
separate metadata-propagation lane's fixed crossing-case migration. These
focused checks do not claim the entire integrated consumer suite passed.

Boundary admission adds no boundary solves. A scalar AL request for charm now
evaluates the two wide spot prices even when the center is exercised, so its
complete five-point regime evidence matches the existing SIMD path. This small
extra forward-evaluation cost is disclosed; no Greek latency measurement was
performed for this component. A fresh metadata-aware corpus receipt is separate
from these correctness tests.
