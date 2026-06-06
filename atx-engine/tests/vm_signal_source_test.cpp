// vm_signal_source_test.cpp — VmSignalSource green-gate + delay knob (P3c-3).
//
// The bridge that finally lets the Phase-3 alpha VM drive the real Phase-2
// BacktestLoop. This suite proves four things, each a TEST not a hope:
//
//   * ADAPTER PASS-THROUGH — a VmSignalSource over a compiled 1-alpha program +
//     a panel produces the SAME current-date cross-section as calling
//     alpha::Engine::evaluate directly on the equivalent chronological Panel and
//     reading the last date's row. The adapter is a thin transpose + extract.
//   * TRANSPOSE CORRECTNESS / NO-LOOK-AHEAD — the alpha::Panel built from the
//     newest-first PanelView is CHRONOLOGICAL: a time-series alpha (delta(close,1))
//     gives the right SIGN, and the signal at the rebalance reads only sealed
//     (<= current) rows.
//   * DRIVES THE REAL BacktestLoop — a VmSignalSource over a simple compiled
//     alpha on a synthetic feed runs the assembled loop to EOF producing
//     deterministic, byte-identical fills/equity (the cross-phase integration
//     BLOCKED since Phase 2, now resolved).
//   * DELAY-0 vs DELAY-1 — the SIGNALS are identical (same program, same panel)
//     but the FILL bars differ by one; the default is Next (delay-1) and the
//     firewall stays intact; delay-0 is opt-in.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/vm.hpp"

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
using atx::engine::Delay;
using atx::engine::EventBus;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::VmSignalSource;
using atx::engine::WeightPolicy;
using atx::engine::alpha::compile_batch;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::Program;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillPayload;
using Bus = EventBus<>;
using Timestamp = atx::core::time::Timestamp;

constexpr usize kCap = 8;
const Symbol kA{1};
const Symbol kB{2};
const Symbol kC{3};

[[nodiscard]] constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// Compile ONE alpha source string into a Program (root 0 == the strategy).
[[nodiscard]] Program compile_one(std::string_view src) {
  const Library lib;
  std::vector<std::string_view> srcs{src};
  auto prog = compile_batch(std::span<const std::string_view>{srcs}, lib);
  EXPECT_TRUE(prog) << "compile_batch must succeed for: " << src;
  return std::move(prog).value();
}

// One constant-OHLCV bar.
[[nodiscard]] BarRow bar_row(const Symbol &symbol, i64 k, i64 price, i64 vol = 1'000'000,
                             bool delisted_final = false) {
  Bar bar{};
  bar.ts = ts(k);
  bar.open = Price::from_int(price);
  bar.high = Price::from_int(price);
  bar.low = Price::from_int(price);
  bar.close = Price::from_int(price);
  bar.volume = Quantity::from_int(vol);
  return BarRow{symbol, bar, ts(k), delisted_final};
}

// ============================================================================
//  Direct-Panel oracle: build the chronological alpha::Panel the adapter SHOULD
//  produce and evaluate the same program over it. The adapter's output must
//  match this exactly (it is a thin transpose + last-row extract).
// ============================================================================

// closes[d][j] is instrument j's close on chronological date d (date 0 earliest).
[[nodiscard]] Panel chronological_close_panel(const std::vector<std::vector<f64>> &closes,
                                              usize inst) {
  const usize dates = closes.size();
  const usize cells = dates * inst;
  // Provide all five OHLCV fields (constant-OHLCV => open==high==low==close).
  std::vector<std::vector<f64>> field_data(5, std::vector<f64>(cells, 0.0));
  for (usize d = 0; d < dates; ++d) {
    for (usize j = 0; j < inst; ++j) {
      const f64 px = closes[d][j];
      for (usize f = 0; f < 5; ++f) {
        field_data[f][d * inst + j] = px;
      }
    }
  }
  std::vector<std::string> names{"open", "high", "low", "close", "volume"};
  auto p = Panel::create(dates, inst, names, field_data, /*universe=*/{});
  EXPECT_TRUE(p);
  return std::move(p).value();
}

// Feed the same chronological closes into a RollingPanel (oldest->newest, the
// loop's natural append order) and return its newest-first PanelView via a held
// panel (the caller keeps the panel alive while reading the view).
struct PanelHolder {
  std::vector<InstrumentId> universe;
  std::unique_ptr<RollingPanel<kCap>> panel;
};

