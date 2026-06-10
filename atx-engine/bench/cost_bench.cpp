// cost_bench.cpp — micro-benchmarks for the S6 cost layer (atx::engine::cost).
//
// Sizes the cost-layer hot paths that a research / sizing pass drives:
//   - calibration fit (calibrate_from_obs) at n ∈ {500, 2000, 8000} observations
//     — the log-linear power-law recovery + the perm through-origin slope;
//   - the IRLS-Huber kernel (irls_huber) in isolation — the iterative reweighting
//     that dominates the temp-channel fit;
//   - the capacity-curve sweep (capacity_for_book) at grid ∈ {8, 32} AUM points;
//   - the daily borrow accrual (daily_borrow) over a short book.
//
// Build is Debug / clang-cl (the project default), so these are UPPER-BOUND
// latencies. Host/build context is recorded by Google Benchmark's own header.
// This bench exists to COMPILE+LINK the cost surface under the bench harness; it
// is not pinned to a perf target.

#include <array>
#include <cmath>  // std::pow, std::log, std::isnan
#include <limits> // std::numeric_limits (NaN panel sentinel)
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/datetime.hpp"      // Timestamp
#include "atx/core/decimal.hpp"       // Decimal
#include "atx/core/domain/domain.hpp" // Bar, Price, Quantity
#include "atx/core/linalg/linalg.hpp" // MatX, VecX (IRLS design)
#include "atx/core/random.hpp"        // Xoshiro256pp (synthetic obs)
#include "atx/core/types.hpp"         // f64, i64, u32, u64, usize

#include "atx/engine/cost/borrow.hpp"        // BorrowModel, daily_borrow
#include "atx/engine/cost/calibration.hpp"   // calibrate_from_obs, CostObs
#include "atx/engine/cost/capacity.hpp"      // capacity_for_book
#include "atx/engine/cost/robust_ls.hpp"     // irls_huber, RobustCfg
#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator + cfg structs
#include "atx/engine/exec/payloads.hpp"      // FillPayload
#include "atx/engine/loop/market.hpp"        // Market, InstrumentStats
#include "atx/engine/loop/panel_types.hpp"   // PanelView, PanelField, MarketSlice, SliceRow
#include "atx/engine/loop/types.hpp"         // InstrumentId
#include "atx/engine/portfolio/portfolio.hpp" // Portfolio

namespace {

using atx::f64;
using atx::i64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::core::time::Timestamp;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::kPanelFieldCount;
using atx::engine::Market;
using atx::engine::MarketSlice;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::Portfolio;
using atx::engine::SliceRow;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::FillPayload;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::VolumeCapCfg;
namespace cost = atx::engine::cost;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

[[nodiscard]] InstrumentId inst(u32 id) noexcept { return InstrumentId{id}; }

// Synthetic observation set — same structural laws the calibration inverts.
[[nodiscard]] std::vector<cost::CostObs> make_obs(usize n) {
  atx::core::Xoshiro256pp rng{7ULL};
  std::vector<cost::CostObs> obs;
  obs.reserve(n);
  for (usize i = 0; i < n; ++i) {
    const f64 sigma = 0.02;
    const f64 p = rng.uniform(0.001, 0.10);
    const f64 temp = 0.8 * sigma * std::pow(p, 0.55) * (1.0 + 0.02 * rng.normal());
    const f64 perm = 0.5 * 0.3 * sigma * p * (1.0 + 0.02 * rng.normal());
    obs.push_back(cost::CostObs{p, sigma, temp, perm});
  }
  return obs;
}

// A log-linear temp design (x = [1, log p], y = log(temp/σ)) for the IRLS kernel.
struct Design {
  MatX X;
  VecX y;
};
[[nodiscard]] Design make_design(usize n) {
  auto obs = make_obs(n);
  MatX X(static_cast<Eigen::Index>(n), 2);
  VecX y(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    const auto r = static_cast<Eigen::Index>(i);
    X(r, 0) = 1.0;
    X(r, 1) = std::log(obs[i].participation);
    y(r) = std::log(obs[i].temp / obs[i].sigma);
  }
  return Design{X, y};
}

// ===========================================================================
//  PanelFixture — minimal volatile-rising panel for the capacity sweep.
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>>& close,
               const std::vector<std::vector<f64>>& volume)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r;
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, volume[r][i]);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
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
  void set(PanelField f, usize phys, usize inst_i, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst_i] = v;
  }
  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<u64> mask_;
};

