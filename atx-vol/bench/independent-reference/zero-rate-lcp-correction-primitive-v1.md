# One-correction obstacle primitive: source-review checkpoint

The separate `one_correction` offline mode solves the captured failing obstacle
stage at its original tolerance. No PDE request or Greek reference decision was
run or changed. The prospective plan was committed as `9641ef8d` before code,
on isolated branch `research/vol-r7-lcp-correction-20261024` from `5df44c59`.

## Solver contract

`solve_obstacle_control` defaults to an exact delegation to the unchanged
`pde_reference.solve_obstacle`, including its result keys. `pde_reference.py`,
`greek_reference.py` and all archived pilot outcomes remain untouched. The new
mode is explicit and is not imported by the current PDE runners.

Each policy update selects the same rows as the original iteration, solves that
unscaled binary64 system once, computes its binary64 residual, solves the same
system for one correction, and adds that correction. Even a zero selected-system
residual receives exactly one correction solve. There is no row scaling, clipping,
changed policy predicate or looser complementarity tolerance.

Admission uses `max(abs(min(M*v-rhs,v-payoff)))` on the original equations. The
mode permits at most `n+2` policy updates and `2*(n+2)` linear solves. It checks
the initial vector and every completed update, including the final permitted
update. An optional nonnegative integer `max_linear_solves` can only reduce
this bound. Each update reserves a complete two-solve pair; a one-call remainder
cannot publish an uncorrected result. An already valid initial vector uses zero
solves even with a zero budget.

`ValueError` rejects invalid inputs and controls. `LcpCorrectionRefused` records
resource exhaustion, iteration exhaustion, linear-solve failure and nonfinite
intermediates. Its counters distinguish attempted policy/correction calls from
completed policy updates. A failure partway through a pair can therefore have
unequal solve counts. Numerical failures retain only the last finite checked
vector and its matching residual, or `None` for both when none exists. These
owned snapshots are read-only. Allocation failures are not given a new recovery
contract by this experiment.

## Captured-stage result

The committed fixture contains only six input arrays from synthetic contract
`zero_rate_closed_form-put-03`, `dirk_l/time-coarse`, obstacle stage 60. It has
8,191 rows and tolerance `1e-10`. Its JSON manifest records the original capture,
model, grid, whole-NPZ hash and each array's hash. It supplies no answer vector
or reference decision to the solver.

| Route / saved vector | Linear solves | Original binary64 residual | 100-digit residual |
|---|---:|---:|---:|
| Unchanged original, last checked | 8,193 | 1.532782789581688e-10 | 1.5331623080123043e-10 |
| Unchanged original, last produced | same run | 1.1461054327810416e-10 | 1.1449447866266177e-10 |
| One correction, accepted | 4 | 1.9895196601282805e-12 | 1.3967060171825459e-12 |
| One correction, budget of three calls | 2 | 1.0324185950594256e-10 | 1.0309835068015894e-10 |

The accepted control completes two policy updates. The limited-budget control
refuses before starting a second pair. All 8,191 original rows are independently
re-evaluated at 100 decimal digits from the exact binary64 inputs. These observed
residuals are not a forward-error bound or a PDE discretization certificate.

The original solver checks before updating; at exhaustion its reported residual
belongs to its last checked vector, while its last produced vector is different.
Both vectors still fail here. The new receipt saves them under separate labels
with independently recomputed residuals. It does not overwrite or reinterpret
the historical failed receipt.

## Validation and evidence

Thirteen new primitive tests plus two unchanged obstacle tests pass. The initial
three tests failed against a delegating stub; later invalid-tolerance and complex/
overflowing array controls also failed before their explicit validation fixes. Controls
cover the captured stage, exhaustive exact-rational small policies, exercised
and continued rows, one-row boundaries, mandatory correction equations, original
default parity, immutable inputs, finite snapshots, each numerical refusal,
partial budgets, attempt counts, the original update bound and stale-vector
adversaries. No full PDE tests or pilot runs are part of this checkpoint.

A retained intermediate test log has a failed one-update expectation for an
initially negative one-row exercise case: the unchanged tie rule needs two
updates there. The final one-row control starts at a positive value so both
tested policies require one update. No numerical code changed for that fixture
correction, and the general multi-update controls remain in the suite.

Evidence is under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/`:

- `lcp-correction-red.log`, `lcp-correction-controls-red.log`,
  `lcp-correction-array-red.log` and the preserved stub establish the failing controls.
- `lcp-correction-green-final.log` records all 15 passing tests.
- `lcp-correction-primitive-v1/` preserves the first replay and paired vectors.
- `lcp-correction-primitive-final-v2/` repeats that same single-stage replay at the
  final source and pins dependencies, source hashes and output vectors.
- `lcp-correction-final-receipt.json` joins the final source, logs and artifacts.

No C++ code, native build, pricing profile or fitting output changed. Python
syntax compilation and `git diff --check` are the applicable hygiene checks.
Fresh source approval is required before the planned twelve zero-rate PDE
requests. A complete 144-request experiment needs its own prospective approval;
neither this primitive nor the twelve controls can qualify the other contracts.
