# Risk engine (`api/risk/`) — P&L explain, projection, SpiderRock bench

Design of record: `docs/superpowers/specs/2026-10-24-atx-vol-risk-engine-design.md` (goals,
decisions D1–D17, gates). Sprint narrative and measured numbers:
`sprints/2026-10-24-risk-engine-sprint.md`. This page is the short tour.

## What it is

A streaming risk engine for a book of listed American options and shares, evaluated against
one market environment at a time, with a Greek-level P&L explain between consecutive
environments. Built for backtests and intraday analytics at high throughput: one frame of
per-contract Greeks per environment, Taylor explain between two frames, deterministic across
thread counts, bounded allocation once warm.

## Headers

| header | role |
|---|---|
| `risk_types.hpp` | `RiskRoute`, `RiskFlag` / `RiskFlags`, `RiskLaneStatus`, `RiskNeeds` |
| `market_state.hpp` | `MarketState`: per-underlier spot / carry (`UnderlierState`, per-expiry `ExpiryCarry`), per-contract marks (`ContractMarks`, SoA), optional surfaces; `MarketStateStorage` owns, `make_market_state` views; `market_state_from_surfaces` for surface-only states |
| `risk_book.hpp` | `OptionLeg` / `ShareLeg`, `RiskBook` (dedups contracts on exact bits), `PreparedRiskBook` (rows grouped by (uid, expiry, side), ascending strike, with the leg→row maps) |
| `risk_frame.hpp` | `RiskFrame`: SoA Greeks per unique contract + per-uid spot / share marks + route, flags, status; `FrameCounters` |
| `risk_kernel.hpp` | `RiskKernel::family()` (sigma-family per (uid, expiry, side) group, cold fallback) and `RiskKernel::cold()` (per-contract American Greeks); `FamilyKernelPolicy`, `ColdKernelPolicy`, `RiskWorkspace` |
| `pnl_explain.hpp` | `explain` / `explain_into`: eight Taylor terms (delta, gamma, vega split into level / skew / curvature in standardized moneyness, theta, rho, vanna, volga, charm) + unexplained, shares as delta-one legs, `VolMoveConvention`, by-uid / by-expiry buckets; `RiskStream` (one frame per state, explain per step) |
| `strike_coord.hpp` | Strike coordinate conversions: strike, log-moneyness, K/S, standardized moneyness, delta under `DeltaConvention::{Forward, Spot, American}`; `TenorInterp::{TotalVariance, ShapeBlend}` and `TenorExtrapPolicy`; every answer carries flags |
| `theo_contract.hpp` | Theoretical contracts (`MaturitySpec` relative or absolute, `StrikeCoord`) resolved on a surface at a timestamp: `resolve_theo`, `resolve_theo_many`, `project_series` (fixed TTE / fixed moneyness or delta across historical surfaces) |
| `historical_var.hpp` | One-period historical VaR / ES over (base, shifted) state pairs for a reference book of theoretical templates (lane D) |
| `api/marketdata/spiderrock_store.hpp` | SpiderRock intraday store loader (`date=<d>/bucket_et=<HHMM>/` parquet partitions → SoA bucket → `MarketStateStorage`), SpiderRock-shaped synthetic store |

## Flow

```
MarketStateStorage ──make_market_state──▶ MarketState (spot, carry, marks, surfaces)
RiskBook ──PreparedRiskBook::create──▶ rows grouped (uid, expiry, side), ascending K
RiskKernel::evaluate(prepared, state, needs, frame, ws)   // one frame per state
explain(book, state_k-1, frame_k-1, state_k, frame_k, opts) // Taylor between two frames
RiskStream: start(state_0) ; step(state_k) → ExplainResult (frames computed once per state)
```

