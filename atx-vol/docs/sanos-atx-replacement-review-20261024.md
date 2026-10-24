# SANOS ideas against the current American SPY fit

This is a source review and prospective experiment, not an implementation or a
new numerical result. ATX is pinned to `b422fe45da3beae6eba31872ff9da06f595b7b64`.
The external checkout is pinned to
`f051df37e9572dc8ecbdf218534ec946a57bc5eb`; its archived source manifest is
`c32525621bacfa15311308dd5c7c1cc98590c8e9ce7e78af36e9e3ba34ccf518` under
`E/repair-iv/sanos-superfast-source-audit-v1`, where `E` denotes
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023`.

**Recommendation:** first measure the constraint graph needed by ATX's actual
violations, then test bounded constraint generation using the existing price
variables and solver. A positive European pricing basis is a separate research
direction. The external active-set solver is not a suitable replacement for the
current American joint projection.

## What transfers, and what does not

The [SANOS paper, version 4](https://arxiv.org/abs/2601.11209v4) develops smooth
European price surfaces, including joint calendar conditions and a positive
local-volatility representation. Its price-domain formulation is relevant.
Its European construction does not establish ATX's American early-exercise,
carry, parity-envelope or reporting-floor contracts. The paper's model and the
linked optimized repository must be evaluated separately.

In the pinned [surface implementation](https://github.com/marwinsteiner/sanos-superfast/blob/f051df37e9572dc8ecbdf218534ec946a57bc5eb/src/surface.cpp#L18),
model knots include market strikes and fillers (`18-59`), so this source does
not demonstrate a dramatically smaller basis. A shared variance defines a
Black pricing matrix; the quadratic loss uses prepared kernels and two moment
equalities (`191-231`). General inequalities are cleared (`224-225`), and
expiries are calibrated separately. The caller ignores the QP outcome and
clips/renormalizes weights (`248-282`). Fitted IVs are deferred. These are useful
kernel/preparation ideas, but this workload does not provide the same complete
American fit or constraint coverage as ATX.

The [QP implementation](https://github.com/marwinsteiner/sanos-superfast/blob/f051df37e9572dc8ecbdf218534ec946a57bc5eb/src/qp_solver.cpp#L144)
gathers and refactors a reduced dense Hessian for each active-set iteration
(`144-175`); it does not provide the across-iteration numerical factor reuse
ATX is investigating. More importantly, its release test uses `Hq+f` without
the equality-multiplier contribution (`292-323`). A source-derived counterexample
is `H=I`, `f=0`, equality rows `(1,1,1)` and `(.5,1,1.5)`, RHS `(1,1)`, and warm
weights `(.5,0,.5)`. That face has equality multiplier `(-.5,0)` and an excluded
coordinate's reduced gradient `-.5`, although its raw gradient is zero. The
loop can report Optimal at objective `1/4`; `(1/3,1/3,1/3)` has objective `1/6`.
This is algebra/source evidence, not a newly executed native reproduction.

The independent methods lane also finds that the repository's SIMD logarithm
does not approximate the required Black kernel closely on its actual input
range. Its exact-input equation control at normalized strike `.99` and variance
`.04` gives a price discrepancy of about `-.00374783` per unit forward.
That control emulates the source arithmetic; it is not a native benchmark.
The frozen method report is `E/calendar/volfi-sanos-methods-v1/review.md`.
Consequently, even the analytic positive-basis shape argument needs independent
implementation checks; copying the SIMD kernel cannot inherit the theorem.

[volfi](https://github.com/wol-fi/volfi) supplies a European Black-Scholes inverse.
It could propose a European anchor or an American IV seed, followed by ATX's
unchanged American checks. It cannot certify an American inverse. The pinned
SANOS wrapper also has an ITM normalization defect documented by the independent
benchmark audit; importing that wrapper is not part of this proposal. Neither
repository's advertised clock is a comparable one-core complete-SPY measurement.

## ATX source map and the remaining work

Line references below refer to the pinned ATX source, not a moving branch.

| Boundary | Source | Implication for a replacement |
|---|---|---|
| Original quotes and targets | `src/fitting/raw_price_draft.cpp:76-117` | Duplicate intersections, recovered internal asks and original raw targets are distinct. Preserve all of them. |
| Selected current objective | `src/fitting/raw_price_targets.cpp:38-64`; `src/fitting/quote_surface.cpp:290-321` | `JointFeasibleMid` selects feasible-box midpoints while retaining raw midpoints separately. Its solver budget is `min(100, joint_max_sweeps)`. |
| Coupled scheduling | `src/fitting/raw_price_draft.cpp:120-158` | Sort by declared calendar time, project all slices, then commit transactionally. Per-expiry parallel fitting cannot replace this coupling. |
| Coordinates and actual floors | `src/fitting/joint_inbox_projection.cpp:139-214` | Eliminate only truly fixed coordinates; keep terminal prepaid/discount floors and exact effective boxes. |
| Existing exact row reductions | `src/fitting/joint_inbox_projection.cpp:104-136,300-330` | Box-implied rows are already removed; calendar rows covered by intermediate overlap are already skipped. Do not count these savings twice. |
| Strike shape, parity and special regime | `src/fitting/joint_inbox_projection.cpp:233-297` | Preserve convexity, endpoint slopes, affine call-minus-put bounds, and non-dividend conditions. |
| Sparse numerical cost | `src/fitting/joint_inbox_projection.cpp:335-426` | Symbolic structure is already reused within a solve. Every iteration still assembles/factors `I+A' diag(z/s) A` and solves two directions. |
| Actual stopping contract | `src/fitting/joint_inbox_projection.cpp:541-616` | Infeasible-start method stops on primal feasibility, not least-squares optimality. Boundary-only feasible sets are supported. |
| Actual publication | `src/fitting/joint_inbox_projection.cpp:619-645,726-763` | All bounds remain barriers; clamp only in the existing certificate path; verify every slice and all-pair calendars before writes. |
| American seed admission | `src/pricing/american_iv.cpp:2989-3039` | Seed source proves nothing. Cold Reference target price, exercise classification, target floor and positive chord remain necessary. |
| Proposal versus proof | `src/pricing/american_iv.cpp:3338-3560` | Screen/Fast proposal and cold proof are separate. Replacing an inverse proposal does not remove proof work. |
| Final IV fallback | `src/fitting/quote_slice.cpp:4219-4348,4400-4510` | Exact model/target/budget keys, native prepared outcomes, candidate fallback and typed Search remain downstream of the price projection. |

