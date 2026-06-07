// phase4_bench.cpp — micro-benchmarks for the Phase-4 signal-combination + risk
// hot paths (P4-10c). MEASURED-ONLY: no correctness assertions, no production code.
//
// Four benches, one per Phase-4 cold/rebalance-cadence path the plan (§8 P4-10
// "Bench") calls out. Each builds its synthetic universe/pool/panel ONCE outside the
// measured loop and measures only the target call; fixtures are FIXED (no RNG, vary
// by index) and small enough that a Debug build finishes each in well under a second
// at a short min-time.
//
//   BM_CombinerFit     — combine::AlphaCombiner::fit(pool, fit_begin, fit_end) over a
//                        synthetic combine::AlphaStore. Parameterized by COMBINE
//                        METHOD (EqualWeight closed-form, IcWeighted window-sharpe,
//                        ShrinkageMv the Ledoit-Wolf shrunk MV solve) and by
//                        (N alphas, T window) via Args({n, t}). Items/s == fits/s.
//   BM_FactorBuild     — risk::FactorModelBuilder::build(panel, window, cap, group)
//                        over a synthetic PanelView (sectors-only K=1 config — no
//                        252-row style lookback). Parameterized by (window, M) via
//                        Args({window, m}). Items/s == builds/s.
//   BM_OptimizerSolve  — risk::PortfolioOptimizer::solve(alpha, V, w_prev) per
//                        rebalance. The FactorModel V is built ONCE outside the
//                        measured loop; only solve() is timed. Parameterized by M.
//                        Items/s == solves/s.
//   BM_WalkForwardLoop — the full combined walk-forward: loop::BacktestLoop::run()
//                        driven by a combine::CombinedSignalSource (EqualWeight blend
//                        of two scripted constituents) + a WeightPolicy over a
//                        synthetic multi-instrument bar feed. The whole feed/clock/
//                        bus/panel/exec/portfolio is rebuilt per iteration (the loop
//                        registers a bus consumer + mutates the portfolio), so this
//                        measures the end-to-end per-RUN cost. Parameterized by the
//                        bar count. Items/s == bars (slices) processed/s.
//
// Build is Debug / clang-cl (the project default), so every ns/iter here is an
// UPPER BOUND, not the optimised figure. Host/build context (CPU, cores, build type)
// is recorded by Google Benchmark's own header and echoed in the report.

#include <cstdint> // int64_t (state counters)
#include <limits>  // std::numeric_limits (the panel NaN sentinel)
#include <memory>  // std::make_unique (per-iteration EventBus)
#include <span>    // std::span
#include <utility> // std::move (constituents into CombinedSignalSource)
#include <vector>  // std::vector (synthetic fixtures)

#include <benchmark/benchmark.h>

#include "atx/core/decimal.hpp"        // Decimal (Portfolio starting cash)
#include "atx/core/domain/domain.hpp"  // Bar / Price / Quantity
#include "atx/core/domain/symbol.hpp"  // Symbol
#include "atx/core/types.hpp"          // f64, i64, u32, usize

#include "atx/engine/bus/event_bus.hpp"          // EventBus
#include "atx/engine/clock/sim_clock.hpp"         // SimClock
#include "atx/engine/combine/combined_source.hpp" // CombinedSignalSource
#include "atx/engine/combine/combiner.hpp"        // AlphaCombiner, CombineMethod, Combination
#include "atx/engine/combine/metrics.hpp"         // AlphaMetrics
#include "atx/engine/combine/store.hpp"           // AlphaStore
#include "atx/engine/data/data_handler.hpp"       // InMemoryBarFeed, BarRow
#include "atx/engine/exec/execution_sim.hpp"      // ExecutionSimulator + cfgs
#include "atx/engine/loop/backtest_loop.hpp"      // BacktestLoop, Schedule, BacktestResult
#include "atx/engine/loop/market.hpp"             // Market, InstrumentStats
#include "atx/engine/loop/panel_types.hpp"        // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/rolling_panel.hpp"      // RollingPanel
#include "atx/engine/loop/signal_source.hpp"      // ISignalSource, ScriptedSignalSource
#include "atx/engine/loop/types.hpp"              // InstrumentId, Universe
#include "atx/engine/loop/weight_policy.hpp"      // WeightPolicy
#include "atx/engine/portfolio/portfolio.hpp"     // Portfolio
#include "atx/engine/risk/factor_model.hpp"       // FactorModelBuilder, FactorModel, FactorModelConfig
#include "atx/engine/risk/optimizer.hpp"          // PortfolioOptimizer, OptimizerConfig