[[nodiscard]] PanelHolder filled_rolling_panel(const std::vector<std::vector<f64>> &closes,
                                               usize lookback) {
  PanelHolder h;
  h.universe = {kA, kB, kC};
  h.panel =
      std::make_unique<RollingPanel<kCap>>(std::span<const InstrumentId>{h.universe}, lookback);
  for (usize d = 0; d < closes.size(); ++d) {
    std::vector<atx::engine::SliceRow> rows;
    for (usize j = 0; j < h.universe.size(); ++j) {
      rows.push_back(atx::engine::SliceRow{
          h.universe[j],
          bar_row(h.universe[j], static_cast<i64>(d) + 1, static_cast<i64>(closes[d][j])).bar,
          false});
    }
    h.panel->append_sealed_row(atx::engine::MarketSlice{
        ts(static_cast<i64>(d) + 1), std::span<const atx::engine::SliceRow>{rows}});
  }
  return h;
}

// ============================================================================
//  ADAPTER PASS-THROUGH + CURRENT-DATE EXTRACTION
// ============================================================================

TEST(VmSignalSource, Evaluate_MatchesDirectEngineLastRow_RankClose) {
  // Three chronological dates; closes rise so cross-sectional rank is stable.
  const std::vector<std::vector<f64>> closes{
      {10.0, 20.0, 30.0}, {11.0, 19.0, 33.0}, {12.0, 25.0, 31.0}};
  const usize inst = 3;
  Program prog = compile_one("rank(close)");

  // Oracle: direct engine over the chronological panel, read the LAST date row.
  Panel oracle_panel = chronological_close_panel(closes, inst);
  Engine oracle_engine{oracle_panel};
  auto oracle = oracle_engine.evaluate(prog);
  ASSERT_TRUE(oracle);
  const std::span<const f64> want = oracle->alpha_cross_section(0, closes.size() - 1);

  // Adapter: same program, fed the newest-first RollingPanel view.
  PanelHolder h = filled_rolling_panel(closes, /*lookback=*/3);
  VmSignalSource src{compile_one("rank(close)")};
  auto got = src.evaluate(h.panel->view());
  ASSERT_TRUE(got);
  ASSERT_EQ(got->values.size(), inst);
  for (usize j = 0; j < inst; ++j) {
    EXPECT_DOUBLE_EQ(got->values[j], want[j]) << "instrument " << j;
  }
}

// ============================================================================
//  TRANSPOSE CORRECTNESS — a time-series alpha proves chronological order.
// ============================================================================

// -delta(close, 1) = -(close[t] - close[t-1]). A RISING close => negative score,
// FALLING => positive. If the transpose reversed time, the sign would flip — so
// the sign IS the chronological-order proof.
TEST(VmSignalSource, Evaluate_DeltaClose_ChronologicalSign) {
  // A rises (10->12), B falls (30->25), C flat (20->20) over the last step.
  const std::vector<std::vector<f64>> closes{
      {8.0, 28.0, 20.0}, {10.0, 30.0, 20.0}, {12.0, 25.0, 20.0}};
  const usize inst = 3;
  PanelHolder h = filled_rolling_panel(closes, /*lookback=*/3);
  VmSignalSource src{compile_one("-delta(close, 1)")};
  auto got = src.evaluate(h.panel->view());
  ASSERT_TRUE(got);
  ASSERT_EQ(got->values.size(), inst);

  // delta on the NEWEST date: A: 12-10=+2 -> -2 ; B: 25-30=-5 -> +5 ; C: 0 -> 0.
  EXPECT_DOUBLE_EQ(got->values[0], -2.0) << "rising close -> negative -delta (chronological)";
  EXPECT_DOUBLE_EQ(got->values[1], 5.0) << "falling close -> positive -delta (chronological)";
  EXPECT_DOUBLE_EQ(got->values[2], 0.0) << "flat close -> zero -delta";
}

// The adapter is PURE in the panel: the same panel contents -> the same signal.
TEST(VmSignalSource, Evaluate_PureInPanel_Repeatable) {
  const std::vector<std::vector<f64>> closes{{10.0, 20.0, 30.0}, {11.0, 22.0, 28.0}};
  PanelHolder h = filled_rolling_panel(closes, /*lookback=*/2);
  VmSignalSource src{compile_one("-delta(close, 1)")};

  auto first = src.evaluate(h.panel->view());
  ASSERT_TRUE(first);
  const std::vector<f64> snapshot(first->values.begin(), first->values.end());

  auto second = src.evaluate(h.panel->view());
  ASSERT_TRUE(second);
  ASSERT_EQ(second->values.size(), snapshot.size());
  for (usize j = 0; j < snapshot.size(); ++j) {
    EXPECT_DOUBLE_EQ(second->values[j], snapshot[j]) << "pure: same panel -> same signal";
  }
}

// ============================================================================
//  BOUNDARY — max_lookback forwards Program::required_lookback.
// ============================================================================

