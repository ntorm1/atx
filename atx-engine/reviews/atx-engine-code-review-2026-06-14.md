# ATX Engine Multi-Agent Code Review

Date: 2026-06-14  
Scope: `C:\Users\natha\OneDrive\Desktop\atx\atx-engine`  
Subject: C++ backtest trading engine correctness, wiring, performance, and roadmap gaps.

## Executive Summary

The engine has a broad and well-tested surface, but the review found several high-priority issues that can make backtest results materially wrong:

- Limit orders are checked against the raw reference mark, then spread/slippage/impact can push the actual fill through the limit.
- Capacity impact mixes share quantity with dollar ADV, understating impact by roughly the instrument price.
- Dollar-neutral portfolios can become non-neutral after name-cap/truncation projection.
- Market volume for absent instruments can remain stale, creating phantom liquidity.
- Permanent impact shifts marks inside the execution simulator, but the main loop does not reliably mark the portfolio after that shift, and the next market update overwrites the shifted mark.

There are also product-wiring gaps: the public engine API is still a placeholder, there is no CLI/config runner, industry-neutral `WeightPolicy` cannot be used through `BacktestLoop`, multiple public worker/config knobs are accepted but unused, and TSDB/shared-memory feed coverage does not yet exercise the full backtest loop.

## Methodology And Verification

Four sub-agents reviewed independent slices:

- Correctness: execution semantics, accounting, lookahead, risk math.
- Wiring: implemented modules/config knobs not reachable from production paths.
- Performance: hot-path algorithmic and allocation issues.
- Roadmap: feature gaps and next-step priorities.

The repo instruction says to prefer MCP graph tools. We attempted MCP first (`get_mcp_health`, `get_architecture`, graph/doc searches), but the active index pointed at `C:/Users/natha/OneDrive/Desktop/C/ats`, not this `atx` worktree. The review therefore fell back to targeted `rg` and direct file reads.

Verification notes:

- Running `cmake --build build --target atx-engine-tests` from the monorepo root timed out after 304 seconds in my integration pass.
- The correctness sub-agent ran targeted ctest suites from `C:\Users\natha\OneDrive\Desktop\atx\build`:
  - `ctest --output-on-failure -R "^(ExecSim|BacktestLoop|Portfolio|WeightPolicy|RollingPanel|DataHandler|MarketData)\."` -> 110/110 passed.
  - `ctest --output-on-failure -R "^(BacktestIntegration|Phase3cIntegration_BridgeE2E|VmSignalSource|CostIntegration|RiskOptimizer|MultiPeriodOptimizer)\."` -> 48/48 passed.
- Running CMake from `atx-engine/build` alone is not valid for this module: `atx-engine/CMakeLists.txt` is a subdirectory file and expects parent targets such as `atx::core` and `atx::tsdb`.

## Priority Findings

| Priority | Category | Finding |
| --- | --- | --- |
| P0 | Correctness | Limit orders can fill through their limit after costs. |
| P0 | Correctness | Capacity impact uses shares divided by dollar ADV. |
| P0 | Correctness | Name-cap/truncation projection can break dollar neutrality. |
| P0 | Correctness | Absent instruments retain stale executable volume. |
| P1 | Correctness | Permanent impact is not reflected consistently in loop equity or future marks. |
| P1 | Wiring | `BacktestLoop` cannot safely use `WeightPolicy::industry_neutral`. |
| P1 | Wiring | Public engine facade/runner is still a placeholder. |
| P1 | Performance | Per-bar portfolio/market sampling does repeated full-universe scans and lookups. |
| P1 | Performance | Execution volume accounting can degrade toward O(fills^2). |

## Correctness Bugs

### P0: Limit Orders Can Fill Through Their Limit

Evidence:

- `include/atx/engine/exec/execution_sim.hpp:327-334` checks limit marketability against raw `ref`.
- `include/atx/engine/exec/execution_sim.hpp:369-385` then applies spread/slippage/temporary impact and emits that post-cost fill price.

Why it is wrong:

