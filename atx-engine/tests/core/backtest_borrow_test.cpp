// backtest_borrow_test.cpp — S4-3c [CORRECTNESS, B5]: borrow must be accrued
// in the loop's settle sequence. cost::borrow.hpp ships a fully-built and
// tested daily_borrow/accrue_borrow, but had ZERO call sites in src/ --
// BacktestLoop's settle sequence never charged short financing, so a short
// book's realized P&L was overstated (it never paid to borrow the stock).
//
// Fix: BacktestLoop gains a trailing, inert-default `cost::BorrowModel borrow`
// constructor parameter (default `{annual_rate=0.0, D360}`) and calls
// cost::accrue_borrow(borrow_, portfolio, market, universe) once per bar,
// right after the settle step and before the panel seal.
//
// By-construction fixture: a single-instrument, frictionless-fill book. A
// scripted signal opens a 100%-of-equity SHORT at bar 1 (Raw transform,
// dollar_neutral=false so a single name can carry a nonzero signed weight);
// the order fills at bar 2's close (the no-look-ahead firewall: decide-t,
// fill-t+1) at an EXACT price (0 slippage/impact/commission), so the realized
// short notional is hand-computable: 1000 shares * $100 = $100,000.
// annual_rate=0.05, D360 -> ONE bar's borrow charge = 100000*0.05/360 exactly.

#include <array>
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
#include "atx/engine/cost/borrow.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/portfolio/portfolio.hpp"

namespace atxtest_backtest_borrow_test {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::engine::BacktestLoop;
using atx::engine::EventBus;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Transform;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::cost::BorrowModel;
using atx::engine::cost::DayCount;
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

using Bus = EventBus<>;
constexpr usize kCap = 8;
constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }
const Symbol kA{1};

[[nodiscard]] ExecutionSimulator make_frictionless_sim() {
  return ExecutionSimulator{
      FillCfg{},
      SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
      ImpactCfg{0.0, 0.5, 0.0},
      CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
      LatencyCfg{0},
      VolumeCapCfg{1.0}};
}

[[nodiscard]] BarRow bar_row(i64 k, i64 price) {
  Bar bar{};
  bar.ts = ts(k);
  bar.open = Price::from_int(price);
  bar.high = Price::from_int(price);
  bar.low = Price::from_int(price);
  bar.close = Price::from_int(price);
  bar.volume = Quantity::from_int(1'000'000);
  return BarRow{kA, bar, ts(k), /*delisted_final=*/false};
}

// Drives a 2-bar, single-name, frictionless backtest that opens a FULL-EQUITY
// position (short if `signal < 0`, long if `signal > 0`) at bar 1, filled at
// bar 2's close ($100). `borrow` is forwarded to BacktestLoop (omit -> the
// inert default). Returns the ending cash after bar 2 (exactly one settled
// day's borrow accrual on the now-open position, if any).
[[nodiscard]] f64 run_two_bar_book(f64 signal, BorrowModel borrow = BorrowModel{}) {
  std::vector<InstrumentId> universe{kA};
  std::vector<BarRow> a{bar_row(1, 100), bar_row(2, 100)};
  std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a}};
  const std::span<const std::span<const BarRow>> sources{spans};

  auto bus = std::make_unique<Bus>();
  SimClock clock;
  InMemoryBarFeed feed{sources, clock, *bus};

  RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, /*max_lookback=*/1};
  // ONE schedule entry: fires at bar 1 (slice 0); bar 2's fire (slice 1) finds
  // the scripted schedule exhausted -> no signal -> no NEW orders (the book
  // stays at the bar-1 decision through bar 2, per ISignalSource contract).
  ScriptedSignalSource src{std::vector<std::vector<f64>>{{signal}}, /*universe_size=*/1,
                           /*max_lookback=*/1};
  WeightPolicy policy{};
  policy.transform = Transform::Raw;    // preserve the SIGN of `signal` (Rank is >=0 only)
  policy.dollar_neutral = false;        // a single name can't demean to nonzero otherwise
  policy.winsorize_limit = 0.0;
  ExecutionSimulator sim = make_frictionless_sim();
  Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{}};
  const Schedule schedule{/*every=*/1};

  BacktestLoop<kCap> loop{feed,   clock,       *bus,        panel,
                          src,    policy,      sim,         portfolio,
                          market, Universe{universe}, schedule,
                          atx::engine::Delay::Next, borrow};
  (void)loop.run();
  return portfolio.cash().to_double();
}

// ===========================================================================
//  ShortBook_BorrowDebitsExactCharge: RED asserts the cash delta between a
//  borrow-on and borrow-off run on the SAME short book equals the exact one-
//  bar charge (fails at 0 pre-fix -- accrue_borrow was never called).
// ===========================================================================
TEST(BacktestBorrow, ShortBook_BorrowDebitsExactCharge) {
  const f64 cash_off = run_two_bar_book(/*signal=*/-1.0); // inert default, 0.0 rate
  const f64 cash_on =
      run_two_bar_book(/*signal=*/-1.0, BorrowModel{/*annual_rate=*/0.05, DayCount::D360});

  // weight=-1.0, dollar_neutral=false, gross_leverage=1.0 (default) -> full
  // 100% short: target_shares = trunc(-1.0*100000/100) = -1000 -> notional 1000*100=100000.
  const f64 expected_charge = 100000.0 * 0.05 / 360.0;
  EXPECT_NEAR(cash_off - cash_on, expected_charge, 1e-6)
      << "a one-bar short hold must debit exactly short_notional*annual_rate/360; "
      << "cash_off=" << cash_off << " cash_on=" << cash_on;
}

// ===========================================================================
//  LongOnlyBook_PaysZeroBorrow: a long-only book has zero short notional, so
//  turning borrow on must not change ending cash at all.
// ===========================================================================
TEST(BacktestBorrow, LongOnlyBook_PaysZeroBorrow) {
  const f64 cash_off = run_two_bar_book(/*signal=*/+1.0);
  const f64 cash_on =
      run_two_bar_book(/*signal=*/+1.0, BorrowModel{/*annual_rate=*/0.05, DayCount::D360});
  EXPECT_NEAR(cash_off, cash_on, 1e-9) << "a long-only book must pay exactly zero borrow";
}

// ===========================================================================
//  ZeroRate_ByteIdenticalToDefault: an EXPLICIT annual_rate=0.0 must produce
//  cash byte-identical to omitting the parameter entirely (the inert default,
//  boundary-pin no-op every pre-existing BacktestLoop caller relies on).
// ===========================================================================
TEST(BacktestBorrow, ZeroRate_ByteIdenticalToDefault) {
  const f64 cash_default = run_two_bar_book(/*signal=*/-1.0);
  const f64 cash_explicit_zero =
      run_two_bar_book(/*signal=*/-1.0, BorrowModel{/*annual_rate=*/0.0, DayCount::D360});
  EXPECT_EQ(cash_default, cash_explicit_zero)
      << "annual_rate=0.0 must be byte-identical to the omitted-parameter default";
}

} // namespace atxtest_backtest_borrow_test