namespace {

using atx::f64;
using atx::i64;
using atx::u32;
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
using atx::engine::ISignalSource;
using atx::engine::kPanelFieldCount;
using atx::engine::Market;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::combine::AlphaCombiner;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::Combination;
using atx::engine::combine::CombinedSignalSource;
using atx::engine::combine::CombineMethod;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::risk::FactorModel;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::PortfolioOptimizer;
using Bus = EventBus<>;
using Timestamp = atx::core::time::Timestamp;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kCap = 64; // RollingPanel ring capacity for the walk-forward loop

[[nodiscard]] constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// ===========================================================================
//  Synthetic-pool helpers (mirror phase4_integration_test.cpp::pool_rows).
//
//  `n` deterministic, mildly-correlated, bounded PnL streams of length `periods`
//  (index 0 is the structural zero — P4-2 §0-F). Vary by index, NEVER by RNG, so the
//  fit is byte-reproducible across iterations. Four base shapes are tiled across N.
// ===========================================================================
[[nodiscard]] std::vector<std::vector<f64>> make_pool_rows(usize n, usize periods) {
  std::vector<std::vector<f64>> rows(n, std::vector<f64>(periods, 0.0));
  for (usize a = 0U; a < n; ++a) {
    for (usize t = 1U; t < periods; ++t) { // t == 0 stays the structural zero
      const f64 x = static_cast<f64>(t);
      const usize shape = a % 4U;
      switch (shape) {
      case 0U:
        rows[a][t] = 0.010 * ((t % 2U == 0U) ? 1.0 : -1.0);
        break;
      case 1U:
        rows[a][t] = 0.008 * ((t % 3U == 0U) ? 2.0 : -1.0);
        break;
      case 2U:
        rows[a][t] = 0.012 * (((t / 2U) % 2U == 0U) ? 1.0 : -1.0) + 0.001;
        break;
      default:
        rows[a][t] = 0.006 * ((t % 4U < 2U) ? 1.0 : -1.0) + 0.0005 * x;
        break;
      }
      // A tiny per-alpha tilt so no two streams are bit-identical (keeps the SCM
      // full-rank for the ShrinkageMv solve when N tiles the four base shapes).
      rows[a][t] += 1e-4 * static_cast<f64>(a + 1U) * ((t % 5U == 0U) ? 1.0 : -1.0);
    }
  }
  return rows;
}

// A `n`-alpha AlphaStore over `periods`-long PnL streams. Single-instrument pool
// (positions length == PnL length); the combiner reads only the PnL rows.
[[nodiscard]] AlphaStore make_store(usize n, usize periods) {
  AlphaStore pool;
  const std::vector<std::vector<f64>> rows = make_pool_rows(n, periods);
  const std::vector<f64> pos(periods, 0.0); // insts == 1 -> period-major == pnl length
  for (const std::vector<f64> &row : rows) {
    const auto r = pool.insert(/*source=*/nullptr, std::span<const f64>{row},
                               std::span<const f64>{pos}, AlphaMetrics{});
    benchmark::DoNotOptimize(r.has_value());
  }
  return pool;
}

// ===========================================================================
//  PanelFixture — owns a PanelView's backing storage (the phase4_integration_test
//  pattern). `n_rows` x `n_inst` close grid (row 0 == newest); open/high/low mirror
//  close; volume is a flat constant. NO NaN gaps (a dense panel for the bench).
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0U; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0U; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical row
      for (usize i = 0U; i < n_inst_; ++i) {
        // Deterministic geometric-ish walk per instrument (no RNG; varies by index).
        f64 px = 100.0 + 10.0 * static_cast<f64>(i);
        px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
        set(PanelField::Open, phys, i, px);
        set(PanelField::High, phys, i, px);
        set(PanelField::Low, phys, i, px);
        set(PanelField::Close, phys, i, px);
        set(PanelField::Volume, phys, i, 1000.0);
        mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
      }
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           head_(),      n_rows_,
                     mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }

  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }

  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<atx::u64> mask_;
};

// Sectors-only factor-model config (one 0/1 dummy column over all instruments) ->
// K == 1 with NO style columns (which would need 252+ rows of lookback). The
// canonical low-lookback factor model (matches phase4_integration_test).
[[nodiscard]] FactorModelConfig single_sector_cfg() {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00; // sectors only -> no per-instrument lookback needed
  return cfg;
}

// A `n_inst` group/sector id vector: 4 sectors round-robin (K == sector count). A
// single sector (n_inst small) collapses to K == 1.
[[nodiscard]] std::vector<u32> make_groups(usize n_inst) {
  std::vector<u32> g(n_inst);
  for (usize i = 0U; i < n_inst; ++i) {
    g[i] = static_cast<u32>(i % 4U) + 1U; // sectors 1..4 round-robin
  }
  return g;
}