The current complete projection has **12,866 free prices and 54,104 retained
rows**, including 25,732 unary bounds, 13,669 two-term rows and 14,703 three-term
rows. Thus only 28,372 rows are candidates for temporary deferral. Keeping bounds
does not preserve all factor coupling: unary rows only change the diagonal;
multi-coordinate rows create graph edges and fill. However, omitting many rows
does not automatically remove many edges, because different rows can share them.
The census below must measure graph structure as well as row counts.

The archived [projection profile](joint-projection-profile-r7.md) reports 35
numeric factors and 70 directions, with 56,010 stored normal-matrix entries and
240,475 strict-lower factor entries. Its ThinLTO component observations were
357.85-398.21 ms, with 200.79-209.30 ms in numeric factorization; these are not
qualified whole-fit timings. Compact rows lowered retained capacity to
3,932,608 bytes without numerical changes. Repeating symbolic caching or merely
changing row allocation misses the remaining factorization cost.

The newer [centrality experiment](joint-centrality-result-20261024.md) reduced
factors from 35 to 29 but raised directions from 70 to 124. Its unqualified
development component observation worsened from 822.001 to 1005.952 ms. These
clocks belong to a different binary/environment from the prior profile. The
whole fit passed only 12/13 gates: two reviewed floor targets moved by nanodollars.
That result remains false; economically small movement is not permission to
change the identification evidence pins.

