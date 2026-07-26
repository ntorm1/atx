# Point-in-time options research bridge

`atx-options-engine` is the typed boundary between the listed-options data and
research plane in `atx-vol` and the generic cross-sectional portfolio stack in
`atx-engine`.

It exists to prevent an options backtest from quietly treating a contract like
one share of stock. The library does not implement another ranker, optimizer,
execution simulator, or portfolio ledger. It prepares option-domain inputs for
the existing engine components and preserves their conservative next-slice
execution contract.

## Implemented XS-1 research slice

`OptionResearchPanel::create` accepts sparse point-in-time observations and
produces:

- a canonical date-by-contract `Dataset`;
- a stable 64-bit contract catalog mapped to caller-owned `SymbolTable`
  identities (the bridge never fabricates instrument IDs);
- an explicit tradability mask and non-tradable reason;
- separate definition, feature, execution, and outcome-label lineage;
- bid/ask, displayed sizes, interval volume, lagged open interest, ADV, return
  volatility, vega, and caller-supplied margin in contract units.

Construction fails closed when:

- research clocks do not satisfy
  `observed <= available <= decision < execution < label_end`;
- a tradable quote does not satisfy
  `definition_available <= quote_event <= quote_available <= decision`;
- a stable contract ID maps to changing underlier, expiry, strike, side, or
  multiplier metadata;
- a quote is crossed, non-positive, missing displayed size, missing risk, or
  missing margin while marked `Tradable`;
- distinct contracts alias the same caller-owned engine instrument ID;
- a nonstandard deliverable is not marked `UnsupportedContract`;
- any required source identity is absent;
- decision/contract keys are duplicated; or
- configured row or dense-cell bounds would be exceeded.

Rows are canonicalized by `(decision_ts_ns, contract_id)`. Contracts are
canonicalized by permanent ID. Caller order therefore cannot change the dense
panel, catalog, universe, or target order. Future `forward_pnl` labels never
enter the decision `Dataset`; they are exposed only through the separately
typed `OptionOutcomeLabel` evaluation view.

## Units and target construction

All quantities at this boundary are listed contracts:

| Value | Unit |
|---|---|
| `bid`, `ask`, `mark` | dollars per option unit |
| premium exposure | `mark * multiplier * contracts` dollars |
| displayed size, interval volume, ADV, open interest | contracts |
| vega | supplied per listed contract |
| initial and maintenance margin | dollars per listed contract |
| commission in the later execution stage | must be configured per contract |

`ListedOptionQuote` carries per-field availability flags, so an observed zero
display or activity count remains distinct from a source schema that did not
provide volume or open interest. The current cbbo-1m loader marks bid/ask sizes
available and volume/open interest unavailable; it rejects sizes outside the
nonnegative `int32` range before narrowing.

`make_option_target_book` maps catalog-aligned engine weights to whole-contract
targets. It supports premium-notional and equal-/weighted-vega sizing, truncates
fractional contracts toward zero, caps position ownership by a lagged-ADV
fraction and a hard per-contract limit, and then applies a deterministic margin
policy. Input weights must have L1 norm no greater than one, making
`gross_budget` an actual upper bound rather than an unconstrained scale factor.
Values within eight floating-point ulps of a whole contract are first snapped
to that integer, preventing representation noise in normalized rank weights
from creating asymmetric one-contract books.

The initial margin gate is deliberately named a conservative
independent-contract research schedule. It provides no spread offsets or
portfolio netting and is not represented as OCC STANS, SPAN, or a broker's
portfolio-margin calculation. Callers can either reject the full target batch or
proportionally clamp it before receiving a result. Expected failures never
mutate caller-owned holdings or cash.

The ADV fraction is a position-capacity control, not an order participation
model. Displayed quote size is not consumed during target construction. A
target is an intent, not a fill. The replay stage must consume future observed
liquidity, prevent two orders from reusing it, and retain or cancel any
remainder under its time-in-force rules.

