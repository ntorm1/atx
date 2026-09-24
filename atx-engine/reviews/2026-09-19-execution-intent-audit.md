# Execution intent reconciliation audit - 2026-09-19

## Research and review

`WeightPolicy::reconcile` computes target shares minus **filled** holdings.
`ExecutionSimulator::settle_pending` keeps partially filled orders in its pending
book. `BacktestLoop::rebalance` previously appended the entire reconciled delta
again, duplicating the pending remainder on each rebalance. Old-side orders also
survived a target reversal and consumed liquidity before the replacement side.

For a constant $100,000 portfolio, $100 prices, +/-500-share targets and a
100-share per-bar cap, the 12-bar regression previously has an arithmetic
expectation of +/-1,100 shares and $220,000 gross exposure. Its corrected contract
is +/-500 shares, $100,000 gross exposure, and $100,000 traded notional.

Primary sources informed the lifecycle decision:

- [QuantConnect order events](https://www.quantconnect.com/docs/v2/writing-algorithms/trading-and-orders/order-events)
  shows canceling the unfilled remainder after a partial fill when updated
  information drives trading.
- [QuantConnect order tickets](https://www.quantconnect.com/docs/v2/writing-algorithms/trading-and-orders/order-management/order-tickets)
  describes explicit update/cancel requests and cancellation when a broker does
  not support updates.
- [Alpaca order lifecycle](https://docs.alpaca.markets/us/docs/orders-at-alpaca)
  distinguishes partial fills, pending cancellation/replacement, and their final
  states. Therefore synchronous simulator replacement is a backtest convention,
  not a live-broker cancellation guarantee.

## Implementation contract

`ExecutionSimulator::replace_pending` accepts the **complete desired remaining
order book**, not incremental trades. It cancels omitted/zero-quantity intents,
preserves the original eligibility timestamp of an identical pending remainder,
and timestamps changed/new orders at the current decision time. Matching includes
instrument, signed remaining quantity, type, and limit price for limit orders.
Matches are consumed once. Output follows deterministic input order.

`BacktestLoop` calls this operation after successful signal evaluation and target
reconciliation. Its simulator must be dedicated to that strategy. Unchanged
targets cannot accumulate duplicate orders or continuously restart latency.
Reversed/zero targets supersede old intent. A failed/exhausted signal preserves
the existing pending book; an all-NaN successful signal retains the existing
weight-policy convention of zero targets and closes positions.

Prior orders still settle before this slice's signal is evaluated. Replacement
does not undo fills, reset the per-bar participation budget, or relax the default
next-slice execution firewall. Same-bar close execution remains explicit opt-in.
Already pending unchanged orders retain their age; replacements cannot inherit
an earlier decision timestamp.

The replacement operation uses reserved reusable scratch and a bounded linear
scan per desired order. This is quadratic in pending order count at rebalance
cadence; indexing is a future measured optimization, not a claimed throughput
improvement. No live broker actions or acknowledgement state machine were added.

## Verification

Owning target: `atx-engine-core-tests`.

Nine new focused regressions cover repeated partial fills, target reversal,
zero-target flattening, exhausted signal behavior, preserved latency before and
after partial fills, same-bar volume accounting, empty-book cancellation, and
fresh decision timestamps. Existing `BacktestLoop.*` and `ExecSim.*` suites cover
the surrounding accounting, execution costs, and no-look-ahead contracts.

`git diff --check` passed. Compilation and executable test results are delegated
to the root build owner in the shared worktree and are pending at this writing;
the pre-fix quantities above are a hand-derived reproduction, not a measured
baseline run. No sanitizer or hygiene-build claim is made.

### Measured result

The root build owner subsequently built `atx-engine-core-tests` in `build-equity`
with Debug clang-cl and ran
`BacktestLoop.*:ExecSim.*:BacktestIntegration.*:BacktestBorrow.*`.
**54/54 tests passed, including all nine new regressions.** The machine-readable
report is [iteration1-execution.xml](../../build-equity/iteration1-execution.xml).
This supersedes the pending executable-validation status above. The pre-fix
baseline remains hand-derived; no additional build or test run was performed for
this documentation update.
