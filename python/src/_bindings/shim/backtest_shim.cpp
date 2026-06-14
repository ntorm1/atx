#include "backtest_shim.hpp"

#include <span>
#include <stdexcept>

#include "atx/core/datetime.hpp"
#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/portfolio/portfolio.hpp"

namespace atxpy {
namespace {

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
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::ExecutionSimulator;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::SymbolTable;
using atx::core::time::Timestamp;

// Exact Decimal from an f64 price/volume input; throws on NaN/inf/range.
Decimal dec(atx::f64 v) {
  auto r = Decimal::from_double(v);
  if (!r) {
    throw std::runtime_error("price/volume not representable as Decimal: " + r.error().to_string());
  }
  return *r;
}

// Storage layer: built FIRST so the collaborators below borrow stable memory.
struct BacktestStorage {
  SymbolTable symbols;
  std::vector<InstrumentId> universe;
  std::vector<std::vector<BarRow>> bar_storage;
  std::vector<std::span<const BarRow>> bar_spans;
  std::vector<InstrumentStats> stats;
  Decimal starting_cash;

  explicit BacktestStorage(const BacktestParams &p) {
    const atx::usize n = p.symbols.size();
    if (p.bars.size() != n) {
      throw std::runtime_error("bars length must equal symbols length");
    }
    universe.reserve(n);
    for (const std::string &name : p.symbols) {
      universe.push_back(symbols.intern(name)); // id i == column i (insertion order)
    }
    bar_storage.resize(n);
    for (atx::usize i = 0; i < n; ++i) {
      const BarsForSymbol &b = p.bars[i];
      const atx::usize rows = b.ts_nanos.size();
      if (b.open.size() != rows || b.high.size() != rows || b.low.size() != rows ||
          b.close.size() != rows || b.volume.size() != rows) {
        throw std::runtime_error("OHLCV column lengths must match ts length");
      }
      bar_storage[i].reserve(rows);
      for (atx::usize r = 0; r < rows; ++r) {
        const Timestamp ts = Timestamp::from_unix_nanos(b.ts_nanos[r]);
        const Bar bar{ts,
                      Price::from_decimal(dec(b.open[r])),
                      Price::from_decimal(dec(b.high[r])),
                      Price::from_decimal(dec(b.low[r])),
                      Price::from_decimal(dec(b.close[r])),
                      Quantity::from_decimal(dec(b.volume[r]))};
        bar_storage[i].push_back(BarRow{universe[i], bar, ts, false});
      }
    }
    bar_spans.reserve(n);
    for (atx::usize i = 0; i < n; ++i) {
      bar_spans.emplace_back(bar_storage[i]);
    }
    if (!p.stats.empty()) {
      if (p.stats.size() != n) {
        throw std::runtime_error("stats must be empty or match symbols length");
      }
      stats = p.stats;
    }
    auto sc = Decimal::from_string(p.starting_cash);
    if (!sc) {
      throw std::runtime_error("starting_cash parse: " + sc.error().to_string());
    }
    starting_cash = *sc;
  }
};

// Collaborator layer: every non-Cap member the loop points at, constructed in
// declaration order over the already-built storage base.
struct BacktestEnv : BacktestStorage {
  SimClock clock;
  EventBus<> bus;
  InMemoryBarFeed feed;
  ScriptedSignalSource signal;
  WeightPolicy policy;
  ExecutionSimulator exec;
  Portfolio portfolio;
  Market market;

  explicit BacktestEnv(const BacktestParams &p)
      : BacktestStorage(p), clock{}, bus{},
        feed{std::span<const std::span<const BarRow>>{bar_spans}, clock, bus},
        signal{p.signals, universe.size(), p.max_lookback}, policy{p.policy},
        exec{p.fill, p.slip, p.impact, p.comm, p.latency, p.volcap},
        portfolio{starting_cash, std::span<const InstrumentId>{universe}},
        market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}} {}
};

template <atx::usize Cap> struct BacktestRunner final : IBacktestRunner {
  BacktestEnv env;
  RollingPanel<Cap> panel;
  BacktestLoop<Cap> loop;

  explicit BacktestRunner(const BacktestParams &p)
      : env{p}, panel{std::span<const InstrumentId>{env.universe}, p.max_lookback},
        loop{env.feed,
             env.clock,
             env.bus,
             panel,
             env.signal,
             env.policy,
             env.exec,
             env.portfolio,
             env.market,
             Universe{env.universe},
             Schedule{p.every},
             p.delay_same ? Delay::Same : Delay::Next} {}

  [[nodiscard]] BacktestResult run() override { return loop.run(); }
};

} // namespace

std::unique_ptr<IBacktestRunner> make_runner(const BacktestParams &p) {
  const atx::usize need = p.max_lookback;
  if (need == 0) {
    throw std::runtime_error("max_lookback must be >= 1");
  }
  if (need <= 8) return std::make_unique<BacktestRunner<8>>(p);
  if (need <= 16) return std::make_unique<BacktestRunner<16>>(p);
  if (need <= 32) return std::make_unique<BacktestRunner<32>>(p);
  if (need <= 64) return std::make_unique<BacktestRunner<64>>(p);
  if (need <= 128) return std::make_unique<BacktestRunner<128>>(p);
  if (need <= 256) return std::make_unique<BacktestRunner<256>>(p);
  if (need <= 512) return std::make_unique<BacktestRunner<512>>(p);
  if (need <= 1024) return std::make_unique<BacktestRunner<1024>>(p);
  if (need <= 4096) return std::make_unique<BacktestRunner<4096>>(p);
  throw std::runtime_error("max_lookback exceeds largest precompiled Cap (4096)");
}

} // namespace atxpy
