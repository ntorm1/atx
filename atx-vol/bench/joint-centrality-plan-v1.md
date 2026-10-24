# Prospective joint-feasibility centrality-correction experiment

Base: local main `5df44c591cf487f4f915699d127193713b954777`. This separate
research branch changes the search direction, not the price constraints,
economic accuracy contract, iteration budget or publication certificates.
Ordinary selection stays unchanged. No new numerical result exists yet.

## Evidence and hypothesis

The existing SPY projection has 12,866 free variables, 54,104 retained constraint
rows, 35 numeric factorizations and 70 directions. Archived baseline attribution
in `calendar/qp-profile-baseline-v1/receipt.json` charges about201ms to factors
and38ms to triangular solves in its first run. These are scoped historical
component observations, not current qualified cold-fit timings. The latest
instrumented full capture attributes780ms to calendar work on a contended host.
It confirms where work remains without establishing a latency comparison.

Existing ordering, packed-factor and FP32/refinement studies are retained; none
is reclassified. The separately reviewed sequential double block backend remains
a distinct experiment. This experiment asks whether additional inexpensive
solves using an existing factor can reduce the number of factors required for
the same feasible-surface contract.

[Colombo and Gondzio, sections2.2–3](https://webhomes.maths.ed.ac.uk/~gondzio/reports/mcjgMCC.pdf)
motivate correcting extreme trial complementarity products while reusing the
Newton matrix. This is an adaptation to ATX's inequality QP and feasibility
stopping rule, not an inherited convergence theorem or a speed guarantee.
The archived PDF at `centrality-research-v1/colombo-gondzio-mcc.pdf` has SHA256
`9b9163bff776404745ad27b6f4997b46b92a3a29a2ff2a28956ffa92dece69ad`.

## Fixed algorithm before any SPY outcome

Start with the unchanged Mehrotra predictor/corrector direction `(dx,ds,dz)`.
Retain the current positive `(s,z)`, the current factor of
`I + A^T diag(z/s) A`, and `mu_target = sigma*mean(s*z)` from that iteration.
Allow at most three additional attempts, with fixed constants gamma=0.1,
aspiration delta=0.1, and minimum accepted step improvement=0.01. No threshold
or maximum is selected after looking at SPY results.

For each attempt, compute the original positivity bounds `ap,ad` capped at1.
If both equal1, stop. Trial steps are `min(ap+0.1,1)` and `min(ad+0.1,1)`.
For each constraint row, form its trial product `v=(s+ap_trial*ds)*(z+ad_trial*dz)`.
Set `t=clamp(v,0.1*mu_target,10*mu_target)-v`. Negative trial products are
permitted; the trial point is only used to construct a direction. Reject a
nonfinite/nonpositive target or nonfinite intermediate without changing the
existing accepted direction. All-zero `t` ends correction without a solve.

Solve the homogeneous primal/dual correction system using the existing factor:
`ddx = H^-1 A^T(t/s)`, `dds = A*ddx`, `ddz=(t-z*dds)/s`.
Equivalently, the existing direction routine receives zero primal/dual residuals
and complementarity residual `-t`. This sign is tested independently against a
dense KKT solve, not inferred from agreement with the same helper.

Form candidate directions in separate preallocated storage. Accept only if both
new positivity bounds are at least `min(1,old_bound+0.01)`, with at least one
strict increase. Otherwise discard the entire candidate and stop correcting.
Every accepted candidate replaces all three directions together. The unchanged
0.995 fraction-to-boundary multiplier is applied once, at the final update.
Never change current iterates during a trial. A refused/nonfinite correction
retains the previous valid direction and cannot bypass the original residual,
positivity, iteration-limit or final certificate checks.

## Bounded work, ownership and observation

Allocate correction scratch once per cold projection and charge setup/destruction.
The original bound on outer iterations remains; each factor has at most five
direction solves, including predictor and corrector. Record attempted/accepted
corrections, failed/nonimproving/zero-target stops, actual extra solves, scratch
bytes, and inclusive correction time. Rejected correction work remains charged.
The normal factorization and direction totals include all work; nested correction
clocks are not added to their parents. No fast-math or precision change.

Use an explicit private, default-off research selection and immutable benchmark
provenance. No environment-based silent production toggle. A refused experiment
does not publish provisional nodes or silently become evidence for the baseline.

## Gates and fixed denominator

First write runtime-failing controls for the homogeneous KKT sign and residual,
both-sided step acceptance, saturated steps, rejection without mutation,
nonfinite values, zero correction, and the three-attempt/five-solve bounds.
Preserve ordinary solver output and all existing projection/refusal controls.
Review the exact component SHA before whole-SPY execution.

Then run the existing complete normalized SPY projection once, retaining every
attempt, solve, refusal, constraint, final certificate and price. Compare actual
factor/direction counts and complete inclusive component work against the
unchanged original path; do not use microbenchmarks as a full-fit claim. Any
new prices must then pass all13 independent actual-API gates on all33 expiries
and12,866 eligible rows, including original raw-spread monetary budgets,
containment, identification preservation and between-node arbitrage checks.

Only after correctness and fresh source review may an archived ThinLTO build
enter a paired, single-worker, quiet-host cold-fit comparison. Every trial stays
in the denominator. A negative result remains negative; combining this variant
with the separate block or proof-batch experiments requires its own exact-source
integration and complete actual-API audit. Subsecond and world leadership remain
unproven until their full requirements are verified.
