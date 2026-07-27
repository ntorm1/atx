# Pre-trade options risk envelope: research and implementation contract

**Research date:** 2026-07-26
**Scope:** deterministic projected- and worst-fill risk controls for the adaptive
US listed-options backtest coordinator.
**Status:** initial bounded engine and adaptive-coordinator integration
implemented on 2026-07-26; the later extensions called out below remain
research backlog.

## Decision

The implementation inserts one bounded `OptionPreTradeRiskEngine` between
adaptive target reconciliation and command materialization:

```text
sealed execution frontier
  -> absolute target and working-leaf reconciliation
  -> authoritative full-reprice risk sidecar
  -> filled / projected / exact fill-subset envelope
  -> scenario and Greek limit evaluation
  -> accept, reduce-only accept, cancel-only, or reject new orders
  -> order materialization and session apply
```

The full-reprice sidecar is authoritative for scenario P&L. Greeks are useful
limits, explanations, and fast diagnostics, but a delta-gamma-vega
approximation is not a substitute for repricing nonlinear option tails.

`OptionRiskPanel` is the immutable canonical sidecar. The initial implementation
grants no cross-underlier risk credit. It also has no
stock or futures hedge frontier. A desired or hypothetical delta hedge therefore
receives no risk credit. This is deliberately conservative for dispersion,
where favorable index/component correlation is the assumption most likely to
fail under stress.

## Source-derived boundaries

OCC STANS is a full-portfolio Monte Carlo methodology over options, futures,
cash instruments, and eligible collateral. Its published overview describes a
two-day horizon, joint price and implied-volatility factors, volatility
clustering, fat-tailed innovations, correlation and tail dependence, 99%
Expected Shortfall, and dependence/concentration stress components. A
deterministic scenario grid in this engine is not STANS and must not be
described as an OCC margin replica.

FINRA Rule 4210 portfolio margin uses theoretical gains and losses at ten
equidistant valuation points. Published reference ranges include +6%/-8% for a
high-capitalization broad index, +/-10% for another broad index, and +/-15% for
an equity or narrow-based index. Those ranges are useful transparent research
profiles. They do not make this engine a FINRA portfolio-margin calculator, and
the engine's theoretical model is not represented as SEC-approved.

Cboe's current options risk specification separates operational execution
limits by risk root, EFID, and EFID group. Its premium-based notional counter
does not net buy/sell or complex-order legs. It also documents that a full
incoming execution, routed-away activity, and distributed best-effort controls
can exceed a threshold before cancellation and rejection take effect. The
backtest may impose a stricter pre-trade envelope, but it cannot use that model
to claim that a live venue limit is unbreachable.

SEC Rule 15c3-5 requires reasonably designed aggregate credit/capital and
erroneous price/size controls for market access, together with documented,
reviewed supervisory controls. This research component can inform those control
categories; it is not a broker-dealer compliance system.

CME SPAN provides a useful implementation precedent for scenario arrays:
price, volatility, and time-to-expiry changes are revalued, positions sharing an
ultimate underlying are grouped, and spread credits are explicit rather than
inferred. This design follows the scenario-array pattern without claiming CME
margin equivalence.

## Authoritative full-reprice sidecar

### Purpose

Risk calculation must not refit surfaces or invoke an allocating pricer inside
the decision loop. A cold-path producer creates an immutable, point-in-time
sidecar containing authoritative shocked-minus-base P&L and named Greek
measures needed at every decision. The hot-path risk engine only validates,
indexes, and reduces those arrays.

The sidecar is model evidence, not market execution evidence. Execution prices
remain governed by the replay engine. The upstream producer owns the identified
base risk mark used to create each scenario P&L row.

### Implemented schema

```text
OptionRiskPanelProvenance
  pricer_model_version
  greek_convention_version
  risk_snapshot_digest
  scenario_manifest_digest

OptionRiskScenario
  scenario_id
  source_identity

OptionRiskContractRow
  decision_ts_ns
  contract_id
  engine_id
  underlier_uid
  observed_ts_ns
  available_ts_ns
  expiry_ts_ns
  strike
  side
  multiplier
  standard_deliverable
  definition_source_identity
  spot_delta_cash_per_contract
  spot_gamma_cash_per_contract
  vega_cash_per_vol_point_per_contract
  theta_cash_per_day_per_contract
  vanna_cash_per_return_vol_point_per_contract
  volga_cash_per_vol_point_squared_per_contract
  premium_cash_notional_per_contract
  status
  risk_source_identity
  surface_source_identity

OptionRiskScenarioPnlRow
  decision_ts_ns
  contract_id
  scenario_id
  observed_ts_ns
  available_ts_ns
  pnl_per_long_contract
  source_identity
```