TEST(VmSignalSource, MaxLookback_ForwardsRequiredLookback) {
  // delta(close, 3) needs a 3-back window; ts_mean(close, 5) needs 5.
  const Program p3 = compile_one("delta(close, 3)");
  const Program p5 = compile_one("ts_mean(close, 5)");
  VmSignalSource s3{compile_one("delta(close, 3)")};
  VmSignalSource s5{compile_one("ts_mean(close, 5)")};
  EXPECT_EQ(s3.max_lookback(), static_cast<usize>(p3.required_lookback));
  EXPECT_EQ(s5.max_lookback(), static_cast<usize>(p5.required_lookback));
  EXPECT_GT(s5.max_lookback(), s3.max_lookback()) << "a deeper window reports a larger lookback";
}

// A single-date panel (lookback 1) still yields a current-date cross-section
// (an element-wise alpha needs no history).
TEST(VmSignalSource, Evaluate_SingleRowPanel_RankClose) {
  const std::vector<std::vector<f64>> closes{{10.0, 30.0, 20.0}};
  PanelHolder h = filled_rolling_panel(closes, /*lookback=*/1);
  VmSignalSource src{compile_one("rank(close)")};
  auto got = src.evaluate(h.panel->view());
  ASSERT_TRUE(got);
  ASSERT_EQ(got->values.size(), 3U);
  // rank in [0,1]: lowest (A=10) -> 0, highest (B=30) -> 1, mid (C=20) -> 0.5.
  EXPECT_DOUBLE_EQ(got->values[0], 0.0);
  EXPECT_DOUBLE_EQ(got->values[1], 1.0);
  EXPECT_DOUBLE_EQ(got->values[2], 0.5);
}

// ============================================================================
//  DRIVES THE REAL BacktestLoop
// ============================================================================

// A run harness over a VmSignalSource compiled from `alpha_src`. lookback sizes
// the RollingPanel; `every` is the rebalance cadence; `delay` the fill timing.
struct VmRun {
  BacktestResult result;
};

[[nodiscard]] VmRun run_vm(const std::vector<std::vector<BarRow>> &per_symbol,
                           std::string_view alpha_src, usize lookback, usize every, Delay delay,
                           int n_slices) {
  std::vector<InstrumentId> universe{kA, kB, kC};
  std::vector<std::span<const BarRow>> spans;
  for (const auto &v : per_symbol) {
    spans.emplace_back(std::span<const BarRow>{v});
  }
  const std::span<const std::span<const BarRow>> sources{spans};

  auto bus = std::make_unique<Bus>();
  SimClock clock;
  InMemoryBarFeed feed{sources, clock, *bus};

  std::vector<InstrumentStats> stats(universe.size(), InstrumentStats{});
  RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, lookback};
  VmSignalSource src{compile_one(alpha_src)};
  const WeightPolicy policy{};
  // Frictionless so the fill bar is the only delay-0/delay-1 difference.
  ExecutionSimulator sim{};
  Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}};

  BacktestLoop<kCap> loop{feed,
                          clock,
                          *bus,
                          panel,
                          src,
                          policy,
                          sim,
                          portfolio,
                          market,
                          Universe{universe},
                          Schedule{every},
                          delay};
  VmRun out;
  out.result = loop.run();
  (void)n_slices;
  return out;
}

// Build n_slices of moving closes so delta(close,1) produces real signals.
[[nodiscard]] std::vector<std::vector<BarRow>> moving_sources(int n_slices) {
  std::vector<BarRow> a, b, c;
  for (int k = 1; k <= n_slices; ++k) {
    a.push_back(bar_row(kA, k, 100 + k)); // A trending up
    b.push_back(bar_row(kB, k, 200 - k)); // B trending down
    c.push_back(bar_row(kC, k, 150));     // C flat
  }
  return {a, b, c};
}

TEST(VmSignalSource, DrivesRealLoop_RunsToEofWithFills) {
  const auto sources = moving_sources(8);
  const VmRun r = run_vm(sources, "-delta(close, 1)", /*lookback=*/2, /*every=*/1, Delay::Next, 8);
  EXPECT_EQ(r.result.slices, 8U);
  EXPECT_GT(r.result.rebalances, 0U);
  EXPECT_FALSE(r.result.fills.empty()) << "the VM-driven loop must trade";
}

TEST(VmSignalSource, DrivesRealLoop_Deterministic) {
  const auto sources = moving_sources(8);
  const VmRun r1 = run_vm(sources, "-delta(close, 1)", 2, 1, Delay::Next, 8);
  const VmRun r2 = run_vm(sources, "-delta(close, 1)", 2, 1, Delay::Next, 8);
  ASSERT_EQ(r1.result.fills.size(), r2.result.fills.size());
  for (usize i = 0; i < r1.result.fills.size(); ++i) {
    const FillPayload &f1 = r1.result.fills[i];
    const FillPayload &f2 = r2.result.fills[i];
    EXPECT_EQ(f1.id.id, f2.id.id);
    EXPECT_EQ(f1.qty, f2.qty);
    EXPECT_EQ(f1.price.raw(), f2.price.raw());
    EXPECT_EQ(f1.t.unix_nanos(), f2.t.unix_nanos());
  }
  EXPECT_DOUBLE_EQ(r1.result.final_equity, r2.result.final_equity);
}

