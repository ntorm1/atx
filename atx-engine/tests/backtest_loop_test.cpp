// backtest_loop_test.cpp — the P2-7 BacktestLoop driver, green on ScriptedSignalSource.
//
// The end-to-end crank: Phase-1 spine (InMemoryBarFeed -> SimClock -> EventBus)
// -> RollingPanel -> ScriptedSignalSource -> WeightPolicy -> ExecutionSimulator
// -> Portfolio, sampled into a BacktestResult. These tests drive the FULL loop on
// a hand-computable synthetic feed and assert the no-look-ahead (decide-t /
// fill-t+1), cadence-gate, and equity-identity contracts. Costs are zeroed in the
// fixture so positions/fills/equity are hand-verifiable; cost-honesty +
// determinism live in backtest_integration_test.cpp (P2-8).

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/portfolio/portfolio.hpp"

namespace {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::engine::BacktestLoop;
using atx::engine::BacktestResult;
using atx::engine::EventBus;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using Timestamp = atx::core::time::Timestamp;

// The feed/bus type is frozen to EventBus<> (Capacity 1<<16, ConsumerCount 1) —
// an ~8 MB ring that MUST be heap-allocated (a stack instance overflows the 1 MB
// Windows thread stack; the established pattern from the Phase-1 tests).
using Bus = EventBus<>;

// Panel ring capacity for the tests (power of two >= max_lookback=1).
constexpr usize kCap = 8;

constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// Three instruments, sorted ascending == column order [A, B, C].
const Symbol kA{1};
const Symbol kB{2};
const Symbol kC{3};

// A frictionless simulator: every cost coefficient zeroed and a 100%-of-volume
// cap, so a fill lands exactly at the bar close with no fee/slippage/impact —
// the only way the end-to-end positions/equity are hand-computable.
[[nodiscard]] ExecutionSimulator make_frictionless_sim() {
  return ExecutionSimulator{
      FillCfg{},
      SlippageCfg{SlippageMode::VolumeShare, /*k=*/0.0, /*bps=*/0.0, /*cap_volshare=*/0.0,
                  /*cap_bps=*/0.0},
      ImpactCfg{/*Y=*/0.0, /*delta=*/0.5, /*gamma=*/0.0},
      CommissionCfg{CommissionMode::PerShare, /*per_share=*/0.0, /*min_fee=*/0.0, /*max_pct=*/1.0,
                    /*per_dollar_bps=*/0.0},
      LatencyCfg{/*latency_nanos=*/0},
      VolumeCapCfg{/*volume_limit=*/1.0}};
}

// One constant-OHLCV bar for `symbol` at slice `k` (ts == knowledge_ts == k).
[[nodiscard]] BarRow bar_row(const Symbol &symbol, i64 k, i64 price) {
  Bar bar{};
  bar.ts = ts(k);
  bar.open = Price::from_int(price);
  bar.high = Price::from_int(price);
  bar.low = Price::from_int(price);
  bar.close = Price::from_int(price);
  bar.volume = Quantity::from_int(1'000'000); // ample vs. the test order sizes
  return BarRow{symbol, bar, ts(k), /*delisted_final=*/false};
}

// What each backtest run exposes for assertions (the portfolio is local to the
// run harness, so its observable state is snapshotted out before it is destroyed).
struct Outcome {
  BacktestResult result;
  std::array<i64, 3> qty{}; // final qty for A, B, C
  f64 equity{0.0};
  f64 net{0.0};
  f64 gross{0.0};
  f64 cash{0.0};
};

// Drive a full backtest over `n_bars` constant-price bars per instrument, a baked
// `signal_schedule` (one vector per rebalance fire), and a `every`-slice cadence.
[[nodiscard]] Outcome run_backtest(int n_bars, const std::vector<std::vector<f64>> &signal_schedule,
                                   usize every, std::array<i64, 3> prices = {100, 50, 200}) {
  std::vector<InstrumentId> universe{kA, kB, kC};

  // Three knowledge_ts-sorted sources, one per instrument, constant price.
  std::vector<BarRow> a;
  std::vector<BarRow> b;
  std::vector<BarRow> c;
  for (int k = 1; k <= n_bars; ++k) {
    a.push_back(bar_row(kA, k, prices[0]));
    b.push_back(bar_row(kB, k, prices[1]));
    c.push_back(bar_row(kC, k, prices[2]));
  }
  std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a}, std::span<const BarRow>{b},
                                             std::span<const BarRow>{c}};
  const std::span<const std::span<const BarRow>> sources{spans};

  auto bus = std::make_unique<Bus>();
  SimClock clock;
  InMemoryBarFeed feed{sources, clock, *bus};

  RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, /*max_lookback=*/1};
  ScriptedSignalSource src{signal_schedule, /*universe_size=*/3, /*max_lookback=*/1};
  const WeightPolicy policy{};
  ExecutionSimulator sim = make_frictionless_sim();
  Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{}};
  const Schedule schedule{every};

  BacktestLoop<kCap> loop{
      feed, clock, *bus, panel, src, policy, sim, portfolio, market, Universe{universe}, schedule};

  Outcome out;
  out.result = loop.run();
  out.qty = {portfolio.holding(kA).qty, portfolio.holding(kB).qty, portfolio.holding(kC).qty};
  out.equity = portfolio.equity();
  out.net = portfolio.net();
  out.gross = portfolio.gross();
  out.cash = portfolio.cash().to_double();
  return out;
}

