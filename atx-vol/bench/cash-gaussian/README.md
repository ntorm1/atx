# Independent stock-state Gaussian reference component

Private research only, default off through `ATX_VOL_BUILD_CASH_GAUSSIAN_RESEARCH`.
This component does not replace a production pricer, the cash reference, or any
of the original 50-case / 150-grid qualification outcomes. It implements the
first operator controls from `docs/cash-dspyp-reference-study-20261024.md`; no
American pilot, benchmark or accuracy-qualified serving policy is included yet.

`StockPwl` owns a continuous piecewise-linear function in stock price on
`[0,infinity)`, with an explicit zero knot and an explicit linear infinity tail.
It permits nonconvex values. Construction validates order, finite values/slopes
and arithmetic before publishing an object. Const spans borrow the object's
storage. Default or moved-from empty objects refuse evaluation. No borrowed
PDE grid, curve, jump map or cached pricing state is retained.

`expectation` integrates every represented interval using its Gaussian mass and
first truncated moment. Same-tail erfc differences and across-zero erf
differences reduce cancellation; compensated summation combines the intervals.
All intervals and the infinite tail are visited, even if their rounded
contribution vanishes. There is no probability cutoff, moneyness cutoff or
normal-CDF approximation borrowed from the existing pricing kernels. Exact-zero
stock, zero time and zero volatility have separate branches. Rates and yields
may be negative. Unsupported finite arithmetic returns `OutOfRange`; no
clipping turns it into an accepted price.

`liquidator_jump` constructs the algebraic composition `f(max(S-D,0))` by adding
the zero branch and shifting stock knots by the original cash amount. Shifts use
binary64 arithmetic; overflow or collapse of distinct knots is refused, never
quantized or silently merged. This is independent of the PDE/lattice `jump_at`,
cell averaging and interpolation. Exercise is not inserted in this first component.

The caller owns a cumulative `Budget`: maximum knots per object, total newly
created knots, expectation calls and interval evaluations. Preflight checks
prevent a limit from producing a partial result. Completed work stays counted
after a numerical refusal. All counters are untimed; node counts describe
published objects, not allocation attempts. Limits cover these operations, not
arbitrary copies a caller makes outside them. Inputs and the budget must not be
mutated concurrently. No global cache or retained allocator is used.

## First frozen controls

Ten test groups pass:

- Constants and linear stock functions check discounted mass and first moment
  across 72 models, including far tails and negative rates/yields.
- A nonconvex stock-value function matches an independently assembled hinge
  expansion using Black-76 option prices.
- 324 European no-cash models cover both sides, short/long maturities, low/high
  volatility, moneyness, negative/zero/positive rates and continuous yield.
- Liquidator composition retains the zero branch and handles cash above spot.
  Fifty-four single-cash-at-payment European controls match independent call
  and call-spread identities.
- Zero stock, zero volatility and zero time have explicit behavior. A retained
  runtime RED found overflow in a rate difference at zero time; the corrected
  identity branch does not compute that irrelevant drift.
- Invalid/moved-state inputs, arithmetic overflow, collapsed cash knots and all
  cumulative resource limits refuse without publishing a partial result.
- An interior-cash European control integrates a separately evaluated Black-76
  post-cash value against the pre-cash normal density. Test-only Simpson rules
  at 8192/16384 subdivisions agree within `2e-10`; their omitted tails have an
  explicit payoff/moment bound of `1.5405894201854106e-30`. This bounded test
  quadrature is separate from the component's complete represented-domain integral.
  Its represented spatial values at 1024/2048/4096 intervals converge to that
  control; the finest error is `2.6022861e-5` USD for both sides. This is spatial
  representation error, not a certified bound or an American result.

The scalar Black-76 function is used **only by the tests**, including alternative
identities. The component source calls no production pricing function or
reference-PDE helper. The analytical moment formula alone does not certify the
future approximated value curve, exercise-time error or tail representation.

Evidence: `calendar/cash-gaussian-controls-v1/` under the r7 cache. It retains
seven initial stub RED failures, the subsequent zero-time RED, all successful
receipts and the archived final executable. Both owning translation units pass
PCH-off checks. Test runtimes in gtest output are not qualified performance data.

```powershell
./scripts/atx-build.ps1 configure -Preset vol-rel '-DATX_VOL_BUILD_CASH_GAUSSIAN_RESEARCH=ON'
./scripts/atx-build.ps1 build atx-vol-cash-gaussian-research-tests -Preset vol-rel -Jobs 2
./build-vol-rel/bin/atx-vol-cash-gaussian-research-tests.exe
```

## Next bounded step

