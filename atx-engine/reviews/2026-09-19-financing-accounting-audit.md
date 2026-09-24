# Financing and mark accounting audit - 2026-09-19

The bounded corrections below are implemented, independently reviewed, and
validated by the coordinator's targeted build and test gate. Numerical examples
below are **hand-derived**; executed acceptance results are recorded at the end.

## Original borrow accrual finding

Before this iteration, `BacktestLoop::on_time_slice` updated market prices, marked
the portfolio, settled orders at the current close, then called
`cost::accrue_borrow` once for that slice.
`daily_borrow` computes `short_notional * annual_rate / day_count_denom` without
an elapsed-time input. Its input is the book after the current close's fills.

Evidence in the reviewed code:

- [backtest_loop.hpp](../include/atx/engine/loop/backtest_loop.hpp):
  `on_time_slice`, `settle_at`, and the unconditional daily accrual call.
- [borrow.hpp](../include/atx/engine/cost/borrow.hpp): `daily_borrow`,
  `accrue_borrow`, and the `D360`, `D365`, `D252` denominator enum.
- [backtest_borrow_test.cpp](../tests/core/backtest_borrow_test.cpp): the original
  two-bar test used timestamps 1 ns and 2 ns, opened the short at the second close,
  and expected a full daily charge immediately. It has now been replaced.

Consequences include frequency-dependent fees, undercounted weekends, a charge
at the instant a short opens, and omission of the preceding held interval when a
short closes. The last issue follows because the post-fill short quantity is
zero before accrual runs.

For $100,000 short notional, annual rate 5%, and ACT/360, the daily amount is
`100000 * 0.05 / 360 = $13.888888889`. These comparisons are hand-derived, not
measured execution results:

| Event or elapsed interval | Original slice-based charge | Implemented elapsed-time charge |
| --- | ---: | ---: |
| Short opens at final observed close | $13.888888889 | $0 |
| Existing short held for one hour | $13.888888889 | $0.578703704 |
| Same hour split into 60 one-minute intervals | $833.333333333 | $0.578703704 |
| Existing short held Friday close to Monday close | $13.888888889 at Monday | $41.666666667 |
| Short covered at the interval-ending close | $0 at that close | Charge the interval held before cover |
| Existing short revisited at the same timestamp | Another daily charge | $0 |

The intraday rows count interval-ending observations after the position is
already open. The original code additionally charged its opening observation. The
weekend row assumes a 72-hour interval with unchanged notional and rate.

## Source review and implemented convention

