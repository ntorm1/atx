// meta_book_bench.cpp — P2-S2-5: the meta-book driver compute profile.
//
// Micro-benchmarks the S2 capstone driver (fund::MetaBook::run) — the two-pass walk that
// runs S sleeves independently (PASS 1), then per period builds the trailing sleeve-return
// Ω, allocates capital, and nets the sleeve books into one fund book (PASS 2). Reports
// funds/sec (one "fund" == one full run() over the schedule) via SetItemsProcessed across
// a grid of (S sleeves, N universe).
//
//   BM_MetaBookRun — sweeps S ∈ {2,4,8} sleeves × N ∈ {200,500,1000} universe over a
//   fixed 8-period schedule. Each sleeve uses the MINIMAL-constraint dispatch (GrossNet +
//   PositionCap ⇒ the as-built PortfolioOptimizer inner solve, no ADMM) so the cost is
//   dominated by the S sleeve walks + the S×S trailing-Ω build + the netting, NOT the QP.
//
// PASS-1 cost scales ~ S·(per-sleeve MultiHorizon walk); the PASS-2 overlay adds the
// trailing covariance (O(S²·lookback)) + the allocator (O(S²) per period) + the netting
// (O(S·N) per period) — all cheap next to the sleeve solves, which is the point: the meta
// overlay is a thin coordination layer over the reused S1 driver. The `sleeves` counter
// makes S visible in the table.
//
// Build is Debug / clang-cl (project default), so EVERY number is an UPPER-BOUND latency,
// NOT a release figure. Mirrors multi_horizon_bench.cpp (no BENCHMARK_MAIN — it lives in
// bench_main.cpp). Synthetic data is a seeded Xoshiro256pp (bench scaffold only; the driver
// is RNG-free and order-fixed).

#include <span>
#include <vector>

#include <benchmark/benchmark.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/random.hpp"        // Xoshiro256pp (synthetic data)
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/fund/meta_book.hpp"     // MetaBook, MetaBookConfig, MetaAllocatorConfig
#include "atx/engine/fund/sleeve.hpp"        // Sleeve, SleeveConfig
#include "atx/engine/risk/factor_model.hpp"  // FactorModel
#include "atx/engine/risk/horizon.hpp"       // SignalHorizon
#include "atx/engine/risk/multi_horizon.hpp" // MultiHorizonConfig, HorizonSources
#include "atx/engine/risk/multi_period.hpp"  // RebalanceSchedule, book::CostInputs

