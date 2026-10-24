# Bounded homogeneous centrality-correction primitive

This private component implements the predeclared direction experiment in
`../joint-centrality-plan-v1.md`. The plan was frozen at `eff495da` before code;
its unchanged content is rebased at `7f2d7a97` onto local-main checkpoint
`b3b2ca19`. The primitive has no production dispatch or whole-QP caller yet.

The caller supplies positive current slack/multiplier vectors, the complete
existing Newton direction, and disjoint preallocated scratch. The synchronous
callback solves a homogeneous primal/dual correction against the caller's
existing matrix factorization. It receives complementarity residual `rc=-t`;
it must write all three scratch directions on success. A callback refusal
discards its entire correction. Callback exceptions retain their ordinary
propagation contract; this component adds no allocation-recovery promise.

Each of at most three attempts computes the current primal and dual positivity
step limits, capped at one. If both already equal one, no solve is needed. At
trial steps `min(old_step+0.1,1)`, complementarity products are projected into
`[0.1*mu_target,10*mu_target]`. Negative trial products are allowed when forming
this target; the current iterate is never overwritten by a trial point.

The combined correction is accepted only if both step limits improve by at
least `0.01`, capped at one, and at least one increases strictly. All three
direction arrays are replaced together. A nonfinite trial/correction, zero
target, failed solve or insufficient step improvement stops correction work
and retains the last complete accepted direction. Unrepresentable target
intervals skip the experiment, preserving the valid original direction.

The component allocates nothing, reads no clock, performs no factorization,
and changes no precision, feasibility tolerance or outer iteration bound.
Counters distinguish attempts, actual solver calls, accepted directions and
each refusal/skip cause. Shape, finite-direction and positive-iterate checks
run before the first solve; invalid input returns false and resets counters.
Disjoint live spans are an internal caller precondition.

Nine focused controls pass. Two solve independent complete KKT systems rather
than reuse the reduced normal equations: a one-variable system checks the
correction sign and three-solve cap, and a coupled two-variable/three-constraint
system checks unequal dimensions and preservation of homogeneous primal/dual
residuals. Other controls cover saturated steps, zero corrections, one-sided
step improvement, nonfinite values, malformed input, target underflow/overflow,
and a later solve refusal after an earlier accepted correction. The current
slack and multiplier vectors remain unchanged throughout.

The first four controls failed at runtime against a refusal stub
before implementation, then passed. Four additional boundary/refusal tests and
the independent coupled KKT test extend that evidence; their earlier logs are
retained. The owning source and test translation units are checked without PCH
before the source-review checkpoint is frozen.

Evidence directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/centrality-research-v1/`.
`red-provenance.json`, `red.xml`, `green.xml`, `complete.xml` and `coupled.xml`
retain the successive controls. `coupled-build.log` and `hygiene.log` record
the owning builds. The external component receipt pins final source and tests.

Still required: independent source review; default-off whole-QP wiring with
reused factors and complete work accounting; full normalized-SPY solve and all
13 scalar actual-API gates; and a separately admitted complete-fit timing
comparison. Passing this primitive does not show fewer factorizations,
convergence acceleration, a changed fit, or any latency improvement.