The captured candidate path also separates about 31 ms of proposal work from
129 ms of scalar proof work on 700 attempts, including 684 acceptances and 16
refusals. Those diagnostic clocks do not justify a speedup claim. They do show
why an ultra-fast European inverse alone cannot remove the dominant American
certification work. See [candidate capture](cold-price-candidate-capture-review-20261024.md);
the executed immutable capture is `E/acceptance/price-candidate-capture-v2/`.
The v1 directory was unexecuted and supplies no numerical evidence.

## First experiment: unchanged-solver census

Stage A is the only proposed first implementation. Add a default-null,
research-only synchronous observer after the unchanged full row builder and
after successful final certification. It must not change options, row order,
iteration order, model, target, arithmetic, stopping rules or publication.
Observe exact values in borrowed spans and copy them only into caller-owned
pre-sized storage; no retained borrowed references, timing reads or observer
allocation on the ordinary path. Recorder exhaustion invalidates the receipt,
not the fit. Build/export costs belong to the observation, not a latency claim.

The fixed SPY census retains every one of the 54,104 rows, original order, exact
coefficient/index/RHS bits, original target, clamped initial point `x0`, published
point, and all bounds/fixed-coordinate metadata. The reader independently
reconstructs row slacks using the declared arithmetic order, checks exact row
counts and classifications, and reports each non-bound row as violated
(`< -1e-10`), near boundary (`[-1e-10,+1e-10]`) or positive. This classification
does not widen the solver's tolerance or infer independent financial accuracy.

The proposed initial set is **all bounds plus exactly the initially violated
non-bound rows**. Report its row arities, touched variables, distinct symmetric
matrix edges, connected components, and fraction of original edges retained.
These are structural observations, not a factorization or timing estimate.
Keep initially tight but unselected rows in the export and in every full scan.
The already-feasible initial case must still return immediately. All original
solver counters, output bits, 33/12,866 coverage and thirteen API gates must
match the ordinary baseline. No reduced solve, parameter search or holdout
selection occurs in Stage A.

Here and below, "original target" means the unchanged target selected by the
current policy before the solver starts. For the canonical `JointFeasibleMid`
policy it is the feasible-box midpoint, not the separately retained raw quote
midpoint. Record both; do not accidentally change the objective during fallback.

## Bounded reduced solve, subject to a separate wiring review

If the census supports a materially smaller graph, Stage B would retain the
full immutable problem and use a deterministic, monotonically growing working
row set. The following proposed limits are fixed before the census result:
**three reduced rounds, each with the unchanged caller budget, then at most one
ordinary full-problem solve with that same budget**. For canonical SPY this is
100 iterations per round and at most 400 across the entire attempt and fallback.
A reduced failure immediately enters that full fallback; it
does not consume the remaining reduced rounds. There is no latency-based cap,
retry substitution, row eviction, alternative objective or epsilon widening.
These limits bound a correctness experiment, not its expected speed. An
eight-iteration reduced cap was considered during design but rejected before
the freeze: the current solver only returns a completed feasible result, so it
could prevent any row-addition round from being observed. No partial or refused
iterate is treated as a successful reduced solve. A tighter performance policy
would require a separate predeclared experiment after this mechanism is shown.

1. Keep all original variables, boxes and target values. Keep all unary box
   rows at every round, preserving their original row order. Select initially
   violated non-bound rows at `x0`; use stable full-row IDs and no duplicates.
2. Solve the reduced rows with the existing infeasible-start algorithm. In this
   first variant every reduced round restarts from the same clamped original
   target; no warm primal/dual state or undocumented preconditioning is added.
3. A reduced success triggers a scan of **every original row**, never only the
   working set. Check every slack is finite and use the unchanged `1e-10`
   primal tolerance. Add every violated omitted row in original order. An
   already-selected violation, nonfinite state, no progress or reduced failure
   triggers full fallback. Do not classify any such outcome as invalid data.
4. If the full scan passes, construct the exact publication candidate, including
   the existing box clamp and fixed-coordinate restoration. Scan all original
   rows again on that candidate: clamping may invalidate coupled inequalities.
   Run the unchanged slice/non-dividend/all-pair calendar certificates. Only
   then may caller-owned nodes change. A failed candidate certificate triggers
   full fallback; it is never published provisionally.
