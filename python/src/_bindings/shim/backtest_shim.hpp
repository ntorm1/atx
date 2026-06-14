#pragma once
#include <memory>
#include <string>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxpy {

// One BarsForSymbol per universe symbol: column-wise OHLCV time series in
// knowledge_ts order. Filled from Python (lists / numpy).
struct BarsForSymbol {
  std::vector<atx::i64> ts_nanos;
  std::vector<atx::f64> open;
  std::vector<atx::f64> high;
  std::vector<atx::f64> low;
  std::vector<atx::f64> close;
  std::vector<atx::f64> volume;
};

// Plain value struct filled from Python. Marshalled into the engine graph by
// make_runner(). No engine objects are constructed until run time.
struct BacktestParams {
  std::vector<std::string> symbols;            // defines universe column order
  std::vector<BarsForSymbol> bars;             // bars[i] <-> symbols[i]
  std::string starting_cash = "1000000.0";     // exact decimal string
  std::vector<std::vector<atx::f64>> signals;  // [rebalance][universe] alpha scores
  atx::usize max_lookback = 1;
  atx::usize every = 1;
  bool delay_same = false;                     // false=Next (firewall), true=Same
  atx::engine::WeightPolicy policy{};
  atx::engine::exec::FillCfg fill{};
  atx::engine::exec::SlippageCfg slip{};
  atx::engine::exec::ImpactCfg impact{};
  atx::engine::exec::CommissionCfg comm{};
  atx::engine::exec::LatencyCfg latency{};
  atx::engine::exec::VolumeCapCfg volcap{};
  std::vector<atx::engine::InstrumentStats> stats{}; // empty or size==symbols
};

// Type-erased runner over the templated RollingPanel<Cap>/BacktestLoop<Cap>.
struct IBacktestRunner {
  virtual ~IBacktestRunner() = default;
  [[nodiscard]] virtual atx::engine::BacktestResult run() = 0;
};

// Picks the smallest precompiled power-of-two Cap >= params.max_lookback.
// Throws std::runtime_error if max_lookback exceeds the largest Cap (4096) or is 0.
[[nodiscard]] std::unique_ptr<IBacktestRunner> make_runner(const BacktestParams &params);

} // namespace atxpy
