# Stage B bounded working-set projection: interface and test contract

This implements the parent-approved Stage B in the c2008138 plan after the full
Stage A census. It changes no model, quote box, variable, input eligibility,
price-accuracy budget or public API. The ordinary default remains the original
full solver. No centrality, QN, new factor backend or artificial strict margin
is combined with this experiment. No candidate SPY execution occurs before
fresh exact-source review of the implementation and its focused controls.

## Dispatch and ownership

Add the default-OFF CMake option `ATX_VOL_JOINT_WORKING_SET_RESEARCH`, confined to
the owning projection translation unit. Its linked-library getter reports the
actual compiled mode in benchmark build provenance. Private
`JointConstraintMode { Compiled, Full, WorkingSet }` is a final defaulted
argument to `project_joint_in_box`, after the existing optional observer/raw
source arguments. `Full` always uses the original path. `Compiled` resolves the
option. Unsupported explicit WorkingSet or invalid modes refuse before writes.
The enabled option is incompatible with centrality and external research QP
backends. The Stage A census CLI refuses WorkingSet-compiled mode; its frozen
matrix-profile semantics describe the original full solve.

The existing full Problem remains immutable and authoritative after construction.
The optional wrapper owns its selection mask, original-index list, reduced-row
storage, trial iterate and certificate outputs. It keeps every free variable
and every original bound row. Allocation/resource failure in this optional
work invokes the original full solver at the original selected target. The
ordinary solver's own allocation/error behavior is not caught and relabeled.
Candidate or partially certified nodes never reach caller-owned storage.

## Exact bounded algorithm

1. Build the original full problem and its unchanged initial clamped target.
   Scan every stored row with the original `Row::value` operation. Select all
   rows with slack `< -1e-10`, plus every bound row. Preserve original row order.
2. Solve that row subset with the **existing** infeasible-start solver, the same
   full target and original per-solve iteration budget. Do not start from a
   previous approximate/reduced iterate. There are at most three reduced solves,
   each with the original caller budget (100 for this SPY study).
3. A failed/incomplete reduced solve is not an admissible partial result. Invoke
   the original full fallback immediately. After a completed solve, scan all
   original rows at the unclamped iterate and at the exact publication clamp.
   Add every violating omitted row in original index order, retaining prior
   selected rows. Never delete or use only a favorable subset of violations.
4. Only a successful all-row published scan may proceed to the existing full
   slice/non-dividend/calendar certificates. Certificate refusal falls back to
   the original full problem. A lack of newly added rows with remaining
   violations also falls back; it cannot spin or certify an invalid result.
5. After the third unsuccessful full scan, make at most one full fallback solve
   with the original target and original iteration budget, followed by the same
   existing certificates. Its result determines success/refusal. At most four
   solves / 400 iterations are allowed on this fixed SPY configuration. All
   their setup, scans, factors, directions and cleanup are charged.

The constraint wrapper is bounded to the already frozen 14,000-variable /
60,000-row research capacities; larger problems take the original full path.
There is no artificial interior perturbation. Boundary-only feasible sets and
fixed coordinates retain the existing treatment. Selection is deterministic
from the complete current row set; it uses no vendor labels or excluded slices.

## Evidence and counters

Add a private fixed-size four-slot work record: mode, reduced/fallback attempts,
selected rows, per-solve iterations and original outcome; full-scan row visits,
newly added rows, worst full slack before/after clamp; numerical, certificate,
setup, allocation and capacity refusal reasons. Existing solver subclocks and
factor/direction counts accumulate across every attempted solve. Matrix/factor
storage maxima are labeled as maxima across actual systems, with per-slot sizes
retained. Full-problem row counts/arities stay separate from reduced counts.
The outer total clock includes setup, row copies, scans, failed work, fallback
and destruction. Successful no-op/full-target cases must not allocate the
optional working-set workspace unnecessarily.

The returned sweep count is the sum of attempted solver iterations; the
experimental contract explicitly treats `max_iterations` as a per-solve cap.
No measured iteration or direction is hidden by resetting a child profile.
Counter instrumentation must not change numerical admission. The standalone
control path and tool report expose linked mode and actual work independently
of an optional timing profile.

## TDD gates before one candidate SPY run

Start from a deliberately Full-dispatch stub and record meaningful runtime RED
for selection/addition/counters. Tiny coupled systems must compare against an
independent dense feasibility/KKT calculation, not just another call to the
same solver. Test an omitted constraint becoming violated, multiple rounds,
publication-clamp checks, fixed coordinates, already-feasible/no-iteration
inputs, narrow and boundary-only feasible boxes, and a valid different solution.

Exercise every bounded fallback: reduced nonconvergence, no-progress scan,
round cap, certificate failure, finite overflow, malformed mode, capacity and
allocation refusal. Use explicit small private controls for otherwise hard-to-
reach failure phases and label them as synthetic; preserve actual successful
and ordinary fallback paths. Verify caller nodes remain unchanged when the full
fallback also refuses. Run the existing owning projection tests with Full and
with the default OFF build, plus owning PCH-off checks and the independent
acceptance readers. No new tolerance is introduced to make a control pass.

Only after fresh review run the canonical SPY once, retaining every outcome,
and apply all thirteen actual-API gates to its own final representation. Changed
valid prices/IVs or floor targets are permitted by the economic contract; output
bit identity is diagnostic. Cold price residual, truthful floor/PriceOnly
semantics, all-valid coverage, original-box containment and all arbitrage checks
remain mandatory. Timing requires a later coordinated same-contract comparison
with retained outputs/proofs, strict host gates and all-run retention.