## Implemented XS-2 Consolidated-L1 replay kernel

`OptionExecutionReplay` turns whole-contract target orders into an exact cash,
position, fill, fee, and final-order ledger. It is deliberately labeled
`ConsolidatedL1`: OPRA/CBBO evidence does not reveal a national queue, hidden
size, venue allocation, or complex-auction availability.

The kernel enforces:

- `decision < order arrival < fill`, with fills only on a strictly later quote
  event;
- one canonical total event order independent of caller input order;
- price priority followed by arrival, caller priority sequence, and order ID;
- separate bid and ask liquidity pools;
- one selected participant's displayed size rather than fictitious aggregate
  NBBO depth;
- a counterfactual debit ledger: unchanged size at the same
  `(participant, price)` cannot be consumed twice, and a displayed-size
  increase exposes only the increment;
- explicit per-side update evidence, so a bid-only update cannot replenish a
  consumed ask;
- whole-contract partial fills, explicit `FirstFutureQuoteOrCancel`, DAY, and
  GTC leaves, cancel/replace ordering, and expiry-before-quote ordering;
- strict quote status, staleness, locked-market, definition-clock, contract
  expiry, source-lineage, and duplicate-key validation;
- exact `Decimal` cash, premium, slippage, adverse-basis-point, and component
  fee accounting with checked multiplier and quantity arithmetic;
- effective-dated, lineage-bearing simple-order tick schedules. Quotes and
  limits must be on their active grid; adverse buys round up and sells round
  down without a floating-point money seam. An order's rule must be known at
  decision time, and delayed quote evidence that straddles a rule transition
  fails closed;
- effective-dated fee rows with knowledge time, non-overlap, per-contract,
  per-order, clearing, regulatory, commission, rebate, and sell-premium lanes;
- configured Strict, Calibrated, and Stress scenarios. Calibrated runs require
  a frozen calibration identity; Strict and Stress cannot consume more than
  25% of displayed size;
- a bounded PIMPL workspace. All successful-run vectors are reserved by
  `create`; the replay path performs no dynamic allocation after workspace
  initialization. A global-allocation-instrumented contract test pins this
  property;
- preallocated per-contract order heaps and logarithmic effective-row lookup,
  avoiding quadratic same-contract insertion and fee-revision scans.

`option_replay_required_workspace_bytes` exposes the exact reserved payload
budget used by `create`. A successful `run` returns spans borrowed from that
workspace; callers must serialize or copy them before the next run.

Every executable run requires a frozen raw-capture sequence-validation
identity. Native `(source, channel, monotone epoch, sequence, packet)`
duplicates, epoch re-entry, and availability regressions are rejected. The
returned summary binds the compiled model and ordering versions, calibration
and sequence identities, exact liquidity/adverse parameters, quote-age and
locked-market policies, and replay horizon. The view also exposes the complete
canonical fee and tick rows, including rows that produced no fill.

The quote update contract is intentionally conservative. At the same selected
participant and price:

```text
historical display 4 -> strategy consumes 4 -> historical display remains 4
modeled remaining  4 ->                    0 ->                         0

historical display later increases to 6 -> modeled increment available is 2
```

Changing selected participant or price starts a new displayed pool. Non-firm,
missing, crossed, halted, stale, and disallowed locked states provide no
executable liquidity. A configured adverse price that breaches the order limit
produces no fill; it is never clamped to the limit.

A one-sided absolute row is context only for its unchanged side: it cannot
refresh that side's evidence clock, trigger opposite-side matching, or change
displayed size. Contradictory price, participant, or raw-size context fails the
run.

The fee schedule key is an externally governed scenario key. For real invoice
fidelity it must encode the venue, product/class, penny class, account
origin/capacity, handling mode, liquidity role, auction/routing flags, premium
band, quantity tier, and contra category applicable to that execution. The
current kernel assumes configured taker/removing-liquidity charges; it does not
infer an execution venue from OPRA.

