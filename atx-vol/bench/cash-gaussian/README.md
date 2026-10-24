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
