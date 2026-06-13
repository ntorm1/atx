# The Backtest Loop, Portfolio Accounting, and Execution Simulator — OSS Deep Dive

> **A Phase-2 grounding report for the ATX engine.**
> How event-driven OSS backtesters (QuantConnect LEAN, Zipline, NautilusTrader, backtrader) turn the
> crank: per-time-slice loop, portfolio/P&L accounting, fill + slippage + commission + market-impact
> modeling, the streaming-bar → rolling-panel bridge, and signal → target-weight → order generation.
> This goes **below** `renaissance-worldquant-deep-dive.md` (which covers the √-impact law and
> event-driven architecture at a high level) into concrete OSS implementation specifics, exact
> formulas, and an implementable C++20 design.

**Compiled:** 2026-06-02
**Method:** Targeted source-level research — official repos (LEAN, Zipline, NautilusTrader, backtrader)
fetched directly where decodable; primary impact papers (Almgren 2005, Tóth/Bouchaud) plus reputable
secondaries; formulas extracted verbatim from source where possible.

**Provenance legend**
- ✅ **VERIFIED** — confirmed against a primary or strong source; URL inline.
- ⚠️ **UNCERTAIN / INFERRED** — secondary source, partial decode, or my inference; treat as guidance.
- 📘 **STANDARD DOMAIN KNOWLEDGE** — well-established quant-engineering practice, not separately cited.

---

## 0. Executive Summary

The four reference engines converge on the **same per-slice crank**:

```
advance clock → mark securities to new prices → settle pending fills from the PRIOR step
→ run user/alpha code (reads only data ≤ now) → emit target/orders → queue orders
→ (next step) match & fill → update portfolio ledger → sample metrics
```

The single most important correctness rule is **temporal separation of decide and fill**: a decision
made on bar `t`'s information must fill no earlier than bar `t+1` (next open or next close). Every
engine enforces this — LEAN via a stale-data guard (`pricesEndTime <= order.Time → no fill`), Zipline
by processing the blotter at the *start* of the next bar before `handle_data`, backtrader by matching
market orders against the *next* bar's open. This is what keeps a backtest free of look-ahead.

The execution-sim cost stack, in application order, is:
**fill (price + volume cap) → slippage (spread/volume-share) → market impact (√-law, temporary +
permanent) → commission → latency/partial-fill spillover.**

Key formulas (all derived below):
- **Zipline VolumeShareSlippage:** `fill = price · (1 ± price_impact · volume_share²)`, `price_impact=0.1`,
  `volume_share = min(filled/bar_volume, 0.025)`. ✅
- **Zipline FixedBasisPointsSlippage:** `fill = price · (1 ± bps/10000)`, default `bps=5`, cap 10% of bar. ✅
- **LEAN VolumeShareSlippageModel:** `slippage_cash = price · price_impact · volume_share²`, same 0.025/0.1 defaults. ✅
- **Square-root impact law (Tóth/Bouchaud):** `I(Q) = Y · σ · √(Q/V)`, `Y` of order unity. ✅
- **Almgren-Chriss (2005, Citigroup fit):** permanent `≈ ½·γ·σ·(X/V)`, temporary `h = η·σ·|X/(V·T)|^{3/5}`,
  `γ=0.314`, `η=0.142`. ✅

The bar→panel bridge is a **bounded trailing ring of cross-sections** (LEAN `RollingWindow<T>` +
`History`, Zipline `data.history(bar_count)`), sized to the alpha program's `max_lookback`, written
*after* each bar closes and read *before* the next decision — never the current in-progress bar.

Signal→weight is `scale`/`order_target_percent`: raw alpha → cross-sectional transform (rank or
z-score, optionally industry-demeaned) → dollar-neutralize (`Σw=0`) → gross-normalize (`Σ|w|=1`) →
diff against current weights → trade list.

---

## 1. Event-Driven Backtest Loop Drivers (how the crank turns)

### 1.1 QuantConnect LEAN — `AlgorithmManager.Run`

