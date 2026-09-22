# Report holding-period audit — 2026-09-19

**Implementation follow-up:** checkpoint 5 implemented correction B for identified
reports: the engine now replays daily marked TRI-unit holdings and cash, actual
trade fees and calendar borrow, and `atx-impl` publishes the bound daily ledger.
The [validation receipt](2026-09-19-replay-validation.json) records 105 passing
focused cases and the independent native decimal comparison. The review below
preserves the original pre-change findings; its legacy-report and meta-allocation
warnings still apply to those paths. A [confirmed source split exception](2026-09-19-klac-source-adjustment-audit.md)
prevents accepting the full supplied diagnostic as economic performance.

The next useful correction is a daily holdings-and-cash replay of the scheduled
target books. The current report samples only one forward return at each rebalance,
then combines that sample with costs, borrow charges, and annualization that refer
to a different period. Replacing the sampled return with five times that return
would preserve the accounting error.

This is a read-only Research → Review checkpoint for the user's low/medium-frequency
equity long/short platform goal. No implementation, build, or strategy promotion was
performed. All defects below predate the identified-artifact checkpoint; the new
axes and schedule bindings make them observable without positional ambiguity.
The source remains an archive snapshot with unknown historical availability,
unverified original vintages, and unknown common-stock/listing eligibility.

## Confirmed current behavior

| Component | Exact behavior and consequence |
|---|---|
| `atx-impl/src/stage_optimize.cpp:101` | Weekly means every five stored panel rows, starting at row zero. It is not an exchange-calendar weekly schedule. It includes the final observation when its index is a multiple of five. |
| `stage_report.cpp:347–403` | Builds `close[d+1]/close[d]-1` from total-return-adjusted close, with a NaN final row. It passes this daily forward-return panel to `book::accumulate_report`, which reads only `sched.periods[s]`. All intervening holding-day returns disappear. |
| `include/atx/engine/book/report.hpp:156–178, 293–305` | Each book earns one selected cross-section. Missing returns and out-of-universe names contribute zero even when their position is nonzero. Transaction and borrow charges are subtracted; `equity_curve` is a cumulative **sum of fractional returns starting at zero**, not NAV or compounded wealth. The `NoSurvivorship` test currently pins missing-return-to-zero behavior; the name does not establish a valid delisting policy. |
| `stage_optimize.cpp:287–299`; `risk/multi_period.hpp:175–180` | Turnover is the L1 difference between successive target/realized weight vectors, with previous weights carried unchanged between decisions. Price drift and financing do not update that previous book. `cost_bps = turnover * configured_rate`; the stored rate is called round-trip cost although the multiplier is full L1 traded-weight distance, not half-turnover. |
| `config.hpp:443–446`; `book/report.hpp:177` | `borrow_bps` has an explicit **flat per-rebalance-period** contract. It is neither an annual rate nor a calendar-day rate. Short target notional is charged once per schedule row, including a terminal row with no future valuation interval. Changing its interpretation in place would break clients. |
| `stage_report.cpp:433, 494–516, 529–542` | The fixed `capacity_gross=1e9` enters capacity utilization, not P&L dollar scaling. P&L remains fractional despite comments calling it dollars. Sharpe annualization uses rebalance spacing, although observed returns span one row. Drawdown uses cumulative additive P&L and samples only rebalance rows. |
| `stage_report.cpp:485–491, 724–733` | IS/OOS labels use the book's decision-row index. There is no holding-interval or boundary-crossing accounting. `equity_curve.csv` stores ordinal schedule row `s`, not actual session identity or return endpoints. |

The native diagnostic at `C:/atx/data/tickerhistory_identified_20260919` has 31
research dates and book periods `[0,5,10,15,20,25,30]`. Its schedule therefore samples
`0→1, 5→6, 10→11, 15→16, 20→21, 25→26`, then adds a zero-return final row. It omits
24 of the 30 observed daily transitions. In this particular diagnostic the first
four books are flat; the live books at 20 and 25 receive two daily returns instead
of the ten transitions through date 30. The final book has nonzero turnover but no
forward observation. `pnl.tsv` confirms a final zero gross-return row. The report's
printed Sharpe and capacity numbers are **not investment evidence**.

