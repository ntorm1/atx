# W1-B1 preparation: one cost surface

**Preparation only.** No W1 production edits or builds are authorized yet. Start
implementation from the forthcoming frozen W0 gate plus Lane 0 scaffold, not this
lane's branch. Surveyed at `d4b86cd23581e5d4bba92b02181001cd0dce36f7`; graph tools
were unavailable, so discovery used targeted `rg` and source reads.

Authority: `docs/plans/2026-09-24-alpha-engine-production-swarm.md`, W1-B1;
findings B-01, B-06, B-07 and the cost component of R-19. Implement economic
behavior first, then add focused acceptance checks. This lane delivers the
authoritative implementation and adapters; A4/R3/B2/B3 retain their DAG ownership
of consumer migration. Do not claim all production paths are unified before those
wires land.

## Existing seams and migration owners

| Current code | Observed behavior | B1 deliverable / later owner |
|---|---|---|
| `book/replay_cost.{hpp,cpp}` | `ReplayCostModel::cost` prices capped dollar fills; `unrationed_cost` prices the whole request; `SqrtImpactCost` already implements spread + commission + power impact. Default Y is 1. | Surface-backed adapter, preserving explicit legacy flat/sqrt entry points; B2 supplies causal rows and metaorder state. |
| `cost/optimizer_cost_terms.hpp` | `cost_terms_from_sqrt_impact` maps kappa and 3/2 coefficients; rejects delta other than 0.5. Carries `untradeable` and `max_trade`, but those need enforcement downstream. | Generate coefficients from the same surface, never a second formula; R3 enforces pins/trade boxes and wires solver costs. R1 owns constraint representation. |
| `cost/calibration.hpp` | `calibrate_from_obs` fits robust log-linear impact, clamps delta, leaves the old intercept. `calibrate_impact` takes simulator `FillPayload::impact` and a Market snapshot. | Fixed-slope intercept refit plus corrected diagnostics; distinguish observed execution estimates from synthetic/model-origin observations. |
| `cost/borrow.hpp`, `book/borrow_schedule.hpp` | Optimizer helpers take annual fractions; replay schedules take annual bps. W0 fee-once semantics forbid fee plus net rebate double counting. | Named checked fraction/bps adapters and modeled tiers; B2 wires fee-quoted schedule. Do not change W0 financing semantics. |
| `alpha/streams.hpp`, `factory/fitness.cpp` | Streams charge only PerDollar commission on target differences. `book_cost_bps` prices last-period holdings via round-trip proxy; absent volume can imply zero cost. | Pure trade-sequence fitness adapter; A4 migrates streams/objective to actual delayed trades and the surface. |
| `atx-impl/src/{research_cost_sim.hpp,replay_report.cpp,equity_allocation.cpp}` | Research simulator accepts `ReplayCostModel`; production report/allocation still use scalar trade bps. Research simulation uses a fixed AUM and gross-drift approximation. | Document surface adapter interface; B2/R3 own production plumbing and marked holdings. Do not treat research simulation as authoritative accounting. |
| `risk/capacity.hpp`, `cost/capacity.hpp`, `cost/cost_aware.hpp` | Capacity prices holdings; the old round-trip helper includes temp/slippage twice and a permanent footprint. | Surface can price a supplied trade path at any NAV; B3 replaces holdings/in-sample capacity and re-optimizes. Do not add legacy round-trip/permanent charges on top. |

## One authoritative units and timing contract

For pretrade NAV `N > 0`, signed executed dollar delta `q`, weight delta `z=q/N`,
prior known dollar ADV `A > 0`, daily return-volatility fraction `sigma`, full
spread fraction `s`, and `Y=0.6` prior:

```text
linear_rate = 0.5 * spread_scale * s + 1e-4
impact_rate = Y_i * sigma * sqrt(abs(q) / A)
cost_dollars = abs(q) * (linear_rate + impact_rate)
cost_return = cost_dollars / N
            = linear_rate*abs(z) + Y_i*sigma*sqrt(N/A)*abs(z)^(3/2)
trade_rate_bps = 1e4 * cost_dollars / abs(q)       [q != 0]
book_cost_bps  = 1e4 * cost_return
```

The `CostSurface` boundary owns these conversions. Quotes should expose spread,
commission, impact, total dollars and total NAV-fraction separately. Zero trade
costs zero; missing/invalid pricing inputs on a nonzero trade are explicitly
unpriceable, never a free executable trade. Optimizer adapters return mandatory
untradeable/zero-trade-cap status; replay adapters cannot fill unpriceable names.
Test full-request parity without caps and filled-request parity at the same capped
quantity separately; a smaller fill is not the same trade.

Production surface v1 pins delta=0.5 for the convex 3/2 optimizer contract. The
general calibrator may estimate/clamp another delta; applying it to v1 requires
an explicitly constrained delta=0.5 intercept fit. Never silently reuse a Y fit
at a different exponent. Preserve calibrated/raw/applied parameters in metadata.

Use actual delta holdings at the execution boundary after marking; target weights
minus stale target weights are not turnover. Raw as-traded price times raw volume
defines dollar ADV. Returns/volatility may use the declared total-return basis;
adjusted price levels must not manufacture liquidity. Borrow is separate holding
accrual: `short_dollars * annual_fraction * elapsed_days/day_basis`, with
`annual_bps=1e4*annual_fraction`. It is not charged once per trade or multiplied by
turnover. Preserve fee-once and elapsed calendar-time semantics.