[[nodiscard]] PanelFixture make_panel(usize n_rows, usize n_inst) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst, 0.0));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1.0e3));
  for (usize i = 0; i < n_inst; ++i) {
    close[0][i] = 100.0;
    for (usize r = 1; r < n_rows; ++r) {
      const f64 ratio = (r % 2U == 1U) ? 1.003 : 1.0002;
      close[r][i] = close[r - 1][i] / ratio;
    }
  }
  return PanelFixture{n_rows, n_inst, close, volume};
}

[[nodiscard]] ExecutionSimulator make_sim() {
  return ExecutionSimulator{FillCfg{},       SlippageCfg{}, ImpactCfg{},
                            CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{}};
}

[[nodiscard]] std::vector<f64> make_grid(usize n) {
  std::vector<f64> g;
  g.reserve(n);
  f64 aum = 1.0e3;
  for (usize i = 0; i < n; ++i) {
    g.push_back(aum);
    aum *= 10.0;
  }
  return g;
}

// ---------------------------------------------------------------------------
//  Benches
// ---------------------------------------------------------------------------

void BM_CalibrateFromObs(benchmark::State& state) {
  const auto obs = make_obs(static_cast<usize>(state.range(0)));
  for (auto _ : state) {
    auto cc = cost::calibrate_from_obs(std::span<const cost::CostObs>{obs}, ImpactCfg{});
    f64 sink = cc.impact.delta; // mutable lvalue -> non-deprecated DoNotOptimize(T&)
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CalibrateFromObs)->Arg(500)->Arg(2000)->Arg(8000);

void BM_IrlsHuber(benchmark::State& state) {
  const Design d = make_design(static_cast<usize>(state.range(0)));
  for (auto _ : state) {
    const cost::RobustFit fit = cost::irls_huber(d.X, d.y, cost::RobustCfg{});
    f64 sink = fit.beta[1]; // mutable lvalue -> non-deprecated DoNotOptimize(T&)
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_IrlsHuber)->Arg(500)->Arg(2000)->Arg(8000);

void BM_CapacitySweep(benchmark::State& state) {
  const auto panel = make_panel(80, 2);
  const ExecutionSimulator sim = make_sim();
  const std::array<f64, 2> w{0.5, 0.5};
  const std::vector<f64> grid = make_grid(static_cast<usize>(state.range(0)));
  for (auto _ : state) {
    auto curve = cost::capacity_for_book(std::span<const f64>{w}, panel.view(), sim,
                                         std::span<const f64>{grid});
    benchmark::DoNotOptimize(curve.data());
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_CapacitySweep)->Arg(8)->Arg(32);

void BM_DailyBorrow(benchmark::State& state) {
  const std::array<InstrumentId, 1> uni{inst(1)};
  const std::array<InstrumentStats, 1> stats{InstrumentStats{}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{stats}};
  const std::array<SliceRow, 1> rows{SliceRow{
      inst(1),
      Bar{Timestamp::from_unix_nanos(1), Price::from_int(100), Price::from_int(100),
          Price::from_int(100), Price::from_int(100), Quantity::from_int(1'000'000)},
      false}};
  mkt.update_prices(MarketSlice{Timestamp::from_unix_nanos(1), std::span<const SliceRow>{rows}});

  Portfolio pf{Decimal::from_int(10'000'000), std::span<const InstrumentId>{uni}};
  pf.apply_fill(FillPayload{inst(1), /*qty=*/-20'000, Decimal::from_int(100), Decimal{}, 0.0,
                            Timestamp::from_unix_nanos(1)});
  const cost::BorrowModel b{/*annual_rate=*/0.05, cost::DayCount::D360};

  for (auto _ : state) {
    Decimal charge = cost::daily_borrow(b, pf, mkt, std::span<const InstrumentId>{uni});
    benchmark::DoNotOptimize(charge); // mutable lvalue -> non-deprecated DoNotOptimize(T&)
  }
}
BENCHMARK(BM_DailyBorrow);

} // namespace
