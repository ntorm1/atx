# Bounded homogeneous centrality correction

This private component implements the predeclared direction experiment in
`../joint-centrality-plan-v1.md`. The plan was frozen at `eff495da` before code;
its unchanged content is rebased at `7f2d7a97` onto local-main checkpoint
`b3b2ca19`. The original primitive checkpoint had no whole-QP caller; the
separately gated integration below starts from local main `a037b1e4`.

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

At the primitive checkpoint, remaining work was: independent source review;
default-off whole-QP wiring with
reused factors and complete work accounting; full normalized-SPY solve and all
13 scalar actual-API gates; and a separately admitted complete-fit timing
comparison. Passing this primitive does not show fewer factorizations,
convergence acceleration, a changed fit, or any latency improvement.

## Default-OFF whole-QP integration

`ATX_VOL_JOINT_CENTRALITY_RESEARCH` is separate from the standalone test option.
It compiles the actual projection caller and links the same reviewed primitive.
Private `JointDirectionMode::Original` bypasses every optional workspace/corrector
operation. `Compiled` resolves the linked solver's build mode; explicit Centrality
in an ordinary build, or an invalid mode, refuses as InvalidInput. Benchmark
provenance queries that actual solver, rather than a tool-local macro.

The callback reuses `NewtonSystem::direction` with zero primal/dual residuals.
Its seven vectors (four constraint-sized and three variable-sized) are allocated
once per projection. An optional pre-sized RHS argument avoids a new RHS allocation
for each correction; original callers retain the original expression and traversal.
Trial output and complementarity residual storage are disjoint from the current
iterate and accepted direction. The fixed correction rules, feasibility tolerance,
outer iteration budget and complete publication certificates are unchanged.

Optional workspace allocation failure disables correction for that solve and
retains the original path. A correction solve allocation failure rejects that
whole correction. Both are explicitly counted; these exception paths are
source-reviewed, not allocator-fault-injected. Normal factor/direction totals
include actual correction solves. Setup, correction and all failed work remain
inside inclusive solve/fit time. Nested profile clocks must not be summed into
their parent. Null profiling never affects algorithm selection or reads a clock.

The actual homogeneous control uses the production factor and callback, with
independent full KKT tests at unequal non-unit slack/dual values and the bounded
maximum test dimensions. Its n<=4/m<=32 limit is only for that private control,
not the real projection. Outputs publish transactionally; validation resets work.
Four valid-system tests first fail against a refusal stub. Six whole-QP tests
then expose missing correction work in the original-direction stub before wiring.

A further finite-input overflow control reproduces an existing numerical defect:
an infinite assembled Hessian can receive Eigen Success and produce a finite
but invalid direction. Both ordinary and research paths now refuse a nonfinite
Hessian before factorization, without changing any finite arithmetic or tolerance.
The first mistyped filter selected zero tests and supplies no pass/failure evidence;
the corrected one-test run fails before the guard and passes afterward.

All fifteen new actual-solver/dispatch controls pass. The complete enabled
quote-slice executable passes 361 tests with six explicit external/child skips.
Five changed owning translation units pass PCH-off compilation (eight object
targets, including shared test owners). Source review must finish before the
first complete normalized-SPY projection or thirteen-gate actual-API audit.
No whole-SPY or latency result is claimed by these component checks.

Evidence is in the directory above: `qp-green.xml`, `wired-green.xml`,
`overflow-red-v2.xml` plus its executable/patch/provenance, `complete-green.xml`,
`complete-quote.xml`, and `qp-hygiene.log`. Independent stub evidence is under
`../acceptance/centrality-qp-control-v1/` relative to the evidence directory.