[Cvxportfolio's official cost documentation](https://www.cvxportfolio.com/en/stable/costs.html)
states that simulation uses actual time between observations, including longer
weekend intervals. Its holding-cost model applies to post-trade holdings over
the subsequent period. That supports duration-scaled research accrual.

[IBKR's official short-sale cost disclosure](https://www.interactivebrokers.com/en/pricing/short-sale-cost.php)
states that borrow fees use settled stock positions. Its
[collateral explanation](https://www.interactivebrokers.com/campus/glossary-terms/collateral-short-sale/)
describes the industry convention of collateral based on 102% of the prior
settlement price, rounded upward to a whole dollar per share. A continuous
trade-date model therefore must not claim to reproduce an IBKR statement.

Implemented bounded convention: accrue financing on the holdings and marks left by
the previous observed slice, over the actual elapsed UTC interval, before
applying the current observation's prices or close fills. This is a continuous
trade-date research model with piecewise-constant observed notional. Opening at
the current close starts exposure for the following interval; covering now does
not erase the cost of the interval just held.

Implementation:

1. `borrow_for_elapsed(..., Duration elapsed)` returns `Result<Decimal>` with
   `short_notional * annual_rate * elapsed_calendar_days / basis`. Apply duration
   before the single conversion to Decimal.
2. `BacktestLoop` tracks the previous slice timestamp and accrues before
   `market.update_prices` and before settlement. The first observation establishes
   the anchor without inventing earlier holding time; EOF adds no future charge.
3. Configuration is validated before zero elapsed/rate can return zero. Backward
   time and invalid rates are rejected. Ordered timestamps are subtracted as
   unsigned integers and bounded before constructing signed elapsed nanoseconds.
4. ACT/360 and ACT/365 are supported. D252 is rejected in the new elapsed-calendar
   seam until an actual trading-session calendar is supplied. The explicit daily
   helper retains its existing convention for compatibility.
5. The same-bar execution opt-in and next-slice firewall are preserved. Accrual runs
   once per elapsed interval, outside either settlement call.

`validate_elapsed_borrow_model` rejects negative/non-finite rates and unsupported
day-count values even for an inert run. Active short marks must be positive and
finite; long/flat holdings do not require marks. The new helper propagates Decimal
conversion errors, including non-finite/out-of-range charges, rather than silently
replacing a failed conversion with zero. `accrue_borrow_for_elapsed` checks exact
cash subtraction before calling the portfolio mutator, so failure leaves cash and
positions unchanged. The old explicitly daily helpers are unchanged.

The loop validates configuration before registering its bus consumer. Runtime
financing errors occur before current-slice market/position mutations and before
advancing the accrual anchor. The run provides a basic guarantee: the feed has
already consumed that observation, earlier slices remain applied, and a failed
run must not be resumed. Equal timestamps accrue zero; their final holdings and
marks become the basis for the next positive interval. No interval is invented
before the first observation or after EOF.

This does not add settlement lags, broker collateral rules, rebates, changing
borrow availability, or per-name historical rates. Those need separate explicit
models. Intraday partition equivalence should allow the bounded rounding error
of individual Decimal cash debits, not require bit-identical partitioned sums.

## Original stale mark finding and implemented correction

`Portfolio::mark_to_market` copies Market prices before `settle_at`.
`ExecutionSimulator::emit_fill` calls `apply_permanent_impact`, which changes
`Market::mark` through `shift_mark`. `Portfolio::apply_fill` updates cash and
positions but does not copy the new market mark. `sample` and the next
`WeightPolicy::reconcile` therefore read stale marked portfolio value while
reconciliation's price denominator already uses the updated Market price.

Relevant sources:

- [portfolio.hpp](../include/atx/engine/portfolio/portfolio.hpp): `apply_fill`,
  `mark_to_market`, `Holding::market_value`, and `equity`.
- [execution_sim.hpp](../include/atx/engine/exec/execution_sim.hpp): `emit_fill`
  and `apply_permanent_impact`.
- [market.hpp](../include/atx/engine/loop/market.hpp): `shift_mark` and persistent
  permanent-impact offsets applied by `update_prices`.
- [weight_policy.hpp](../include/atx/engine/loop/weight_policy.hpp): `reconcile`
  reads portfolio equity and Market marks separately.

Hand-derived example: starting cash $100,000; buy 100 shares at $100; no fees,
slippage, or temporary impact. With ADV 1,000, sigma 0.1, and gamma 2, the existing
permanent-impact formula shifts the mark upward by $1. Cash is $90,000 and Market
marks $101, but Holding still marks $100. Current reported equity is $100,000;
consistent valuation under this existing model is $100,100. The discrepancy
persists until the next mark refresh and remains in final results if EOF occurs.

Implemented fix: refresh portfolio marks after applying the complete settlement
batch inside `settle_at`, so both ordinary and opt-in same-bar settlement paths
finish with consistent marks. This also makes the prior-slice marks used by the
financing convention consistent. No Portfolio API change was needed.
This repairs accounting consistency; it does not establish the economic accuracy
of the existing permanent-impact pricing convention.

## Focused acceptance scenarios

1. **First observation, opening, and EOF:** opening at a final close incurs zero
   elapsed borrow; the first observed timestamp does not imply a past interval.
2. **Intraday partitioning and repeated timestamp:** one hour and 60 minute-long
   intervals produce the same aggregate charge within Decimal rounding bounds;
   a repeated timestamp adds zero.
3. **Weekend and basis:** a 72-hour Friday-Monday interval charges three daily
   amounts; ACT/365 is verified independently and D252 is rejected at the new seam.
4. **Position transition:** closing, reducing, or flipping at an interval's end
   charges the preceding holdings, then uses the resulting holdings next time.
5. **Boundaries:** long-only and zero-rate runs remain unchanged; negative elapsed
   time, non-finite/negative rates, and unsupported basis fail explicitly.
6. **Permanent impact:** for both execution delays, holding marks equal Market
   marks after fills; equity, exposure, subsequent target sizing, and EOF results
   reflect them. The zero-impact path retains existing behavior.

Independent review found no blocker in duration arithmetic, prior-book ordering,
zero-path validation, checked Decimal cash debit, repeated timestamps, or the
post-batch mark refresh. The focused fixtures are `BorrowElapsed.*` (13 tests)
in `tests/core/borrow_test.cpp`, `BacktestBorrow.*` (12 tests) in
`tests/core/backtest_borrow_test.cpp`, and `BacktestAccounting.*` (four tests) in
`tests/core/backtest_accounting_test.cpp`, all in `atx-engine-core-tests`.
The loop's timestamp-span overflow check was reviewed arithmetically; the current
clock begins at epoch zero, so that signed-crossing case is not exercised by the
loop fixtures. The helper's minimum/maximum Duration cases are covered.

## Executed acceptance

The coordinator's final iteration-two gate passed all 120 selected tests with
zero failures or skips in 12.82 seconds of CTest wall time. This includes all
13 `BorrowElapsed.*`, 12 `BacktestBorrow.*`, and four `BacktestAccounting.*` tests.
Sources were stable for validation. Builds of `atx-impl`,
`atx-engine-core-tests`, `atx-engine-data-tests`, and `atx-shm-worker` passed;
the risk target also linked through its shared-PCH dependency. CLI help exited
zero. The wrapper handled benign CMake GLOB diagnostics on stderr without
misclassifying the successful build; an intentionally nonexistent target
returned exit code one, preserving actual native failure detection.

Evidence: `build-equity/iteration2-final-build.log`,
`build-equity/iteration2-cli-build.log`, `build-equity/iteration2-final.log`,
`build-equity/iteration2-final.xml`, and `build-equity/native-failure.log`.
