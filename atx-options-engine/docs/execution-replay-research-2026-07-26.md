# US listed-options execution replay: research and model contract

**Research date:** 2026-07-26
**Scope:** US exchange-listed equity, ETF, and index options; simple and complex
orders; historical execution simulation for systematic research.
**Status:** Design input, not legal advice and not a representation of broker or
exchange behavior.

## Executive decision

Build replay as an evidence-graded, event-sourced simulator. A result must carry
the highest fidelity that its input data can actually support:

| Grade | Minimum evidence | Permitted claim |
|---|---|---|
| `ResearchMark` | Point-in-time bid/ask snapshots | Mark-to-market research only; no fill or capacity claim |
| `ConsolidatedL1` | Lossless OPRA participant quotes/trades with native sequence and availability time | Conservative immediate-execution bounds at one displayed participant quote; calibrated passive-fill scenarios |
| `VenueBook` | Exchange-native order-by-order depth, trades, status, native sequence, and historical product configuration | Venue-specific queue/allocation replay for visible simple-book interest |
| `NativeComplexAuction` | Simple and complex depth plus auction messages, complex definitions, and historical rules | Explicit complex-book/auction scenarios, still counterfactual rather than an assertion that the historical order would have filled |
| `CalibratedProduction` | A lower grade plus the strategy's order acknowledgements, fills, cancels, rejects, route, capacity/origin, and invoices | Empirically calibrated fill/cost distributions with held-out error measurements |

The default backtest should be `ConsolidatedL1` and pessimistic. Passive fills
must be disabled or explicitly labeled probabilistic until exchange-native depth
and calibration exist. Atomic complex fills must be disabled until native
complex-book and auction evidence exist.

## Facts that constrain the model

### OPRA is consolidated top-of-market evidence, not a national queue