The retained arrays are date-major/contract-major for Greeks and
date-major/scenario-major/contract-major for P&L. Construction uses checked size
arithmetic and caller-configured contract-row, scenario, scenario-row, and byte
limits. It canonicalizes and validates the complete dense grids before exposing
a view.

The two 256-bit digests are caller-attested artifact identities. `create()` only
rejects an all-zero digest; it does not recompute or cryptographically verify
the supplied bytes. A trusted loader must verify them before construction. The
panel's 64-bit FNV definition hash covers all canonical values, clocks, catalog
fields, identities, manifests, and digests, but is deliberately only a
deterministic regression fingerprint, not cryptographic provenance.

### Construction and validation

For every tradable risk cell:

- definition, multiplier, expiry, exercise style, deliverable, risk root,
  currency, surface, rates, dividends, borrow, and FX inputs must be
  point-in-time and available no later than the decision;
- the contract definition must match the research panel and execution catalog;
- `base_value` and every required scenario value must be finite and inside the
  configured pricer domain;
- the scenario manifest and its ordering must be identical for every contract
  in a run;
- any volatility or rate clamp must be part of the scenario policy and recorded
  in status; an unconfigured clamp is an error;
- duplicate `(decision_ts_ns, contract_id)` rows, missing scenarios, mismatched
  lineage, and availability after the decision fail closed.

The authoritative sidecar should use the engine's production-quality full
repricer for the product: European and American contracts must not silently
share an approximation that changes exercise semantics. Unsupported adjusted
deliverables remain unavailable.

## Unit conventions

All public risk fields require units in their names or type contracts.

- Position and order quantities are signed whole listed contracts.
- Option prices are dollars per option unit.
- The multiplier is underlying units per listed contract.
- Scenario value and P&L are dollars per listed contract after applying the
  multiplier.
- `spot_delta_cash_per_contract` is dollars for a +100% proportional spot move
  under the local first-derivative convention.
- `spot_gamma_cash_per_contract` is the corresponding raw second derivative;
  it excludes the one-half Taylor P&L factor.
- `vega_cash_per_vol_point_per_contract` is dollars for one absolute
  implied-volatility percentage-point move.
- `vanna_cash_per_return_vol_point_per_contract` is the cross derivative for a
  100% spot return and one volatility-point move.
- `volga_cash_per_vol_point_squared_per_contract` is the raw second volatility
  derivative and excludes the one-half Taylor P&L factor.
- `theta_cash_per_day_per_contract` is dollars per calendar day under the
  versioned upstream Greek convention.
- `scenario_pnl` is shocked value minus base value, in dollars per contract.
- `scenario_loss` is `-scenario_pnl`; positive values are losses.
- `premium_cash_notional` is
  `abs(contracts) * option_price * multiplier` dollars.
- `exchange_premium_counter` is the separately named
  `abs(contracts) * option_price` counter used to mirror Cboe's published
  premium-times-contract examples. It must never be confused with cash
  notional.
- Spot shocks are fractional returns. Implied-volatility shocks are absolute
  volatility points. Time shocks are integer nanoseconds.

Signed and gross-absolute Greek totals must both be available. A signed net
Greek never satisfies a gross concentration limit.

## Position and fill-state model

At a sealed decision frontier, for contract `i`:

```text
P_i = filled contracts
q_ij = signed leaves for every active existing order
c_ik = signed leaves for every candidate order
```

Active existing leaves include `Scheduled`, `Working`, `PartiallyFilled`, and
`PendingCancel`. A cancel command removes no exposure until its modeled
cancel-effective event. The risk engine evaluates:

```text
filled_i            = P_i
projected_i         = P_i + sum(q_ij) + sum(c_ik)
worst_short_i       = P_i + sum(min(q_ij, 0)) + sum(min(c_ik, 0))
worst_long_i        = P_i + sum(max(q_ij, 0)) + sum(max(c_ik, 0))
```

Net projected contracts are insufficient. Opposite live buys and sells can
hide gross churn and distinct adverse fill paths.

### Exact fill-subset envelope

Each active leaf has a fill variable `f` in `[0, 1]`. Whole-order endpoints
represent no fill and complete remaining fill. Any partial integer fill lies
between them.

