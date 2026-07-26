# Adaptive options execution session: research and design contract

**Research date:** 2026-07-26
**Scope:** deterministic, point-in-time execution state for adaptive US
listed-options backtests.
**Status:** implemented XS-3A foundation; target generation and working-leaf
reconciliation remain the next coordinator layer.

## Decision

Adaptive research uses one persistent execution session:

```text
start immutable market/catalog state
  -> advance_to(frontier)
  -> observe sealed account and order state
  -> apply exactly one future-effective command batch
  -> repeat
  -> finish
```

Naively replaying longer prefixes from fresh state is invalid. It would reset
working orders, fees, cash, and partially consumed displayed liquidity, so the
same historical quote size could fund more than one counterfactual fill. A
checkpointed prefix replay could be valid but inefficient. The session instead
merges an immutable canonical market-event stream with a bounded dynamic
command-event heap.

The observation frontier is inclusive relative to the preloaded,
sequence-attested archive: every loaded event whose availability time is at or
before the frontier is settled before observation. It is not proof that every
independent source channel is complete without corresponding per-channel
capture watermarks. Commands decided at that frontier must become available
strictly later. This decision barrier prevents a newly generated order from
consuming a quote that was already visible to the policy.

## Pinned lifecycle semantics

- A submitted order is `Scheduled` until its modeled arrival event.
- Modeled arrival deterministically changes it to `Working`; eligible modeled
  fills change it to `PartiallyFilled` or `Filled`.
- A synthetic cancel request changes a live order to `PendingCancel`, but its
  leaves remain working exposure until modeled cancel availability.
- Eligible modeled fills before cancel availability reduce both working and
  pending-cancel leaves.
- Cancel, order expiry, and the caller-supplied contract cutoff precede quote
  processing at an equal availability timestamp; quote processing precedes
  submit. This is a conservative model tie-break, not an exchange rule.
- Unknown cancels and cancels of terminal orders are explicit audit outcomes.
  An unknown target is pinned at request acceptance and cannot capture an order
  created later with the same identifier.
- Every state change is appended to an immutable transition ledger.
- A complete command basket is canonicalized and validated before mutation.
  Validation failure is retryable; a processing failure terminalizes the
  session so no partially advanced frontier is exposed.
- Admission reserves transition capacity for command acceptance and each
  mandatory future submit/cancel outcome. Data-dependent fill/expiry
  transitions remain bounded by the configured ledger and may fail the active
  session if that independent capacity is exhausted.
- Dynamic order and cancellation IDs are globally increasing, which keeps the
  bounded lookup append-only. Canonical sorting makes input permutation stable.

The frontier exposes position, scheduled leaves, working leaves,
pending-cancel leaves, and projected contracts. Pending-cancel leaves are a
subset of working leaves and are counted only once. A later target reconciler
must compare its target with projected exposure, not filled position alone.
These contract aggregates are signed net totals; the reconciler must also
inspect per-order leaves to detect offsetting buy/sell orders and gross churn.

## Bounded implementation

`OptionExecutionSession::create` reserves all successful-lifecycle storage.
The hot lifecycle (`start`, `advance_to`, `apply_commands`, `finish`) performs
no dynamic allocation after initialization. A global intrusive pairing heap
stores price-priority order roots without per-contract capacity partitions, so
all configured orders may legitimately concentrate in one contract.
Epoch-stamped touched-contract staging makes command application proportional
to the contracts in that basket rather than scanning the full catalog at every
frontier.

The session has separate model and ordering versions and a canonical command
trace hash. The hash binds the accepted frontier sequence and command fields;
the existing replay summary separately binds data, sequence attestation,
scenario, calibration, and execution parameters. This is not yet a complete
strategy-run provenance manifest: target-policy identity, feature identity,
frontier schedule identity, and final artifact content identity must be bound
by the coordinator.

## Fidelity boundaries

