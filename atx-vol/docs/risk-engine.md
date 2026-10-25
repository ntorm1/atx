# Risk engine (`api/risk/`) — P&L explain, projection, historical VaR, SpiderRock bench

Design of record: `docs/superpowers/specs/2026-10-24-atx-vol-risk-engine-design.md` (goals,
decisions D1–D23, gates). Sprint narrative and measured numbers:
`sprints/2026-10-24-risk-engine-sprint.md`. This page is the short tour of the shipped engine.
Companions: [`risk-bench.md`](risk-bench.md) (bench CLI, loader conventions, report schema, per-leg
dump), [`historical-var.md`](historical-var.md) (VaR semantics and migration),
[`persist.md`](persist.md) (archived market states) and [`persist-history.md`](persist-history.md)
(surface timelines, recorded risk runs).

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
| `market_state.hpp` | `MarketState`: per-underlier spot / carry (`UnderlierState`, per-expiry `ExpiryCarry` incl. `T_override`), per-contract marks (`ContractMarks`, SoA), optional surfaces; `MarketStateStorage` owns, `make_market_state` views; `market_state_from_surfaces` for surface-only states |
| `risk_book.hpp` | `OptionLeg` / `ShareLeg`, `RiskBook` (dedups contracts on exact bits), `PreparedRiskBook` (rows grouped by (uid, expiry, side), ascending strike, with the leg→row maps) |
| `risk_frame.hpp` | `RiskFrame`: SoA Greeks per unique contract + per-uid spot / share marks + route, flags, status; `FrameCounters` |
| `risk_kernel.hpp` | `RiskKernel::family()` (sigma-family per (uid, expiry, side) group, cold fallback) and `RiskKernel::cold()` (per-contract American Greeks); `FamilyKernelPolicy`, `ColdKernelPolicy`, `RiskWorkspace` |
| `pnl_explain.hpp` | `explain` / `explain_into`: eight Taylor terms (delta, gamma, vega split into level / skew / curvature in standardized moneyness, theta, rho, vanna, volga, charm), the carry axis, unexplained; shares as delta-one legs; `VolMoveConvention`; by-uid / by-expiry buckets; `RiskStream` (one frame per state, explain per step) |
| `strike_coord.hpp` | Strike coordinate conversions: strike, log-moneyness, K/S, standardized moneyness, delta under `DeltaConvention::{Forward, Spot, American}`; `TenorInterp::{TotalVariance, ShapeBlend}` and `TenorExtrapPolicy`; every answer carries flags |
| `theo_contract.hpp` | Theoretical contracts (`MaturitySpec` relative or absolute, `StrikeCoord`) resolved on a surface at a timestamp: `resolve_theo`, `resolve_theo_many`, `project_series` (fixed TTE / fixed moneyness or delta across historical surfaces) |
| `historical_var.hpp` | `historical_var`: one-period historical VaR / ES over (base, shifted) state pairs for a reference book of templates (`TheoLeg`) plus share legs; `summarize_historical_var`. Replaces the deleted `src/analytics/var.*` / `historical_projection.*`; `dispersion_book_var` runs on it (below) |
| `api/marketdata/spiderrock_store.hpp` | SpiderRock intraday store loader (`date=<d>/bucket_et=<HHMM>/` parquet partitions → SoA bucket → `MarketStateStorage`), clock and carry modes, SpiderRock-shaped synthetic store |
| `api/persist/state_archive.hpp`, `risk_run_archive.hpp` | archived `MarketState`s served zero-copy; recorded frames / explains / VaR results (see "Persistence") |

## Flow

```
MarketStateStorage ──make_market_state──▶ MarketState (spot, carry, marks, surfaces)
  (or StateStore::open_bucket(date, HHMM) → MarketStateArchive::state(), ~11 ms for a top-100 bucket)
RiskBook ──PreparedRiskBook::create──▶ rows grouped (uid, expiry, side), ascending K
RiskKernel::evaluate(prepared, state, needs, frame, ws)    // one frame per state
explain(book, state_k-1, frame_k-1, state_k, frame_k, opts) // Taylor between two frames
RiskStream: start(state_0) ; step(state_k) → ExplainResult (frames computed once per state)
RiskRunWriter::append_frame / append_explain               // optional recording
```

