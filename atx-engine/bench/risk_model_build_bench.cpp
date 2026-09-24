// risk_model_build_bench.cpp — L7: full risk-model build at production shape.
//
// Shape: 3000 names × 14 styles × 60 industries (+ market) × 504 days, per-date
// (time-varying) style exposures, √cap weights, planted 3-factor latent structure.
//
//   * BM_L7FactorReturns    — estimate_factor_returns over the 504-day window (the
//                             structured O(N·Ks²) per-date WLS with the exact
//                             industry sum-to-zero constraint).
//   * BM_L7ModelBuildFund   — HybridFactorModelBuilder::build, K_s fixed at 0 (fast
//                             fundamental model: regressions + LW F + specific D).
//   * BM_L7ModelBuildHybrid — the same + Bai-Ng K_s selection + 2-pass APCA block.
//   * BM_L7DenseWlsOneDate  — reference: one date's DENSE WLS (N×75 design, the
//                             unstructured path) to put the per-date cost in context.
//
// Items processed = dates regressed. Synthetic data is seeded Xoshiro256pp.

#include <cmath>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/linalg/regression.hpp" // wls (dense reference)
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/hybrid_factor_model.hpp"

namespace atx_bench_l7_riskmodel {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using namespace atx::engine::risk; // NOLINT(google-build-using-namespace) bench-local

constexpr usize kN = 3000U;
constexpr usize kKs = 14U;
constexpr usize kG = 60U;
constexpr usize kT = 504U;

struct World {
  ReturnPanel ret;
  ExposureSeries exp;
};

const World &world() {
  static const World w = [] {
    atx::core::Xoshiro256pp rng{2026U};
    World out;
    out.exp.market = true;
    out.exp.n_industries = static_cast<u32>(kG);
    const MatX base = MatX::NullaryExpr(static_cast<Eigen::Index>(kN),
                                        static_cast<Eigen::Index>(kKs),
                                        [&rng]() { return rng.normal(); });
    MatX latent(static_cast<Eigen::Index>(kN), 3);
    for (usize i = 0; i < kN; ++i) {
      out.exp.industry.push_back(static_cast<u32>(i % kG));
      out.exp.cap.push_back(1e9 * std::exp(1.5 * rng.normal()));
      for (Eigen::Index c = 0; c < 3; ++c) {
        latent(static_cast<Eigen::Index>(i), c) = rng.normal();
      }
    }
    // Per-date exposures: a slowly drifting copy of the base (rows 0..kT).
    out.exp.style.reserve(kT + 1U);
    MatX cur = base;
    for (usize t = 0; t <= kT; ++t) {
      for (Eigen::Index c = 0; c < cur.cols(); ++c) {
        for (Eigen::Index i = 0; i < cur.rows(); i += 37) { // sparse drift, cheap setup
          cur(i, c) += 0.05 * rng.normal();
        }
      }
      out.exp.style.push_back(cur);
    }
    out.ret.r.resize(static_cast<Eigen::Index>(kT), static_cast<Eigen::Index>(kN));
    VecX f(static_cast<Eigen::Index>(1U + kG + kKs));
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(kT); ++t) {
      for (Eigen::Index c = 0; c < f.size(); ++c) {
        f[c] = 0.005 * rng.normal();
      }
      const f64 h0 = 0.01 * rng.normal();
      const f64 h1 = 0.008 * rng.normal();
      const f64 h2 = 0.006 * rng.normal();
      const MatX &x = out.exp.style[static_cast<usize>(t) + 1U];
      for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(kN); ++i) {
        f64 r = f[0] + f[1 + static_cast<Eigen::Index>(out.exp.industry[static_cast<usize>(i)])];
        for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(kKs); ++c) {
          r += x(i, c) * f[1 + static_cast<Eigen::Index>(kG) + c];
        }
        r += latent(i, 0) * h0 + latent(i, 1) * h1 + latent(i, 2) * h2 + 0.02 * rng.normal();
        out.ret.r(t, i) = r;
      }
    }
    return out;
  }();
  return w;
}

void BM_L7FactorReturns(benchmark::State &state) {
  const World &w = world();
  for (auto _ : state) {
    auto fr = estimate_factor_returns(w.ret, w.exp, 0U, kT - 1U);
    benchmark::DoNotOptimize(fr);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kT - 1U));
}
BENCHMARK(BM_L7FactorReturns)->Unit(benchmark::kMillisecond);

void run_build(benchmark::State &state, StatFactorSelect sel) {
  const World &w = world();
  HybridCfg cfg;
  cfg.window = kT - 1U;
  cfg.select = sel;
  cfg.n_stat_fixed = 0U;
  usize ks = 0U;
  for (auto _ : state) {
    auto hm = HybridFactorModelBuilder::build(w.ret, w.exp, cfg, 0U);
    if (!hm) {
      state.SkipWithError(hm.error().message().c_str());
      return;
    }
    ks = hm->k_stat;
    benchmark::DoNotOptimize(hm);
  }
  state.counters["k_stat"] = static_cast<f64>(ks);
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kT - 1U));
}

void BM_L7ModelBuildFund(benchmark::State &state) { run_build(state, StatFactorSelect::Fixed); }
BENCHMARK(BM_L7ModelBuildFund)->Unit(benchmark::kMillisecond);

void BM_L7ModelBuildHybrid(benchmark::State &state) {
  run_build(state, StatFactorSelect::BaiNgIc2);
}
BENCHMARK(BM_L7ModelBuildHybrid)->Unit(benchmark::kMillisecond);

void BM_L7DenseWlsOneDate(benchmark::State &state) {
  const World &w = world();
  const Eigen::Index k = static_cast<Eigen::Index>(kG + kKs); // industries + styles
  MatX x = MatX::Zero(static_cast<Eigen::Index>(kN), k);
  VecX wt(static_cast<Eigen::Index>(kN));
  for (usize i = 0; i < kN; ++i) {
    const Eigen::Index ii = static_cast<Eigen::Index>(i);
    x(ii, static_cast<Eigen::Index>(w.exp.industry[i])) = 1.0;
    x.row(ii).tail(static_cast<Eigen::Index>(kKs)) = w.exp.style[1].row(ii);
    wt[ii] = std::sqrt(w.exp.cap[i]);
  }
  const VecX y = w.ret.r.row(0).transpose();
  for (auto _ : state) {
    auto fit = atx::core::linalg::wls(x, y, wt);
    benchmark::DoNotOptimize(fit);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L7DenseWlsOneDate)->Unit(benchmark::kMillisecond);

} // namespace atx_bench_l7_riskmodel