For a fixed market scenario `s`, let `u_is` be scenario P&L per contract. The
portfolio P&L for a fill vector is:

```text
PnL_s(f) = sum_i P_i * u_is
         + sum_ij f_ij * q_ij * u_is
         + sum_ik f_ik * c_ik * u_is
```

This expression is affine in every fill variable. Its worst value over
independent partial fills is therefore attained at endpoints. The exact
worst-fill subset for scenario `s` fills a leaf when its incremental scenario
P&L is negative and leaves it unfilled otherwise. No exponential enumeration is
needed for scenario P&L.

Signed Greek exposure is also affine in fills. Exact minimum and maximum
endpoints are computed by summing negative and positive leaf contributions
separately. The maximum absolute exposure is the worse absolute endpoint.

For any future nonseparable constraint that cannot be reduced to affine
scenario P&L or an interval endpoint, the engine must either:

1. enumerate all fill subsets within a configured maximum active-leaf count;
2. use a registered conservative upper bound whose version is audited; or
3. reject exposure increase.

It must not silently substitute the all-fill state. The independent-fill
envelope can include combinations that shared venue liquidity would not permit;
that conservatism is intentional. Atomic or proportional multi-leg fills are
never assumed.

## Risk measures and limit hierarchy

The implemented engine reports filled and projected-all-fill point metrics plus
baseline and candidate exact worst-fill metrics. Hard limits apply to the
account/basket envelope and to the maximum single-underlier scenario loss.
Finer policy scopes remain a later extension:

```text
contract -> risk root -> decision basket -> strategy -> account
```

### Operational gross measures

The broader control model should eventually keep these non-netted:

- absolute contracts and contracts per order;
- open-order and active-leaf count;
- exchange premium counter and premium cash notional;
- cash debit;
- order, volume, execution-count, and notional rate windows;
- displayed-size and interval-volume participation;
- gross multiplier-adjusted units;
- concentration by contract, expiry, risk root, and basket.

### Greek measures

The implemented hard limits cover signed absolute delta, gamma, vega, theta,
vanna, and volga plus gross gamma, vega, vanna, and volga. Share-delta and
alternate normalized conventions remain possible future measures:

- signed and gross share delta;
- signed and gross `dollar_delta_1pct`;
- signed and gross `dollar_gamma_1pct`;
- signed and gross `vega_01`;
- signed and gross vanna and volga;
- theta loss over the named horizon.

Limits must bind an exact convention version and unit. Missing higher-order
Greeks may be tolerated only when authoritative full-reprice scenario coverage
is complete and the configured policy does not require those Greek limits.

### Scenario measures

The implemented hard scenario measures are:

- maximum positive scenario loss;
- worst loss by risk root;
- sum of risk-root worst losses;
- maximum single-root loss independently across all scenarios.

Largest-contract contribution and additional concentration/gap measures remain
future policy extensions.

Empirical VaR or Expected Shortfall is deferred until a point-in-time,
versioned, weighted historical scenario distribution and its validation report
exist. A deterministic grid maximum must not be labeled ES.

## Scenario library

The scenario manifest is immutable and versioned. A minimum research library
should contain:

- spot shocks at configurable symmetric and asymmetric levels;
- the FINRA-style ten-point grids as explicitly named reference profiles;
- parallel implied-volatility shocks;
- skew tilt, term-slope, and surface-curvature shocks;
- joint spot-down/vol-up and spot-up/vol-down leverage scenarios;
- both sticky-strike and sticky-delta surface dynamics, or a configured
  conservative choice of the worse result;
- one- and two-calendar-day decay;
- rate, dividend, borrow, and FX shocks where those inputs affect value;
- single-root and largest-component idiosyncratic gaps;
- for dispersion, index/component volatility-basis widening and narrowing plus
  correlation-up, correlation-down, and correlation-collapse cases;
- reverse-stress searches over a bounded spot/volatility box, when a
  deterministic solver and capacity bound are available.

Scenario generation must preserve contract domain validity without inventing
data. A scenario that cannot be priced is unavailable, not zero P&L.

The implemented reducer treats scenario IDs as opaque. Shock parameters and
surface dynamics live in the caller-verified external manifest identified by
`scenario_manifest_digest`; this slice does not generate or reinterpret them.

## Netting and risk credit

The default policy is `ZeroCrossUnderlierCredit`.