## Related consumers and capacity

`atx-impl/src/research_sim.hpp::frictionless_sim` is a cost-coefficient preset,
not a holdings replay. Discover/combine call
`include/atx/engine/alpha/streams.hpp::extract_streams`; its daily convention is
`positions[t-1] * (close[t]/close[t-1]-1)`, with a structural zero at index zero.
This correctly places the selected target before its earned return, but it is
still a daily target-weight approximation. It does not carry drifting holdings,
cash, borrowing, fills, or market impact. Only the simulator's per-dollar
commission coefficient can enter that lightweight stream, and the stages use
the frictionless preset. These alpha-research streams must not become an oracle
for weekly portfolio NAV.

`stage_metabook.cpp:581–594` supplies the **backward** daily return `t-1→t` to
`fund/meta_book.cpp::realized_sleeve_pnl`, which multiplies it by the newly formed
book at `t`. The fund report repeats that pairing at `meta_book.cpp:475–488`.
For weekly decisions this both misses the rest of the interval and attributes a
return already observed when the book was formed. Sleeve P&L feeds trailing
covariance through `build_trailing_omega` and hence capital allocations; it is
not only a reporting problem. The header claim that supplied returns cannot
affect books is true for first-pass sleeve books, but not the multi-sleeve fund
allocation in pass two. The fund equity curve compounds, unlike `BookReport`.

`stage_metabook.cpp:707–710` writes `cost_bps=0` even when the configured cost rate
and net turnover are positive. Crossing benefit is telemetry, not a financing or
transaction debit. Further, `fund/netting.cpp:95` uses current capital weights
for both current and prior sleeve targets. A change in capital allocation with
unchanged sleeve targets can move the actual fund book while this turnover
formula reports zero. A replay must calculate external trading from the actual
previous **fund holdings**, not reuse that scalar as realized cost.

Capacity is currently a separate scenario with mismatched economics:

- `stage_report.cpp:107–188, 704–706` applies the **last** book retrospectively
  to every historical raw-close return. This is not the realized scheduled book
  return. Raw-close returns also contain split jumps; they are inappropriate for
  economic return or volatility estimates without a pure price-action treatment.
- The curve subtracts estimated impact from that final-book daily gross edge.
  It does not debit the actual scheduled trades, flat fees, or borrow and does
  not use an execution horizon aligned with holding periods. `capacity_point_aum`
  can be infinity when no curve is available; that is not proof of unlimited capacity.
- `stage_report.cpp:578–687` computes a **position/liquidity footprint** from
  `abs(target_weight) * report_aum / trailing_raw_dollar_ADV`, not participation
  by executed trades. It caps the reported ratio at 100%, hiding the extent of
  an exceedance. Missing volume produces zero metrics rather than an explicit
  unavailable result. Trailing ADV itself is correctly anchored at each decision,
  in contrast to the final-book scenario.

## Primary-source review