Freeze and review these operators before running the predeclared American pilot.
The next private owner will independently normalize events, construct the stock
grid, apply explicit exercise/cash steps and retain every pilot outcome. The
event-only call domain must explicitly require nonnegative rates and zero
continuous yield. General puts use nested exercise dates; no call-domain theorem
is silently applied to them. The original all-50 input manifest and all-150
baseline results remain unchanged. Other cash models receive explicit unsupported
or uncomputed status until independently implemented.

## Independent event owner

`EventOwner` owns raw and normalized events, its fixed stock grid and descending
exercise clock. It independently validates the liquidator model, stable-groups
exact-equal event dates and applies the documented relative expiry window. It
uses no PDE/Markov adapter, grid, jump, interpolation or value. Calls permit an
event-only American clock only for nonnegative rates and zero continuous yield;
puts use the independently nested uniform exercise clock in `pilot-plan.md`.

The PWL maximum inserts every finite or infinite-tail crossing with intrinsic.
Nonconvex continuation functions are allowed. A strict crossing that cannot be
represented as a distinct binary64 stock knot returns a typed refusal and counts
the failed intersection; no epsilon or time value is inserted. Zero volatility
uses affine knot/value rollback. Zero stock stays absorbing. Positive diffusion
evaluates every represented interval on every fixed stock node, then propagates
the linear infinite tail. True-tail and repeated-interpolation approximation
remain unresolved; neither a rigorous price enclosure nor a Bermudan bound is
claimed for this finite representation.

The owner resets caller-owned work at each price call, then retains completed
expectations, interval contributions, constructed knots, exercise projections,
cash jumps and steps even on refusal. Preparation bounds raw events, grid size
and clock size before large iteration. It does not turn any refused intermediate
state into a partial price. The frozen 32-outcome pilot is correctness-only;
wall-clock benchmark claims require a separate reviewed experiment.

Before any American pilot price, the unavailable-owner stub failed all seven
initial controls. The implemented owner and three additional adversarial groups
pass 10 owner tests plus the 10 unchanged primitive groups; both new owning
translation units pass PCH-off checks. The owner checks include deterministic
forward-clock enumeration, European Black76 identities, an interior-cash
independent quadrature comparison, nonconvex/tail crossings, finite overflow,
moved owners, reset diagnostics and cumulative work refusal. The first PCH-off
attempt needed CMake regeneration for the new files; that failure is retained.

## Fixed pilot runner and independent reader

The same default-off CMake option builds `atx-vol-cash-gaussian-pilot`. It accepts
no command-line tuning. It serializes all 50 original inputs and the declared
32 outcomes, including preparation/rollback refusals, exact price bits, full
normalized events, exercise clock, stock grid and work counters. It does not
read any reference value. Unselected original cases are explicitly uncomputed.

`pilot_audit.py` pins the unchanged historical 50-case/150-grid reference file
by SHA256. It joins every raw model/event bit, reconstructs all event clocks and
fixed spatial grids, validates resource/count and status consistency, retains
every refusal and reports the separate spatial/time axes. A 32-ULP allowance
is used only to reconstruct C++ libm/lerp grid/clock values in Python; raw input,
cash amounts, event endpoints and serialized output bits receive no such slack.
The 0.001 comparison includes the old reference's empirical uncertainty. Passing
it neither promotes this engine to a reference nor establishes a rigorous bound.

```powershell
& C:/atx-wt/pool-5/scripts/atx-build.ps1 configure -Preset vol-rel `
  -DATX_VOL_BUILD_CASH_GAUSSIAN_RESEARCH=ON
& C:/atx-wt/pool-5/scripts/atx-build.ps1 build atx-vol-cash-gaussian-pilot `
  -Preset vol-rel -Jobs 2
python -m unittest discover -s atx-vol/bench/cash-gaussian -p test_pilot_audit.py
# Archive the clean source/executable identity, then invoke that binary with no
# arguments in a new receipt directory; stdout is JSON, stderr lists every result.
python atx-vol/bench/cash-gaussian/pilot_audit.py --reference <frozen-replay.json> `
  --report <new-native.json> --output <new-summary.json>