A buy limit at 100 can pass because `ref <= 100`, then fill above 100 after spread/slippage/impact. A sell limit can similarly receive less than its limit. That violates the normal limit-order contract and can overstate fills for strategies that depend on price constraints.

Expected behavior:

For a buy limit, final execution price should be `<= limit`. For a sell limit, final execution price should be `>= limit`.

Suggested fix:

Compute the candidate post-cost fill price before consuming bar volume, then reject the fill or cap the fill price according to the chosen limit-order semantics. Because `volume_capped_qty()` currently increments the per-bar volume accumulator, the implementation should separate "peek fillable" from "commit filled volume" or move accumulator mutation after price acceptance.

Add regression tests:

- Buy limit where `ref == limit` and fixed bps/slippage makes `fill_px > limit`.
- Sell limit where `ref == limit` and slippage makes `fill_px < limit`.
- Same cases with temporary impact enabled.

### P0: Capacity Impact Divides Shares By Dollar ADV

Evidence:

- `include/atx/engine/risk/capacity.hpp:162-177` defines `dollar_adv()` as mean `close * volume`.
- `include/atx/engine/risk/capacity.hpp:238-240` computes `shares = notional / price`, then `part = shares / adv`.

Why it is wrong:

The numerator is shares, but the denominator is dollars. For a $50 stock, this understates participation by roughly 50x. Capacity curves will look too optimistic and may admit capacity that should fail.

Expected behavior:

Participation should be either:

- `notional / dollar_adv`, or
- `shares / share_adv`.

Suggested fix:

Pick one unit convention and name it explicitly. Since the helper already computes dollar ADV, the minimal fix is:

```cpp
const atx::f64 part = notional / adv;
```

Add a hand-computed capacity test with nonzero price, volume, AUM, and impact coefficients so the expected bps cost catches the unit error.

### P0: Cap Projection Can Break Dollar Neutrality

Evidence:

- `include/atx/engine/risk/optimizer.hpp:291-299` demeans and gross-normalizes, then calls `cap_clip_renorm`.
- `include/atx/engine/risk/optimizer.hpp:324-359` clips/rescales absolute weights without re-enforcing `sum(w) == 0`.
- `include/atx/engine/loop/weight_policy.hpp:246-259` follows the same gross-normalize then truncate-renorm pattern.

Why it is wrong:

When dollar-neutral mode is requested, the final portfolio should remain neutral. Clipping and redistributing by absolute mass can skew the positive and negative sides differently. The correctness reviewer found a feasible four-name capped neutral case that can return gross 1.0 with net exposure around 0.234.

Expected behavior:

If dollar neutrality is enabled, final weights should satisfy net exposure near zero while respecting gross and per-name caps.

Suggested fix:

Project positive and negative sides separately onto capped simplexes of `gross / 2`, or implement a constrained projection that enforces net, gross, and cap together. Add tests for feasible and infeasible cap cases in both `WeightPolicy` and `PortfolioOptimizer`.

### P0: Stale Bar Volume Creates Phantom Liquidity

Evidence:

- `include/atx/engine/loop/market.hpp:109-122` updates only instruments present in the current slice.
- `include/atx/engine/loop/market.hpp:129-132` returns the stored `volumes_` value.
- `include/atx/engine/exec/execution_sim.hpp:342-357` uses `Market::bar_volume()` for the per-bar volume cap.

Why it is wrong:

Absent instruments keep prior volume. An order in a later slice can fill against old volume even when the instrument has no current bar or has delisted. This is especially dangerous because preserving the last mark for valuation may be reasonable, but preserving executable volume is not.

Expected behavior:

An absent instrument should have zero current executable volume unless the current slice includes a valid trading bar or a separately modeled liquidation/auction event.

Suggested fix:

Track mark and executable volume separately. Keep last mark if needed for valuation, but reset `volumes_` to zero at the start of `Market::update_prices()` or track a current-slice epoch so `bar_volume()` returns zero for non-present instruments.

Add regression tests:

- Bar at t1 has volume, t2 omits the instrument, an open order at t2 should not fill.
- A delisted final bar can fill on the valid final bar but not on later absent slices.