Within one legal risk account and one risk root, full-reprice scenario P&L may
net calls, puts, strikes, and expiries under the same simultaneous shock. Gross
operational counters and gross Greek limits still do not net.

Across risk roots, the initial hard account loss is:

```text
account_loss = max_s sum_r max(0, -root_pnl[r, s])
single_root_loss = max_(r, s) max(0, -root_pnl[r, s])
```

The account calculation preserves the common scenario but floors each root's
gain at zero before summing, so it grants no favorable single-name/index,
sector/component, or cross-currency offset. The single-root maximum is tracked
independently and records its own binding scenario/root IDs.

Any future cross-root credit requires a separately versioned eligibility map,
simultaneous scenario definition, haircut, credit cap, legal-account boundary,
currency conversion, and validation report. Missing or stale eligibility
evidence means zero credit.

Working buy and sell leaves never net for order count, rate, volume, premium, or
other operational limits. No positions net across clearing accounts. Adjusted
deliverables, incompatible settlement types, and unsupported expiries receive
no spread credit.

Economic scenario netting in this engine is not legal or broker margin netting.

## Basket admission and scaling

The risk engine evaluates the entire proposed cancel/order basket before
materialization. A passing basket is accepted unchanged.

Uniform basket scaling is not implemented in this slice. A failing
exposure-increasing basket is rejected in full. A future implementation may
scale by one deterministic scalar `alpha` in `[0, 1]`:

1. multiply every new order quantity by the same `alpha`;
2. convert every leg toward zero to whole contracts;
3. preserve cancellation commands;
4. rebuild the exact fill envelope;
5. rerun every operational, Greek, and scenario limit.

The search ordering and tie-break must be versioned. Independent per-leg clamps
are prohibited because they can destroy dispersion hedge ratios. If no positive
whole-contract basket passes, reject new orders and retain any required
cancellations.

## Implemented dispositions

```text
Accept
ReduceOnlyAccept
CancelOnly
RejectNewOrders
```

These are per-decision dispositions, not a separately persisted risk-mode state
machine.

### Accept

New exposure is allowed only when filled, projected, exact worst-fill, and
candidate-added states pass all hard limits.

### ReduceOnlyAccept

Risk inputs are complete, but current or projected state breaches a limit. A
candidate is admissible only when:

- no previously unbreached hard measure becomes breached;
- every breached hard measure is no worse under every allowed fill subset;
- worst scenario loss is no worse;
- at least one breached measure strictly improves for a nonzero fill; and
- the candidate does not rely on a cancellation becoming effective.

Simple order direction is not sufficient to prove reduce-only for options.
Removing one leg of a spread can worsen scenario risk even when absolute
contracts fall.

### CancelOnly

An existing live-order envelope breaches a limit and either there is no
candidate basket or the candidate fails the reduce-only proof. Only
cancellation commands are admitted. Pending cancellations remain in the
envelope.

### RejectNewOrders

The baseline live-order envelope is within limits, but a candidate would
breach. New orders are suppressed while ordinary reconciliation cancellations
remain allowed. When the baseline already breaches and a candidate fails the
reduce-only proof, the disposition is `CancelOnly` instead.

Invalid, unavailable, nonfinite, inexact, or capacity-exceeding evidence returns
an error and fails the coordinator run before applying that decision's
commands. The engine never claims immediate flattening: in-flight and pending-
cancel leaves remain fillable until the replayed cancel-effective event, just
as Cboe documents threshold overshoot and distributed cancellation latency.

## Fail-closed rules

No missing value silently becomes zero risk.

The current dense sidecar fails the complete run when any required item is
missing, nonfinite, stale, inconsistent, or unavailable:

- mark, base value, scenario value, required Greek, spot, volatility surface,
  rates, dividends, borrow, or FX;
- multiplier, expiry, exercise style, deliverable, currency, or risk root;
- scenario row or scenario-manifest identity;
- netting map, limit row, or model/convention version;
- source content digest, sequence attestation, or required channel watermark.

If an existing filled position has no bounded authoritative valuation, the
engine returns an error; it does not report a finite account loss. The initial
dense schema requires every named Greek even when its corresponding hard limit
is disabled.

Unconfigured model clamps, pricer domain errors, arithmetic overflow, capacity
exhaustion, duplicate identities, partial basket valuation, and scenario-order
mismatch reject the complete decision before session mutation.

Near expiry, no exercise, assignment, or settlement credit is granted. A
nonzero position at the synthetic cutoff retains the existing fail-closed
boundary.

## Audit and provenance