```

## Predeclared representation follow-up

`followup-plan.md` freezes 18 no-cash/no-exercise European semigroup outcomes and
the five exact previously refused crossings. `repeat_expectation` consumes a
PWL and repeatedly applies the unchanged integral on a fixed stock grid. It
performs every requested query and interval even for constant/linear inputs;
all work and resource refusals remain counted.

The event owner now accepts a default-null private `CrossingObserver`. It is
called only at an existing rounded-crossing refusal with the already computed
operands and borrowed const PWL spans. It computes no extra price, changes no
decision and must not throw. A collector must copy retained spans and mark its
own failure without changing the original result. This is an observational
research seam, with no production API or default numerical-policy change.

Four initial stub/unwired-observer tests fail at runtime; after implementation,
all 25 native groups pass, including six repeated constant/linear checks, exact
work identities, independent European values, partial resource failures and
finite/tail/owner observer parity. Three owning source checks pass with PCH off.
These controls precede the new 18-outcome numerical run; neither the prior 32
prices nor their accuracy classifications are replaced.

Peer review found that the consumed input could itself own the borrowed grid.
The repeated driver now takes a bounded owned grid copy before replacing any
PWL, and the alias/disjoint-grid regression checks equal values and work. The
ordinary allocator left freed bytes readable in the retained pre-fix test, so
that run passed and is **not** described as runtime-red evidence. The ownership
defect is established by source lifetime analysis; no ASan result is claimed.
`grid_nodes` also states the size of this setup copy, separately from constructed
PWL-knot counts. No follow-up pilot result was produced before this correction.

`atx-vol-cash-gaussian-followup` is a separate fixed, argument-free native
diagnostic. Its 18 European results have no cash or exercise; the original raw
events remain in the full-50 manifest. Its five observer attempts must exactly
match the original refusal text/code and every work count. The native tool
receives no baseline file or price. Captured PWL data are copied during the
synchronous callback; no borrowed spans escape to serialization.

`followup_audit.py` pins both original reference and prior 32-outcome pilot hashes.
It independently evaluates Black76, checks exact full semigroup work identities
and reconstructs the captured PWL slope/subtraction/fused evaluation before
reconstructing each crossing at Decimal precision 100 and 200. It reports the
nearest binary64 point and local affine-line chord excess, with no full-price
bound. The one-step Black76 diagnostic uses a fixed 5e-10 absolute check,
declared before the 18 results; every multi-step residual remains visible.
Three reader controls cover complete/refused denominators and 18 malformed
model/work/capture mutations. The original 32 numerical outcomes are unchanged.

The single frozen follow-up run completes all 18 European controls and preserves
all five prior crossing refusals. Six one-step values match Black76 within
1.43e-13 USD, while all twelve repeated controls exceed 0.001 USD error. Decimal
reconstruction places every captured crossing nearest to an existing endpoint;
no rounding treatment is applied. See the
[full diagnostic report](../../docs/cash-gaussian-followup-review-20261024.md)
and [prospective cubic plan](cubic-plan.md). The cubic representation is design
only; no new numerical implementation, American reference or timing result is
included.

## Stage A local cubic-weight control

`cubic-moment-plan.md` predeclares a separate 12-cell/2-precision study, including
adjacent high/low stock and positive near-zero time. `cubic_weights.hpp/.cpp`
uses the installed Boost headers only in explicit research targets. It computes
scaled Bernstein price/derivative weights at fixed 100 or 200 decimal digits,
returns decimal strings and counts actual moment/normal-function/coefficient
work. Unsupported numerical domains, conditioning and budgets return typed
refusals; no tails are dropped and no rigorous transcendental bound is claimed.

Three stub tests failed at runtime. Six new native controls plus the unchanged
26 Gaussian controls now pass; three owning TUs pass with PCH off. The private
probe exports all24 declared precision/cell outcomes, and its independent reader
uses positive-integrand SciPy quadrature, input bits, partition identities and
two-precision comparisons. Its malformed and common-bias controls require an
explicit native fixture via `ATX_CUBIC_WEIGHT_CONTROL_FIXTURE`.

This component does not construct a new option curve, run the18 semigroup
requests, perform American exercise, establish Stage A completeness or replace
the previous negative PWL receipts. Full-domain/tail, repeated-polynomial and
owner/derivative-kink controls remain necessary before progression.

The first frozen24 export is negative: all24 compute and all168 independent
positive-integrand quadrature checks pass, but adjacent-high/low/unit fail the
unchanged1e-40 precision-agreement gate. The numerical reader and original
summary remain unchanged. A test-only correction permits a valid negative
fixture instead of requiring every fixture to pass its numerical gate.
See the [complete result](../../docs/cash-cubic-weight-review-20261024.md) and
[prospective direct local-weight series](cubic-local-series-plan.md). The latter
freezes a positive-integrand construction, truncation and work caps before
implementation; it does not authorize the18 semigroup run.

The explicit `cubic_local_weight_control` primitive now implements that frozen
construction with separately owned work and route metadata. It forms the seven
weights directly from local density, retains raw moments only for diagnostic
identities, and labels the original-moment route when the analytic/order domain
does not admit the series. Caller resource exhaustion never selects fallback.
The original entry point and v1 producer remain unchanged in meaning; a focused
default-route regression reproduces the entire old24 JSON byte-for-byte.

Three stub controls failed at runtime; ten new local-series groups plus all32
previous groups pass. Controls independently check rational integrals through
beta moments, both precisions across stock scales, positive-integrand Simpson
quadrature, cubic value/Delta identities, first admissible orders/remainders at
250 digits, exact original-route fallback and typed low-budget/invalid cases.
Two owning translation units pass with PCH off (the numerical owner is compiled
for both research targets). Source handles allocation failure; no injected
allocator-failure test or sanitizer result is claimed by this component.

`workspace_bytes` is a conservative construction reservation including fixed
scalar/short-array temporaries, not a measured heap/stack peak. Actual selection,
special-function and coefficient-product counts are separate. Reported remainder
strings cover real-arithmetic series truncation only, evaluated at fixed
precision; they exclude arithmetic/transcendental and complete-price errors.
Neither Stage A completeness nor18-request progression is established.

Peer review of3ece878 found two diagnostic-contract gaps: only the maximum
condition survived publication, and local probes could fall back before their
scalar workspace was charged. Both defects reproduce runtime RED. The narrow
follow-up publishes all seven local-series ratios and reserves bounded scalar
workspace before any positive-lower local arithmetic. Width/variation/order
nonadmission keeps that reservation; a selected-buffer refusal retains only
completed scalar reservation. Zero-lower direct original routing still uses
no local scalar workspace. All45 native groups and owning2-TU PCH-off checks
pass after correction; the original failed review and prior receipt remain.
No successor24 export or18-request run was performed.

## Successor local-series receipt tool

`atx-vol-cash-cubic-local-control` is a separate, default-off research target.
It uses the exact original twelve cells at both precisions and emits all24
outcomes, including every refusal. It accepts no input file or numerical knob.
The schema pins the reviewed declaration and primitive, and records decimal
weights, all seven conditions, separate value/derivative truncation formulas,
route/reason, selected orders and actual work. Compiled scalar object sizes
make the conservative 256-object scalar and 2312-object full reservations
explicit; these remain reservations rather than measured stack/heap peaks.

`cubic_local_audit.py` is a separate reader; the original numerical reader,
native24 bytes and failed precision decisions remain hash-pinned. The reader
checks all24 input bit patterns, all12 unchanged1e-40 precision gates and
all168 positive-integrand quadrature comparisons at the original tolerances.
Original-route weights and raw-moment diagnostics must equal the frozen
original decimal outputs exactly. Every failure remains in the denominator.
It does not convert a truncation formula into a complete-price error bound.

The reader reconstructs order selection and real-arithmetic truncation formulas
at Decimal precision250, using direct powers/factorials independently of the
native recurrence. It forms unscaled polynomial powers before factorial
division and exact rational Bernstein integrals to reconstruct all seven
condition ratios. The absolute prefactor uses an independent pi construction:
`pi=16*atan(1/5)-4*atan(1/239)`, with fixed224/64 terms of the
[alternating arctangent series](https://dlmf.nist.gov/4.24.E3). The identity
follows because `tan(4*atan(1/5))=120/119` and subtracting `atan(1/239)` gives
tangent1 in `(0,pi/2)`. Its alternating truncation is below1e-300; this is
separate from Decimal rounding, transcendental evaluation and pricing error.
Exact beta-integral and independently specified pi-digit controls test this
reader-only construction. No dependency or production numerical path changes.

Three derivation stub tests and two receipt-reader tests failed at runtime
before implementation. The test-only synthetic protocol fixture deliberately
retains the historical negative weights while supplying independently derived
local metadata; it is not a native successor result. The reader must report
the same three failed precision controls. Ten new Python groups cover this
negative fixture,22 malformed mutations, work/resource/refusal accounting,
balanced weight corruption detected by independent quadrature, and exclusive
output creation; the four unchanged reader groups also pass. The initial
test import syntax error remains logged separately from the runtime failures.

The exporter compiles and its owning translation unit passes with PCH disabled;
only its invalid-argument exit has been executed, producing no numerical rows.
The initial hygiene lookup failed because the new target had not been configured;
the explicit configure and subsequent owning-TU check both pass.
The numerical primitive remains frozen
at8436a3b. The successor24 export is held for fresh source review, and no18
option semigroup run is authorized by this tool. The original50 cash cases,
150 PDE outcomes,32 Gaussian pilot outcomes and18+5 follow-up remain unchanged.

After all three source approvals, the single frozen successor24 run completes
all24 outcomes and passes all12 unchanged precision checks and168 quadrature
checks. Eight outcomes use the direct local series;16 retain exact original
weights. The three previous precision failures remain preserved, while their
successor differences fall below1.66e-81. See the
[complete result and work receipt](../../docs/cash-cubic-local-weight-review-20261024.md).
This does not complete Stage A or authorize the18-option semigroup run.
