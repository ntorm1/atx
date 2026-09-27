# Independent bounded R3 factor-cost source review

Reviewer: pool5 / w0_gate_audit; frozen pool4 sources only.

- Production: `65bdf3da27cbb8322b2a041227a77fb911baf2e3`.
- Separate implementation extraction: `ec6719b2d9817db7d53f113c6a61d6280edfa4fd`.
- Initial six fixture sources: `db36df91767d49476f9306539286b45a967144d9`.
- Reviewed defect correction plus seventh fixture:
  `fc80a5eed0d4bcc9d2e438218e5a25fd3f35675c`.
- Direct implementation includes: `6be7f67f2dc0a214d525625449c67779a1f1188c`.

Verdict: APPROVE corrected source for focused compilation qualification. No remaining
confirmed blocker in this bounded review. No compiler, numerical experiment, test
execution, market-data read or other worktree mutation was performed. Source approval
is separate from runtime convergence, legacy numerical parity, scale and tradeability.
Root is deliberately deferring new R3 behavior until the build-boundary baseline.

## Finding R3-R1, high: standalone impact reference indexed without shape admission

The new costed fa_compile book-consistency loop read C.turnover_ref[i] whenever
C.impact.active. check_problem and validate_relative_metadata only admitted that
reference for has_turnover or a positive scalar turnover penalty. Concrete admitted
shape: M=2, valid box constraints, no turnover budget/penalty, active quadratic impact
coefficients {.1,.1}, C.turnover_ref={0}, terms.w_prev={0,0}. The i=1 comparison reads
past the supplied reference. The actual PortfolioOptimizer bridge builds a complete
reference, but the public solve_with_costs/solve_factor_admm APIs also accept direct
MaterializedConstraints inputs and therefore remained affected.

`fc80a5ee` closes this before fa_compile: for active impact a nonempty reference must
have exactly M finite elements. Empty remains the declared flat reference; the later
book-equality check still rejects a nonzero cost book that disagrees. The added case
covers the reported short vector, a full-length NaN vector, and the valid empty-flat
branch. Its assertions are inspected source, not observed runtime passes. Legacy
non-costed admission/arithmetic is unchanged by this V2-only repair.

Separate include hygiene finding: cost_terms.cpp used FactorAdmmConfig and
solve_factor_admm via a transitive qp_solver.hpp include that root's private solver
extraction removes. `6be7f67f` supplies direct qp_factor_admm.hpp and admm_schedule.hpp
includes. This is an integration/build-boundary repair, not an algorithm change.

## Inspected numerical and execution contracts

Trade costs use NAV-return units: linear one-way spread/commission times absolute
weight change, three-halves impact times absolute change to power 1.5, and a holding
fee on max(0,-w). The scalar trade proximal equation reduces to
s^2 + (1.5*c/rho)*s - (abs(v)-kappa/rho) = 0, with delta=sign(v)*s^2 after the no-trade
threshold. The implementation uses a rationalized/exponent-separated form, including
the representable subnormal large-ratio branch, rather than subtracting two nearly
equal roots. The independent fixture uses a derivative-root oracle and an explicit
subnormal case; actual compiler arithmetic remains to be qualified.

The holdings prox is asymmetric: it soft-thresholds the positive leg by the gross
multiplier and the negative leg by that multiplier plus borrow/rho, then clamps to
boxes. A monotone multiplier search enforces the gross ball. Equality-row rho boosts
only affect fixed box coordinates whose projection is fixed independently of rho.
The trade prox similarly combines scalar and per-name linear costs, three-halves
impact, and an explicit turnover-ball multiplier around the actually held book.

Untradeable and finite nonpositive max_trade inputs become exact prior-holding pins.
Positive max_trade becomes an absolute change box. Observed locate caps intersect
the lower holding bound; incompatible pins/locates or original holding boxes fail.
The old economic-only view remains distinct from executable_view. V1 explicitly
refuses newly supplied execution-limit spans instead of silently ignoring them.
Missing alpha in solve_surface becomes zero expected-return tilt while retaining
the prior holding and its costs/constraints.

V2 retains the factor-space Woodbury solve, validates cost/config/shape/finiteness
before its new work, and admits combined scratch before problem compilation. It
refuses unsupported cones/elastic constraints. It does not run the scalar-only
legacy polish or silently fall back to another solver. The final book is pinned/
clamped to execution bounds, then the complete original economic set is rechecked.
Final finite state/objective, consensus and stationarity are recomputed after that
adjustment; requested absolute/relative residual tolerances must pass even if an
iteration cap is reached. A short run is an error, not an asserted converged book.

The actual PortfolioOptimizer::solve_surface interface checks model/surface numeric
instrument order, held-book geometry, decision-time snapshot and positive NAV via
the adapter. It materializes explicit sparse hard constraints, forwards configured
risk/turnover terms, and calls only FactorProxV2. Surface recipe/snapshot hashes,
borrow policy and unavailable-liquidity pin count are returned. Modeled borrow must
be available under the default policy; disabling it is explicit. Borrow estimates
do not assert a locate, and observed locate limits remain separate caller inputs.

The fixture compares execution charges to CostSurface quote_weight_change for the
same returned quantity/NAV and checks annual borrow conversion, source order/time,
missing liquidity and unavailable borrow. This binds the intended consumer behavior
without claiming actual fills, real borrow availability, or calibrated expected
returns. No new application-stage default, complete solve-run manifest, benchmark
result or empirical capacity/alpha qualification is inferred from this API slice.

## Mechanical extraction and remaining qualification

A read-only text comparison against the two frozen commits independently confirmed
that ec6719b2's entire private detail namespace matches 65bdf3da after removing inline,
and solve_factor_admm's body matches exactly with LF newlines. Public config/output
layout and helper declarations remain available; arithmetic changes belong to the
separate production commit and were reviewed above. This is a source comparison,
not a compiled bit-parity or compile-latency measurement.

Seven owning cases are prepared: scalar prox/oracle/extreme scales; heterogeneous
costs and explicit V1 comparison; hard pins/caps/contradictory locate; gross/turnover
and nonconvergence/budget refusals; malformed standalone impact reference; actual
PortfolioOptimizer/CostSurface units/axis/held-book composition; availability and
explicit borrow policy. All remain uncompiled/unrun in this independent review.
Root owns the single combined build and focused numerical/legacy qualification.