OPRA's current Pillar output specification says that each participant exchange
sends quote and sale messages, and its BBO appendage identifies one participant.
When multiple participants quote the same best price, OPRA selects the BBO
participant by price, then largest size, then earliest time. Thus the reported
BBO size is the selected participant's size; it is not the sum of all contracts
displayed at that price across exchanges. The specification also permits locked
and crossed BBOs and distinguishes BBO-eligible from indicative, non-firm,
rotation, and halted quotes. See the [OPRA Pillar Output
Specification](https://cdn.opraplan.com/documents/OPRA_Pillar_Output_Specification.pdf),
especially sections 6.04, 7.01-7.12, and Appendix D.

Actionable consequences:

- Store every participant quote when available. Do not call a single OPRA BBO
  size "aggregate NBBO depth."
- Treat zero-size and zero-price fields according to OPRA message semantics.
  Do not silently convert an observed zero into missing data.
- Exclude ineligible/non-firm/rotation/halted states from executable liquidity.
  Locked or crossed states require a configured policy and must never be
  repaired silently.
- Quote size is a contemporaneous displayed commitment, not guaranteed
  remaining capacity after latency, routing, and competing flow.

OPRA supplies a per-line block sequence. A block timestamp is the time OPRA
finished processing the block, and messages after the first within a block are
implicitly sequenced. Regular-session traffic is distributed over many
multicast lines. There is therefore no OPRA-specified global order across all
lines at an identical time. Use the original capture/receive order when
available; otherwise impose a documented deterministic merge key and retain an
`ordering_ambiguous` audit flag. Do not describe that imposed cross-line order
as the historical exchange order. See sections 3.05.6-3.05.8 and Appendix B of
the [OPRA output
specification](https://cdn.opraplan.com/documents/OPRA_Pillar_Output_Specification.pdf).

OPRA sale-condition messages include late/out-of-sequence reports,
cancellations, single-leg auctions and crosses, floor trades, and multi-leg
book/auction/cross trades. The current trade-identifier field is reserved and
zero-filled. A correct decoder must apply the published conditions and must not
treat every print as new regular executable volume. See sections 6.03 and 7.33
of the [OPRA output
specification](https://cdn.opraplan.com/documents/OPRA_Pillar_Output_Specification.pdf).

### Exchange-native depth materially improves evidence, but does not reveal all liquidity

Cboe's PITCH specification states that all *visible* orders and executions are
represented with add, modify, reduce, delete, and execution messages and
day-specific order IDs. It also states that executions of hidden, routed, and
reserve interest arrive as Trade messages; those Trade messages do not alter
the visible book, and their side field is always `B` regardless of resting
side. Consequently, a trade-print tick rule is not an authoritative aggressor
classifier. See the [Cboe US Options Multicast PITCH
Specification](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-equitiesoptions-multicast-pitch-specification).

Cboe rules also permit reserve orders with non-displayed quantity and
replenishment. Displayed depth is a lower bound on potential resting liquidity,
not total liquidity. See Cboe Rule 5.7(c), "Reserve Order," in the [current C1
rule book](https://cdn.cboe.com/resources/regulation/rule_book/C1_Exchange_Rule_Book.pdf).

For a venue-book replay:

- Apply book-mutating messages by native unit and sequence, not by timestamp
  sort.
- Preserve transaction begin/end blocks atomically. The PITCH specification
  explicitly permits delaying publication until a transaction end.
- Never mutate the visible book for a native Trade message when the
  specification says it is informational.
- Apply trade breaks/corrections to the execution ledger.
- Validate daily sequence continuity, gap recovery, unit clear, trading status,
  and end-of-session handling before allowing fills.

### Priority and allocation are venue, product, account, and time dependent

Cboe Rule 5.32 permits price-time or pro-rata base allocation on a
class-by-class basis and permits priority overlays, including Priority Customer
priority. The public membership page currently summarizes the four Cboe options
exchanges as Pro Rata, Price-Time, Pro Rata, and Customer Priority/Pro Rata,
respectively, but this summary is not sufficient historical class
configuration. See [Cboe Rule
5.32](https://cdn.cboe.com/resources/regulation/rule_book/C1_Exchange_Rule_Book.pdf)
and the [Cboe options membership
page](https://www.cboe.com/markets/us/options/membership).

Model choices:

- `PriceTime`: queue ahead is the visible same-price quantity accepted before
  the simulated order, adjusted only by attributable executions,
  cancels/reductions, and priority-preserving/resetting modifications.
- `ProRata`: allocation depends on the simulated displayed size, total eligible
  same-price size, rounding, residual rules, and overlays at each execution.
- `Unknown`: no deterministic passive fill. Use a calibrated distribution or
  the conservative no-fill outcome.
- Origin/capacity is mandatory input. A Customer order, Professional Customer,
  broker-dealer, market maker, and proprietary firm order cannot be assumed to
  have the same priority or fees.

Historical product configuration must be effective-dated. A current rule book
or current exchange summary must never be applied retroactively without
evidence.

Protected quotations constrain routing but do not make prints trivial. Cboe
Rule 5.66 generally prohibits trade-throughs and defines protected quotations
as an eligible exchange's best displayed bid or offer disseminated through
OPRA, while listing exceptions for rotations, crossed markets, ISO handling,
non-firm quotes, and complex trades. A router simulator must either model venue
routing and those exceptions or stop at the protected displayed size. See
[Cboe Rules 5.65-5.66](https://cdn.cboe.com/resources/regulation/rule_book/C1_Exchange_Rule_Book.pdf).

### Auctions and complex orders are separate markets

Cboe's current rules provide separate simple books, complex order books,
synthetic best bids/offers, complex order auctions, AIM/C-AIM, and other
mechanisms. AIM and C-AIM notification messages are not included in the
disseminated BBO or OPRA, and the auction period may be 100-1,000 milliseconds.
Complex executions can interact with simple legs under rules that are not
recoverable from a simple NBBO snapshot. See Cboe Rules 5.33, 5.37, and 5.38 in
the [C1 rule
book](https://cdn.cboe.com/resources/regulation/rule_book/C1_Exchange_Rule_Book.pdf).

Cboe publishes separate [complex
PITCH](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-options-complex-multicast-pitch-specification)
and [auction
feed](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-options-auction-feed-specification)
specifications. Complex PITCH carries visible complex orders and executions;
the ordinary PITCH feed carries the corresponding single-leg execution
information. The standalone auction feed is unsequenced and missed messages
cannot be retrieved, whereas PITCH has native sequencing and gap recovery.
Prefer lossless PITCH history for replay.

Required behavior:

- An OPRA multi-leg print is ex-post evidence that a complex trade occurred,
  not evidence that the strategy could have joined it.
- Never synthesize an "atomic" complex fill by filling every leg independently
  at simple-book BBOs.
- With only simple-book evidence, expose an explicitly named `Legged` mode that
  models sequential leg fills, latency, hedge slippage, partial completion, and
  unwind risk. It is not atomic complex execution.
- Enable `AtomicComplex` only with native complex definition/book/auction
  evidence and an explicit auction participation policy. Even then, label the
  outcome counterfactual and capacity constrained.

## Recommended replay contract

### Clocks and causality

Every market event should retain:

1. exchange/source event time, if supplied;
2. SIP/vendor processing time;
3. local capture/receive time, if supplied;
4. normalized availability time used by research;
5. native venue/channel/unit and sequence;
6. packet/block index and retransmission/correction status.

A decision at `t` may read only state with `availability_time <= t`. An order
becomes exchange-live only after sampled or configured decision, network,
gateway, and matching latency. A cancel/replace has its own latency and the
original order remains exposed until cancellation is effective. Never use a
future quote to fill an order merely because it occupies the next row or bar.

Canonical deterministic event ordering:

```text
(availability_time, source_rank, channel_or_unit, native_sequence,
 packet_index, stable_ingest_ordinal)
```

Native sequence takes precedence within its native stream. `source_rank` is
only a reproducible merge convention across streams with no authoritative
global order. Persist this key in the audit trail.

### Order lifecycle

Use an explicit state machine:

```text
Created -> Sent -> AcceptedLive -> PartiallyFilled -> Filled
                         |              |
                         +-> CancelPending -> Cancelled
                         +-> Rejected
                         +-> Expired
```

Record every transition and reason. Validate tick, lot, session, product,
limit, TIF, routing, and risk rules before acceptance. Do not allow a fill
before `AcceptedLive`, after a terminal state, beyond leaves quantity, or
beyond the shared liquidity ledger.

### Shared liquidity and fill rules

All strategy orders must contend for one replay-owned liquidity ledger. A
displayed contract must not be consumed twice by two orders, strategies, or
portfolio legs.

For `ConsolidatedL1`:

- A marketable buy may consume at most the selected executable participant's
  displayed offer after activation; a sell uses its displayed bid.
- Apply configurable latency and a calibrated availability haircut or
  fill-probability model. Stop after that displayed size; do not invent deeper
  prices.
- A passive order does not fill deterministically from a later OPRA print.
  Either return no fill or use a versioned, calibrated stochastic model.
- Do not consume auction, cross, floor, multi-leg, late, cancelled, or
  otherwise ineligible prints as ordinary simple-book contra flow.

For `VenueBook`:

- Insert the simulated order at the venue and apply the effective historical
  allocation algorithm and account overlay.
- Queue depletion comes from native execution/cancel/modify events. A price
  touch alone is not a fill.
- Treat newly arriving same-price interest according to the actual allocation
  rule; do not assume it is always behind the order.
- Unknown hidden interest may improve aggressive fills but must not be used to
  grant optimistic passive priority.

Provide at least three named scenarios:

- `Strict`: immediate fills are haircut; unknown passive fills are zero.
- `Calibrated`: parameters are estimated only from training-period execution
  logs and frozen before the test interval.
- `Stress`: worse latency, lower available size/fill rate, full spread
  crossing, higher impact, and fee tier deterioration.

### Impact and implementation shortfall

Use arrival-price implementation shortfall as the common cost measure. The
[Almgren-Chriss model](https://doi.org/10.21314/JOR.2001.041) provides a useful
parent-order scheduling framework that trades off price risk against temporary
and permanent impact. It does **not** identify option limit-order fills, queue
priority, auction allocation, or nonlinear option/underlier cross-impact.

Recommended decomposition:

```text
realized cost =
    quoted spread cost
  + latency/adverse-selection cost
  + visible-depth sweep cost
  + calibrated temporary/permanent impact
  + explicit fees and rebates
  + hedge execution cost
  + financing/borrow/carry
```

Calibrate option and hedge legs separately, then estimate cross-impact by
underlier, expiry bucket, delta/vega bucket, and common execution window.
Capacity should be constrained simultaneously by displayed size, option ADV,
underlier hedge ADV, portfolio vega/gamma, and risk/margin—not by option ADV
alone.

Academic results argue for a range, not a universal spread constant. Goyal and
Saretto report that option-strategy returns decline substantially after
transaction costs in [Cross-Section of Option Returns and
Volatility](https://www.ruf.rice.edu/~jgsfss/goyal_041808.pdf). Muravyev and
Pearson show in their sample that execution timing can make effective spreads
materially smaller than conventional quoted-spread estimates in [Option
Trading Costs Are Lower than You
Think](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2580548). Therefore:

- full bid/ask crossing is a defensible stress case;
- midpoint execution is not a defensible default;
- a blanket fraction of spread is not a calibrated model;
- claimed improvement from timing must arise from point-in-time order logic
  and held-out execution data, not from applying the paper's sample average.

## Fee and cash ledger

Fees must be effective-dated data, not constants in strategy code. At minimum,
the lookup key needs:

```text
trade_date, venue, product/class, simple_or_complex, penny_class,
account_origin/capacity, electronic_or_manual, add_or_remove,
auction/cross/routed flag, premium band, quantity tier, contra category
```

Store each component separately and permit negative exchange fees for rebates:

1. exchange transaction fee/rebate and routing fee;
2. broker commission/service charge;
3. OCC clearing fee;
4. exchange Options Regulatory Fee (ORF);
5. FINRA Trading Activity Fee (TAF), when the actor and transaction are in
   scope;
6. Section 31/SRO sales-value fee, when the sale is covered;
7. exercise/assignment and underlying-delivery costs;
8. CAT or other firm-level allocations only when the tested actor actually
   bears them;
9. financing, stock borrow, and stock/futures hedge fees in their own ledgers.

Why a single per-contract fee is wrong:

- The [Cboe C1 fee
  schedule](https://www.cboe.com/us/options/membership/fee_schedule/) varies by
  product, capacity, electronic/manual handling, liquidity role, auction,
  premium, size, route, and contra category. As of 2026-07-20 its ORF is
  $0.01248 per contract side on qualifying customer-range executions, but that
  value and scope are effective-dated.
- The [OCC schedule](https://www.theocc.com/company-information/schedule-of-fees)
  currently lists $0.025 per contract for clearing and a per-line-item exercise
  fee. Whether the strategy directly bears those amounts depends on its
  clearing agreement.
- [FINRA Schedule A,
  Section 1](https://www.finra.org/rules-guidance/rulebooks/corporate-organization/section-1-member-regulatory-fees)
  assesses TAF on covered option sales but provides actor/transaction
  exemptions, including index-option transactions and certain proprietary
  exchange-member activity. It cannot be applied to every sell fill blindly.
- The SEC's [FY 2026 Section 31
  advisory](https://www.sec.gov/rules-regulations/fee-rate-advisories/2026-2)
  set $20.60 per million for covered sales from 2026-04-04, after a zero-rate
  period. The SEC explains that SROs owe Section 31 and generally pass charges
  to members; rate changes can occur mid-year. The [SEC collection
  rule](https://www.sec.gov/rules-regulations/2004/06/collection-practices-under-section-31-exchange-act)
  also distinguishes index-option exemptions, option sales, and physical
  delivery on exercise; cash settlement does not create an underlying
  securities sale.

Accrue trade costs for implementation-shortfall P&L at execution, but retain
the legal charge/settlement date for cash accounting and reconciliation.
Monthly volume tiers, caps, minimums, and invoice rounding require a
post-trade-period adjustment rather than pretending every charge is known at
fill time.

## Determinism, auditability, and performance

- Minimum price variation is point-in-time product data, not a universal
  penny. Current Cboe Rule 5.4, for example, distinguishes non-Penny classes
  ($0.05 below $3 and $0.10 at or above), Penny classes ($0.01/$0.05), and
  all-price penny products including SPY, IWM, QQQ, and XSP. Persist the
  effective rule and conservatively round modeled buys up and sells down to its
  active grid. See [Cboe C1 Rule
  5.4](https://cdn.cboe.com/resources/regulation/rule_book/C1_Exchange_Rule_Book.pdf).
- Use integer ticks and integer contracts at the replay boundary. Convert to
  cash through checked multiplier arithmetic.
- Preallocate bounded event, order, book, and fill storage per replay shard.
  No hot-path allocation after initialization.
- Use stable instrument IDs and effective-dated definitions; never infer a
  contract solely from a reusable display symbol.
- Any stochastic fill/latency model must use a counter-based or otherwise
  order-independent random draw keyed by
  `(run_seed, model_version, order_id, event_id, draw_type)`. Thread count and
  shard scheduling must not change results.
- Persist input dataset fingerprint, decoder version, rules/fees snapshot,
  model version, parameters, seed, ordering convention, and scenario in every
  run manifest.
- Required invariants include sequence continuity; nonnegative book size and
  leaves quantity; fills no earlier than acceptance; no overfill; no double use
  of shared liquidity; cash/position conservation; and bitwise-identical
  results across repeated runs and supported thread counts.

Parallelize by independent venue/unit or underlier shards while preserving
native order within each shard. Portfolio-risk snapshots occur at explicit
barriers; do not introduce nondeterministic cross-shard mutation.

## Calibration and validation roadmap

1. **Capture evidence.** Acquire lossless OPRA plus exchange-native simple,
   complex, and auction history with native sequence, status, definitions, and
   receive/availability clocks. Retain raw packets or immutable normalized
   records.
2. **Build a truth replay.** Reconstruct each venue book and validate it against
   exchange top-of-book, OPRA participant quotes, sequence/gap statistics,
   cumulative volume, and trade breaks before simulating strategy orders.
3. **Collect production labels.** Join order send/ack/live/cancel/fill/reject
   records and route/venue/capacity to the exact market state. Paper fills are
   not substitutes for production labels unless explicitly modeled as such.
4. **Estimate by regime.** Bucket or model latency, available-size ratio,
   passive fill probability, partial-fill distribution, effective spread,
   adverse selection, and impact by venue/product, DTE, delta/moneyness, spread
   ticks, quote age, displayed size, order size, time of day, volatility, and
   order type.
5. **Avoid leakage.** Fit only on data available before the test interval.
   Version and freeze parameters; walk forward on untouched dates and
   underliers.
6. **Validate probability, cost, and capacity.** Report fill reliability
   curves, Brier/log loss for probabilistic fills, quantity error, latency
   quantiles, implementation-shortfall error, tail error, and predicted versus
   realized capacity. Break results out by regime instead of reporting only an
   average.
7. **Reconcile fees.** Compare the component ledger against broker, exchange,
   OCC, and regulatory invoices across fee changes and month-end tiers.
8. **Stress and bound.** Publish `Strict`, `Calibrated`, and `Stress` P&L and
   capacity together. A signal is deployable only if the economic conclusion
   survives predeclared execution and fee uncertainty.
9. **Shadow before promotion.** Run the exact replay policy against live
   shadow orders, then small-capital production. Promotion gates are
   predeclared held-out calibration tolerances, not an improved backtest.

## Claims the engine must reject

- "OPRA BBO size is total national depth."
- "The quote was displayed, therefore the full size would have filled."
- "A trade printed at my limit, therefore my passive order filled."
- "A price touched or traded through my limit, therefore I had queue priority."
- "Every Cboe class uses the same pro-rata or price-time allocation."
- "Current exchange rules and fees are valid for all historical dates."
- "All option sales pay the same TAF, ORF, or Section 31 charge."
- "Midpoint marks are executable prices."
- "A multi-leg OPRA print proves an atomic strategy fill was available."
- "Independent leg fills are equivalent to a complex-order execution."
- "Almgren-Chriss supplies an options limit-order fill model."
- "A historical replay reproduces the market that would have existed after
  inserting a material strategy order."
- "Deterministic output proves model accuracy." It proves reproducibility only.

## Primary-source implementation set

- [OPRA document library](https://www.opraplan.com/document-library)
- [OPRA Pillar output specification](https://cdn.opraplan.com/documents/OPRA_Pillar_Output_Specification.pdf)
- [Cboe C1 current rule book](https://cdn.cboe.com/resources/regulation/rule_book/C1_Exchange_Rule_Book.pdf)
- [Cboe US Options PITCH specification](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-equitiesoptions-multicast-pitch-specification)
- [Cboe complex PITCH specification](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-options-complex-multicast-pitch-specification)
- [Cboe options auction-feed specification](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-options-auction-feed-specification)
- [Cboe US Options fee schedules](https://www.cboe.com/us/options/membership/fee_schedule/)
- [OCC schedule of fees](https://www.theocc.com/company-information/schedule-of-fees)
- [FINRA member regulatory fees](https://www.finra.org/rules-guidance/rulebooks/corporate-organization/section-1-member-regulatory-fees)
- [SEC Section 31 basic information](https://www.sec.gov/rules-regulations/fee-rate-advisories/section-31-transaction-fees-basic-information-firms)
- [Almgren and Chriss, *Optimal execution of portfolio transactions*](https://doi.org/10.21314/JOR.2001.041)
- [Goyal and Saretto, *Cross-Section of Option Returns and Volatility*](https://www.ruf.rice.edu/~jgsfss/goyal_041808.pdf)
- [Muravyev and Pearson, *Option Trading Costs Are Lower than You Think*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2580548)

All exchange rules, specifications, product configuration, and fees above are
subject to change. Archive the exact effective-dated artifacts used by each
backtest rather than resolving "latest" links at replay time.
