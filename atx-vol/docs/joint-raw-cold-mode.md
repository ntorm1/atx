# Explicit joint raw-price cold fit

`QuoteSurfaceConfig::cold_fit = QuoteSurfaceColdFit::JointFeasibleMid` selects a new
surface objective. The default `ExistingPolicy` retains the existing point,
channel and term-prior ladder. This mode is an explicit change of fitting
objective, not an acceleration claimed to reproduce that ladder's prices.

The new path builds private drafts from every original quote. Each target is
the midpoint of its quote interval after the lower edge is intersected with
proven intrinsic and terminal European floors (and any selected call floor).
It jointly projects these targets onto the reviewed slice/calendar constraints,
and identifies each final node side once. Its final curve is built from genuine
identified OTM final-price observations. Original market-IV admission is not
invented: rows retain their original quotes, have no market IV or admission,
and carry the final identification. The final curve supplies meaningful curve
and prior queries; published American prices remain the certified piecewise
linear price representation. There is no neighbour-prior optimization in this
explicit raw-price objective.

All draft contracts in [raw-price-draft.md](raw-price-draft.md) apply: exact raw
boxes, duplicate intersections, supplied carry, continuous or aggregate-PV cash
model, all paired sides, PriceOnly classification, actual terminal floors and
optional proved-floor-only ask recovery. The original raw spreads remain the
monetary accuracy budgets. Only the pre-existing explicit ask recovery may widen
a contradictory ask; the target rule cannot widen a quote or enlarge a budget.
The projection budget is `min(100, repair.joint_max_sweeps)`; budget exhaustion
does not assert data infeasibility. The solver returns a certified feasible
point guided by squared distance, not a proof of exact least-squares optimality.

Every expiry must finish with a real curve, certified prices and a Success or
PriceOnly answer for every original row. The final representation always runs
`validate_published_representation` with identification enabled, independently
of the legacy publication switch. Configured distance-to-market limits and,
under the knot ladder, the channel containment limit remain acceptance gates.
Failure of either gate rejects the whole attempted mode; it never withholds
rows just to pass. Successful new-mode slices receive grade B and method
`JointFeasibleMid`, after all these checks. Legacy point/knot admission and refit
fields retain their defaults because those attempts did not occur.

The surface is published atomically. Unsupported inputs, infeasible projection,
iteration exhaustion, insufficient final-curve evidence, or a publication
failure discard every local candidate and run the existing surface policy on
the original inputs. Every resulting slot retains its actual legacy method,
grade and error, with the same `cold_fit_fallback` reason:

| Reason | Meaning |
| --- | --- |
| `unsupported_input` | A complete supported raw draft cannot be built. |
| `projection_infeasible` | A typed exact-box conflict was established. |
| `projection_nonconverged` | The deterministic projection budget was spent. |
| `finalization_failed` | A projected draft could not become a complete identified fit. |
| `publication_failed` | Final coverage, configured quality gates or API validation failed. |
| `none` | The default policy was selected, or the whole joint mode succeeded. |

Requested policy must be read together with effective method and fallback when
reporting latency. A fallback cannot qualify a joint-mode speed result. Actual
new-mode timing includes draft construction, joint projection, finalization
and final publication checking. The existing per-expiry recorders also retain
the entire failed attempt before timing legacy fallback. Global projection and
publication work are charged to the first input slot; those costs are not that
expiry's independent fitting time. The first version performs these stages on
the calling thread regardless of the legacy `fit_workers` setting.

Caller snapshot provenance is copied to every published fit in both paths.
The runtime final-classification policies and context must not be silently lost
through archival; the owning archive layer must persist them or explicitly
refuse unsupported records. This mode does not establish Greek accuracy or a
subsecond complete-fit result. Those remain independent whole-fit gates.

Tests cover default identity, actual final-price publication, snapshot metadata,
duplicate rows, all-or-nothing unsupported/infeasible/nonconverged/finalizer/API
fallback, and conservation of timing through a failed attempt.

The earlier original-midpoint experiment identified only 11,767 IVs and demoted
840 baseline rows. The explicit feasible-midpoint objective identifies 12,850
while preserving all 33 expiries, 12,866 rows and exactly 15 box departures in
the private smoke. Two baseline identified rows become genuine PriceOnly under
the new fitted targets. The strict no-demotion diagnostic remains failed;
promotion requires independent API/accuracy validation and explicit review of
those two classification transitions. See
[raw-price-target-experiment.md](raw-price-target-experiment.md). No qualified
complete-fit latency or Greek accuracy is established by these smoke results.