Every successful risk evaluation is embedded in the bounded
`OptionAdaptiveDecisionAudit` as an `OptionPreTradeRiskEvaluation` containing:

```text
filled
baseline_projected
baseline_worst_fill
candidate_projected
candidate_worst_fill
baseline_breach_mask
candidate_breach_mask
disposition
input_hash
```

The point and worst-fill records preserve binding account-scenario and
independent root-scenario/root IDs. The coordinator audit also binds target,
command, input-state, session, run-definition, and decision-trace fingerprints.
The exact fill subset is represented by its canonical low/high interval rather
than materialized as a leaf bitset.

A deterministic `RiskRunDefinition` digest should bind:

- exact git/build/model/convention versions;
- capacities and ordering/tie-break rules;
- catalog, panel, sidecar, scenario, surface, curve, FX, limit, and netting
  content identities;
- initial account state;
- all latency and lifecycle configuration;
- the complete canonical replay selection.

The implementation uses canonical 64-bit FNV fingerprints for deterministic
regression and binds the caller-attested 256-bit sidecar digests into those
fingerprints. A future artifact writer should add canonical serialization,
cryptographic run-definition/output digests, and separate runtime attestation
fields. Existing 64-bit fingerprints are not provenance attestations.

## Implemented C++20 surface

```cpp
enum class OptionRiskDisposition : std::uint8_t {
  Accept,
  ReduceOnlyAccept,
  CancelOnly,
  RejectNewOrders,
};

struct OptionRiskEngineLimits {
  std::size_t max_contracts;
  std::size_t max_live_leaves;
  std::size_t max_candidate_leaves;
  std::size_t max_scenarios;
  std::size_t max_underliers;
  std::size_t max_workspace_bytes;
};

class OptionPreTradeRiskEngine {
public:
  static Result<OptionPreTradeRiskEngine>
  create(OptionRiskEngineLimits limits);

  Result<OptionPreTradeRiskEvaluation>
  evaluate(const OptionRiskPanel &risk_panel,
           std::size_t date_index,
           std::span<const OptionInstrument> contract_catalog,
           std::span<const std::int64_t> filled_contracts,
           std::span<const OptionRiskLeaf> live_leaves,
           std::span<const OptionRiskLeaf> candidate_leaves,
           const OptionRiskHardLimits &hard_limits);
};
```

`create()` reserves all quantity intervals, canonical root mapping, and
scenario-reduction scratch. A successful `evaluate()` performs no allocation.
The returned evaluation owns value records and does not borrow mutable spans.
The scenario cube is read once per evaluation while five independent
filled/projected/worst-fill account and root extrema are reduced in parallel.

Direct callers must validate the contract catalog's provenance lineage against
each row's `definition_source_identity`; the adaptive coordinator performs this
check before calling the reducer. `OptionRiskRowStatus::Ok` is a trusted
upstream freshness attestation. The panel validates point-in-time ordering, but
this slice does not impose a second wall-clock age threshold; upstream loaders
must mark rows `StaleMarket` under their versioned age policy. Adding a hashed,
per-source age-limit policy remains a later extension.

All arithmetic and comparisons use finite binary64 values. Limits are
inclusive and the rounded aggregate is authoritative. Deployments that require
outward rounding or additional numerical conservatism must apply a documented
limit haircut upstream.

## Initial implementation slice

The implemented first slice is intentionally narrower than a clearing model:

1. immutable canonical date-major full-reprice sidecar and validator;
2. externally produced, versioned scenario manifest and dense P&L cube;
3. account scenario max loss, independent single-root max loss, signed Greeks,
   gross higher-order Greeks, gross premium, and open-order contracts;
4. exact independent fill-subset scenario loss and Greek intervals;
5. account/basket limits plus a single-underlier scenario-loss limit;
6. zero cross-underlier credit;
7. `Accept`, `ReduceOnlyAccept`, `CancelOnly`, and `RejectNewOrders`;
8. bounded decision audits with canonical noncryptographic fingerprints;
9. integration after reconciliation and before order materialization;
10. exact workspace accounting, no-allocation hot-path tests, focused
    benchmarks, and a fixed-seed brute-force subset oracle.

Scenario generation itself, per-contract/strategy/legal-account policy scopes,
uniform whole-basket integer scaling, persistent risk modes, cryptographic
run-definition artifacts, and durable append-only audit storage are deferred.
So are reducer-owned maximum-age policies; freshness is currently attested by
the upstream row status and bound source identities.