✅ LEAN's master loop lives in `Engine/AlgorithmManager.cs`
(https://github.com/QuantConnect/Lean/blob/master/Engine/AlgorithmManager.cs). Per `TimeSlice` (a
`Slice` = all data at one instant; see *Timeslices* docs
https://www.quantconnect.com/docs/v2/writing-algorithms/key-concepts/time-modeling/timeslices), the
documented sequence is:

1. **Stream / time-sync** — `Stream()` yields a `TimeSlice` from the Synchronizer/`SubscriptionSynchronizer`;
   `algorithm.SetDateTime(time)` advances the clock.
2. **Security price update** — securities updated with new market data *before* user code (comment:
   "Update the securities properties: first before calling user code to avoid issues with data").
3. **Cash / portfolio sync** — `cash.Update()` (currency conversion), portfolio total-value invalidated/recomputed.
4. **Realtime + corporate actions** — `realtime.SetTime()`, then `HandleSplits()` then `HandleDividends()` (order matters).
5. **Consolidators** — registered consolidators updated (bar construction) before custom-data handlers.
6. **Algorithm callback** — `if (timeSlice.Slice.HasData) algorithm.OnData(algorithm.CurrentSlice);`
   (also universe selection, scheduled events, insights).
7. **Transaction processing** — `transactionHandler.ProcessSynchronousEvents()` executes fills / order mechanics.
8. **Results sampling** — `results.ProcessSynchronousEvents()` snapshots the portfolio *post-execution*.
9. **End-of-step** — `algorithm.OnEndOfTimeStep()`.

The **engine overview** (https://www.quantconnect.com/docs/v2/writing-algorithms/key-concepts/algorithm-engine):
"LEAN runs an algorithm manager that synchronizes the data your algorithm requests, injects the data
into your algorithm so you can place trades, processes your orders, and then updates your algorithm state."
✅ In backtest, "the fill model can only evaluate if the order should fill on a minute-by-minute frequency"
— i.e. **fill granularity = subscription resolution**
(https://www.quantconnect.com/docs/v2/writing-algorithms/key-concepts/event-handlers).

📘 The takeaway: LEAN marks-to-market and settles **before** running user code only for *prices*; new
orders placed in `OnData` are queued and handled by the **`BacktestingTransactionHandler`** (a
`BrokerageTransactionHandler` subclass) on the next synchronous-events pass — they do not fill on the
bar that created them.

### 1.2 Zipline — `AlgorithmSimulator.transform` (`tradesimulation.py`)

✅ Source: `zipline/gens/tradesimulation.py`
(https://github.com/quantopian/zipline/blob/master/zipline/gens/tradesimulation.py). The clock
(`MinuteSimulationClock`, `zipline/gens/sim_engine.pyx`) emits typed actions; the dispatch loop is:

```python
for dt, action in self.clock:
    if action == BAR:                  every_bar(dt)
    elif action == SESSION_START:      once_a_day(dt)
    elif action == SESSION_END:        # cleanup + daily perf message
    elif action == BEFORE_TRADING_START_BAR:  algo.before_trading_start(current_data)
    elif action == MINUTE_END:         # minute perf message
```

✅ **`every_bar(dt)` order (critical, this is where look-ahead is prevented):**
1. capital changes;
2. `algo.on_dt_changed(dt)` — advance sim time;
3. **process the blotter from the prior bar:**
   `new_transactions, new_commissions, closed_orders = blotter.get_transactions(current_data)`;
   `blotter.prune_orders(closed_orders)`;
4. push fills to the ledger: `metrics_tracker.process_transaction()` / `process_commission()`;
5. **only now** run user code: `handle_data(algo, current_data, dt)`;
6. collect new orders the user just placed (`new_orders = blotter.new_orders; blotter.new_orders = []`)
   and register them (`metrics_tracker.process_order()`) — these fill on the *next* bar.

✅ **`once_a_day` (SESSION_START):** overnight capital changes → `on_dt_changed(midnight)` →
`metrics_tracker.handle_market_open()` (mark-to-market) → splits via `data_portal.get_splits()` then
`blotter.process_splits()` + `metrics_tracker.handle_splits()`.
✅ **SESSION_END:** `_cleanup_expired_assets`, `blotter.execute_cancel_policy(SESSION_END)`,
`algo.validate_account_controls()`, then yield the daily performance message.

✅ Orders are held by the **`SimulationBlotter`** (`zipline/finance/blotter/simulation_blotter.py`);
`get_transactions` runs each open order through the **slippage** model (which also caps fill volume)
and then the **commission** model. `order_target_percent` (API in `zipline/api.pyi`,
https://github.com/quantopian/zipline/blob/master/zipline/api.pyi) computes the share delta between
current and target % of `portfolio_value` and submits a single order.

### 1.3 NautilusTrader — `BacktestEngine` / `SimulatedExchange` / `OrderMatchingEngine`

✅ (Docs: https://nautilustrader.io/docs/latest/concepts/backtesting/; crate
https://docs.rs/nautilus-backtest). Rust-native, nanosecond-resolution, event-driven. Per data point
the matching engine runs **three phases**:
1. **Exchange processing** — update the (simulated) order book from incoming market data; fill resting orders against the new state.
2. **Strategy notification** — data engine dispatches to actors/strategies; they may submit/cancel/modify.
3. **Venue settlement** — drain queued commands, iterate matching engines; *cascading* orders (e.g. a
   stop-loss submitted inside `on_order_filled`) settle within the **same timestamp/event cycle**.

✅ **Bar execution** (`bar_execution=True` default): each bar is expanded to **O,H,L,C** price points
with volume split **25% each**; processing follows a fixed `O→H→L→C` or adaptive path to estimate the
intrabar price trajectory. **Trade-tick execution** (`trade_execution=True`): a trade tick consumes
liquidity and triggers resting-limit fills at the trade price. **Quote execution**: L1 top-of-book
updates a single-level book; market orders fill at bid/ask.

✅ **`FillModel`** parameters (probabilistic, for L1 data only):
- `prob_fill_on_limit` (default `1.0`) — fill likelihood when a limit level is *touched but not crossed*;
  `0.0`=back-of-queue, `1.0`=front-of-queue, `0.5`=mid-queue.
- `prob_slippage` (default `0.0`) — probability of a **one-tick adverse** move on fill (L1 only).
- ⚠️ `prob_fill_on_stop` appears in older docs/community references but was **not** in the page I
  fetched; treat as version-dependent.
- With **L2/L3** book data, `prob_slippage` is ignored — real depth determines impact.

✅ **`LatencyModel`**: defers commands by placing them in the venue's in-flight queue with a *future*
timestamp (settle-loop); zero-latency still settles correctly. Per-operation nanosecond latencies
(insert/update/delete) are configurable. ⚠️ Exact default `base_latency_nanos` not in the fetched page.

### 1.4 backtrader — `Cerebro` + `BackBroker`

✅ (Docs: https://www.backtrader.com/docu/broker/ ; source
https://github.com/mementum/backtrader/blob/master/backtrader/brokers/bbroker.py). Per bar, Cerebro:
notifies the strategy of broker events → tells the broker to **accept queued orders and execute
pending orders against the new bar** → calls `strategy.next()`.

✅ **Default timing (no cheating):** an order issued at the end of bar `t` "will be matched with the
next incoming price which is the open price" — i.e. **market orders fill at bar `t+1`'s open**.
✅ **Cheat-on-Close (`set_coc(True)`):** order issued and executed at the **same bar's close** (notification
arrives next bar). **Cheat-on-Open (`set_coo(True)` / `BackBroker(coo=True)`):** order placed in
`next_open` fills at the *current* bar's open. `cerebro.broker_coo=True` propagates COO. These "cheat"
modes are explicit, opt-in look-ahead conveniences — the **default is lookahead-safe**.

### 1.5 The decide→execute timing rule (cross-engine synthesis)

📘 The universal anti-lookahead rule, stated four ways:

| Engine | Mechanism | Default fill of a decision made on bar t |
|---|---|---|
| LEAN | `pricesEndTime <= order.Time → return (no fill)`; `GetBestEffortTradeBar(order.Time)` returns the *first bar after* order time | next bar |
| Zipline | blotter `get_transactions` runs at the **top** of the next `every_bar`, before `handle_data` | next bar |
| Nautilus | order queued, matched in the venue-settlement phase of the next (or same, via latency) data point | next tick/bar (latency-shifted) |
| backtrader | market order matched against next bar's **open** | next bar open |

📘 Recommended ATX default: **decide at bar `t` close → fill at bar `t+1` open** (the most common
daily-rebalance convention), with "fill at `t+1` close" as an option for close-to-close strategies.
Provide an explicit, *named* "cheat" flag (like backtrader) if same-bar-close fills are ever wanted —
never make it the default.

---

## 2. Portfolio Accounting (positions, cash, exposure, P&L)

### 2.1 LEAN — `SecurityPortfolioManager` / `SecurityHolding`

✅ (Source `Common/Securities/SecurityHolding.cs`
https://github.com/QuantConnect/Lean/blob/master/Common/Securities/SecurityHolding.cs; manager
class-ref https://www.lean.io/docs/v2/lean-engine/class-reference/classQuantConnect_1_1Securities_1_1SecurityPortfolioManager.html).

- **Average-cost basis.** LEAN uses **cost-averaging**: holdings cost = weighted average of purchase
  prices (docs: "weighted average of all your purchase prices"). `HoldingsCost = Quantity · AveragePrice · multiplier`.
- **Unrealized P&L** delegates to `TotalCloseProfit()`: `potentialExitValue − entryValue − fees`,
  i.e. effectively `Quantity · (CurrentPrice − AveragePrice) · ContractMultiplier`, fee-adjusted.
  `UnrealizedProfit` is only nonzero when `|Quantity| > 0`.
- **Realized P&L** accumulates via `AddNewProfit(pl)` → `_profit += pl`; fees via `AddNewFee` →
  `_totalFees`; `NetProfit = Profit − TotalFees`.
- **Market value:** `HoldingsValue = Quantity · CurrentPrice · multiplier`; `AbsoluteHoldingsCost = |HoldingsCost|`.
- **Portfolio total value:** `Cash + TotalUnrealisedProfit + TotalUnleveredAbsoluteHoldingsCost`
  (manager docs). ⚠️ `SetHoldings(qty, avgPrice)` just *assigns*; the weighted-average update and
  realized-P&L split on a reducing/flipping fill are computed **upstream in the fill pipeline**, not in
  this setter — so the lifecycle logic (below) is the engine's responsibility, not the holding's.

### 2.2 Zipline — `Portfolio` / `Account` / `Ledger`

✅ (`zipline/finance/ledger.py`, `zipline/protocol.py`; metrics in `zipline/finance/metrics/`). The
`MetricsTracker` owns a `Ledger`, which owns `PositionTracker`, `_cash_flow`, and exposes
`portfolio` and `account`. Accessible in-algo as `context.portfolio.{positions, cash, portfolio_value, pnl}`
and `context.account.{leverage, net_liquidation, ...}`
(https://zipline.ml4trading.io/api-reference.html). Positions carry `amount`, `cost_basis`,
`last_sale_price`; `portfolio_value = cash + Σ position market values`; mark-to-market happens at
`handle_market_open` and per bar via the metrics tracker. Leverage = gross exposure / net liquidation.

### 2.3 NautilusTrader — `Portfolio` / `AccountsManager`

✅ The `Portfolio` aggregates `Account` state and net positions; `AccountsManager` applies fills to
account balances (multi-currency, cash vs. margin accounts), tracking realized P&L per position and
unrealized P&L marked to the latest quote/trade. (Docs: nautilustrader.io concepts.) ⚠️ Exact method
names not source-verified here.

### 2.4 Position lifecycle, exposure, long-short bookkeeping (the model to implement)

📘 The cost-honest position-lifecycle state machine every engine implements (explicitly or upstream of
the holding object):

```
On fill (signed qty q at price p, fee f):
  if sign(q) == sign(position) or position == 0:        # OPEN / INCREASE
      new_avg = (|pos|·avg + |q|·p) / (|pos| + |q|)
      pos += q;  avg = new_avg;  realized -= f
  else:                                                  # REDUCE / CLOSE / FLIP
      closed = min(|q|, |pos|)
      realized += closed · (p − avg) · sign(pos) − f     # booked against avg cost
      pos += q
      if sign(pos) flipped:  avg = p                     # remainder opens new leg at p
      # if pos hits 0, avg is reset
  cash -= q·p + f                                          # cash ledger (buy reduces cash)
```

- **Unrealized P&L:** `Σ_i pos_i · (mark_i − avg_i) · mult_i`. **Mark** = last trade/close (or mid).
- **Gross exposure** `= Σ_i |pos_i · mark_i|`; **net exposure** `= Σ_i pos_i · mark_i`;
  **leverage** `= gross / equity`. **Dollar-neutral** ⇒ net ≈ 0; **market-neutral** ⇒ net ≈ 0 *and*
  factor-beta ≈ 0.
- **Returns series:** `r_t = (equity_t − equity_{t-1} − net_external_flows_t) / equity_{t-1}` (time-weighted).
- 📘 **Short financing/borrow cost:** a daily accrual `borrow_bps/252 · |short_mkt_value|` debited to
  cash (mention-level; out of Phase-2 core scope but leave a hook).

---

## 3. Execution / Fill Simulation (the cost-honesty core)

### 3.1 Fill models

📘 / ✅ Standard fill modes and where each engine sits:

- **Immediate / market fill.** LEAN `ImmediateFillModel` (default for non-equity): fills *completely
  and immediately* at the directional touch price — buy at ask, sell at bid
  (https://www.quantconnect.com/docs/v2/writing-algorithms/reality-modeling/trade-fills/supported-models/immediate-model).
- **Equity fill with lookahead guard.** LEAN `EquityFillModel`
  (https://www.lean.io/docs/v2/lean-engine/class-reference/classQuantConnect_1_1Orders_1_1Fills_1_1EquityFillModel.html):
  ✅ Market buy fills at `GetBestEffortAskPrice()`, sell at bid, then slippage. **LimitFill** uses
  `GetBestEffortTradeBar(asset, order.Time)` = *first bar after order time*, fills only if the bar's
  range penetrates the limit (`tradeBar.Low < order.LimitPrice` for a buy), and honors favorable gaps
  (`tradeBar.Open < limit → fill at open`). **StopLimitFill** guards same-bar fills explicitly:
  `if (pricesEndTime <= order.Time) return fill;` and bails on `tradeBar == null`. This stale-data
  check is the look-ahead firewall.
- **Limit fill conditions.** Fill iff the bar traded through the limit; price-improve on gaps.
- **Partial fills / volume caps.** Both Zipline and LEAN cap per-bar fill volume to a fraction of bar
  volume; the unfilled remainder rolls to subsequent bars (see §3.2). NautilusTrader splits bar volume
  25% across O/H/L/C and can fill across those sub-points.
- **Probabilistic fill.** NautilusTrader `prob_fill_on_limit` / `prob_slippage` (§1.3) — queue-position
  realism for L1 data; superseded by real depth at L2/L3.

### 3.2 Slippage models — exact formulas

✅ **Zipline `VolumeShareSlippage`** (`zipline/finance/slippage.py`,
https://github.com/quantopian/zipline/blob/master/zipline/finance/slippage.py):

```
volume_share = min(shares_filled_this_order / bar_volume, volume_limit)     # volume_limit default 0.025
simulated_impact = volume_share² · price_impact · price · sign(direction)   # price_impact default 0.1
fill_price = price · (1 + price_impact · volume_share²)   # buy
           = price · (1 − price_impact · volume_share²)   # sell
```
Per-bar volume cap: `max_volume = volume_limit · bar_volume`; the model tracks `volume_for_bar`
(accumulated across all of this strategy's orders in the bar) and fills only
`min(open_amount, max_volume − volume_for_bar)` — the rest waits for the next bar. **Impact is quadratic
in participation** (note: this is *not* the √-law; it is steeper at small sizes, shallower-then-steeper
overall — a Zipline modeling choice, not a market-microstructure law).

✅ **Zipline `FixedBasisPointsSlippage`** (the modern recommended default):
```
percentage = basis_points / 10000          # default basis_points = 5.0  → 5 bps
fill_price  = price · (1 + percentage · direction)
max_volume  = volume_limit · bar_volume     # default volume_limit = 0.1 (10%)
```
✅ **Zipline `FixedSlippage(spread)`**: fill at `price ± spread/2`, no volume cap.

✅ **LEAN `VolumeShareSlippageModel`** (`Common/Orders/Slippage/VolumeShareSlippageModel.cs`):
```csharp
volumeShare      = Math.Min(order.AbsoluteQuantity / barVolume, _volumeLimit);  // default 0.025
slippagePercent  = volumeShare * volumeShare * _priceImpact;                    // default 0.1
return slippagePercent * lastData.Value;                                        // slippage in cash/share
```
i.e. `slippage_per_share = price · price_impact · volume_share²` — algebraically identical to Zipline's.
✅ **LEAN `ConstantSlippageModel`** is the **default for equities**: a fixed % (or 0) applied to fill price
(https://www.quantconnect.com/docs/v2/writing-algorithms/reality-modeling/slippage/supported-models).

📘 **Spread cost** is the universal floor: cross **half the bid-ask spread** on any aggressive order
(`±spread/2`); model it even when you skip impact.

### 3.3 Commission / fee models — exact defaults

✅ **Zipline** (`zipline/finance/commission.py`):
- `PerShare`: `DEFAULT_PER_SHARE_COST = 0.001` ($0.001/share), `min_trade_cost` default `0.0`;
  `cost = max(shares · cost_per_share, min_trade_cost)` via `calculate_per_unit_commission`. **This is
  Zipline's default equity commission.**
- `PerTrade`: flat `cost` charged on the first fill of an order (`0` thereafter).
- `PerDollar`: `DEFAULT_PER_DOLLAR_COST = 0.0015` → `cost = |amount| · price · 0.0015` (15 bps).
- `PerContract` (futures): `DEFAULT_PER_CONTRACT_COST = 0.85`/contract.

✅ **LEAN `InteractiveBrokersFeeModel`** (US equity)
(https://github.com/QuantConnect/Lean/blob/master/Common/Orders/Fees/InteractiveBrokersFeeModel.cs):
**$0.005/share, $1.00 minimum per order, 0.5% of trade value maximum.** (⚠️ Community notes the IB real
max is 1%; LEAN uses 0.5% — i.e. defaults drift from reality; make yours configurable.)

📘 Typical realistic equity commission for a modern institutional sim: **0.5–2 bps** all-in; retail
per-share `$0.005` ≈ a few bps on a $50–$200 name. For the WorldQuant-style 0.6–6.4-day-turnover alphas,
**commission is the *small* cost; slippage + impact dominate** — consistent with the prior report's AQR finding.

---

## 4. Market Impact — the √-law and Almgren-Chriss

> The prior report established √-impact at a high level (`impact ≈ Y·σ·(Q/ADV)^δ`, δ≈0.5, configurable
> 0.45–0.65; impact dominates at scale). Here are the *primary* formulations and the numbers to calibrate against.

### 4.1 The square-root law (Tóth, Bouchaud, et al.)

✅ **Canonical form** (Bouchaud, *The Square-Root Law of Market Impact*,
https://bouchaud.substack.com/p/the-square-root-law-of-market-impact; Tóth et al. 2011, *Anomalous
price impact*, https://arxiv.org/abs/1105.1694):

```
I(Q) = Y · σ_daily · √( Q / V_daily )
```
- `Q` = total meta-order size (shares), `V_daily` = daily volume, `σ_daily` = daily volatility,
  `Y` = **dimensionless constant of order unity** (typically reported ~0.5–1).
- ✅ Empirically the exponent "remains stubbornly anchored around **δ = 1/2**" across markets, sizes,
  and even option markets (https://arxiv.org/abs/1602.03043); recent Tokyo-exchange surveys confirm
  near-universality (https://arxiv.org/html/2411.13965v1).
- ✅ **Crucially: impact depends on total `Q`, barely on the execution schedule (`N` child orders, time
  `T`).** Mechanism: a **V-shaped latent liquidity** that vanishes near the current price ⇒ concave
  (square-root) impact.

⚠️ Do **not** hardcode `Y`; it is the one free parameter you must fit per-universe/regime. The
√-dependence is robust; the *level* is not universal.

### 4.2 Almgren, Thum, Hauptmann, Li (2005) — *Direct Estimation of Equity Market Impact*

✅ (Citigroup desk data, ~700k US orders, Dec-2001–Jun-2003;
https://www.cis.upenn.edu/~mkearns/finread/costestim.pdf; coefficients corroborated by
https://www.prettyquant.com/post/2022-09-03-market-impact-models/). They **split impact into permanent
and temporary** and, notably, **reject pure square-root for the *temporary* term in favor of a 3/5
power**:

```
Permanent (moves the reference price for everyone, ~linear in size):
    g(X) ≈ ½ · γ · σ · (X / V)               with γ = 0.314

Temporary (paid on your own trade, reverts after you stop):
    h(v) = η · σ · | X / (V · T) |^{3/5}      with η = 0.142,  exponent β = 3/5 = 0.6
```
- `X` = shares traded, `V` = ADV, `σ` = daily volatility, `T` = trade duration (fraction of a day),
  `X/(V·T)` = participation rate.
- ✅ They **could not reject a *linear* permanent impact**; the temporary exponent **0.6** is why the
  prior report says "δ spans 0.45–0.65, Almgren ~0.6". (The full trajectory-cost expression in some
  secondaries adds a `(Θ/V)^{1/4}` turnover term, `Θ`=shares outstanding — a refinement, not core.)

### 4.3 Almgren-Chriss optimal-execution framing

📘 / ✅ In the Almgren-Chriss optimal-execution model
(https://www.simtrade.fr/blog_simtrade/understanding-almgren-chriss-model-for-optimal-trade-execution/):
**permanent impact** is proportional to shares traded (market-depth coefficient) and moves the
*reference* price; **temporary impact** has a fixed part (≈ **half the bid-ask spread**) plus a variable
part proportional to **trading speed**, and only affects *your* fill price (reverts after). The model
trades impact (favors slow) against timing/volatility risk (favors fast) to get an optimal schedule.

📘 **How a backtester applies this** (the design rule):
- **Temporary impact → fill price** (you pay it; it does not persist): `fill = ref_price · (1 ± temp_impact)`.
- **Permanent impact → the mark** (it persists, so future P&L on the position reflects the new reference):
  shift the security's reference/mark by `perm_impact` after your trade.
- 📘 Most OSS engines model **only a lumped slippage** (Zipline/LEAN volume-share) and treat impact as a
  **bolt-on**; none ship a calibrated √-impact-with-permanent-component model out of the box. **Ignoring
  size-dependent impact makes a backtester pass losing-at-scale strategies** — exactly the failure mode
  this engine must avoid (prior report, AQR).

---

## 5. The Bar→Panel Bridge (streaming events → rolling columnar panel) — CRITICAL

This is the least-documented and most ATX-specific piece: feeding the **alpha-expression VM**, which
wants a `date × instrument` float64 panel with `max_lookback` trailing rows, from a streaming,
point-in-time event spine — **without look-ahead** and with **bounded memory**.

### 5.1 How OSS engines expose trailing history

✅ **LEAN `RollingWindow<T>`** — a fixed-capacity, newest-at-`[0]` circular buffer the algorithm fills
itself; `History` API returns a trailing `DataFrame`/`Slice` set
(https://www.quantconnect.com/docs/v2/writing-algorithms/securities/handling-data). The `Session`
object is itself a `RollingWindow` (size 0 default; set to 2 to see the prior session at `[1]`).
✅ **Zipline `data.history(assets, fields, bar_count, frequency)`** — returns a trailing window of
`bar_count` rows, **split/dividend-adjusted as of the current sim time**, with valid fields
`price/open/high/low/close/volume/last_traded`
(https://zipline.ml4trading.io/api-reference.html). ✅ **`data.current(assets, fields)`** is *the
current bar only*. Both are "tied to the current simulation minute … query prices as of that minute and
**looking backward**" — i.e. PIT by construction.
✅ **NautilusTrader** keeps a bar/quote **Cache** per instrument that strategies query for the last-N.
📘 **Qlib** windows its `date × instrument` panel via the expression engine's rolling operators with a
`max_lookback` derived from the deepest `Ref/Mean/Corr` window (see the DSL deep-dive).

### 5.2 Point-in-time correctness + bounded memory (the rule and the structure)

📘 The invariant: **the panel visible at decision time `t` contains only rows with
`knowledge_time ≤ t`.** Mechanically:

1. Bars are **appended after they close** (their `end_time ≤ t`); the *current, in-progress* bar is
   never exposed to a decision that fires before its close. (LEAN: `GetBestEffortTradeBar` returns the
   *first bar after* order time; Zipline: blotter settles at the *top* of the next bar before
   `handle_data`.)
2. The trailing window keeps **only `max_lookback` rows** (the deepest lookback the loaded alpha program
   needs — computed by the VM compiler from its `ts_*` window args). Older rows are evicted. **Memory =
   `max_lookback × n_instruments × n_fields × 8 bytes`**, fixed.
3. Fundamentals/PIT fields use **bitemporal** `(event_time, knowledge_time)` and are admitted only when
   `knowledge_time ≤ t` (prior report Part IV).

### 5.3 Per-symbol streams → column-major panel (the assembly)

📘 The bridge converts N per-symbol event streams into one column-major block:
- Maintain a **ring of cross-sections**: a contiguous `float64` matrix `[max_lookback rows × n_instruments cols]`
  per field (`close`, `volume`, `vwap`, `returns`, …), newest row at a rolling head index (`head = (head+1) & (cap−1)`,
  `cap` power-of-two). Column-major so the VM's **cross-sectional** ops (rank/scale/neutralize across
  instruments at one date = one row) and **time-series** ops (per-instrument down a column) are both
  cache-friendly.
- On each completed bar for symbol `s` at date `t`: write `panel[field][head][col(s)] = value`. When all
  symbols for date `t` are in (k-way merge boundary / SimClock tick), the head row is "sealed" and the
  date becomes readable.
- **Missing symbols / universe membership:** a per-date **membership bitmask**; absent symbols get `NaN`;
  the VM's reductions use an explicit `min_periods`/NaN policy (DSL deep-dive). Universe is point-in-time
  (`as-of` snapshot) to kill survivorship bias.

### 5.4 Rebalance cadence

✅ **Evaluate-every-bar** vs **scheduled**. Zipline `schedule_function(func, date_rule, time_rule)`
(e.g. `date_rules.week_start()`, `time_rules.market_open()`); LEAN `Schedule.On(DateRules.MonthStart,
TimeRules.AfterMarketOpen, …)`. 📘 ATX should support both: a cheap per-bar panel update, but the
**alpha eval + rebalance** fires only on the configured cadence (daily-close is the WorldQuant-style
default), so turnover/cost are controlled.

---

## 6. Signal → Target Portfolio → Orders

📘 / ✅ The standard pipeline (the VM emits a raw cross-sectional alpha vector `a` over the live universe):

```
1. Clean:        winsorize / clip outliers; drop NaN (or impute to cross-sectional median).
2. Transform:    s = rank(a)  (percentile, Alpha101 rank, [0,1])   OR   s = zscore(a)
                 optional: industry/sector demean  (indneutralize / group_neutralize).
3. Center:       w_raw = s − mean(s)          → dollar-neutral, Σw = 0  (long-short).
4. Gross-normalize:  w = w_raw / Σ|w_raw|       → Σ|w| = 1   (this is Alpha101 `scale(x, a=1)`:
                                                  "rescale so Σ|x| = a").  ✅
5. (optional) quantile L/S: long top decile, short bottom decile, equal-weight within.
6. Leverage:     w *= target_gross_leverage.
7. Reconcile:    target_shares_i = w_i · equity / price_i ;
                 trade_i = target_shares_i − current_shares_i   (order_target_percent semantics).  ✅
```

✅ `order_target_percent(asset, 0.05)` adjusts a position "to a target percent of the current portfolio
value … equivalent to placing an order for the difference between the target percent and the current
percent" (https://zipline.ml4trading.io/api-reference.html). ✅ Alpha101 `scale(x,a)` =
`x · a / Σ|x|` is exactly the gross-normalization step (DSL deep-dive §1.2). 📘 **Turnover** =
`½·Σ|w_t − w_{t−1}|`; penalize it (or trade only the part of the delta that beats expected cost) to
control churn given the §3 cost stack.

---

## 7. Determinism & Correctness in the Loop

📘 / ✅ Reproducibility rules (the engine's determinism invariant; mirrors the prior report's "one
system" discipline):

- **Stable event ordering.** k-way-merge ties broken by a **total order** `(timestamp, source_id,
  symbol_id, seq)` — never by hash-map or pointer order. The Disruptor spine already gives single-producer
  in-order delivery.
- **Deterministic fill ordering.** Process open orders in a fixed order (insertion sequence / order-id);
  the per-bar volume cap (`volume_for_bar`) then allocates scarce liquidity reproducibly. Zipline's
  `SimulationBlotter` iterates orders deterministically and accumulates `volume_for_bar` per bar.
- **Fixed-point / integer money where possible.** Avoid order-dependent float reductions; sum cash and
  P&L in a fixed accumulation order. (NaN-aware reductions must use a defined NaN policy, not IEEE
  default propagation that varies with SIMD lane order.)
- **No wall-clock, no RNG without a seeded, logged stream.** NautilusTrader's `prob_fill`/`prob_slippage`
  are RNG-driven — if used, the seed must be part of the run config and logged, or fills won't reproduce.
- **Backtest == live code path.** Same matching/accounting code in sim and live ⇒ no sim/live skew
  (LEAN and Nautilus both do this).

---

## 8. Component-Mapping Table (ATX ← OSS reference)

| ATX component | OSS reference (class / path) | What we borrow |
|---|---|---|
| Backtest loop driver | LEAN `AlgorithmManager.Run`; Zipline `AlgorithmSimulator.transform` | Per-slice order: mark → settle-prior-fills → user code → queue orders → next-slice fill → sample |
| Clock / cadence | Zipline `MinuteSimulationClock` (typed actions); LEAN Synchronizer | Typed `SESSION_START / BAR / BEFORE_TRADING_START / SESSION_END` actions; settle at top of bar |
| Order book / blotter | Zipline `SimulationBlotter`; Nautilus `OrderMatchingEngine` | Hold open orders; `get_transactions` runs slippage+commission; deterministic iteration |
| Fill model (lookahead guard) | LEAN `EquityFillModel` (`pricesEndTime <= order.Time → no fill`) | First-bar-after-order-time fill; gap handling; stale-data firewall |
| Probabilistic fill / queue pos | Nautilus `FillModel` (`prob_fill_on_limit`, `prob_slippage`) | Optional L1 queue-position realism (seeded RNG) |
| Slippage (volume-share) | Zipline `VolumeShareSlippage`; LEAN `VolumeShareSlippageModel` | `price·impact·share²`, 2.5% bar cap, partial-fill spillover |
| Slippage (bps) | Zipline `FixedBasisPointsSlippage` (5 bps, 10% cap) | Cheap default; spread floor `±spread/2` |
| Commission | Zipline `PerShare`/`PerDollar`/`PerTrade`; LEAN `InteractiveBrokersFeeModel` | $/share + min + max-% ; per-dollar bps |
| Market impact | Almgren 2005 (`γ=0.314`,`η=0.142`,β=0.6); Tóth/Bouchaud √-law | Temporary→fill, permanent→mark; configurable δ∈[0.45,0.65] |
| Latency | Nautilus `LatencyModel` (in-flight queue, future ts) | Defer order→fill by ns/ticks via the event queue |
| Portfolio / holdings | LEAN `SecurityHolding`/`SecurityPortfolioManager`; Zipline `Ledger` | Avg-cost basis, realized/unrealized split, mark-to-market, exposure/leverage |
| Rolling history | LEAN `RollingWindow<T>`+`History`; Zipline `data.history()` | Bounded trailing window, PIT, append-after-close |
| Signal→weight | Zipline `order_target_percent`; Alpha101 `scale`/`rank` | rank/z → demean (Σw=0) → gross-norm (Σ|w|=1) → diff to trades |
| Rebalance schedule | Zipline `schedule_function`; LEAN `Schedule.On` | Per-bar panel update, scheduled eval/rebalance |

---

## 9. Concrete Recommendations for the C++20 Design

### 9.1 The loop driver shape

📘 A single deterministic crank over the Disruptor event stream. Per `SimClock` tick (one sealed date,
or one bar):

```cpp
void BacktestLoop::on_time_slice(Timestamp t, const Slice& slice) {
    market_.update_prices(slice);                 // 1. mark securities (prices only)
    portfolio_.mark_to_market(market_);           // 2. recompute equity/exposure on new marks
    exec_.settle_pending(t, market_);             // 3. FILL orders queued on the PRIOR slice
        // → fills update portfolio_ (avg-cost, realized/unrealized, cash)
    panel_.append_sealed_row(slice);              // 4. write completed bar into rolling panel
    if (schedule_.fires(t)) {                      // 5. cadence gate (daily close / scheduled)
        auto signal  = vm_.evaluate(panel_.view()); // 6. alpha VM over PIT panel
        auto weights = policy_.to_target_weights(signal, portfolio_, market_);
        auto orders  = policy_.reconcile(weights, portfolio_, market_); // target − current
        exec_.queue(orders, t);                   // 7. queue → fill on NEXT slice (no lookahead)
    }
    metrics_.sample(t, portfolio_);               // 8. record equity/returns/turnover
}
```

Rules: settle-then-decide-then-queue; **a freshly queued order never fills in the same slice.** This is
Zipline's `every_bar` order with LEAN's stale-data firewall.

### 9.2 Portfolio accounting data structures

```cpp
struct Holding {
    InstrumentId id;
    int64_t  qty        = 0;       // signed shares (integer)
    double   avg_price  = 0.0;     // average-cost basis
    double   realized   = 0.0;     // booked P&L (ex-fees)
    double   fees       = 0.0;
    double   mark       = 0.0;     // last trade/close/mid
    double market_value()      const { return qty * mark; }
    double unrealized()        const { return qty * (mark - avg_price); }
};

struct Portfolio {
    double cash;
    FixedVector<Holding> holdings;          // dense over the active universe
    void apply_fill(InstrumentId, int64_t q, double p, double fee);  // §2.4 state machine
    double equity()  const;                  // cash + Σ market_value
    double gross()   const;                  // Σ |market_value|
    double net()     const;                  // Σ market_value
    double leverage()const { return gross() / equity(); }
};
```
`apply_fill` implements the open/increase/reduce/close/flip machine of §2.4 (weighted-avg on increase,
realized P&L on reduce, avg reset on flip). Sum cash/P&L in a **fixed order** for determinism.

### 9.3 ExecutionSimulator interface (fill → slippage → √-impact → commission → latency → partial)

```cpp
struct Order { InstrumentId id; int64_t qty; OrderType type; double limit; Timestamp queued_at; };
struct Fill  { InstrumentId id; int64_t qty; double price; double fee; double impact; Timestamp t; };

class ExecutionSimulator {
public:
    // settle_pending: for each open order, attempt fills against THIS slice (queued earlier).
    void settle_pending(Timestamp t, const Market& mkt);

private:
    // pluggable models (strategy pattern, like LEAN/Zipline):
    FillModel*       fill_;        // EquityFill-style: needs bar.end_time > order.queued_at
    SlippageModel*   slip_;        // VolumeShare (price·k·share²) OR FixedBps (±bps/1e4)
    ImpactModel*     impact_;      // sqrt-law temporary + linear permanent
    CommissionModel* comm_;        // per-share+min+max% OR per-dollar bps
    LatencyModel*    lat_;         // delay queued_at→eligible by N ns/ticks

    // per-bar liquidity cap, accumulated across this strategy's orders (Zipline volume_for_bar):
    double max_fill(double bar_volume) const { return volume_limit_ * bar_volume; }
};
```

**Model formulas to implement (defaults):**
- Fill eligibility: `bar.end_time > order.queued_at` (firewall) AND exchange open.
- Volume cap: `fillable = min(|order.open_qty|, volume_limit·bar_volume − volume_for_bar)`; `volume_limit` default 0.025 (or 0.10 for bps model). Remainder rolls forward.
- Slippage (choose one): `fill = ref·(1 ± k·share²)` (`k=0.1`, `share=fillable/bar_volume`) **or** `fill = ref·(1 ± bps/1e4)` (`bps=5`).
- **Temporary impact (paid, → fill price):** `temp = Y·σ·(part)^δ`, `part = fillable/(bar_volume)`, `δ` default 0.5 (configurable 0.45–0.65; Almgren temp 0.6 with `η≈0.142`). Apply *adversely* to `fill`.
- **Permanent impact (→ mark):** `perm = ½·γ·σ·(fillable/ADV)` (`γ≈0.314`, linear). After the fill, shift the instrument's reference/mark by `perm·sign`.
- Commission: `max(|qty|·per_share, min_fee)` capped at `max_pct·notional` (default $0.005 / $1 / 0.5%) **or** `|notional|·per_dollar_bps`.
- Spread floor: always `±spread/2` on aggressive orders.

Make `Y`, `δ`, `γ`, `η`, `volume_limit`, `bps`, commission all config — **never hardcode**; fit per universe.

### 9.4 Bar→panel rolling-window bridge (bounded, point-in-time)

```cpp
template <std::size_t Cap>          // Cap = power-of-two ≥ max_lookback
class RollingPanel {
    static_assert((Cap & (Cap-1)) == 0);
    // column-major per field: [Cap rows] × [n_instruments] float64
    std::array<AlignedMatrix, kNumFields> fields_;
    std::size_t head_ = 0;                  // newest sealed row
    std::vector<std::uint64_t> membership_; // per-row universe bitmask
public:
    void append_sealed_row(const Slice& s); // head=(head+1)&(Cap-1); write field cols; NaN absent
    PanelView view() const;                 // last max_lookback rows, newest-first; PIT by construction
};
```
- Written **only after a bar closes** (`end_time ≤ t`); the VM reads `view()` *before* queuing orders ⇒ no look-ahead.
- Memory fixed: `Cap · n_instruments · n_fields · 8 B`. `max_lookback` comes from the VM compiler (deepest `ts_*` window).
- Column-major ⇒ cross-sectional ops (one row) and time-series ops (one column) both contiguous; NaN + `min_periods` policy explicit; universe membership PIT.

### 9.5 Signal → target-weight policy

```cpp
struct WeightPolicy {
    Transform transform = Rank;       // Rank | ZScore
    bool   industry_neutral = false;  // demean within group
    bool   dollar_neutral   = true;   // center: Σw = 0
    double gross_leverage   = 1.0;    // Σ|w| = gross_leverage
    Quantile ls = {};                 // optional top/bottom-decile L/S

    // raw alpha → cleaned → transformed → demeaned → gross-normalized → leveraged weights
    AlignedVector to_target_weights(const Signal&, const Portfolio&, const Market&) const;
    // weights → target shares → diff vs current → order list (order_target_percent semantics)
    std::vector<Order> reconcile(const AlignedVector& w, const Portfolio&, const Market&) const;
};
```
Pipeline = §6: winsorize → `rank`/`zscore` → optional `indneutralize` → subtract mean (Σw=0) →
divide by `Σ|w|` then × leverage (Alpha101 `scale`) → `target = w·equity/price` → `trade = target − current`.
Optionally gate trades by expected-cost vs expected-alpha to damp turnover.

---

## 10. Do-Not-Overclaim

- ⚠️ **Almgren coefficients `γ=0.314`, `η=0.142`** are from a 2001–2003 Citigroup US-equity fit
  (corroborated by prettyquant's code, not decoded from the paper PDF directly here). They are a
  *starting point*, not universal constants — re-fit for your universe/era.
- ⚠️ **Square-root `Y`** is only ever "of order unity" in the sources; **no single authoritative value**.
  Calibrate it.
- ⚠️ **NautilusTrader specifics** (`prob_fill_on_stop`, exact `base_latency_nanos` defaults,
  `Portfolio`/`AccountsManager` method names) come from docs, not source-decoded — version-dependent.
- ⚠️ **LEAN `SecurityHolding`** does the weighted-average/realized-split **upstream** of `SetHoldings`;
  I inferred the lifecycle math (§2.4) from standard practice + the unrealized/realized getters, not from
  the exact fill-pipeline code.
- ⚠️ **Zipline `VolumeShareSlippage` is quadratic-in-participation**, *not* the √-law — don't conflate the
  two; the √-law is a separate (more realistic at scale) model you must add.
- ⚠️ **LEAN IB fee 0.5% max** diverges from IB's stated 1% — defaults drift from reality across all
  engines; treat every cost default as a knob.
- 📘 Items tagged 📘 (lifecycle state machine, panel layout, determinism rules, the C++ snippets) are
  engineering guidance synthesized from the verified mechanics, not direct quotations.

---

## 11. Reference Index

**Loop drivers**
- LEAN `AlgorithmManager.cs` — https://github.com/QuantConnect/Lean/blob/master/Engine/AlgorithmManager.cs
- LEAN Algorithm Engine — https://www.quantconnect.com/docs/v2/writing-algorithms/key-concepts/algorithm-engine
- LEAN Timeslices — https://www.quantconnect.com/docs/v2/writing-algorithms/key-concepts/time-modeling/timeslices
- LEAN Event Handlers — https://www.quantconnect.com/docs/v2/writing-algorithms/key-concepts/event-handlers
- Zipline `tradesimulation.py` — https://github.com/quantopian/zipline/blob/master/zipline/gens/tradesimulation.py
- Zipline `algorithm.py` — https://github.com/quantopian/zipline/blob/master/zipline/algorithm.py
- Zipline API (`order_target_percent`, `history`, `schedule_function`) — https://zipline.ml4trading.io/api-reference.html
- Nautilus backtesting concepts — https://nautilustrader.io/docs/latest/concepts/backtesting/
- nautilus-backtest crate — https://docs.rs/nautilus-backtest
- backtrader Broker — https://www.backtrader.com/docu/broker/ ; Cheat-on-Open — https://www.backtrader.com/docu/cerebro/cheat-on-open/cheat-on-open/
- backtrader `bbroker.py` — https://github.com/mementum/backtrader/blob/master/backtrader/brokers/bbroker.py

**Fill / slippage / commission**
- LEAN `EquityFillModel` — https://www.lean.io/docs/v2/lean-engine/class-reference/classQuantConnect_1_1Orders_1_1Fills_1_1EquityFillModel.html
- LEAN `ImmediateFillModel` model — https://www.quantconnect.com/docs/v2/writing-algorithms/reality-modeling/trade-fills/supported-models/immediate-model
- LEAN `VolumeShareSlippageModel.cs` — https://github.com/QuantConnect/Lean/blob/master/Common/Orders/Slippage/VolumeShareSlippageModel.cs
- LEAN slippage supported models — https://www.quantconnect.com/docs/v2/writing-algorithms/reality-modeling/slippage/supported-models
- LEAN `InteractiveBrokersFeeModel.cs` — https://github.com/QuantConnect/Lean/blob/master/Common/Orders/Fees/InteractiveBrokersFeeModel.cs
- Zipline `slippage.py` — https://github.com/quantopian/zipline/blob/master/zipline/finance/slippage.py
- Zipline slippage docs — https://zipline.ml4trading.io/_modules/zipline/finance/slippage.html
- Zipline `commission.py` — https://github.com/quantopian/zipline/blob/master/zipline/finance/commission.py

**Portfolio**
- LEAN `SecurityHolding.cs` — https://github.com/QuantConnect/Lean/blob/master/Common/Securities/SecurityHolding.cs
- LEAN `SecurityPortfolioManager` class-ref — https://www.lean.io/docs/v2/lean-engine/class-reference/classQuantConnect_1_1Securities_1_1SecurityPortfolioManager.html
- LEAN portfolio key concepts — https://www.quantconnect.com/docs/v2/writing-algorithms/portfolio/key-concepts

**Market impact**
- Almgren, Thum, Hauptmann, Li, *Direct Estimation of Equity Market Impact* (2005) — https://www.cis.upenn.edu/~mkearns/finread/costestim.pdf
- Bouchaud, *The Square-Root Law of Market Impact* — https://bouchaud.substack.com/p/the-square-root-law-of-market-impact
- Tóth et al., *Anomalous price impact…* (2011) — https://arxiv.org/abs/1105.1694
- *Square-root impact law also holds for option markets* — https://arxiv.org/abs/1602.03043
- Tokyo-exchange universality survey — https://arxiv.org/html/2411.13965v1
- prettyquant, *Market Impact Models* (γ/η code) — https://www.prettyquant.com/post/2022-09-03-market-impact-models/
- Almgren-Chriss explainer — https://www.simtrade.fr/blog_simtrade/understanding-almgren-chriss-model-for-optimal-trade-execution/

**History / panel**
- LEAN handling data / `RollingWindow` — https://www.quantconnect.com/docs/v2/writing-algorithms/securities/handling-data
- Zipline `data.history` / `data.current` — https://zipline.ml4trading.io/api-reference.html

---

*Companion to `renaissance-worldquant-deep-dive.md` (high-level √-impact + event-driven architecture)
and `alpha-expression-dsl-deep-dive.md` (the VM/panel operator surface). This report supplies the
Phase-2 loop, portfolio, and execution-sim implementation specifics.*