Boyd et al., *Multi-Period Trading via Convex Optimization*, sections 2.1–2.6,
define signed asset holdings plus cash, debit trading/holding costs from cash,
and propagate post-trade holdings through the next asset returns. Weights drift
with those returns. The paper distinguishes this simulation from the simplified
weight dynamics used inside an optimization problem. It also explains that
total-return prices imply dividend reinvestment, whereas explicit cash dividends
require separate cash-flow treatment.
[Original Stanford paper](https://web.stanford.edu/~boyd/papers/pdf/cvx_portfolio.pdf).

The reference implementation's `MarketSimulator.simulate` derives trades from
current marked weights, converts them to dollars, applies trading restrictions,
recomputes cash, debits realized costs, then advances holdings. Its outer loop
iterates pairs of observation times and records the final valuation without a
new terminal investment interval. This is a useful concrete comparison, not a
reason to import its entire execution model.
[Maintainer simulator source](https://raw.githubusercontent.com/cvxgrp/cvxportfolio/master/cvxportfolio/simulator.py).

Cvxportfolio distinguishes transaction costs on traded dollars from holding fees
on post-trade positions. Simulation uses actual elapsed time, including longer
weekend accrual. Its default data returns already include dividends, so adding
dividend cash flows again would double count them.
[Official cost documentation](https://www.cvxportfolio.com/en/stable/costs.html).
Our proposed simple borrow model would remain a research approximation; it would
not reproduce broker collateral, settlement, rebates, or locate availability.

## Correction choices

**A — limited interval attribution.** Replace the sampled one-row return by
`TRI[end]/TRI[start]-1` for every positive-length holding interval, omit the final
zero-length target, and label the result as interval target-weight attribution.
This is a small patch with a useful numerical oracle. It still cannot produce
daily drawdown, duration-correct marked financing, actual trading costs, or a
self-financing NAV from the current scalar sidecars. It should not be described
as the completed portfolio replay.

**B — recommended smallest coherent replay.** Add one cold deterministic engine
kernel for scheduled targets, daily marked signed asset values, and cash; route
the identified report through it. Keep the existing `accumulate_report` contract
for legacy attribution callers rather than silently changing its pinned units.
The first slice is an **adjusted-return research replay**, not physical-share
execution: fixed total-return-index units are held between rebalances, so dividend
reinvestment is implicit. Raw prices and volume remain separate execution/liquidity
inputs. The implementation boundaries are the new kernel, report integration,
explicit replay cost configuration, and a versioned report receipt.

The proposed contract is:

1. Start with explicit positive NAV, all cash by default. Require exact identified
   axes, strictly increasing times, ordered schedule indices, finite targets and
   valid prices for nonzero holdings. Record decision and valuation indices
   separately. Treat a book at row `p` as a target effective at that row's close
   **only under an explicit hypothetical timing convention**; an optional declared
   delay maps decisions to effective observations. Neither convention establishes
   that this snapshot was historically available. The known T+1 note does not
   establish original vintages, and a one-row delay is not calendar/T+1 proof.
2. For every observed interval `[d,d+1)`, first carry the previously marked holdings
   to its start. When a target becomes effective, set target asset dollars to
   `target_weight * pre-trade_NAV`. Trade the difference from current asset dollars;
   debit purchases and transaction costs from cash. On other dates trade nothing.
   Existing optimizer turnover remains planning telemetry and is not reused as a
   realized debit. Use one explicit per-traded-dollar fee coefficient; do not infer
   it by dividing rounded sidecar charges by turnover.
3. Advance each signed asset value using that interval's adjusted return. Accrue
   short fees on the stated start-of-interval short value and exact calendar
   duration, with an explicit D360/D365 simple-interest basis. Add a distinct annual
   borrow parameter; retain or explicitly reject legacy flat-per-period borrow
   in this new mode, never reinterpret it. Cash lending/debit interest can be zero
   in the first slice, with that omission recorded. No dividend debit is added
   on top of total returns.
4. Record daily gross dollar P&L, trade debit, financing debit, current cash,
   holdings value, NAV, and normalized net return. Check the cash/asset/NAV identity
   at every step; stop on nonfinite state or nonpositive NAV. The final observation
   is valuation-only, without a new target trade, borrow interval, or assumed free
   liquidation. Terminal targets can be retained as unexecuted decisions.
5. Universe eligibility controls new desired exposure; it never erases an existing
   position's price move. A missing valuation for a held position fails closed
   unless an explicit liquidation/write-down event exists. A missing unused name
   can remain a gap. Report daily drawdown and a clearly declared annualization
   convention. Preserve a boundary label for intervals spanning a fit/OOS cutoff;
   conservatively begin OOS at the first effective rebalance at/after the cutoff,
   unless a separately documented carried-inventory policy is chosen.
6. Compute realized trade/ADV participation from the replay's dollar trade vector,
   alongside separately named position footprints. Do not issue the existing
   final-book/backcast curve as strategy capacity. For this slice mark strategy
   capacity unavailable; a subsequent bounded change can rerun the same chronological
   ledger across AUM with calibrated impact. Missing market-liquidity inputs also
   yield unavailable status, not zero cost or infinite capacity.

Do not silently feed the new return convention into meta allocation while leaving
its old pairing unchanged. A follow-up should feed **completed prior holding
intervals** into sleeve covariance and charge external fund turnover including
capital-allocation changes. Until that work lands, meta telemetry and the new
report must remain clearly distinguished; the report can replay a supplied fund
target schedule, but cannot certify how those targets were formed.

## Six focused acceptance scenarios

1. **Full weekly coverage and daily drift.** Six prices
   `[100,110,121,121,121,121]`, initial NAV 100, one 50%-long target and 50% cash,
   no costs. Holding to the sixth valuation yields NAV **110.5**, not the sampled
   105 or implicitly rebalanced 110.25. Every daily transition appears exactly
   once. A new target on the final row has no trade or fee. Appending future data
   must preserve all earlier pre-trade valuations.
2. **Actual trade costs and self-financing cash.** Flat price, NAV 100, first target
   100% long then 50% long at a later eligible rebalance; 10 bps per dollar traded.
   Opening cost is 0.1 and NAV becomes 99.9. The second target is 49.95 dollars,
   so the sale is 50.05 and its fee is 0.05005. Unchanged intermediate holdings
   generate no repeated trading fees. Cash plus signed positions equals NAV.
3. **Weekend borrow and explicit units.** Flat short position worth 50, annual
   rate 3.65%, D365: Friday→Monday costs **0.015**, one weekday costs **0.005**.
   Covering before the next interval ends further accrual; long-only and zero-rate
   paths cost zero. Invalid rates, unsupported D252 elapsed convention, invalid
   timestamps, and conflicting legacy/annual flags reject rather than coerce.
4. **Eligibility exit versus absent price.** A held name loses universe membership
   but still has a valid 20% price loss: the loss remains in NAV. Replacing its
   required mark with NaN fails with date/security diagnostics and no published
   success receipt; a NaN in an unheld name does not change the ledger. A split
   that leaves adjusted close unchanged produces zero economic P&L.
5. **Timing, cutoff, and terminal accounting.** Weekly schedule with a fit boundary
   inside one holding interval records exact return endpoints and decision origin.
   An explicit delay cannot earn the pre-effective price move. The straddling
   interval is not silently assigned wholly OOS; terminal decisions have no
   invented outcome. Reordered identities or modified schedule bindings reject.
6. **One chronological trade path for fees/capacity and meta checks.** Large steady
   holdings with no trade have zero executed participation despite a nonzero
   position footprint. A target reversal uses the full dollar delta and uncapped
   diagnostic participation; changing later volume cannot alter earlier estimates.
   Strategy capacity remains unavailable until its chronological replay exists.
   A two-sleeve capital rotation with unchanged sleeve targets must still produce
   nonzero external fund turnover; the meta integration test must pair each book
   with subsequent completed returns, never its own already-observed return.

Relevant existing suites are `BookReport.*`, `ReportBorrow.*`,
`StageReportBorrow.*`, `StageReportCapacityCurve.*`, the report participation/capacity
fixtures, and fund meta-book/netting tests. Several intentionally pin the old
attribution semantics; preserve those under the old API and add the six meaningful
replay oracles rather than rewriting their expectations to hide the migration.
Measure the corrected native diagnostic by covered intervals, cash/asset/NAV
reconciliation, costs, missing-held-mark status, and repeated report hashes.
Do not measure this checkpoint by improvements in its printed strategy Sharpe.