The focused Debug benchmark includes a 100,000-order single-contract book in
addition to wide and repeated-partial-fill cases. The concentrated case
completes in about one second on the development host (about 110,000 fills per
second); this is a regression baseline, not a production Release claim.

## Intended pipeline

For each decision timestamp:

1. Read the signal row from `OptionPanelField::Signal`. Non-tradable and absent
   cells are `NaN`.
2. Pass that row and `OptionResearchPanel::universe()` to
   `atx::engine::loop::WeightPolicy` for rank/z-score transforms,
   neutralization, gross exposure, and name caps.
3. Pass the resulting weights and current whole-contract holdings to
   `make_option_target_book`.
4. Convert the target book with `make_option_order_batch`. Decision-time
   bid/ask sets a marketable limit, never a fill.
5. Feed future `OptionTopOfBookEvent` rows and the orders into a preallocated
   `OptionExecutionReplay`.
6. Consume `OptionFill`, `OptionOrderAudit`, `OptionCancelAudit`,
   `OptionPositionSnapshot`, and `OptionReplaySummary` before reusing the
   workspace.

The engine's generic stock `WeightPolicy::reconcile` must not be used directly
for options: it divides equity by the mark as if the instrument were one share
and does not know the contract multiplier, vega, or margin.

The batch replay kernel is suitable for a fixed historical order schedule and
for one decision frontier at a time. A complete adaptive backtest coordinator
must settle the prior frontier, expose realized positions and live leaves,
then build the next target. Precomputing every future order would ignore partial
fills and is not an acceptable substitute. That incremental
`advance -> observe account -> target -> apply commands` coordinator is the next
layer.

## Research basis

The design follows the fidelity boundaries documented by the primary data and
market sources:

- [Databento MBP-1](https://databento.com/docs/schemas-and-data-formats/mbp-1?historical=python&live=raw)
  distinguishes event and receive timestamps and carries displayed BBO size.
- [Databento symbology](https://databento.com/docs/standards-and-conventions/symbology)
  warns that publisher instrument IDs may be remapped or reused; the bridge
  therefore requires a caller-supplied permanent contract ID.
- [Databento point-in-time corporate actions](https://databento.com/docs/venues-and-datasets/corporate-actions)
  motivates separate definition availability and immutable lineage.
- [OCC corporate-action information](https://www.theocc.com/clearance-and-settlement/corporate-action-information-submission-form)
  remains authoritative for adjusted option deliverables. Adjusted deliverables
  are currently explicit `UnsupportedContract` rows.
- [NautilusTrader backtesting](https://nautilustrader.io/docs/latest/concepts/backtesting/)
  and [order-book modeling](https://nautilustrader.io/docs/latest/concepts/order_book/)
  make the L1/L2/L3 fidelity boundary explicit: displayed top-of-book data
  cannot justify synthetic depth or queue position.
- [FINRA Rule 4210](https://www.finra.org/rules-guidance/rulebooks/finra-rules/4210?page=1)
  and the [OCC margin methodology](https://www.theocc.com/risk-management/margin-methodology)
  motivate describing the current deterministic margin input honestly rather
  than claiming production portfolio margin.
- [Goyal and Saretto](https://www.cis.upenn.edu/~mkearns/finread/CrossOptions.pdf)
  motivates executable cross-sectional option portfolios rather than isolated
  signal returns.
- [Execution replay research note](docs/execution-replay-research-2026-07-26.md)
  defines the evidence grades, OPRA/venue fidelity boundary, fee lanes,
  calibration gates, and claims this engine must reject.

## Explicitly deferred

The next milestone is the adaptive date-major coordinator described above,
including working-leaf-aware target reconciliation and an immutable lifecycle
transition ledger. Production completeness also requires adjusted
deliverables, official settlement, exercise/assignment, stock/futures hedge
replay, borrow/locates, venue-native simple and complex books, auction
participation, calibrated cross-impact, and a broker- or clearing-calibrated
margin model.
