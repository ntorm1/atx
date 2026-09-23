// combine_bench.cpp — Lane 5 signal-space combiner benchmarks. MEASURED-ONLY.
//
//   BM_KakushadzeKernel/N      kakushadze_weights on an M=252 × N alpha-return matrix
//                              (acceptance: N=2000 < 50 ms Release).
//   BM_GrinoldKahnKernel/N     grinold_kahn_weights on a 252 × N IC matrix, LW2004 target.
//   BM_HrpWeights/N            hrp_weights on an N×N covariance.
//   BM_Lw2020/N                shrink_nonlinear_lw2020 on a 252 × N sample (c>1 at N>=500).
//   BM_StoreFit<Method>/K/Ni   end-to-end fit from a SignalStore (T=252 dates) incl. the
//                              per-date IC / alpha-return / FMB cross-section passes.
//
// Fixtures are built once outside the timed loop from a fixed LCG (no global RNG).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/engine/combine/cov_targets.hpp"
#include "atx/engine/combine/hrp.hpp"
#include "atx/engine/combine/signal_combiner.hpp"
#include "atx/engine/combine/signal_store.hpp"

namespace atx_bench_l5_combine {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
namespace cb = atx::engine::combine;

struct Lcg {
  std::uint64_t s;
  f64 gauss() {
    const auto uni = [this]() {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      return (static_cast<f64>(s >> 11U) + 0.5) / 9007199254740992.0;
    };
    const f64 u1 = uni();
    const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

// M × N returns with a common factor and per-alpha drift.
MatX alpha_returns(Eigen::Index m, Eigen::Index n, std::uint64_t seed) {
  Lcg g{seed};
  MatX r(m, n);
  for (Eigen::Index t = 0; t < m; ++t) {
    const f64 common = g.gauss();
    for (Eigen::Index a = 0; a < n; ++a) {
      r(t, a) = 0.001 * static_cast<f64>(a % 7) + 0.3 * common + g.gauss();
    }
  }
  return r;
}

void BM_KakushadzeKernel(benchmark::State &state) {
  const auto n = static_cast<Eigen::Index>(state.range(0));
  const MatX r = alpha_returns(252, n, 1U);
  std::vector<f64> e(static_cast<usize>(n));
  for (Eigen::Index a = 0; a < n; ++a) {
    e[static_cast<usize>(a)] = r.col(a).tail(21).mean();
  }
  for (auto _ : state) {
    auto w = cb::kakushadze_weights(r, e, {}, 1e-8);
    benchmark::DoNotOptimize(w);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_KakushadzeKernel)->Arg(100)->Arg(500)->Arg(2000)->Unit(benchmark::kMillisecond);

void BM_GrinoldKahnKernel(benchmark::State &state) {
  const auto n = static_cast<Eigen::Index>(state.range(0));
  const MatX ic = 0.02 * alpha_returns(252, n, 2U);
  for (auto _ : state) {
    auto w = cb::grinold_kahn_weights(ic, cb::CovTarget::LwIdentity);
    benchmark::DoNotOptimize(w);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GrinoldKahnKernel)->Arg(100)->Arg(500)->Arg(2000)->Unit(benchmark::kMillisecond);

void BM_HrpWeights(benchmark::State &state) {
  const auto n = static_cast<Eigen::Index>(state.range(0));
  const auto cov = cb::shrink_lw_identity(alpha_returns(252, n, 3U));
  for (auto _ : state) {
    auto w = cb::hrp_weights(cov->sigma);
    benchmark::DoNotOptimize(w);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HrpWeights)->Arg(100)->Arg(500)->Arg(2000)->Unit(benchmark::kMillisecond);

void BM_Lw2020(benchmark::State &state) {
  const auto n = static_cast<Eigen::Index>(state.range(0));
  const MatX x = alpha_returns(252, n, 4U);
  for (auto _ : state) {
    auto s = cb::shrink_nonlinear_lw2020(x);
    benchmark::DoNotOptimize(s);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Lw2020)->Arg(100)->Arg(500)->Arg(2000)->Unit(benchmark::kMillisecond);

// Shared store fixture per (K, Ni), built lazily once.
const cb::SignalStore &store_fixture(usize k, usize ni) {
  static std::vector<std::pair<std::pair<usize, usize>, std::unique_ptr<cb::SignalStore>>> cache;
  for (const auto &e : cache) {
    if (e.first.first == k && e.first.second == ni) {
      return *e.second;
    }
  }
  constexpr usize kT = 252;
  auto st = cb::SignalStore::create(kT, ni);
  Lcg g{static_cast<std::uint64_t>(k * 131U + ni)};
  std::vector<f64> latent(kT * ni);
  for (f64 &x : latent) {
    x = g.gauss();
  }
  std::vector<f64> panel(kT * ni);
  for (usize a = 0U; a < k; ++a) {
    for (usize c = 0U; c < panel.size(); ++c) {
      panel[c] = ((a % 4U) == 0U ? latent[c] : 0.0) + g.gauss();
    }
    if (!st->add_signal(panel).has_value()) {
      std::abort();
    }
  }
  std::vector<f64> fwd(kT * ni);
  for (usize c = 0U; c < fwd.size(); ++c) {
    fwd[c] = 0.02 * latent[c] + g.gauss();
  }
  if (!st->set_forward_returns(fwd).has_value()) {
    std::abort();
  }
  cache.emplace_back(std::make_pair(k, ni), std::make_unique<cb::SignalStore>(std::move(*st)));
  return *cache.back().second;
}

template <class C> void BM_StoreFit(benchmark::State &state) {
  const auto k = static_cast<usize>(state.range(0));
  const auto ni = static_cast<usize>(state.range(1));
  const cb::SignalStore &st = store_fixture(k, ni);
  const C c{};
  for (auto _ : state) {
    auto w = c.fit(st, cb::FitWindow{0, 252});
    benchmark::DoNotOptimize(w);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StoreFit<cb::IcirEwmaCombiner>)->Args({100, 500})->Args({500, 100})->Unit(benchmark::kMillisecond);
BENCHMARK(BM_StoreFit<cb::GrinoldKahnCombiner>)->Args({100, 500})->Args({500, 100})->Unit(benchmark::kMillisecond);
BENCHMARK(BM_StoreFit<cb::KakushadzeRegression>)->Args({100, 500})->Args({500, 100})->Unit(benchmark::kMillisecond);
BENCHMARK(BM_StoreFit<cb::HrpCombiner>)->Args({100, 500})->Args({500, 100})->Unit(benchmark::kMillisecond);
BENCHMARK(BM_StoreFit<cb::FamaMacBethRidge>)->Args({100, 500})->Unit(benchmark::kMillisecond);

} // namespace atx_bench_l5_combine