// No-look-ahead: the earliest fill never lands on the slice-0 decision time (the
// firewall holds for the default Next).
TEST(VmSignalSource, DrivesRealLoop_NoFillOnDecisionSlice_Next) {
  const auto sources = moving_sources(8);
  const VmRun r = run_vm(sources, "-delta(close, 1)", 2, 1, Delay::Next, 8);
  ASSERT_FALSE(r.result.fills.empty());
  for (const FillPayload &f : r.result.fills) {
    EXPECT_GT(f.t.unix_nanos(), 1) << "delay-1: no fill on the slice-0 (ts=1) decision";
  }
}

// ============================================================================
//  DELAY-0 vs DELAY-1 — same signals, fill bar differs by one.
// ============================================================================

TEST(VmSignalSource, Delay0_FillsOneBarEarlierThanDelay1) {
  const auto sources = moving_sources(8);
  const VmRun next = run_vm(sources, "-delta(close, 1)", 2, 1, Delay::Next, 8);
  const VmRun same = run_vm(sources, "-delta(close, 1)", 2, 1, Delay::Same, 8);

  ASSERT_FALSE(next.result.fills.empty());
  ASSERT_FALSE(same.result.fills.empty());

  // delay-0 (Same) admits a fill on the SAME bar the decision was made; delay-1
  // (Next) does not. The earliest fill timestamp under Same is therefore <= the
  // earliest under Next, and strictly earlier when both first rebalance at t.
  i64 first_next = next.result.fills.front().t.unix_nanos();
  i64 first_same = same.result.fills.front().t.unix_nanos();
  for (const FillPayload &f : next.result.fills) {
    first_next = (f.t.unix_nanos() < first_next) ? f.t.unix_nanos() : first_next;
  }
  for (const FillPayload &f : same.result.fills) {
    first_same = (f.t.unix_nanos() < first_same) ? f.t.unix_nanos() : first_same;
  }
  EXPECT_LT(first_same, first_next) << "delay-0 fills one bar earlier than delay-1";
}

// The default (no Delay argument) is Next — the conservative firewall. A loop
// built WITHOUT a Delay argument must produce byte-identical fills to one built
// with an explicit Delay::Next (so the default truly equals Next).
TEST(VmSignalSource, DefaultDelayIsNext_FirewallIntact) {
  const auto sources = moving_sources(8);

  // Explicit Next via the shared harness.
  const VmRun explicit_next = run_vm(sources, "-delta(close, 1)", 2, 1, Delay::Next, 8);

  // Ctor default (no Delay argument) — inline the wiring so the trailing arg is
  // genuinely omitted (the harness always passes one).
  std::vector<InstrumentId> universe{kA, kB, kC};
  std::vector<std::span<const BarRow>> spans;
  for (const auto &v : sources) {
    spans.emplace_back(std::span<const BarRow>{v});
  }
  const std::span<const std::span<const BarRow>> srcs{spans};

  auto bus = std::make_unique<Bus>();
  SimClock clock;
  InMemoryBarFeed feed{srcs, clock, *bus};
  std::vector<InstrumentStats> stats(universe.size(), InstrumentStats{});
  RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, 2};
  VmSignalSource source{compile_one("-delta(close, 1)")};
  const WeightPolicy policy{};
  ExecutionSimulator sim{};
  Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}};
  // No Delay argument -> ctor default (must be Next).
  BacktestLoop<kCap> loop{feed,       clock, *bus,      panel,  source,
                          policy,     sim,   portfolio, market, Universe{universe},
                          Schedule{1}};
  const BacktestResult def = loop.run();

  ASSERT_FALSE(def.fills.empty());
  ASSERT_EQ(def.fills.size(), explicit_next.result.fills.size());
  for (usize i = 0; i < def.fills.size(); ++i) {
    EXPECT_EQ(def.fills[i].t.unix_nanos(), explicit_next.result.fills[i].t.unix_nanos());
    EXPECT_EQ(def.fills[i].qty, explicit_next.result.fills[i].qty);
  }
  // Firewall: still no fill on the slice-0 decision time under the default.
  for (const FillPayload &f : def.fills) {
    EXPECT_GT(f.t.unix_nanos(), 1) << "default (Next) keeps the no-look-ahead firewall";
  }
}

} // namespace