This remains a conservative Consolidated-L1 simulator. The internally exact
cash and position ledger is exact only for its configured counterfactual fills
and fees. It does not claim exchange queue position, hidden liquidity, routing,
venue allocation, atomic complex execution, market impact, exercise,
assignment, or official settlement.

`expiry_ts_ns` is a caller-supplied synthetic trading cutoff. It does not model
product-specific last trading, expiration exercise, assignment, or settlement.
A nonzero position at that cutoff therefore fails closed until effective-dated
last-trade, expiration, exercise-instruction, and settlement evidence exists.
The current `GoodTillCanceled` means only "remain open until explicit cancel,
replay end, or the synthetic contract cutoff" inside one replay. It does not
model exchange sessions, carried-order restatements, Done-for-Day, disconnect
policies, or cross-session cancel windows.

Databento documents that event timestamps from independent venue gateways need
not be globally monotone. A stable merge key can make replay deterministic, but
cannot manufacture cross-stream causality. The replay retains its ambiguity
flag rather than describing the imposed order as historical fact.

FIX distinguishes the event that changes an order from its aggregate order
status, and explicitly permits fills while a cancel or replace is pending. That
fact informs the session's pending-cancel exposure. The current lifecycle is
still synthetic: modeled arrival deterministically accepts, and modeled cancel
availability deterministically cancels unless the order is already terminal.
It has no venue/broker acknowledgments, new/cancel/modify rejects,
`PendingNew`, native replace, or transport/disconnect state.

## Primary sources

- [FIX Trading Community order-state changes](https://www.fixtrading.org/online-specification/order-state-changes/)
  defines pending-cancel/replace transitions and intervening fills.
- [Cboe Titanium US Options BOEv3](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-options-boev3-specification)
  provides a current exchange-native order-entry and execution protocol
  reference.
- [Cboe SPX contract specifications](https://www.cboe.com/tradable-products/sp-500/spx-options/spx-specifications/)
  illustrate why last trading and settlement cannot be collapsed into one
  generic expiration timestamp.
- [Cboe Options Exchange 24x5 FAQ](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-options-exchange-245-faq)
  documents session and carried-order distinctions omitted by synthetic GTC.
- [Databento common fields, enums, and types](https://databento.com/docs/standards-and-conventions/common-fields-enums-types)
  documents event and receive timestamps and their ordering limitations.
- [OPRA Pillar output specification](https://cdn.opraplan.com/documents/OPRA_Pillar_Output_Specification.pdf)
  defines consolidated options quote sequencing and participant-BBO evidence.
- [OCC Rules](https://www.theocc.com/getmedia/9d3854cd-b782-450f-bcf7-33169b0576ce/occrules.pdf)
  are the authority for exercise, assignment, and clearing boundaries.
- [NautilusTrader event-driven backtesting](https://nautilustrader.io/docs/latest/concepts/backtesting/)
  is a useful implementation reference for a persistent event-driven
  backtest engine.
- [Boyd et al., Multi-Period Trading via Convex Optimization](https://web.stanford.edu/~boyd/papers/cvx_portfolio.html)
  motivates observe-and-replan, receding-horizon portfolio control.
- [SLSA provenance](https://slsa.dev/spec/v1.0/provenance) and
  [W3C PROV-DM](https://www.w3.org/TR/prov-dm/) inform the future immutable run
  manifest and derivation graph.

## Next coordinator contract

The next layer is a no-allocation date-major coordinator:

```text
frontier observation
  -> point-in-time signal/weight policy
  -> option-aware target sizing and risk gate
  -> reconcile target against position plus live leaves
  -> future-effective cancel/order commands
  -> session apply
```

Its default retarget policy should cancel conflicting or excess working leaves
and defer a new independent order until the cancel is effective or a later
frontier. Unconditional cancel-plus-new can overshoot when the old order fills
during cancel latency; such behavior must be selected and audited explicitly,
never silently inferred. Exchange-native replace and its acknowledgment/reject
semantics remain deferred.
