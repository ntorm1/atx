# Fixed event-owner and American pilot declaration

Declared after primitive freeze `80ae1ca2`, before any American result. This
extends the eight-case pilot already specified in the DSPY reference report.
No new grids will be selected after observing prices. The scalar primitive and
all original 50-case / 150-grid receipts remain unchanged.

## Independent owner

The owner accepts the raw stock, strike, maturity, volatility, constant rate and
yield, side, exercise style and cash events. It has its own validated model and
event schedule; it does not call reference-PDE validation, event_schedule,
time_grid, grid construction, jump_at or interpolation. Sharing the original
raw input fixture and the documented `1e-12` expiry-date tolerance is intentional.

The independent adapter retains every raw event in its output provenance. It
rejects nonfinite fields/negative cash, ignores nonpositive dates and zero cash,
ignores dates later than the documented expiry window, combines exact-equal
interior dates, and assigns events inside the expiry window to maturity.
It retains the original unrounded date and cash values. Resource limits are
checked before unbounded allocation or iteration. Repeated-date tests must
verify the intended cash sum; no date quantization is allowed.

At expiry build the intrinsic stock-value PWL. Apply terminal cash through the
liquidator operator, then pre-event exercise for American contracts. At an
interior event, the post-event value has already considered post-event exercise;
apply the cash map and then compare against pre-event intrinsic. Find every
linear intersection with intrinsic, including a crossing on the infinite tail.
Do not assume one exercise boundary or global convexity of liquidator puts.
Record unrepresentable/rounded intersections explicitly; do not insert a
minimum time value or reuse a fitted exercise boundary.

European options roll only between cash events. American calls use the same
event-only clock **only for `rate >= 0` and `yield == 0`**; otherwise this initial
mode returns typed unsupported. American puts have independent nested exercise
dates on every cash-free segment. Zero volatility and zero stock receive their
explicit supported limiting behavior, not an invented positive volatility.
Non-liquidator models remain unsupported in this first owner.

Zero-volatility rollback uses affine stock-PWL composition: divide each knot by
`exp((rate-yield)*dt)`, discount its value, and propagate the linear tail. This
avoids constructing a degenerate positive-volatility logarithmic grid. The
put exercise clock remains the declared discrete clock. Zero stock uses its
absorbing state on the same clock, with exact discount and exercise comparisons.
These limiting branches do not invent a grid width or a volatility floor.

The fixed stock grid contains zero, spot and strike plus logarithmic nodes. For
`N` positive-grid intervals, its positive endpoints are

`low = min(S,K) * exp(-8*sigma*sqrt(T))`

`high = (max(S,K) + sum(active cash)) * exp(8*sigma*sqrt(T))`.

Use `N+1` positive nodes including both endpoints; add spot and strike only when
not already present by exact comparison. Overflow, nonpositive endpoints or
collapsed distinct nodes cause typed refusal. There is no domain adjustment
using reference prices or exercise classifications. The represented tail stays
linear to infinity and all contributions are integrated; approximation of the
true value outside the node range remains a separately disclosed error.

At each diffusion step evaluate the existing PWL expectation on this fixed
stock grid. Its outgoing linear slope propagates as `slope*exp(-yield*dt)`;
the zero value follows the exact discounted zero branch. Exercise inserts any
needed crossings before the next step. This is an approximation in value
representation and exercise time, even though each PWL Gaussian integral is
analytical. In particular, finite-grid interpolation can destroy a theoretical
Bermudan lower-bound interpretation. No such bound is claimed.

## Declared pilot population and resolutions

Original IDs: **A1p, A1c, E64p, E64c, ILp, ILc, DSPYp, DSPYc**. Export all 50
original inputs and mark the other 42 explicitly uncomputed in this pilot.
Do not relabel the six alternate-cash-model baseline cases or drop any refusal.

The four calls each use positive-grid interval counts `256,512,1024` with only
event-date exercise: **12 requested call outcomes**.

For each of the four puts use these exact five pairs: **20 requested put outcomes**.

| Positive-grid intervals | Nominal exercise count | Time level |
|---:|---:|---:|
| 256 | 128 | 0 |
| 512 | 128 | 0 |
| 1024 | 128 | 0 |
| 256 | 256 | 1 |
| 256 | 512 | 2 |

For each segment of length `L`, the base exercise count is
`max(1,ceil(128*L/T))`. Multiply that integer by `2^level`; never reapply the
ceiling at a refined level. Construct endpoints explicitly so event dates are
exact. Uniform interior nodes provide an independent exercise-time discretization;
this owner does not borrow the PDE's graded CN clock.

There are **32 requested outcomes**, including every typed refusal. Each request
gets an independent budget: at most 8192 knots per PWL, `2^24` newly constructed
knots, `2^20` expectation calls and `2^28` interval contributions, plus at most
64 raw cash events and 4096 diffusion/exercise steps. Exhaustion produces a
typed result with completed work retained. It does not trigger a larger hidden
budget, a fallback reference price or removal from the denominator.

Before the pilot, test event normalization/order, deterministic and zero-state
models, both sides of an exercise/cash event, exact PWL maximum including
nonconvex/multiple-crossing inputs, call-domain refusal, immutable input lifetime,
step limits and cumulative-budget refusal. Retain each failed control and every
pilot result. Hash raw inputs, source, executable and unrounded output. Record
actual query/interval/knot/step work and typed statuses; no benchmark clocks.

The independent reader joins exact original model/event values and compares
successful prices against the unchanged historical reference plus its empirical
uncertainty. It reports both axes and all 32 outcomes without passing them off as
full-matrix qualification. A result with less than 0.001 residual is not enough
to make this engine a reference: independent convergence to below 0.0001
empirical uncertainty remains the separate intended reference gate. No new
reference replaces the frozen one during this pilot.
