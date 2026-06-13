// book_bench.cpp — S7-5 operating-book micro-benchmark (the capstone bench half).
// MEASURED-ONLY: no correctness assertions, no production code. Mirrors
// research_bench.cpp / learn_bench.cpp's Google Benchmark form (BENCHMARK macros;
// the bench/CMakeLists globs *_bench.cpp into atx-engine-bench — no CMake edit).
//
// Three headline measurements over the S7 carriers the BookPipeline composes:
//
//   BM_MultiPeriodRebalance — the receding-horizon driver (risk::MultiPeriodOptimizer
//     ::run) across schedule length {64, 256} x universe {500, 2000}. The factor model
//     + alpha rows are built ONCE in the fixture; only run() is timed.
//
//   BM_DeadFactorExtract — risk::extract_dead_factors over a real Library of planted
//     dead alphas, swept over |dead| {100, 1000}. The library is built ONCE per size
//     outside the timed loop; only the eigen-extraction is timed.
//
//   BM_AccumulateReport — book::accumulate_report throughput rolling a realized book
//     chain into a BookReport (the report path the pipeline closes on).
//
// Build is Debug / clang-cl (the project default), so every figure is an UPPER BOUND.
// A short min-time / iteration cap keeps each Debug case well under a second.

#include <cmath>        // std::cos
#include <cstdint>      // fixed-width
#include <filesystem>   // per-fixture temp dir for the dead-factor library
#include <numbers>      // std::numbers::pi
#include <span>
#include <string>
#include <system_error> // std::error_code
#include <utility>      // std::move
#include <vector>

#include <benchmark/benchmark.h>

#include <Eigen/Core> // Eigen::Index

#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"          // f64, u32, u64, usize

#include "atx/engine/alpha/panel.hpp" // alpha::Panel, FieldId

#include "atx/engine/combine/gate.hpp"    // combine::AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaId

#include "atx/engine/library/library.hpp"   // library::Library, AlphaCandidate, AdmitKind
#include "atx/engine/library/lifecycle.hpp" // library::LifecycleState
#include "atx/engine/library/record.hpp"    // library::Provenance

#include "atx/engine/book/report.hpp"       // book::accumulate_report, BookReport, CostInputs
#include "atx/engine/risk/dead_factor.hpp"  // risk::extract_dead_factors
#include "atx/engine/risk/factor_model.hpp" // risk::FactorModel
#include "atx/engine/risk/multi_period.hpp" // risk::MultiPeriodOptimizer, RebalanceSchedule

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::GateConfig;

namespace lib = atx::engine::library;
namespace risk = atx::engine::risk;
namespace book = atx::engine::book;

// A K=2 SPD factor model over M instruments: X is a deterministic dense exposure block,
// F a small SPD 2x2, D a positive specific-variance vector. FactorModel::create re-checks
// SPD via its Cholesky, so this is a genuine factored covariance.
[[nodiscard]] risk::FactorModel make_factor_model(usize m) {
  const Eigen::Index mm = static_cast<Eigen::Index>(m);
  MatX x(mm, 2);
  for (Eigen::Index i = 0; i < mm; ++i) {
    x(i, 0) = 1.0;                                                  // a market column
    x(i, 1) = (static_cast<f64>(i) / static_cast<f64>(m)) - 0.5;    // a centered style column
  }
  MatX f(2, 2);
  f << 0.04, 0.005, 0.005, 0.02; // SPD
  VecX d(mm);
  for (Eigen::Index i = 0; i < mm; ++i) {
    d[i] = 0.01 + 0.001 * static_cast<f64>(i % 7);
  }
  auto fm = risk::FactorModel::create(std::move(x), std::move(f), std::move(d),
                                      /*fit_begin*/ 0U, /*fit_end*/ 64U);
  return std::move(fm.value()); // bench fixture: inputs are valid by construction
}

// A deterministic alpha cross-section of length m (a mild centered signal, NaN-free).
[[nodiscard]] std::vector<f64> make_alpha(usize m, usize period) {
  std::vector<f64> a(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    const f64 phase = static_cast<f64>(i + period) / static_cast<f64>(m);
    a[i] = std::cos(2.0 * std::numbers::pi * phase) * 0.01;
  }
  return a;
}

// ---------------------------------------------------------------------------
//  BM_MultiPeriodRebalance — schedule length {64, 256} x universe {500, 2000}.
// ---------------------------------------------------------------------------
void BM_MultiPeriodRebalance(benchmark::State &state) {
  const usize sched_len = static_cast<usize>(state.range(0));
  const usize universe = static_cast<usize>(state.range(1));

  // Fixture (built ONCE): the factor model, the alpha rows, the schedule, the cost.
  const risk::FactorModel V = make_factor_model(universe);
  std::vector<std::vector<f64>> alpha_rows(sched_len);
  for (usize s = 0; s < sched_len; ++s) {
    alpha_rows[s] = make_alpha(universe, s);
  }
  risk::RebalanceSchedule sched;
  sched.periods.reserve(sched_len);
  for (usize s = 0; s < sched_len; ++s) {
    sched.periods.push_back(s);
  }
  const book::CostInputs cost{/*kappa*/ 0.001, /*round_trip_cost_bps*/ 2.0,
                              /*capacity_gross*/ 3.0};
  risk::MultiPeriodConfig cfg;
  cfg.single.gross_leverage = 2.0;
  cfg.single.max_iters = 32;
  cfg.trade_rate = 1.0;
  const risk::MultiPeriodOptimizer opt{cfg};

  const auto alpha_at = [&alpha_rows](usize s) -> std::span<const f64> {
    return std::span<const f64>{alpha_rows[s]};
  };
  const auto model_at = [&V](usize) -> const risk::FactorModel & { return V; };

  for (auto _ : state) {
    auto r = opt.run(sched, alpha_at, model_at, cost);
    benchmark::DoNotOptimize(r);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(sched_len));
}
BENCHMARK(BM_MultiPeriodRebalance)
    ->Args({64, 500})
    ->Args({256, 500})
    ->Args({64, 2000})
    ->Args({256, 2000})
    ->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