Vol resolution for a listed contract is marks-first (the state's own `ContractMarks`), then
the surface, flagged `MarkMissing` / `SurfaceMissing`. Carry precedence per contract:
`ExpiryCarry` row for its exact expiry → flat rate / q with escrowed cash dividends →
`CarryFrom::Surface`. A failed `RiskStream::step` (or an exception out of it) leaves the stream
requiring a new `start`; no pointer to a caller-owned state survives it.

## Kernels, routes and flags

Every lane carries a `RiskRoute` (`Family`, `ColdAnalytic`, `ColdFd`, `Exercised`, `Share`,
`Refused`) and flags; a refused lane is excluded from totals and counted with its base premium,
never a NaN in a sum. The family kernel builds one geometry per (uid, expiry) block and one
`AlSigmaFamily` per (uid, expiry, side) group over the group's sigma box, then serves each lane
from it. Lanes it does not serve go to the cold route, each with its reason:

| flag | meaning |
|---|---|
| `ColdFallback` | the family refused the lane (outside the box, a per-Greek refusal, a failed prepare, …) |
| `NearExerciseEdge` (+ `ColdFallback`) | inside the family's own edge band: the WHOLE lane goes cold (`cold_near_edge`, design D11). On the pure cold kernel the bit means the exercise regime only |
| `ShortTenor` (+ `ColdFallback`) | T < `kAlSigmaFamilyMinTau` (3/365): no family exists for the block (the guard is not lowered, D19) |
| `FamilyUneconomic` (no `ColdFallback`) | the group's family cannot amortize (family economics below): priced cold-analytic directly |

With `cold_fallback` off these lanes stay `Refused` with their flags. Every cold lane whose
inputs equal its block's geometry is priced on the prepared geometry, bit-identical to the
unprepared call. `FrameCounters` reports `families_built`, `family_nodes` (nodes per family =
`family_nodes / families_built`), `family_rows`, `cold_rows`, `uneconomic_rows`,
`short_tenor_rows`, `exercised_rows`, `refused_rows` and the evaluate / family-build clocks.

### `FamilyKernelPolicy` knobs (shipped defaults; lane A2)

| field | default | effect | bench flag |
|---|---|---|---|
| `n_sigma` | 12 | node CEILING (4..16) of the adaptive count | `--n-sigma` |
| `n_sigma_min` | 5 | node floor, capped at `n_sigma` (≥ 4) | `--n-sigma-min` |
| `sigma_node_spacing` | 0.06 | target log-sigma spacing h: n = clamp(⌈ln(σ_hi/σ_lo)/h⌉ + 1, floor, n_sigma) | `--sigma-node-spacing` |
| `sigma_box_pad` | 0.05 (was 0.25) | box = [min σ·(1 − pad), max σ·(1 + pad)]; boxes are rebuilt per state, so the pad buys width, not containment | `--sigma-box-pad` |
| `short_box_tau` / `n_sigma_min_short` | 0.3 y / 4 | a block with T < `short_box_tau` takes the floor `min(n_sigma_min_short, n_sigma)`; 0 = off | `--short-box-tau`, `--n-sigma-min-short` |
| `family_economics` | on | a group with `n·family_node_cost_us ≥ n_eligible·cold_lane_cost_us` (×3 / ×1.4 under rho) is priced cold-analytic, `FamilyUneconomic`; active only with `cold_fallback` and `analytic_cold`; reads no clock | `--family-economics` |
| `family_node_cost_us` / `cold_lane_cost_us` | 73 / 270 | in-situ costs of one sigma node / one cold-analytic lane; only their ratio enters | `--family-node-cost-us`, `--cold-lane-cost-us` |
| `dynamic_schedule` | true | blocks claimed one at a time (`run_dynamic`) instead of static contiguous chunks; same bits either way | `--family-schedule` |
| `cold_near_edge`, `cold_fallback`, `analytic_cold` | on | D11 edge rule; cold route present; analytic (AL) vs FD cold route | — |
| `sigma_box_floor`, `al_opts`, `n_threads` | 0.02, unset, 0 (auto) | box floor; AL options (policy → surface → `al_fast_opts()`, D12); fan-out | `--threads` |

Both floors ≥ `n_sigma` with pad 0.25 and economics off restore the pre-A2 fixed 12-node kernel.
Calibration (both bench stores, `--kernel both`, `cold.max_rel_price_family`): the binding regime is
a narrow long-dated box (T 0.5–2.3 y), where 4 nodes miss the 1e-6 price gate; the shipped
defaults hold it at ≤ 3.7e-7 (licensed) with about 5 nodes per family (12 before). The
bench echoes the effective policy (`inputs.family_policy`); its flag syntax and ranges are in
`risk-bench.md` "CLI". Every decision reads only the group's static shape and the policy, so totals
are bit-identical for any thread count and either schedule.

## Clock (design D22)

`ExpiryCarry::T_override` (NaN = the state's `TimeSpec`; finite ⇒ > 0, else `InvalidArgument`) is
THE T of its expiry for both kernels and for the explain: the kernel prices the expiry at it
(geometry, family and cold lanes), and the explain rolls it by the contract's own
`T_base − T_shifted` (theta / charm, `d_time`) whenever either state overrides the expiry. One
private helper (`risk_detail::expiry_T`) serves the kernel, `MarketState::resolve` / `carry_at`,
the explain and `historical_var`'s clock check. The SpiderRock loader fills it with each expiry's
median vendor `years` under `SpiderRockClock::VendorYears` (the spec default,
`kSpiderRockConventionsVersion` 3); `State` keeps the `TimeSpec` clock. Why: VolTime matches the
vendor's `years` to ≤ 0.02 d until the first NYSE early close and then drifts +0.47 d per early
close (`VolTimeCalendar` has full closures only; an early-close table is a separate core-clock
decision). Bench: `--time vendor|voltime|calendar365` (`risk-bench.md` "Clock").

## Carry (design D4, D20, D23)

The SpiderRock state carries one `ExpiryCarry{expiry_ns, pv_divs, rate, q, T_override}` per
(uid, expiry). Its basis (`SpiderRockCarryMode`) is `Published` (D4: the vendor's per-expiry
medians, pv_divs from `ddivPv`, `F = (S − pv_divs)·e^{(r−q)T}`) or `ImpliedFromCalls` (D20, the
loader default: F implied from the vendor's OTM calls, ≥ `kImpliedCarryMinCalls` (3) calls with
IQR ≤ `kImpliedCarryMaxIqrRel` (1e-3) × F, else the published row; q is expressed on the expiry's
own clock so the engine reproduces F exactly). **Sticky mode** (D23b): the 5-argument
`market_state_from_bucket(…, previous)` builds a bucket on the previous bucket's table — an implied
expiry holds to `kImpliedCarryStickyHoldFactor` (3) × the guard, a published one turns implied only
at `kImpliedCarryStickyEnterFactor` (0.5) × it; the bench does this under `--store`
(`--sticky-carry`). Algorithm, counts and measured flips: `risk-bench.md` "State per bucket and the
carry modes".

## Explain conventions you must know

- Vega split (`ExplainOptions::vega_split`) pivots in standardized moneyness
  z = k / (σ_atm √T) at ±`z_ref`; `VolMoveConvention::ContractOwn` (default) measures the
  contract's own mark move at its rolled T; `CommonBaseMaturity` holds T (the
  `PortfolioPricer::pnl_explain` convention, bit-exact on every Taylor column against it).
- Identities are gated at ≤ 2 ulp: Σ eight terms + carry + unexplained == pnl_total per row;
  level + skew + curvature + resid == vega term.
- **Carry axis** (D23a): a re-mark of an expiry's carry inputs (`ExpiryCarry` q / pv_divs, or
  flat_q) between two states moves its forward at fixed spot by
  J = ln((S_b − pv_t)/(S_b − pv_b)) − (q_t − q_b)·T_b; per leg `pnl_carry = w·Δ_b·S_b·J`
  (`ExplainResult::pnl_carry`, the 21st per-leg double column; `ExplainTotals::pnl_carry`).
  Exact for the forward-driven price (≥ 99.9 % captured). **Known limit (accepted):** it is the
  delta proxy, so an American put whose early exercise binds over-captures its carry P&L by
  1.10–1.18× (r = 4.3 %, T = 0.3); `pnl_unexplained` absorbs the remainder exactly. Under implied
  carry a rate re-mark moves q by dr, so `pnl_carry` books −w·Δ·S·T·dr, offset only by rho
  (`rate_move_without_rho` flags the step). `CarryFrom::Surface` states and a row present in one
  state only are not decomposed. No solve; unmoved carry ⇒ +0.0 and the pre-axis bits.