Empirical ES, calibrated dependence, reverse-stress optimization, cross-root
credits, broker margin replication, and stock/futures hedge replay remain later
milestones.

## Acceptance invariants

- Pending-cancel exposure is identical to working exposure until
  cancel-effective.
- No candidate passes all-fill but fails an allowed adverse fill subset.
- Scenario loss and Greek interval bounds match brute-force subset enumeration
  on small randomized baskets.
- Opposite buy/sell leaves cannot disappear through net aggregation.
- Any future uniform scaling never increases an absolute order quantity and
  always rechecks after integer conversion.
- Input permutation cannot change a decision or its deterministic digest.
- Missing, stale, or nonfinite risk evidence never produces zero exposure or
  zero loss.
- No cross-underlier offset affects a hard limit under the default policy.
- `ReduceOnly` cannot worsen any breached hard measure for any fill subset.
- `CancelOnly` emits no new order.
- A risk rejection has a reasoned audit record; a hard evaluation error is
  returned through the coordinator error path.
- Successful hot-path evaluation performs no allocation.

## Explicit limitations

This design is a deterministic research pre-trade gate. It is not:

- OCC STANS or an OCC margin estimate;
- FINRA portfolio margin or an SEC-approved theoretical model;
- broker buying-power, house-margin, credit, or compliance logic;
- an exchange risk-limit or execution guarantee;
- proof that a limit cannot be exceeded after orders become live;
- atomic complex-order execution;
- a queue, hidden-liquidity, routing, auction, or market-impact model;
- exercise, assignment, settlement, adjusted-deliverable, borrow, or locate
  completeness;
- a calibrated empirical ES model in the initial slice.

There is no synchronized hedge-underlier execution frontier yet. The engine
therefore grants no credit for proposed stock or futures hedges, and it cannot
claim delta-neutral execution of dispersion baskets. Existing option-only
scenario results remain Consolidated-L1, model-on-model research evidence.

## Primary and foundational sources

- [OCC Margin Methodology](https://www.theocc.com/risk-management/margin-methodology)
  describes STANS full-portfolio Monte Carlo, 99% Expected Shortfall, joint risk
  factors, and dependence/concentration stress components.
- [Cboe Titanium U.S. Options Risk Management Specification](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-options-risk-management-specification)
  documents risk-root/EFID limits, gross premium-based notional, cancel/reject
  behavior, best-effort processing, and threshold-overshoot cases.
- [Cboe Titanium U.S. Options BOEv3 Specification](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-options-boev3-specification)
  documents order-entry operational controls including fat-finger and open-order
  boundaries.
- [SEC Rule 15c3-5 adopting release](https://www.sec.gov/files/rules/final/2010/34-63241fr.pdf)
  provides the market-access credit/capital and erroneous-order control basis.
- [SEC Market Access Rule FAQ](https://www.sec.gov/rules-regulations/staff-guidance/trading-markets-frequently-asked-questions/divisionsmarketregfaq-0)
  discusses threshold governance, automated orders, and ongoing review.
- [FINRA Rule 4210](https://www.finra.org/rules-guidance/rulebooks/finra-rules/4210?page=1)
  defines portfolio-margin security classes, theoretical valuation points, and
  published reference ranges.
- [FINRA SEA Rule 15c3-1a Appendix A interpretations](https://www.finra.org/rules-guidance/guidance/interpretations-financial-operational-rules/sea-rule-15c3-1a-and-related-interpretations)
  provides related option theoretical-price and portfolio-risk interpretations.
- [CME SPAN Methodology Overview](https://www.cmegroup.com/solutions/risk-management/performance-bonds-margins/span-methodology-overview.html)
  documents price/volatility/time scenario arrays, ultimate-underlying groups,
  and explicit spread charges and credits.
- [CME SPAN 2 Methodology](https://www.cmegroup.com/clearing/risk-management/span-overview/span-2-methodology.html)
  documents historical scenario analysis and explicit implied-volatility
  surface/skew, concentration, and liquidity components.
- [Artzner, Delbaen, Eber, and Heath, “Coherent Measures of Risk”](https://doi.org/10.1111/1467-9965.00068)
  establishes the coherent-risk framework and the importance of subadditivity.
- [Boyd et al., “Multi-Period Trading via Convex Optimization”](https://stanford.edu/~boyd/papers/cvx_portfolio.html)
  motivates constrained receding-horizon decisions that execute the current
  action and replan from later realized state.