//  BM_DeadFactorExtract — |dead| {100, 1000}.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string bench_tmpdir(const std::string &tag) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s7_book_bench" / tag;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  (void)ec;
  std::filesystem::create_directories(dir, ec);
  (void)ec;
  return dir.string();
}

[[nodiscard]] GateConfig permissive_gate() {
  GateConfig cfg;
  cfg.min_sharpe = -1e9;
  cfg.min_fitness = -1e9;
  cfg.max_turnover = 1e9;
  cfg.max_pool_corr = 1.1;
  return cfg;
}

void BM_DeadFactorExtract(benchmark::State &state) {
  const usize n_dead = static_cast<usize>(state.range(0));
  constexpr usize kM = 64U; // a fixed instrument universe for the overlap matrix
  constexpr usize kT = 2U;  // holdings at period 1 carry the dead direction

  lib::Library library =
      lib::Library::open(bench_tmpdir(std::to_string(n_dead)), permissive_gate(), {4242ULL});
  const AlphaGate gate{permissive_gate()};

  // Build ONCE outside the timed loop: admit n_dead alphas, walk each to Dead.
  struct Owner {
    std::vector<f64> pnl;
    std::vector<f64> pos_flat;
  };
  std::vector<Owner> owners(n_dead);
  std::vector<AlphaId> ids;
  ids.reserve(n_dead);
  for (usize k = 0; k < n_dead; ++k) {
    Owner &o = owners[k];
    o.pnl.assign(kT, 0.0);
    o.pnl[1] = 0.01 + 0.0001 * static_cast<f64>(k);
    o.pos_flat.assign(kT * kM, 0.0);
    const f64 center = static_cast<f64>(k % kM);
    for (usize i = 0; i < kM; ++i) {
      const f64 d = (static_cast<f64>(i) - center) / static_cast<f64>(kM);
      o.pos_flat[1 * kM + i] = std::cos(std::numbers::pi * d);
    }
    AlphaMetrics m{};
    m.sharpe = 5.0;
    m.turnover = 0.05;
    m.returns = 1.0;
    m.drawdown = 0.1;
    m.margin = 10.0;
    m.fitness = 5.0;
    m.holding_days = 20.0;
    const lib::Provenance prov{"dead", std::vector<u64>{}, /*op*/ 0, /*seed*/ 100 + k};
    const lib::AlphaCandidate cand{/*canon_hash*/ 0x100ULL + k, o.pnl,  o.pos_flat,
                                   m,                           prov,   /*as_of*/ 0U,
                                   /*source*/ nullptr};
    const auto v = library.admit(cand, gate);
    ids.push_back(v.id);
  }
  for (const AlphaId id : ids) {
    (void)library.mark(id, lib::LifecycleState::Live, 2U);
    (void)library.mark(id, lib::LifecycleState::Decaying, 3U);
    (void)library.mark(id, lib::LifecycleState::Dead, 4U);
  }

  const std::span<const AlphaId> dead{ids};
  for (auto _ : state) {
    auto df = risk::extract_dead_factors(library, dead, /*as_of*/ 1U, kM);
    benchmark::DoNotOptimize(df);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(n_dead));
}
BENCHMARK(BM_DeadFactorExtract)->Arg(100)->Arg(1000)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
//  BM_AccumulateReport — report-roll throughput over a realized book chain.
// ---------------------------------------------------------------------------
void BM_AccumulateReport(benchmark::State &state) {
  const usize sched_len = static_cast<usize>(state.range(0));
  constexpr usize kM = 500U;
  const usize panel_dates = sched_len + 8U;

  // A minimal real Panel carrying the returns field the report reads.
  std::vector<f64> rev(panel_dates * kM);
  for (usize t = 0; t < panel_dates; ++t) {
    for (usize i = 0; i < kM; ++i) {
      rev[t * kM + i] = 0.001 * std::cos(static_cast<f64>(t + i));
    }
  }
  auto panel_r = atx::engine::alpha::Panel::create(panel_dates, kM, {"rev"}, {rev}, {});
  const atx::engine::alpha::Panel panel = std::move(panel_r.value());
  const atx::engine::alpha::FieldId rev_id = panel.field_id("rev").value();

  const risk::FactorModel V = make_factor_model(kM);

  // A realized book chain (books / turnover / cost_bps), one entry per schedule period.
  risk::MultiPeriodResult books;
  risk::RebalanceSchedule sched;
  for (usize s = 0; s < sched_len; ++s) {
    std::vector<f64> book(kM);
    for (usize i = 0; i < kM; ++i) {
      book[i] = make_alpha(kM, s)[i];
    }
    books.books.push_back(std::move(book));
    books.turnover.push_back(0.05);
    books.cost_bps.push_back(1.0);
    sched.periods.push_back(s);
  }

  // A tiny real Library for the lifecycle census (empty store is fine — census reads 0s).
  lib::Library library =
      lib::Library::open(bench_tmpdir("report_" + std::to_string(sched_len)), GateConfig{}, {1ULL});

  for (auto _ : state) {
    auto rep = book::accumulate_report(books, panel, rev_id, sched, V,
                                       /*capacity_gross*/ 3.0, library, /*as_of*/ 0U);
    benchmark::DoNotOptimize(rep);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(sched_len));
}
BENCHMARK(BM_AccumulateReport)->Arg(64)->Arg(256)->Unit(benchmark::kMillisecond);

} // namespace