// A schedule of `count` identical [3, 2, 1] signal vectors (A highest, C lowest).
[[nodiscard]] std::vector<std::vector<f64>> ramp_schedule(int count) {
  return std::vector<std::vector<f64>>(static_cast<usize>(count), std::vector<f64>{3.0, 2.0, 1.0});
}

// ============================================================================
//  End-to-end hand-computed run. Signal [3,2,1] -> rank -> dollar-neutral ->
//  gross=1: weights A=+0.5, B=0, C=-0.5. With equity 100k, prices 100/50/200:
//  target A=+500, C=-250 (B flat). Decided at slice 0, filled at slice 1; held
//  to the end (subsequent rebalances are on-target -> no new trades). Frictionless
//  -> equity stays exactly 100,000 the whole run (dollar-neutral, Σ MV == 0).
// ============================================================================
TEST(BacktestLoop, EndToEnd_ScriptedRamp_PositionsFillsEquityMatch) {
  const Outcome o = run_backtest(/*n_bars=*/10, ramp_schedule(10), /*every=*/1);

  EXPECT_EQ(o.qty[0], 500) << "A target = trunc(0.5*100000/100)";
  EXPECT_EQ(o.qty[1], 0) << "B has zero weight";
  EXPECT_EQ(o.qty[2], -250) << "C target = trunc(-0.5*100000/200)";

  ASSERT_EQ(o.result.fills.size(), 2U) << "exactly the two opening fills (A, C)";
  for (const auto &f : o.result.fills) {
    EXPECT_EQ(f.t.unix_nanos(), 2) << "decided at slice 0 (t=1), filled at slice 1 (t=2)";
  }

  EXPECT_NEAR(o.equity, 100'000.0, 1e-6) << "frictionless dollar-neutral -> equity unchanged";
  EXPECT_NEAR(o.net, 0.0, 1e-6) << "dollar-neutral book -> net ~ 0";
  EXPECT_NEAR(o.gross, 100'000.0, 1e-6) << "gross = |500*100| + |250*200|";
  EXPECT_NEAR(o.result.turnover, 100'000.0, 1e-6) << "traded notional 500*100 + 250*200";

  EXPECT_EQ(o.result.slices, 10U);
  EXPECT_EQ(o.result.rebalances, 10U) << "every=1 -> a rebalance every slice";
  ASSERT_EQ(o.result.equity_curve.size(), 10U);
  for (const auto &s : o.result.equity_curve) {
    EXPECT_NEAR(s.equity, 100'000.0, 1e-6) << "equity constant across the run";
  }
}

// ============================================================================
//  No-look-ahead at the loop level: an order decided on slice 0 (t=1) fills on
//  slice 1 (t=2), never on slice 0. A two-bar feed makes the t+1 fill explicit.
// ============================================================================
TEST(BacktestLoop, OrderDecidedAtT_FillsAtTPlus1) {
  const Outcome o = run_backtest(/*n_bars=*/2, ramp_schedule(2), /*every=*/1);

  ASSERT_EQ(o.result.fills.size(), 2U);
  for (const auto &f : o.result.fills) {
    EXPECT_EQ(f.t.unix_nanos(), 2) << "fill lands on the NEXT slice, not the decision slice";
  }
  EXPECT_EQ(o.qty[0], 500);
  EXPECT_EQ(o.qty[2], -250);
}

// ============================================================================
//  Boundary: a single-bar feed. The order is decided on slice 0 but there is no
//  slice 1 to settle it -> ZERO fills (the firewall has no future bar to fill on).
// ============================================================================
TEST(BacktestLoop, SingleBarFeed_NoFillPossible) {
  const Outcome o = run_backtest(/*n_bars=*/1, ramp_schedule(1), /*every=*/1);

  EXPECT_TRUE(o.result.fills.empty()) << "no t+1 slice -> the queued order never fills";
  EXPECT_EQ(o.qty[0], 0);
  EXPECT_EQ(o.qty[1], 0);
  EXPECT_EQ(o.qty[2], 0);
  EXPECT_EQ(o.result.slices, 1U);
  EXPECT_EQ(o.result.rebalances, 1U) << "the schedule still fires on the one slice";
  EXPECT_NEAR(o.equity, 100'000.0, 1e-6);
}

// ============================================================================
//  Cadence gate: with every=3 over 10 slices the schedule fires on slices
//  0,3,6,9 -> exactly 4 rebalances (not 10).
// ============================================================================
TEST(BacktestLoop, CadenceGate_FiresOnlyOnSchedule) {
  const Outcome o = run_backtest(/*n_bars=*/10, ramp_schedule(4), /*every=*/3);
  EXPECT_EQ(o.result.rebalances, 4U) << "fires on slices 0, 3, 6, 9";
}

// ============================================================================
//  A flat (all-equal) signal demeans to all-zero weights -> no targets, no
//  trades. Positions stay flat and equity is untouched.
// ============================================================================
TEST(BacktestLoop, FlatSignal_NoTrades) {
  const std::vector<std::vector<f64>> flat(3, std::vector<f64>{5.0, 5.0, 5.0});
  const Outcome o = run_backtest(/*n_bars=*/3, flat, /*every=*/1);

  EXPECT_TRUE(o.result.fills.empty());
  EXPECT_EQ(o.qty[0], 0);
  EXPECT_EQ(o.qty[1], 0);
  EXPECT_EQ(o.qty[2], 0);
  EXPECT_NEAR(o.result.turnover, 0.0, 1e-12);
  EXPECT_NEAR(o.equity, 100'000.0, 1e-6);
}

// ============================================================================
//  An all-NaN signal ("no opinion" everywhere) yields zero weights -> no orders.
// ============================================================================
TEST(BacktestLoop, NaNSignal_NoOrders) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<std::vector<f64>> all_nan(3, std::vector<f64>{nan, nan, nan});
  const Outcome o = run_backtest(/*n_bars=*/3, all_nan, /*every=*/1);

  EXPECT_TRUE(o.result.fills.empty());
  EXPECT_EQ(o.qty[0], 0);
  EXPECT_EQ(o.qty[1], 0);
  EXPECT_EQ(o.qty[2], 0);
}

// ============================================================================
//  Equity identity: equity == cash + Σ market_value (net), every slice. Pinned
//  on the end-to-end run where the book carries a non-trivial long/short position.
// ============================================================================
TEST(BacktestLoop, EquityEqualsCashPlusNet) {
  const Outcome o = run_backtest(/*n_bars=*/10, ramp_schedule(10), /*every=*/1);
  EXPECT_NEAR(o.equity, o.cash + o.net, 1e-6) << "equity = cash + Σ market_value";
}

// ============================================================================
//  Boundary: an empty feed (no bars) runs zero slices and leaves the book
//  untouched — no crash, no phantom samples.
// ============================================================================
TEST(BacktestLoop, EmptyFeed_NoSlices) {
  const Outcome o = run_backtest(/*n_bars=*/0, {}, /*every=*/1);

  EXPECT_EQ(o.result.slices, 0U);
  EXPECT_EQ(o.result.rebalances, 0U);
  EXPECT_TRUE(o.result.equity_curve.empty());
  EXPECT_TRUE(o.result.fills.empty());
  EXPECT_NEAR(o.result.final_equity, 100'000.0, 1e-6);
  EXPECT_NEAR(o.cash, 100'000.0, 1e-6);
}

} // namespace