- `ExplainOptions::trapezoid_second_order` (opt-in) averages base and shifted gamma / volga / vanna
  (SpiderRock's risk-cube convention).
- Totals are Neumaier-compensated in leg order (column-parallel, same bits); results are
  bit-identical across thread counts (a gate, not a goal).
- Shares are delta-one legs: `pnl_delta = qty · ΔS`; no financing / dividend lines yet.
- `DeltaConvention::Spot` was added; `strike_at_delta` in analytics refuses it and points at
  `convert_strike_coord`. `TenorInterp::ShapeBlend` is additive on `CurveSurface` /
  `PricedSurface` / views; the default `TotalVariance` path is bit-unchanged.

## Historical VaR (design D13, D21)

`historical_var(book, scenarios, config)`: per (base, shifted) pair of surface-backed states the
templates are resolved on the base surface (fixed TTE with a re-solved delta or a held
moneyness), the resolved legs plus the shares form one `RiskBook`, both states are evaluated with
`config.kernel` (family by default, cold as oracle) and explained; VaR / ES are nearest-rank loss
quantiles over the ADMITTED scenarios with `tail_mean` (the Greek split of the tail, carry
included). Admission is strict: at least one leg in the totals, every excluded leg's premium known,
and `excluded_premium < max_excluded_premium_frac · |pv_base|` (default 0: any exclusion drops the
scenario); a state with no surface at all refuses a templated scenario; a clock that refuses an
expiry or the step drops only that scenario (checked through `expiry_T`, so an overridden expiry is
admitted). Structural errors (a workspace or frame bound to another book, …) fail the call; zero
admitted scenarios succeed with NaN VaR / ES / `reference_pv`. D21 adds `keep_resolved_legs` /
`resolved_legs` and `summarize_historical_var(frames, confidences)`, the shared quantile half
(windowed evaluation and artifact verification are bit-identical to one call). Scenarios fan out
only when the kernel is single-threaded; one workspace is re-bound per scenario book with
`RiskWorkspace::reserve`. `dispersion_book_var` runs on it over CONSECUTIVE surface-set pairs
(N sets → N − 1 scenarios; BREAKING, re-pinned). Full semantics, failure table and migration:
[`historical-var.md`](historical-var.md).

## Persistence

- **Market states:** `build_state_store_from_spiderrock` writes each bucket's `MarketStateStorage`
  as an ATXVPF `.atxms` file (fingerprinted by the loader conventions version, library version,
  store spec and source files); `StateStore::open_bucket` serves a `MarketState` whose marks, term
  carry (incl. `T_override`) and dividends are spans into the mapping — top-100 bucket ≈ 11 ms vs
  ≈ 1.3 s from Parquet, and a `RiskStream` explain over archived states is bit-equal to
  Parquet-built ones. Keep the archive alive while the state is in use. See
  [`persist.md`](persist.md) (tool `atx-vol-state-store`, verify levels, lifetime rules).
- **Risk runs and surface history:** `RiskRunWriter` records `RiskFrame` / `ExplainResult` /
  `HistoricalVarResult` steps into immutable segments (21 explain columns incl. `pnl_carry`);
  `RiskRunArchive` reopens them zero-copy; `SurfaceTimeline` holds a uid's fitted surfaces across
  snapshots for `project_series`. See [`persist-history.md`](persist-history.md) (tool
  `atx-vol-history-store`).
- The persisted layouts `static_assert` the risk structs they copy (`ExpiryCarry`,
  `FrameCounters`, `ExplainTotals`, `sizeof(ExplainResult)` / `sizeof(RiskFrame)`): appending a
  field to one of them fails the persist build until its layout and fingerprint are re-pinned.

## Benchmark

`atx-vol-risk-bench` ([`risk-bench.md`](risk-bench.md)): 10k options / 100 underliers / 100 share
legs over the SpiderRock 30-minute store, 10:00–15:00. Licensed drop: partition with
`scripts/spiderrock_intraday_to_store.py`, run with `--store <root> --date 2026-10-14`; synthetic:
`--synthetic <dir>`. Flags that matter for this page: `--kernel family|cold|both` (cold comparison
and the family price gate `cold.max_rel_price_family`), `--threads`, `--determinism` (a 1-thread
shadow stream, bit-equal totals), `--needs rho`, `--trapezoid`, `--time`, `--sticky-carry`, the
eight `FamilyKernelPolicy` flags above, and `--dump-legs` / `--dump-steps` (per-leg parity CSV).
Report schema v3. Baselines `bench/baselines/…risk-bench-*.json` are pinned from a quiet-host
`vol-rel-avx2` run. The loader keeps American exercise, Spot framework and standard option roots
only (D16, D17, D20).

## Known limits

- European / forward-framework index options (SPX, NDX, XSP, RUT, OEX, XAU) are excluded by the
  loader; a European (Black-76 forward) route is deferred.
- Vendor parity against SpiderRock on the real book is a diagnostic, not a gate (licensed
  2026-10-14: price sym-rel 0.40 %, 96.5 % inside bid/ask); the residual sits in published-fallback
  long-dated dividend payers.
- The carry axis is the delta proxy (American-put over-capture above); an exact carry revaluation
  for ITM American puts is a follow-up.
- `explain_into` keeps a thread-local grow-only scratch; an explicit workspace parameter is a
  post-sprint API cleanup (D18), as is stating `RiskWorkspace::reserve`'s rebind contract.
- No Python bindings for `api/risk/` or `api/persist/` yet; no sequential-revaluation / Shapley
  attribution.
