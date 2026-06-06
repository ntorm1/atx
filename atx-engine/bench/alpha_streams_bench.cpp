// alpha_streams_bench.cpp — P3c-4: per-alpha stream extraction throughput +
// VmSignalSource in-loop overhead.
//
// Two measured regions (Debug / clang-cl — UPPER-BOUND latencies, NOT release
// numbers; host/build context rides in Google Benchmark's own header):
//
//   * BM_ExtractStreams — extract_streams over an N-alpha SignalSet on a fixed
//     dates×instruments panel. Reports alphas/s (one "item" == one alpha-stream
//     built). This is the cold-ish research-cadence Phase-3 -> Phase-4 handoff.
//   * BM_VmSignalSourceEvaluate — one VmSignalSource::evaluate over a filled
//     RollingPanel view (transpose + alpha::Panel build + VM run + current-date
//     extract). Reports ns/rebalance — the per-rebalance bridge overhead the
//     BacktestLoop pays (evaluate runs at the rebalance cadence, not per bar).
//
// Both reuse the same fixed-seed synthetic data the unit suites use (no <random>,
// never clocked — fully deterministic).

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::engine::InstrumentId;
using atx::engine::MarketSlice;
using atx::engine::RollingPanel;
using atx::engine::SliceRow;
using atx::engine::VmSignalSource;
using atx::engine::WeightPolicy;
using atx::engine::alpha::compile_batch;
using atx::engine::alpha::extract_streams;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
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

constexpr usize kStreamDates = 256;
constexpr usize kStreamInsts = 64;
constexpr usize kStreamAlphas = 16;
constexpr usize kVmCap = 64;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// A frictionless ExecutionSimulator (costs off -> the analytic stream).
[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

// Fixed-seed synthetic close matrix (date-major), deterministic LCG.
[[nodiscard]] std::vector<f64> make_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> close(dates * insts);
  std::uint64_t state = seed | 1ULL;
  auto next = [&state]() noexcept -> f64 {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) / static_cast<f64>(1ULL << 53);
  };
  for (usize i = 0; i < close.size(); ++i) {
    close[i] = 10.0 + next() * 190.0;
  }
  return close;
}

// An N-alpha SignalSet of fixed-seed pseudo-signals (deterministic LCG).
[[nodiscard]] SignalSet make_signals(usize dates, usize insts, usize n_alphas, std::uint64_t seed) {
  SignalSet s;
  s.dates = dates;
  s.instruments = insts;
  std::uint64_t state = seed | 1ULL;
  auto next = [&state]() noexcept -> f64 {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) / static_cast<f64>(1ULL << 53);
  };
  for (usize a = 0; a < n_alphas; ++a) {
    std::vector<f64> vals(dates * insts);
    for (usize i = 0; i < vals.size(); ++i) {
      vals[i] = next() * 2.0 - 1.0;
    }
    s.alphas.push_back(SignalSet::Alpha{"a" + std::to_string(a), std::move(vals)});
  }
  return s;
}

// ---- BM_ExtractStreams: per-alpha stream extraction (alphas/s) --------------

void BM_ExtractStreams(benchmark::State &state) {
  std::vector<std::string> fields{"close"};
  std::vector<std::vector<f64>> cols{make_close(kStreamDates, kStreamInsts, 0x5712EA34ULL)};
  auto pr = Panel::create(kStreamDates, kStreamInsts, fields, cols, {});
  if (!pr) {
    state.SkipWithError("panel build failed");
    return;
  }
  const Panel panel = std::move(pr.value());
  const SignalSet sig = make_signals(kStreamDates, kStreamInsts, kStreamAlphas, 0xA1FA0011ULL);
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();

  for (auto _ : state) {
    auto out = extract_streams(sig, policy, panel, sim);
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }
  // One "item" == one alpha-stream built per iteration.
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kStreamAlphas));
  state.counters["alphas"] = static_cast<double>(kStreamAlphas);
  state.counters["periods"] = static_cast<double>(kStreamDates);
  state.counters["instruments"] = static_cast<double>(kStreamInsts);
}
BENCHMARK(BM_ExtractStreams)->Unit(benchmark::kMicrosecond);

// ---- BM_VmSignalSourceEvaluate: in-loop bridge overhead (ns/rebalance) ------

// One constant-OHLCV sealed slice row.
[[nodiscard]] SliceRow slice_row(const Symbol &sym, i64 k, f64 price) {
  Bar bar{};
  bar.ts = Timestamp::from_unix_nanos(k);
  const auto px = Price::from_int(static_cast<i64>(price));
  bar.open = px;
  bar.high = px;
  bar.low = px;
  bar.close = px;
  bar.volume = Quantity::from_int(1'000'000);
  return SliceRow{sym, bar, false};
}

void BM_VmSignalSourceEvaluate(benchmark::State &state) {
  // A modest universe + lookback — the per-rebalance shape a real loop hands the
  // bridge (transpose + alpha::Panel build + VM run dominate, not the size).
  constexpr usize kInsts = 16;
  constexpr usize kLookback = 10;
  std::vector<InstrumentId> universe;
  for (usize j = 0; j < kInsts; ++j) {
    universe.push_back(Symbol{static_cast<atx::u32>(j + 1)});
  }
  RollingPanel<kVmCap> panel{std::span<const InstrumentId>{universe}, kLookback};

  // Fill the rolling window (oldest->newest) with fixed-seed moving closes.
  const std::vector<f64> close = make_close(kLookback, kInsts, 0xC0FFEE11ULL);
  for (usize d = 0; d < kLookback; ++d) {
    std::vector<SliceRow> rows;
    for (usize j = 0; j < kInsts; ++j) {
      rows.push_back(slice_row(universe[j], static_cast<i64>(d) + 1, close[d * kInsts + j]));
    }
    panel.append_sealed_row(MarketSlice{Timestamp::from_unix_nanos(static_cast<i64>(d) + 1),
                                        std::span<const SliceRow>{rows}});
  }

  // A real time-series alpha so the VM exercises a trailing-window kernel.
  const std::vector<std::string_view> srcs{"rank(close) - ts_mean(close, 5)"};
  auto prog = compile_batch(std::span<const std::string_view>{srcs}, shared_lib());
  if (!prog) {
    state.SkipWithError("alpha compile failed");
    return;
  }
  VmSignalSource src{std::move(prog).value()};

  // Warm once (size the source-owned scratch), then time the per-rebalance call.
  auto warm = src.evaluate(panel.view());
  if (!warm) {
    state.SkipWithError("warm evaluate failed");
    return;
  }
  for (auto _ : state) {
    auto out = src.evaluate(panel.view());
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }
  // One "item" == one rebalance evaluate.
  state.SetItemsProcessed(state.iterations());
  state.counters["instruments"] = static_cast<double>(kInsts);
  state.counters["lookback"] = static_cast<double>(kLookback);
}
BENCHMARK(BM_VmSignalSourceEvaluate)->Unit(benchmark::kNanosecond);

} // namespace
