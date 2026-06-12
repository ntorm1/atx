// learn_bench.cpp — throughput micro-benchmark for the S5 learned-alpha kernels.
// MEASURED-ONLY: no correctness assertions, no production code. Built only under
// -DATX_BUILD_BENCH=ON (auto-globbed into atx-engine-bench, NOT the default test
// gate). Mirrors factory_bench.cpp's harness convention exactly.
//
// The learn units are COLD research paths (one full fit per call), so each case
// measures the END-TO-END per-FIT cost over a planted-edge FeatureMatrix built ONCE
// outside the measured loop; only the fit_* / kernel call is timed. The fixtures
// reuse the deterministic-LCG idiom of the S5 tests so the fits do real work (a
// genuine planted edge, not a degenerate all-zero design).
//
// Cases:
//   * BM_ElasticNetFit      — the Pattern-B edge-1 CD kernel, at two design sizes.
//   * BM_LinearFit          — fit_linear (per-horizon CPCV OOS walk + deploy refit).
//   * BM_GbtFit             — fit_gbt at {n_trees=50, 200} (the strictest cost).
//   * BM_HmmBaumWelch       — baum_welch EM (reports EM iters/sec via the cfg cap).
//   * BM_StackingFit        — fit_stack (linear benchmark + nonlinear GBT base).
//
// Build is Debug / clang-cl (the project default), so every figure is an UPPER
// BOUND on cost, not the optimised number. Short min-time keeps each case quick.

#include <cstdint> // std::uint64_t (the deterministic LCG state)
#include <limits>  // std::numeric_limits (tail NaN label)
#include <vector>  // std::vector (synthetic design / matrix storage)

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp" // f64, u16, u32, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig
#include "atx/engine/learn/elastic_net.hpp"    // ElasticNetCfg, elastic_net
#include "atx/engine/learn/ensemble.hpp"       // StackingCfg, fit_stack
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/gbt.hpp"            // GbtCfg, fit_gbt
#include "atx/engine/learn/hmm.hpp"            // HmmCfg, baum_welch
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation
#include "atx/engine/learn/linear_alpha.hpp"   // LinearAlphaCfg, fit_linear