An immutable per-date surface snapshot binds instrument identities/order,
availability/as-of time, source hashes, estimator/window conventions, rates,
calibration origin, scale knobs and version in its canonical hash. Reject future
availability. Market data and model estimates must be available before the
decision; execution uses the declared prior-known liquidity snapshot, never that
session's eventual OHLC/volume. Keep O(names) snapshot storage and bounded trailing
windows; do not allocate a dense dates x names x candidate cube. Consumer owners
retain marked-NAV/trade-state memory and accounting ownership.

## Implemented capabilities required from B1

1. `CostSurface` and quote/adaptor implementation above; hash finite values with
   stable canonical ordering and explicit missing tags. All adapters share the
   same hash and component calculations.
2. Daily Corwin-Schultz, Abdi-Ranaldo and EDGE spread estimators, equal-weight
   composite, explicit missing/negative-estimate policy and scale knob. Scale is
   a recorded assumption, not evidence of observed bid/ask quotes. Pin an upstream
   reference revision and fixtures before claiming 1e-10 parity. EDGE outputs a
   full-spread fraction, requires at least three observations, and preserves a
   regular grid for missing observations; its signed/absolute convention must be
   explicit. [Primary EDGE reference](https://github.com/eguidotti/bidask),
   [C++ units](https://github.com/eguidotti/bidask/tree/main/c%2B%2B).
3. For clamped delta `d`, robustly refit the intercept to
   `log(temp/sigma)-d*log(participation)` with a one-column design; recompute Y,
   residuals, fit quality and applicable uncertainty. Record clamp/fixed-slope
   status. Validate finite inputs, not merely positive comparisons. A fit to the
   simulator's own output is a synthetic consistency check, not observed-cost
   calibration.
4. Borrow tiers with explicit versioned predictor units/thresholds: GC 25-30 bps,
   warm 1-5% annual, special 5-50% annual; use known size, raw price, SI/float and
   IPO age. Unknown predictors are flagged and follow a declared conservative
   fallback, never silently assigned cheap GC. These are **modeled fee priors**:
   no observed borrow calibration exists here, and fees do not establish locate
   availability or executable short capacity. A modeled tier cannot create a
   positive locate limit; B2 must consume independent locate/recall evidence.
5. FIM size/idiosyncratic-volatility adjusters need their units pinned. Table VII
   uses `log(1 + ME_in_USD_billions)`, annualized percent idiosyncratic volatility,
   and participation in percent of one-year average dollar volume. Its
   contemporaneous-return controls are unavailable as causal pretrade predictors.
   Keep that empirical reference separate from the convex surface; do not add
   both full models. The plan's approximate 13.7/32 bps examples need a declared
   intercept/control scenario and reference column before becoming an exact
   fixture; do not invent coefficients to hit them.
   [FIM 2018, Table VII, PDF p.62](https://spinup-000d1a-wp-offload-media.s3.amazonaws.com/faculty/wp-content/uploads/sites/3/2021/08/Trading-Cost.pdf#page=63).

## Owned files and Lane 0 request

B1 owns new `include/atx/engine/cost/{cost_surface,spread_estimators,borrow_tiers}.hpp`
and matching `src/cost/*.cpp`, existing `cost/{calibration,optimizer_cost_terms,borrow}.hpp`
as needed, and `book/replay_cost.{hpp,cpp}`. Keep heavyweight estimator/hash bodies
in source. No production edits in A4/R3/B2/B3/I1-owned consumers without root's
explicit ownership coordination.

Lane 0 should create **three** source stubs and add them once to
`atx-engine/CMakeLists.txt`: `src/cost/cost_surface.cpp`,
`src/cost/spread_estimators.cpp`, `src/cost/borrow_tiers.cpp`. `book/replay_cost.cpp`
is already listed. Existing core/book/risk test groups suffice; new cost fixtures
go under `tests/core/`, adapter/replay fixtures under their existing groups. No new
test framework, external runtime dependency, dispatch line or preset is required.
Register the new surface with X1's causal harness when its interface lands.

## Post-implementation acceptance

- Same signed trade/snapshot/NAV: fitness, optimizer evaluation and replay agree
  in return units within 1e-12, including components, both sides and capped cases.
  Long-to-short reversal charges the full delta; zero trade and unchanged marked
  holdings have no trade cost. Borrow fraction/bps adapters agree exactly within
  floating conversion tolerance and do not double-count a rebate.
- Synthetic independent fills recover Y/delta within 5%; a forced clamp verifies
  intercept refitting and honest diagnostics. Future-fill append does not alter
  a closed calibration snapshot.
- Pinned spread reference fixtures match 1e-10 across normal/missing/flat/invalid
  OHLC cases with the same declared conventions. Composite and scale changes are
  hashed; record the plan's post-2003 upward-bias limitation.
- Tier bounds and predictor availability are explicit; no missing input yields
  false zero cost or an invented locate. Reject inconsistent units/nonfinite NAV.
- Future perturbation preserves prefix quotes, hashes and adapter outputs;
  snapshot construction uses no later observations. Fixed trades with NAV scaled
  show the expected square-root impact-rate scaling. This is cost parity, not a
  claim that the old capacity estimator is corrected.
- Resolve/reference the FIM scenario before claiming its numerical acceptance.
  Run appropriate existing cost/calibration/replay/risk adapters through the
  build wrapper with isolated dependencies; add no tests before implementation.
