# One-price family candidates for repaired IVs

The explicit repaired-price policy can accept a prepared family candidate before
paying for a strict K3 IV bracket. It applies only to positive immutable raw-row
budgets, the retained continuous-carry family, and the existing native price
window. The default strict route, zero budgets, unquoted queries, and scalar
unprepared queries keep their previous paths.

For a converged family lane with a positive usable vega, the proposal is
`sigma_lane - budget / (2 * vega)`. This intentionally selects a slightly lower
IV under the monetary policy. Vega proposes a point; it supplies no acceptance
bound. The point must remain above `kIvMin` and inside the prepared sigma box.
One `family_prices_bounded` evaluation gives an outward-rounded interval `[L,U]`.
Acceptance requires all of:

* `L > iv_premium_edge_band_top(...)`;
* `U < target`;
* outward-rounded `target - L <= budget`;
* exact current family, preparation stamp, geometry/options and request identity.

The interval proves the candidate's price residual under the family's existing
`RefinementValidated` error contract. That class is empirical, based on tail
refinement and independent build probes; it is not rigorous interval analysis.
Extending the below-target witness to `P(kIvMin) < target` additionally inherits
K3's empirical monotonic-domain premise. A local positive vega alone does not
prove global monotonicity of the discretized cold map. This optimization does
not create a general floor certificate. The independent full-input audit must
still compare every reported IV against the actual cold Reference `P(kIvMin)`
and its original raw-spread budget. Any failed floor row blocks promotion;
affected lanes need an actual cold floor witness rather than a looser gate.

Refused proposals are compacted in original order into the unchanged K3 batch.
Unresolved/low-premium routes retain the existing classified cold candidate and
its actual floor check, then historical typed Search. Neither prices nor quote
boxes are modified, and a refusal does not itself turn a row into PriceOnly.
The published identification cache keeps the final answer, so query IVs match
published IVs. Derivative accuracy and a strict `1e-7` IV error are not implied.

`IvPreparedEconomic` is separate from enclosure/confirm records. It binds the
interval, sigma, target, budget and model inputs to the exact prepared family.
Consumption rechecks those keys. It never sets `RootAcceptance`, never counts a
strict K3 success, and never retains a prepared-work pointer after publication.
`reimply_economic_attempts` counts bounded candidate evaluations,
`reimply_economic_roots` counts consumed economic answers, and
`reimply_economic_ns` measures their batch preparation. These prices are separate
from K3 enclosure-price counters.

The focused regressions cover cold residuals and actual cold floor prices,
deliberately optimistic near-floor/plateau proposals, stale request/options and
family re-preparation, tight/zero budgets, default scalar/batch bit parity,
economic publication and cached query metadata. Full SPY correctness and quiet
one-core performance are separate integration gates; this component makes no
whole-fit speed or qualification claim.