5. On any cap, scratch-allocation failure or numerical failure, destroy optional
   reduced state, restore the untouched original target and call the ordinary
   full solver once. Its result and typed refusals are authoritative. A reduced
   failure is not an infeasibility witness. Construction's existing direct
   contradictory-box witnesses remain unchanged.

The reduced normal matrix remains positive definite in exact arithmetic:
`v' H_W v = ||v||^2 + sum_j (z_j/s_j)(a_j v)^2 > 0` for nonzero `v` and positive
finite slacks/duals. Thus no artificial strict-feasibility margin or added
constraint is needed. This is an algebraic property, not a floating-point
conditioning guarantee; retain the finite-Hessian and solve refusal checks.
Some feasible quote sets have no strict interior, so requiring one would be a
coverage regression.

This is constraint generation around a **feasibility** solver. The usual argument
that a feasible reduced-QP optimum is the full optimum does not apply because
ATX intentionally stops before proving optimality. Only complete final feasibility
and the existing financial/API checks justify success. Different price bits and
IVs are possible; no baseline equivalence theorem is claimed. In particular,
the two reviewed floor transitions must keep their exact evidence contract or
undergo the same explicit independent review; no blanket PriceOnly demotion or
fixture widening is allowed.

Use at most one reduced row copy and one reduced factor/workspace at a time.
Retain the original row storage, bounded membership/ID buffers, target and
candidate separately; check size/byte arithmetic before allocation. Destroy the
reduced factor before fallback constructs the full one. Record actual capacities
and peak ownership; do not report only live numeric lengths. Count all selected
rows, full scans, symbolic rebuilds, factors, directions, setup, refused work,
fallback work and destruction. If a reduced pattern keeps the full symbolic
graph, disclose that it cannot establish the intended fill reduction.

Required controls include an omitted row that becomes violated, propagation
across several additions, initially tight rows, boundary-only feasible quotes,
locked coordinates, duplicate/redundant rows, an initially feasible target,
disconnected variables, conflicting raw boxes, finite-input overflow, scratch
allocation failure, per-round/cumulative caps, and publication-clamp violations.
Verify transactional rollback and unchanged fallback output bits. An adversarial
chain needing more than three additions must fall back without losing coverage.
Independent small dense feasibility witnesses and full PWL/calendar checks are
required; a reduced solver's own reported success is not the oracle.

## Prior attempts and decision order

- Dykstra is not this proposal. `docs/LEDGER.md:526` records 400,000 sweeps and
  104 seconds for a spread-weighted SPY slice; [in-box projection](inbox-projection.md)
  and [joint projection](joint-calendar-projection.md) explain the slow propagation
  and missing joint constraints. Do not repeat a cyclic local-projection loop.
- `src/fitting/dense_slice.cpp:194-224` already has an active-set QP, but it
  rejects an infeasible initial point and uses dense matrices. It is not a
  direct substitute for the infeasible raw SPY targets.
- Cached/packed LDLT already failed to establish a useful gain; centrality is
  a retained negative result. The [one-secant factor-reuse plan](../bench/joint-quasi-newton-plan-v1.md)
  is an independent alternative, with unchanged full graph but new inverse-action
  error/fallback obligations. Do not combine it with row generation in the first
  experiment, or attribute their effects interchangeably.
- A smaller positive basis changes the attainable price set and representation.
  Tight boxes and active convexity chords can then become unreachable. American
  call/put/carry and joint-calendar restrictions still need explicit construction
  and full certificates; European positivity alone is insufficient. Reserve it
  for a separate model experiment after the current-model census.

Advance only if the full workload remains served under the unchanged raw-spread
price budget and all thirteen actual-API gates. Then build matched ordinary and
research ThinLTO binaries, archive them, and perform a separately declared quiet
one-physical-core comparison with all attempts retained. A successful component,
fewer rows, or an attractive external README clock does not establish subsecond
complete fitting, Greek accuracy or general HFT superiority.