// A deterministic length-M alpha cross-section (no RNG; varies by index, bounded).
[[nodiscard]] std::vector<f64> make_alpha(usize m) {
  std::vector<f64> a(m);
  for (usize i = 0U; i < m; ++i) {
    a[i] = (static_cast<f64>(i % 7U) - 3.0) * 0.25 + 0.01 * static_cast<f64>(i % 3U);
  }
  return a;
}

// ===========================================================================
//  BM_CombinerFit — combine::AlphaCombiner::fit over a synthetic AlphaStore.
//
//  state.range(0) == N alphas, state.range(1) == T fit window. The method is a
//  template parameter so EqualWeight / IcWeighted / ShrinkageMv each register as a
//  distinct row. The store is built ONCE; only fit() is measured.
// ===========================================================================
template <CombineMethod kMethod> void BM_CombinerFit(benchmark::State &state) {
  const usize n = static_cast<usize>(state.range(0));
  const usize t = static_cast<usize>(state.range(1));
  const usize periods = t + 1U;     // +1 so the fit window [1, t+1) has T == t periods
  const AlphaStore pool = make_store(n, periods);
  AlphaCombiner comb;
  comb.cfg.method = kMethod;

  for (auto _ : state) {
    auto r = comb.fit(pool, /*fit_begin=*/1U, /*fit_end=*/periods);
    benchmark::DoNotOptimize(r.has_value() ? r.value().weights.data() : nullptr);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
// EqualWeight: the closed-form 1/N baseline (no covariance / solve).
BENCHMARK(BM_CombinerFit<CombineMethod::EqualWeight>)
    ->Args({8, 64})
    ->Args({32, 128})
    ->Args({64, 256});
// IcWeighted: per-alpha window-sharpe proxy (a moment pass per alpha).
BENCHMARK(BM_CombinerFit<CombineMethod::IcWeighted>)
    ->Args({8, 64})
    ->Args({32, 128})
    ->Args({64, 256});
// ShrinkageMv: the Ledoit-Wolf shrunk mean-variance solve (the cold-path heavy fit).
BENCHMARK(BM_CombinerFit<CombineMethod::ShrinkageMv>)
    ->Args({8, 64})
    ->Args({32, 128})
    ->Args({64, 256});

// ===========================================================================
//  BM_FactorBuild — risk::FactorModelBuilder::build over a synthetic PanelView.
//
//  state.range(0) == window (trailing rows), state.range(1) == M instruments. K is
//  the sector group count (4 round-robin sectors for M >= 4). The panel is built
//  ONCE; only build() is measured.
// ===========================================================================
void BM_FactorBuild(benchmark::State &state) {
  const usize window = static_cast<usize>(state.range(0));
  const usize m = static_cast<usize>(state.range(1));
  const usize n_rows = window + 2U; // a couple extra OLD rows beyond the window's reach
  const PanelFixture fx{n_rows, m};
  const std::vector<u32> group = make_groups(m);
  FactorModelBuilder builder{single_sector_cfg()};

  for (auto _ : state) {
    auto model = builder.build(fx.view(), window, std::span<const f64>{},
                               std::span<const u32>{group});
    benchmark::DoNotOptimize(model.has_value());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FactorBuild)->Args({8, 16})->Args({16, 64})->Args({32, 128});

// ===========================================================================
//  BM_OptimizerSolve — risk::PortfolioOptimizer::solve per rebalance.
//
//  state.range(0) == M instruments. The FactorModel V is built ONCE outside the
//  measured loop (the per-rebalance reality: the model is fit on a cadence, the
//  solve runs every rebalance). Only solve() is measured. λ > 0 so the V^-1 tilt
//  path (apply_inverse) is exercised. w_prev is empty (turnover from flat).
// ===========================================================================
void BM_OptimizerSolve(benchmark::State &state) {
  const usize m = static_cast<usize>(state.range(0));
  const usize window = 16U; // enough trailing rows for the factor fit
  const PanelFixture fx{window + 2U, m};
  const std::vector<u32> group = make_groups(m);
  FactorModelBuilder builder{single_sector_cfg()};
  const auto built = builder.build(fx.view(), window, std::span<const f64>{},
                                   std::span<const u32>{group});
  if (!built.has_value()) {
    state.SkipWithError("BM_OptimizerSolve: FactorModelBuilder::build failed");
    return;
  }
  const FactorModel &model = built.value();
  const std::vector<f64> alpha = make_alpha(m);

  OptimizerConfig ocfg;
  ocfg.risk_aversion = 1.0; // λ > 0 -> the V^-1 tilt path (apply_inverse)
  ocfg.gross_leverage = 1.0;
  ocfg.name_cap = 0.6;
  PortfolioOptimizer opt;
  opt.cfg = ocfg;

  for (auto _ : state) {
    auto book = opt.solve(std::span<const f64>{alpha}, model, std::span<const f64>{});
    benchmark::DoNotOptimize(book.has_value() ? book.value().data() : nullptr);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OptimizerSolve)->Arg(16)->Arg(64)->Arg(128);

// ===========================================================================
//  BM_WalkForwardLoop — the full combined walk-forward BacktestLoop::run().
//
//  A CombinedSignalSource (EqualWeight gross-1 blend of two scripted constituents,
//  each emitting a fixed [3,2,1]-style ramp) drives the REAL BacktestLoop through a
//  WeightPolicy over a 3-instrument synthetic bar feed. The whole feed/clock/bus/
//  panel/exec/portfolio is rebuilt per iteration (the loop registers a bus consumer
//  + drains the portfolio), so this measures the end-to-end per-RUN cost. range(0)
//  == bar count; items == bars (slices) processed.
// ===========================================================================
void BM_WalkForwardLoop(benchmark::State &state) {
  const int n_bars = static_cast<int>(state.range(0));
  const Symbol a_sym{1};
  const Symbol b_sym{2};
  const Symbol c_sym{3};
  const std::vector<InstrumentId> universe{a_sym, b_sym, c_sym};

  // Synthetic 3-instrument bar feed (built once; the feed is replayed each run via a
  // fresh InMemoryBarFeed over the same row storage).
  std::vector<BarRow> a;
  std::vector<BarRow> b;
  std::vector<BarRow> c;
  a.reserve(static_cast<usize>(n_bars));
  b.reserve(static_cast<usize>(n_bars));
  c.reserve(static_cast<usize>(n_bars));
  for (int k = 1; k <= n_bars; ++k) {
    const auto mk = [&](const Symbol &sym, i64 price) {
      Bar bar{};
      bar.ts = ts(k);
      bar.open = Price::from_int(price);
      bar.high = Price::from_int(price);
      bar.low = Price::from_int(price);
      bar.close = Price::from_int(price);
      bar.volume = Quantity::from_int(1'000'000);
      return BarRow{sym, bar, ts(k), false};
    };
    a.push_back(mk(a_sym, 100));
    b.push_back(mk(b_sym, 50));
    c.push_back(mk(c_sym, 200));
  }
  const std::vector<std::span<const BarRow>> spans{
      std::span<const BarRow>{a}, std::span<const BarRow>{b}, std::span<const BarRow>{c}};
  const std::span<const std::span<const BarRow>> sources{spans};

  // Two scripted constituents; an EqualWeight gross-1 blend of two identical [3,2,1]
  // schedules == [3,2,1] every rebalance (the combine capstone trick), so the
  // mega-alpha is a deterministic dollar-neutral ramp.
  const std::vector<std::vector<f64>> sched(static_cast<usize>(n_bars),
                                            std::vector<f64>{3.0, 2.0, 1.0});
  const std::vector<InstrumentStats> stats(universe.size(), InstrumentStats{});
  const Schedule schedule{1U};
  const WeightPolicy policy{};

  for (auto _ : state) {
    // Per-RUN scratch: a fresh bus (the loop registers consumer 0), clock, panel,
    // exec, portfolio, market — all rebuilt so each iteration is an independent run.
    auto bus = std::make_unique<Bus>();
    SimClock clock;
    InMemoryBarFeed feed{sources, clock, *bus};
    RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, /*max_lookback=*/1U};
    ScriptedSignalSource c0{sched, /*universe_size=*/3U, /*max_lookback=*/1U};
    ScriptedSignalSource c1{sched, /*universe_size=*/3U, /*max_lookback=*/1U};
    std::vector<ISignalSource *> constituents{&c0, &c1};
    CombinedSignalSource mega{std::move(constituents), Combination{{0.5, 0.5}, 0U, 0U},
                              CombineMethod::EqualWeight};
    ExecutionSimulator sim{FillCfg{},       SlippageCfg{}, ImpactCfg{},
                           CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{/*volume_limit=*/1.0}};
    Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
    Market market{std::span<const InstrumentId>{universe},
                  std::span<const InstrumentStats>{stats}};

    BacktestLoop<kCap> loop{feed,   clock, *bus,      panel,  mega,
                            policy, sim,   portfolio, market, Universe{universe},
                            schedule};
    auto result = loop.run();
    benchmark::DoNotOptimize(result.final_equity);
    benchmark::DoNotOptimize(result.fills.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n_bars));
}
BENCHMARK(BM_WalkForwardLoop)->Arg(16)->Arg(64)->Arg(256);

} // namespace