namespace {

using atx::f64;
using atx::usize;
using atx::core::Xoshiro256pp;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::book::CostInputs;
using atx::engine::fund::MetaAllocatorConfig;
using atx::engine::fund::MetaBook;
using atx::engine::fund::RiskBudgetMethod;
using atx::engine::fund::Sleeve;
using atx::engine::fund::SleeveConfig;
using atx::engine::risk::FactorModel;
using atx::engine::risk::HorizonSources;
using atx::engine::risk::MultiHorizonConfig;
using atx::engine::risk::PositionCap;
using atx::engine::risk::RebalanceSchedule;
using atx::engine::risk::SignalHorizon;

constexpr usize kK = 5U; // factors (modest, fixed)
constexpr usize kT = 8U; // schedule periods

// An N-instrument, K-factor FactorModel. Seeded; F = I (SPD), D > 0. Valid by construction.
[[nodiscard]] FactorModel make_model(usize n, atx::u64 seed) {
  Xoshiro256pp rng{seed};
  MatX x(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(kK));
  for (usize i = 0; i < n; ++i) {
    for (usize c = 0; c < kK; ++c) {
      x(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(c)) = 0.1 * rng.normal();
    }
  }
  MatX f = MatX::Identity(static_cast<Eigen::Index>(kK), static_cast<Eigen::Index>(kK));
  VecX d = VecX::Constant(static_cast<Eigen::Index>(n), 0.2);
  auto m = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  return std::move(*m);
}

// A seeded dollar-neutral-able alpha cross-section (length N).
[[nodiscard]] std::vector<f64> make_alpha(usize n, atx::u64 seed) {
  Xoshiro256pp rng{seed};
  std::vector<f64> a(n);
  for (usize i = 0; i < n; ++i) {
    a[i] = rng.normal();
  }
  return a;
}

// Minimal MH config (GrossNet + PositionCap ⇒ the dispatch path, no ADMM).
[[nodiscard]] MultiHorizonConfig minimal_cfg() {
  MultiHorizonConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.constraints.gross.gross_leverage = 1.0;
  cfg.constraints.gross.dollar_neutral = true;
  cfg.constraints.pos = PositionCap{1.0};
  cfg.horizon = 1U;
  cfg.trade_rate = 1.0;
  cfg.prox_max_iters = 64U;
  cfg.capacity_bound_gross = true;
  return cfg;
}

// The whole fixture for one (S, N) point: per-period models, per-sleeve alpha series, and
// a per-period returns series — all owned here so the spans outlive run().
struct Fixture {
  std::vector<FactorModel> models;                  // [period]
  std::vector<std::vector<std::vector<f64>>> alpha; // [sleeve][period] -> length N
  std::vector<std::vector<f64>> returns;            // [period] -> length N
  MetaBook mb;
};

[[nodiscard]] Fixture make_fixture(usize s_sleeves, usize n) {
  Fixture fx;
  for (usize p = 0; p < kT; ++p) {
    fx.models.push_back(make_model(n, 0x1000ULL + p));
  }
  fx.alpha.resize(s_sleeves);
  for (usize j = 0; j < s_sleeves; ++j) {
    fx.alpha[j].resize(kT);
    for (usize p = 0; p < kT; ++p) {
      fx.alpha[j][p] = make_alpha(n, 0x9000ULL + j * 31U + p);
    }
  }
  for (usize p = 0; p < kT; ++p) {
    fx.returns.push_back(make_alpha(n, 0xA000ULL + p)); // a nonzero per-instrument return
  }

  MetaAllocatorConfig alloc;
  alloc.method = RiskBudgetMethod::EqualRiskContribution;
  alloc.fractional_kelly = 0.5;
  alloc.max_gross = 4.0;
  alloc.solve_iters = 64U;
  fx.mb.cfg.alloc = alloc;
  fx.mb.cfg.risk_lookback = 60U;
  for (usize j = 0; j < s_sleeves; ++j) {
    SleeveConfig sc;
    sc.mh = minimal_cfg();
    sc.capacity_gross = 1e9;
    fx.mb.sleeves.push_back(Sleeve{sc});
  }
  return fx;
}

// ---------------------------------------------------------------------------
//  BM_MetaBookRun — funds/sec across (S sleeves, N universe).
// ---------------------------------------------------------------------------
void BM_MetaBookRun(benchmark::State &state) {
  const auto s_sleeves = static_cast<usize>(state.range(0));
  const auto n = static_cast<usize>(state.range(1));
  Fixture fx = make_fixture(s_sleeves, n);

  const RebalanceSchedule sched{[] {
    std::vector<usize> v(kT);
    for (usize p = 0; p < kT; ++p) {
      v[p] = p;
    }
    return v;
  }()};
  const CostInputs cost{/*kappa=*/0.0, /*round_trip_cost_bps=*/7.5, /*capacity_gross=*/1e9};

  const auto sources_at = [&](usize sleeve, usize period) {
    HorizonSources hs;
    hs.pairs.emplace_back(std::span<const f64>(fx.alpha[sleeve][period]), SignalHorizon::identity());
    return hs;
  };
  const auto model_at = [&](usize period) -> const FactorModel & { return fx.models[period]; };
  const auto returns_at = [&](usize period) {
    return std::span<const f64>(fx.returns[period]);
  };

  for (auto _ : state) {
    auto r = fx.mb.run(sched, sources_at, model_at, returns_at, cost);
    benchmark::DoNotOptimize(r);
  }
  // One "fund" processed per iteration (a full run() over the schedule).
  state.SetItemsProcessed(state.iterations());
  state.counters["sleeves"] = static_cast<double>(s_sleeves);
  state.counters["universe"] = static_cast<double>(n);
}
BENCHMARK(BM_MetaBookRun)
    ->Args({2, 200})
    ->Args({4, 200})
    ->Args({8, 200})
    ->Args({2, 500})
    ->Args({4, 500})
    ->Args({2, 1000})
    ->Args({4, 1000})
    ->Unit(benchmark::kMillisecond);

} // namespace