Vol resolution for a listed contract is marks-first (the state's own `ContractMarks`), then
the surface, flagged `MarkMissing` / `SurfaceMissing`. Carry precedence per contract:
`ExpiryCarry` row for its expiry → flat rate / q with escrowed cash dividends →
`CarryFrom::Surface`. The family kernel builds one sigma family per (uid, expiry, side) group
over the group's sigma box; lanes it cannot serve (short tenor below the family guard, outside
the box, near the exercise edge, per-Greek refusals) go to the cold route with
`ColdFallback` and the reason flag, never to a silent number.

## Conventions you must know

- Vega split (`ExplainOptions::vega_split`) pivots in standardized moneyness
  z = k / (σ_atm √T) at ±`z_ref`; `VolMoveConvention::ContractOwn` (default) measures the
  contract's own mark move at its rolled T; `CommonBaseMaturity` holds T.
- Identities are gated at ≤ 2 ulp: Σ eight terms (+ carry) + unexplained == pnl_total per
  row; level + skew + curvature + resid == vega term.
- Carry axis (design D23a, lane E): a re-mark of an expiry's carry inputs (`ExpiryCarry`
  q / pv_divs, or flat_q) between two states moves its forward at fixed spot by
  J = ln((S_b − pv_t)/(S_b − pv_b)) − (q_t − q_b)·T_b; per leg `pnl_carry = w·Δ_b·S_b·J`
  (`ExplainResult::pnl_carry`, the 21st per-leg double column; `ExplainTotals::pnl_carry`).
  Exact for the forward-driven price (calls with q_eff ≤ 0 ≤ r, puts with r ≤ 0 ≤ q_eff:
  ≥ 99.9 % captured); an American put whose early exercise binds over-captures (1.10–1.18×
  at r = 4.3 %, T = 0.3: its exercise premium's own carry sensitivity stays in unexplained).
  `CarryFrom::Surface` states and a row present in one state only are not decomposed. No
  solve; unmoved carry ⇒ +0.0 and the pre-axis bits.
- Per-expiry clock (design D22, lane E): `ExpiryCarry::T_override` (finite ⇒ > 0) is THE T
  of its expiry for both kernels and the explain (dt = the contract's own
  T_base − T_shifted whenever either state overrides the expiry); NaN keeps the
  `TimeSpec`. The SpiderRock loader fills it from the vendor's `years`
  (`SpiderRockClock::VendorYears`).
- Sticky carry mode (design D23b, lane E): the SpiderRock loader can build a bucket's state
  on the previous bucket's implied / published table (`market_state_from_bucket` with
  `previous`): an implied expiry holds to 3× the IQR guard, a published one turns implied
  only at half of it; the bench does this under `--store` (`--sticky-carry`).
- Totals are Neumaier-compensated in leg order; results are bit-identical across thread
  counts (a gate, not a goal).
- Shares are delta-one legs: `pnl_delta = qty · ΔS`; no financing / dividend lines yet.
- `DeltaConvention::Spot` was added; `strike_at_delta` in analytics refuses it and points at
  `convert_strike_coord`.
- `TenorInterp::ShapeBlend` is additive on `CurveSurface` / `PricedSurface` / views; the
  default `TotalVariance` path is bit-unchanged.

## Benchmark

`atx-vol-risk-bench` (`docs/risk-bench.md`): 10k options / 100 underliers / 100 share legs
over the SpiderRock 30-minute store, 10:00–15:00. Licensed drop: partition with
`scripts/spiderrock_intraday_to_store.py`, run with `--store <root> --date 2026-10-14`.
Synthetic: `--synthetic <dir>`. Baselines under `bench/baselines/…risk-bench-*.json`. The
loader filters to American exercise and standard option roots (design D16 / D17).

## Known limits (as of the sprint)

- European / forward-framework index options (SPX, NDX, XSP, RUT) are excluded by the
  loader; a European route is deferred.
- Vendor parity against SpiderRock `de` / `ve` / `srPrc` on the real book is a diagnostic,
  not a gate; see the sprint note for the measured gap and its investigation.
- `explain_into` keeps a thread-local grow-only scratch; an explicit workspace parameter is a
  post-sprint API cleanup candidate.
- No Python bindings for `api/risk/` yet.
