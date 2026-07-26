# Point-in-time options research bridge

`atx-options-engine` is the typed boundary between the listed-options data and
research plane in `atx-vol` and the generic cross-sectional portfolio stack in
`atx-engine`.

It exists to prevent an options backtest from quietly treating a contract like
one share of stock. The library does not implement another ranker, optimizer,
execution simulator, or portfolio ledger. It prepares option-domain inputs for
the existing engine components and preserves their conservative next-slice
execution contract.

## Implemented XS-1 slice

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

## Intended pipeline

For each decision timestamp:

1. Read the signal row from `OptionPanelField::Signal`. Non-tradable and absent
   cells are `NaN`.
2. Pass that row and `OptionResearchPanel::universe()` to
   `atx::engine::loop::WeightPolicy` for rank/z-score transforms,
   neutralization, gross exposure, and name caps.
3. Pass the resulting weights and current whole-contract holdings to
   `make_option_target_book`.
4. Reconcile contract targets into option orders.
5. Queue those orders into a strictly later market slice. Use bid/ask and
   displayed size for a marketable-L1 model, or the existing
   `ExecutionSimulator` with option-calibrated per-contract costs and volumes.
6. Commit fills through the portfolio ledger in stable contract/order order.

The engine's generic stock `WeightPolicy::reconcile` must not be used directly
for options: it divides equity by the mark as if the instrument were one share
and does not know the contract multiplier, vega, or margin.

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

## Explicitly deferred

The next replay milestone must add a date-major order/fill ledger with shared
displayed-liquidity consumption, partial fills, effective-dated fee lanes, and
fill attribution. Production completeness also requires adjusted deliverables,
corporate actions, official settlement, exercise/assignment, borrow/locates,
multi-leg atomicity, and a broker- or clearing-calibrated margin model.