### P1: Spread Semantics Are Inconsistent

Evidence:

- `include/atx/engine/loop/market.hpp:73-77` documents `InstrumentStats::spread` as half-spread.
- `include/atx/engine/exec/execution_sim.hpp:391-410` computes a spread floor as `st.spread * 0.5 / ref`.

Why it is wrong:

If `spread` is already a half-spread, multiplying by `0.5` charges a quarter of the full spread. The sub-agent found tests that appear to treat `spread` as full spread. This ambiguity will produce different transaction costs depending on caller interpretation.

Expected behavior:

One contract only:

- `spread` is full bid/ask spread and execution uses half of it, or
- `spread` is half-spread and execution uses it directly.

Suggested fix:

Rename/document the field as `full_spread` or remove the extra `* 0.5`. Update tests and calibration code to pin the chosen convention.

### P1: Permanent Impact Does Not Persist Or Mark Equity Reliably

Evidence:

- `include/atx/engine/loop/backtest_loop.hpp:251-259` marks the portfolio before settlement.
- `include/atx/engine/exec/execution_sim.hpp:377` applies permanent impact by shifting the market mark during fill emission.
- `include/atx/engine/loop/market.hpp:119-121` overwrites marks from each new slice.
- `include/atx/engine/loop/backtest_loop.hpp:318-322` samples equity/gross/net without a second post-fill mark-to-market.

Why it is wrong:

The execution simulator shifts the market mark after `Portfolio::mark_to_market()` has already run. The equity sample for that slice can miss the shifted mark. On the next bar, `Market::update_prices()` overwrites the shifted mark, so the "permanent" impact does not reliably persist into future valuation or later fills.

Expected behavior:

Either:

- permanent impact is truly persistent and participates in current/future marks, or
- it is not persistent and should be modeled as explicit fill cost rather than a market mark shift.

Suggested fix:

Decide the model. If permanent impact persists, carry per-instrument impact offsets across market updates and re-mark the portfolio after settlement before sampling. If it does not persist, remove/rename the mark shift and make the cost path explicit.

## Modules Not Fully Wired In

### P1: Public Engine Facade Is Still A Placeholder

Evidence:

- `include/atx/engine/engine.hpp:5-6` exposes only `int step(int value)`.
- `src/engine.cpp:7-10` delegates to `atx::core::checked_add`.
- `atx-engine/CMakeLists.txt` builds many engine modules but no production executable.

Impact:

The engine exists as a static library plus tests/benchmarks, but consumers do not have a real public runner that wires data, signal, execution, risk, portfolio, reporting, and validation together.

Suggested next action:

Add either:

- a production facade such as `run_backtest(const BacktestConfig&)`, or
- an `atx-engine-run` CLI with config validation, deterministic manifest output, feed selection, universe, schedule, execution/risk/cost knobs, and report paths.

### P1: `industry_neutral` Cannot Safely Run Through `BacktestLoop`

Evidence:

- `include/atx/engine/loop/weight_policy.hpp:191-206` says `group_map` is required when `industry_neutral` is true.
- `include/atx/engine/loop/backtest_loop.hpp:283` calls `policy_->to_target_weights(*signal, universe_)` without a group map.

Impact:

The group-neutralization module is implemented and unit-tested, but the production loop cannot supply its required input. Debug builds assert; release builds risk out-of-bounds access because `ATX_ASSERT` may compile out.

Suggested next action:

Thread a universe-aligned group map into `BacktestLoop`, or reject `industry_neutral` in the loop constructor when no group map is supplied.

### P2: Factory Search Worker Count Is Configured But Discarded

Evidence:

- `src/factory/search_driver.cpp:41` constructs `parallel::DetPool det_pool{cfg.n_workers}`.
- `src/factory/search_driver.cpp:133-150` documents the single-thread fallback and then `static_cast<void>(det_pool)`.
- `src/factory/search_driver.cpp:151-157` builds a fresh `alpha::Engine` per candidate.

Impact:

Large formulaic alpha searches do not scale with `SearchConfig::n_workers`, despite exposing the knob and having parallel evaluation support tested elsewhere.

Suggested next action:

Re-enable `parallel_evaluate` after verifying the per-worker engine warm-up/reuse contract across evolved stateful programs, or remove/rename `n_workers` from this path until it is real.

### P2: Learned Pipeline `workers` Is An Explicit No-Op

Evidence:

- `include/atx/engine/learn/pipeline.hpp:27-35` documents that `workers` is accepted but reaches no fit.
- `include/atx/engine/learn/pipeline.hpp:72-75` exposes `PipelineCfg::workers`.
- `src/learn/pipeline.cpp:55-71` threads seed/config into fits but not worker count.

Impact:

Callers can configure parallelism that has no effect. This is honestly documented, so it is not a hidden bug, but it is still a product/API gap.

Suggested next action:

Either wire `workers` into CPCV/model fitting or move it behind a future/experimental config.

### P2: Public Risk Knobs Return `NotImplemented`

Evidence:

- `include/atx/engine/risk/multi_horizon.hpp:118` exposes `stacked_mpc`.
- `include/atx/engine/risk/multi_horizon.hpp:154-157` returns `ErrorCode::NotImplemented` when it is true.
- `include/atx/engine/risk/factor_model.hpp:487-490` returns `NotImplemented` when `n_dead_factors > 0`.

Impact:

These are honest residuals, not silent skips. The risk is that production config can request modes that appear supported until runtime.

Suggested next action:

Make these explicitly experimental/internal, or implement the stacked MPC QP and dead-factor augmentation through the existing lower-level pieces.

### P2: TSDB And Shared-Memory Feeds Are Not Exercised Through `BacktestLoop`

Evidence:

- `include/atx/engine/data/shm_bar_feed.hpp:49-60` implements `IDataHandler::step()`.
- `include/atx/engine/data/shm_bar_feed.hpp:81-99` publishes rows to the bus.
- The wiring review found existing e2e coverage drains these feeds manually rather than running them through `BacktestLoop`.

Impact:

Segment/day-boundary/symbol behavior is tested as event publication, but not in the actual loop path that updates market, panel, execution, portfolio, and result sampling.

Suggested next action:

Add a `MultiSegmentBarFeed/ShmBarFeed -> BacktestLoop` integration test compared against an equivalent `InMemoryBarFeed` run.

### P2: Borrow And Financing Costs Are Not In The Main Loop

Evidence:

- `include/atx/engine/cost/borrow.hpp` defines `daily_borrow()` and `accrue_borrow()`.
- `include/atx/engine/portfolio/portfolio.hpp:236-237` exposes a financing debit path.
- Search found borrow usage in tests/bench, but not in `BacktestLoop`.

Impact:

Short books can be evaluated without daily borrow/financing drag in the main backtest result unless callers wire a separate path.

Suggested next action:

Add an optional financing/borrow accrual step to `BacktestLoop`, with a schedule/day-count convention and report attribution.

## Performance Bugs And Hotspots

### P1: Per-Bar Portfolio Sampling Repeats Full-Universe Work

Evidence:

- `include/atx/engine/loop/backtest_loop.hpp:251-253` updates market and marks portfolio every slice.
- `include/atx/engine/portfolio/portfolio.hpp:184-188` scans all holdings for mark-to-market.
- `include/atx/engine/portfolio/portfolio.hpp:199-224` scans holdings separately for equity, gross, and net.
- `include/atx/engine/loop/market.hpp:127-138` resolves marks/stats through id lookups.

Impact:

The loop does O(bars * universe * log universe) work for market/portfolio refresh plus multiple O(universe) scans per sample.

Suggested optimization:

Compute equity/gross/net during one mark-to-market pass and cache them. Add dense index-based market accessors (`mark_at`, `volume_at`, `stats_at`) or feed dense indices through `SliceRow` to avoid repeated binary searches.

### P1: Execution Volume Accumulator Can Become Quadratic

Evidence:

- `include/atx/engine/exec/execution_sim.hpp:342-357` calls `vol_filled_for()` and `add_vol_filled()` per order.
- The reviewer found both helpers are linear scans over the per-bar accumulator.
- Each settled order also repeats market lookups for mark, volume, and stats.

Impact:

Many orders in one bar, especially many distinct instruments, degrade toward O(fills^2) plus repeated id lookup cost.

Suggested optimization:

Resolve dense instrument index once and store filled volume in a vector indexed by instrument. Reset with a touched-index list or epoch counter so per-bar reset remains O(touched), not O(universe).

### P1: VM Signal Source Rebuilds And Copies A Full Panel Per Rebalance

Evidence:

- `include/atx/engine/loop/signal_source.hpp:262-269` builds an `alpha::Panel`, constructs an `alpha::Engine`, and materializes a `SignalSet`.
- `include/atx/engine/loop/signal_source.hpp:323-348` transposes `dates * instruments * 5` fields, then `Panel::create()` copies into an owned panel.

Impact:

With `schedule.every == 1`, each bar can allocate/copy the full trailing OHLCV panel before doing any alpha work.

Suggested optimization:

Add a borrowed `alpha::Panel` path from the reused scratch, or add an `Engine::evaluate_into_current_cross_section` API over `PanelView` so only the root/current date is materialized.

### P2: Weight Policy Allocates And Has O(groups * live) Neutralization

Evidence:

- `include/atx/engine/loop/backtest_loop.hpp:283-286` allocates weights and orders per rebalance.
- `include/atx/engine/loop/weight_policy.hpp:207-258` allocates weights, live indices, dense scratch, transform output, and group vectors.
- `include/atx/engine/loop/weight_policy.hpp:347-375` scans all live names twice for every distinct group.

Impact:

Granular group maps can become O(N^2) at rebalance cadence.

Suggested optimization:

Keep scratch buffers across calls. For group neutralization, sort `(group, dense_index)` once and reduce contiguous ranges, or use bounded group-id accumulators with an epoch reset.

### P2: Market Data Is Decoded Twice Per Slice Row

Evidence:

- `include/atx/engine/loop/backtest_loop.hpp:246` copies `MarketPayload` into `SliceRow`.
- `include/atx/engine/loop/market.hpp:115-121` looks up id and converts close/volume.
- `RollingPanel::append_sealed_row()` also performs symbol lookup and OHLCV conversion.

Impact:

Every slice row pays duplicate payload copy, duplicate id lookup, and duplicate Decimal-to-double conversion before both market and panel writes.

Suggested optimization:

Decode once into a dense slice row containing instrument index and f64 OHLCV values, then feed both `Market` and `RollingPanel` from that representation.

### P3: Report TSV Generation Allocates Heavily

Evidence:

- The performance reviewer found `include/atx/engine/book/report.hpp:190+` builds TSV bodies through per-cell strings and no apparent size reserve.

Impact:

Cold path, but large factor/exposure reports can churn small allocations and build entire files in memory.

Suggested optimization:

Use `std::to_chars` into the destination buffer, reserve an estimated output size, or stream buffered rows.

## Feature Gaps And Suggested Roadmap

### P0: Add A Real CLI And Config Runner

Current state:

The package builds a static library and tests/benchmarks, while `engine.hpp` is a placeholder.

Next steps:

- Add `atx-engine-run validate-config`.
- Add `atx-engine-run run-backtest`.
- Emit a deterministic run manifest: git SHA, config hash, data manifest, seed, clock/session calendar, build flags, and report paths.
- Support feed path, universe, schedule, strategy, execution, risk, borrow, validation, and report config.

### P0: Improve Broker And Exchange Semantics

Current state:

The simulator has market/limit orders, FIFO open-order processing, volume cap, slippage, impact, commission, and latency. It does not model a real order lifecycle or exchange microstructure.

Next steps:

- Order IDs, status lifecycle, cancel/replace.
- Stop, stop-limit, IOC, FOK, MOC/LOC, auction/close semantics.
- Bid/ask or L2 queue model.
- Halts, auction bars, partial session calendars.
- Maker/taker fees, rebates, locate failures.
- Optional seeded stochastic fills.