namespace {

using atx::f64;
using atx::u16;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::learn::FeatureMatrix;
namespace eval = atx::engine::eval;
namespace learn = atx::engine::learn;

// Deterministic LCG -> uniform(-1, 1) (no RNG dep; the S5 fixture idiom).
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// A planted-edge (n x p) standardized-ish design + label y = 0.8*col0 + noise. The
// elastic-net CD kernel then does real work (a genuine partial-correlation sweep),
// not a degenerate all-zero fit.
struct DesignXY {
  atx::core::linalg::MatX X;
  atx::core::linalg::VecX y;
};

[[nodiscard]] DesignXY planted_design(usize n, usize p, std::uint64_t seed) {
  DesignXY d;
  d.X.resize(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(p));
  d.y.resize(static_cast<Eigen::Index>(n));
  Lcg rng{seed};
  for (usize i = 0; i < n; ++i) {
    f64 c0 = 0.0;
    for (usize j = 0; j < p; ++j) {
      const f64 v = rng.next();
      if (j == 0U) {
        c0 = v;
      }
      d.X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = v;
    }
    d.y(static_cast<Eigen::Index>(i)) = 0.8 * c0 + 0.05 * rng.next();
  }
  return d;
}

// A planted-edge FeatureMatrix (n_dates x n_inst, n_features), label = a linear
// blend of the first two columns + small noise (so fit_linear / fit_gbt find a real
// edge). Single horizon 1; the tail label is NaN (unknowable).
[[nodiscard]] FeatureMatrix planted_fm(usize n_dates, usize n_inst, usize n_features,
                                       std::uint64_t seed) {
  FeatureMatrix fm;
  fm.n_dates = n_dates;
  fm.n_instruments = n_inst;
  fm.n_features = n_features;
  fm.Y.assign(1U, {});
  Lcg rng{seed};
  std::vector<f64> c0_of_row;
  std::vector<f64> c1_of_row;
  std::vector<f64> noise_of_row;
  for (usize d = 0; d < n_dates; ++d) {
    for (usize i = 0; i < n_inst; ++i) {
      fm.push_row(d, i);
      f64 c0 = 0.0;
      f64 c1 = 0.0;
      for (usize f = 0; f < n_features; ++f) {
        const f64 v = rng.next();
        if (f == 0U) {
          c0 = v;
        } else if (f == 1U) {
          c1 = v;
        }
        fm.X.push_back(v);
      }
      c0_of_row.push_back(c0);
      c1_of_row.push_back(c1);
      noise_of_row.push_back(rng.next());
      fm.row_valid.push_back(1U);
    }
  }
  for (usize r = 0; r < fm.n_rows(); ++r) {
    const usize d = fm.row_date[r];
    if (d + 1U >= n_dates) {
      fm.Y[0].push_back(std::numeric_limits<f64>::quiet_NaN());
    } else {
      fm.Y[0].push_back(0.6 * c0_of_row[r] + 0.4 * c1_of_row[r] + 0.05 * noise_of_row[r]);
    }
  }
  return fm;
}

// A standard CPCV walk sized to the bench fixtures.
[[nodiscard]] eval::CpcvConfig bench_cpcv() {
  return eval::CpcvConfig{/*n_groups=*/5, /*n_test_groups=*/1, /*embargo=*/0.0};
}

// ===========================================================================
//  BM_ElasticNetFit — the Pattern-B edge-1 CD kernel at two design sizes.
//  state.range(0) == n rows, state.range(1) == p features. items_per_second is
//  fits/sec (one fit per iteration).
// ===========================================================================
void BM_ElasticNetFit(benchmark::State &state) {
  const usize n = static_cast<usize>(state.range(0));
  const usize p = static_cast<usize>(state.range(1));
  const DesignXY d = planted_design(n, p, /*seed=*/0xE1A5u);
  const learn::ElasticNetCfg cfg{/*lambda=*/0.05, /*alpha=*/0.5, /*max_iter=*/2000, /*tol=*/1e-9};
  for (auto _ : state) {
    atx::core::linalg::VecX beta = learn::elastic_net(d.X, d.y, cfg);
    benchmark::DoNotOptimize(beta);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ElasticNetFit)->Args({500, 8})->Args({2000, 16})->Unit(benchmark::kMicrosecond);

// ===========================================================================
//  BM_LinearFit — fit_linear (per-horizon CPCV OOS walk + deployed refit) over a
//  planted-edge FeatureMatrix. items_per_second is fits/sec.
// ===========================================================================
void BM_LinearFit(benchmark::State &state) {
  const FeatureMatrix fm = planted_fm(/*n_dates=*/60U, /*n_inst=*/16U, /*n_features=*/6U, 0x11Au);
  const learn::LatentAugmentation aug;
  learn::LinearAlphaCfg cfg;
  cfg.en = learn::ElasticNetCfg{/*lambda=*/0.05, /*alpha=*/0.5, /*max_iter=*/2000, /*tol=*/1e-9};
  cfg.use_ridge_baseline = false;
  cfg.cpcv = bench_cpcv();
  cfg.master_seed = 42ULL;
  cfg.horizons = {1};
  for (auto _ : state) {
    learn::LearnedModel m = learn::fit_linear(fm, aug, cfg);
    benchmark::DoNotOptimize(m);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LinearFit)->Unit(benchmark::kMillisecond);

// ===========================================================================
//  BM_GbtFit — fit_gbt at {n_trees=50, 200} over a planted-edge FeatureMatrix.
//  state.range(0) == n_trees. items_per_second is fits/sec; "trees_per_sec" is the
//  derived per-tree throughput.
// ===========================================================================
void BM_GbtFit(benchmark::State &state) {
  const u32 n_trees = static_cast<u32>(state.range(0));
  const FeatureMatrix fm = planted_fm(/*n_dates=*/60U, /*n_inst=*/16U, /*n_features=*/6U, 0x6B7u);
  const learn::LatentAugmentation aug;
  learn::GbtCfg cfg;
  cfg.n_trees = n_trees;
  cfg.cpcv = bench_cpcv();
  cfg.master_seed = 42ULL;
  cfg.horizons = {1};
  for (auto _ : state) {
    learn::LearnedModel m = learn::fit_gbt(fm, aug, cfg);
    benchmark::DoNotOptimize(m);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["trees_per_sec"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * static_cast<double>(n_trees),
      benchmark::Counter::kIsRate);
}
BENCHMARK(BM_GbtFit)->Arg(50)->Arg(200)->Unit(benchmark::kMillisecond);

// ===========================================================================
//  BM_HmmBaumWelch — baum_welch EM over a synthetic 1-D observation series.
//  state.range(0) == T (series length). items_per_second is fits/sec; "em_iters
//  _per_sec" the derived per-EM-iteration throughput (the cfg's iter cap times
//  fits/sec — an UPPER bound, since EM may converge before the cap).
// ===========================================================================
void BM_HmmBaumWelch(benchmark::State &state) {
  const usize T = static_cast<usize>(state.range(0));
  atx::core::linalg::MatX obs(static_cast<Eigen::Index>(T), 1);
  Lcg rng{0x4317u};
  for (usize t = 0; t < T; ++t) {
    // A two-regime-ish series: a slow square wave + noise, so EM has real structure.
    const f64 wave = ((t / 25U) % 2U == 0U) ? -1.0 : 1.0;
    obs(static_cast<Eigen::Index>(t), 0) = wave + 0.4 * rng.next();
  }
  learn::HmmCfg cfg;
  cfg.n_states = 2U;
  cfg.max_iter = 50U;
  cfg.master_seed = 7ULL;
  for (auto _ : state) {
    learn::Hmm h = learn::baum_welch(obs, cfg);
    benchmark::DoNotOptimize(h);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["em_iters_per_sec_max"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * static_cast<double>(cfg.max_iter),
      benchmark::Counter::kIsRate);
}
BENCHMARK(BM_HmmBaumWelch)->Arg(200)->Arg(800)->Unit(benchmark::kMillisecond);

// ===========================================================================
//  BM_StackingFit — fit_stack (linear benchmark + nonlinear GBT base on the SAME
//  meta) over a planted-edge meta-FeatureMatrix. items_per_second is stack-fits/sec
//  (the §0.4 gate's end-to-end cost — two models scored on shared folds).
// ===========================================================================
void BM_StackingFit(benchmark::State &state) {
  const FeatureMatrix meta = planted_fm(/*n_dates=*/48U, /*n_inst=*/14U, /*n_features=*/4U, 0x57Au);
  learn::StackingCfg cfg;
  cfg.base = learn::StackingCfg::Base::Gbt;
  cfg.cpcv = bench_cpcv();
  cfg.master_seed = 42ULL;
  cfg.horizons = {1};
  for (auto _ : state) {
    learn::StackingVerdict v = learn::fit_stack(meta, /*regime=*/nullptr, cfg);
    benchmark::DoNotOptimize(v);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StackingFit)->Unit(benchmark::kMillisecond);

} // namespace
