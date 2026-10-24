# American Greek finite-difference refinement diagnostics

The cold Greek exercise-admission fix leaves measured short-expiry step errors
open. This follow-up provides a private diagnostic interface for studying those
errors. It changes no public default step, formula, exercise-admission rule or
derivative accuracy budget. Availability remains distinct from accuracy.

`src/pricing/american_greek_fd_audit.hpp` exposes explicit spot, volatility,
rate and maturity steps to the existing memoized cold FD implementation. Its
fixed-size observation record captures all seventeen actual price operands,
model inputs, solved exercise boundaries and exercise classifications. The
record observes existing calculations: it adds no price call, boundary solve or
dynamic allocation. Calls and puts use the same homogeneity/boundary evidence
as the production calculation. Public `american_greeks_fd` calls the shared
implementation without overrides or observation.

Invalid explicit steps and nonfinite model inputs refuse before pricing.
Existing near-expiry forward-time and unsupported-down-rate forward-rho rules
remain active. The output records the adjusted volatility step and both
one-sided flags. Aliased operands retain the actual center/spot-endpoint model
inputs and have `evaluated=false`; they never pretend a negative-time or
unsupported-rate sample was priced.

Work counters distinguish cold and warm memo boundary solves, prices from
prepared boundaries, European closed forms and degenerate limits. An opaque
fallback through the public price API has its own count: it is not represented
as one known boundary solve. These are operation counts, not qualified latency.

The initial regression suite verifies both sides, cold/warm memo accounting,
all numeric output bits and field metadata against public defaults, one-sided
aliases, volatility clamping, invalid inputs and zero allocations. The complete
fifteen-contract native pilot is byte-identical to the frozen post-admission
pilot, SHA256 `43a4b090c4e1ef60670b53c98d783d08b8ce80e9c7c32e474bc4e237aa50414c`.
Runtime tests pass 29 cases with one counter-disabled skip and one pre-existing
disabled case; both isolated allocation tests pass. Owning PCH-off checks pass.
Receipts are in `acceptance/g2-step-seam-*` and
`acceptance/g2-finite-model-red.*` under the r7 evidence root.

The next experiment compares legacy steps with spot
`min(0.001*S, 0.02*S*sigma*sqrt(T))` and relative-time steps, individually and
together, over several scales and model quadratures. Every requested-field
outcome and exercise refusal must remain in the denominator. Convergence,
roundoff cancellation, model-profile drift and declared shock contributions
are diagnostics; neither a small price residual nor a stable finite difference
establishes a Greek error bound. No production adaptive scheme is selected here.