### P0: Add Production Data Validation And PIT Metadata

Current state:

The in-memory feed is careful about chronological replay, but shared-memory segments assume fixed OHLCV fields and publish `delisted_final=false`.

Next steps:

- Validate segment schema with recoverable errors.
- Add point-in-time symbol maps and corporate-action metadata.
- Carry delisting metadata in sealed segments.
- Validate calendar/session alignment.
- Detect stale, missing, duplicate, and outlier bars.
- Produce feed manifests and data-quality reports.

### P1: Make Backtest Results Evaluation-Grade

Current state:

`BacktestResult` records equity curve, fills, cash/equity, turnover, slices, and rebalance count. Rich metrics exist separately under `eval`.

Next steps:

- Add an equity-to-returns adapter.
- Report Sharpe, Sortino, IR, drawdown depth/duration, hit rate, turnover ratios, exposure stats, and cost attribution.
- Wire metrics into report output and CLI artifacts.

### P1: Promote Validation From Tests To Run Gates

Current state:

Bias-audit utilities and PBO/DSR tests exist, but run-level validation artifacts are not wired into the production path.

Next steps:

- No-lookahead truncation runs.
- Survivorship and delisting checks.
- PBO/DSR thresholds.
- Benchmark alignment checks.
- Fail/waive semantics in the run manifest.

### P1: Integrate Portfolio, Borrow, And Risk Into The Main Loop

Current state:

Portfolio accounting, borrow, cost/capacity, and risk optimizers exist, but the main `BacktestLoop` is still primarily signal -> weights -> orders -> execution -> portfolio.

Next steps:

- Accrue borrow/financing on a configurable schedule.
- Add margin/leverage/locate/dividend support.
- Add a risk-aware target path to the loop, not only separate book-pipeline drivers.
- Support group maps and factor models as first-class loop inputs.

### P2: Decide On True Multi-Period Optimization

Current state:

The shipped multi-horizon path is a deterministic driver/aim-collapse path; true stacked MPC returns `NotImplemented`.

Next steps:

- Decide whether production accepts the deterministic driver.
- If not, implement the stacked QP/MPC path and add convergence diagnostics plus scenario stress tests.

### P2: Add Production-Scale Benchmarks

Current state:

Bench coverage exists, but important benchmarks use synthetic shapes such as 50 symbols/252 bars or 256 orders.

Next steps:

- Release-mode baseline suite with large universe and multi-year fixtures.
- Stress partial fills, high order counts, sparse panels, delisting, and multi-segment real data.
- Track regressions for loop bars/sec, fills/sec, strategy eval/sec, optimizer latency, and report generation.

## Recommended Fix Order

1. Add failing regression tests for the P0 correctness issues before changing behavior.
2. Fix limit-order post-cost gating and stale executable volume first. These directly affect fills.
3. Fix capacity unit math and add hand-computed capacity tests.
4. Replace cap/truncation projection with a neutrality-preserving projection.
5. Decide and implement permanent-impact semantics.
6. Wire `industry_neutral` safely through `BacktestLoop` or reject it at construction.
7. Add a minimal production runner/facade and a `Shm/MultiSegment feed -> BacktestLoop` e2e test.
8. Benchmark before optimizing the major hot paths, then tackle dense-index market access and VM panel copying.

## Sub-Agent Coverage

- Correctness reviewer: found six correctness issues and ran targeted ctest suites. Existing targeted tests passed, so these need new regressions.
- Wiring reviewer: found placeholder facade, unsafe industry-neutral loop wiring, unused workers, public `NotImplemented` knobs, book/TSDB integration gaps.
- Performance reviewer: found repeated universe scans, O(fills^2) execution accounting, VM panel rebuild/copy, weight-policy allocation/group complexity, duplicate market decoding, report allocation churn.
- Roadmap reviewer: prioritized CLI/config, broker realism, data validation/PIT metadata, evaluation-grade metrics, validation gates, risk/borrow integration, true MPC, and production-scale benchmarks.
