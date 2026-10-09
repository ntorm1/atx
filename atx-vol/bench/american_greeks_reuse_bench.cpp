// american_greeks_reuse_bench.cpp — the FD-boundary-reuse A/B, as a benchmark.
//
// Relocated from TEST(AmericanGreeks, DISABLED_FdBoundaryReuse_Speedup) in
// tests/american_test.cpp, which timed four American-put-greeks implementations
// over a fixed 40-point grid and asserted an EXPECT_GT ordering on elapsed
// milliseconds — a flaky A/B micro-benchmark, not a correctness check.
//
// Here each of the four measured code paths is its own benchmark case over the
// SAME grid, so Google Benchmark's ns/op and items/s (greeks computed per second)
// make the speedup comparison directly instead of a brittle wall-clock EXPECT_GT:
//
//   american_greeks/fd_ref        — the pre-P1a 17-solve cold FD reference
//   american_greeks/fd_fast       — american_greeks_fd, 7 cold boundary solves
//   american_greeks/fd_warm       — american_greeks_fd, 6 bumped boundaries warm-seeded
//   american_greeks/andersen_lake — american_greeks_al, the ~5-solve analytic path
//
// The former test's ordering (ref > fast > warm, fast > al) is now read straight
// off the per-case rates. Setup (grid, opts) lives outside the timed loop; each
// case reports items_processed = iterations x grid_points.

#include <cstdint>
#include <optional>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp" // american_put_greeks_batch, SimdIsa
#include "atx/vol/api/simd/cpu.hpp"                      // have_avx2
#include "atx/vol/api/core/types.hpp"

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

// The base contract and the 40-point put grid the relocated A/B swept:
// 5 strikes x 4 maturities x 2 vols. S/r/q are fixed across the grid.
constexpr double kS = 100.0;
constexpr double kR = 0.05;
constexpr double kQ = 0.03;

struct Pt {
  double K;
  double T;
  double sigma;
};

[[nodiscard]] std::vector<Pt> make_grid() {
  std::vector<Pt> grid;
  for (const double K : {70.0, 85.0, 100.0, 115.0, 130.0}) {
    for (const double T : {0.05, 0.25, 0.75, 1.5}) {
      for (const double sigma : {0.15, 0.30}) {
        grid.push_back({K, T, sigma});
      }
    }
  }
  return grid;
}

// The pre-P1a algorithm, reproduced verbatim from the former test so fd_ref
// measures the true 17-stencil cold cost (every stencil a full cold
// american_price). A failed price reads 0.0 — the grid is all valid American
// puts, so this branch is never taken; it only keeps the path total-function.
[[nodiscard]] AmericanGreeks greeks_fd_reference(double S, double K, double T,
                                                 double sigma, double r, double q,
                                                 Side side) {
  const double hS = 1.0e-3 * S;
  double hv = 1.0e-3;
  if (sigma - hv <= 0.0) {
    hv = 0.5 * sigma;
  }
  const double hr = 1.0e-4;
  const double hT = 1.0e-3;
  const bool near_expiry = (T - hT <= 1.0e-8);
  auto P = [&](double dS, double dsig, double dr, double dT) {
    const auto p = american_price(S + dS, K, T + dT, sigma + dsig, r + dr, q, side,
                                  AmericanMethod::AndersenLake, std::nullopt);
    return p ? *p : 0.0;
  };
  const double p0 = P(0, 0, 0, 0);
  const double p_Sp = P(+hS, 0, 0, 0);
  const double p_Sm = P(-hS, 0, 0, 0);
  const double p_vp = P(0, +hv, 0, 0);
  const double p_vm = P(0, -hv, 0, 0);
  const double p_rp = P(0, 0, +hr, 0);
  const double p_rm = P(0, 0, -hr, 0);
  const double p_Tp = P(0, 0, 0, +hT);
  const double p_Tm = near_expiry ? p0 : P(0, 0, 0, -hT);
  const double p_SpVp = P(+hS, +hv, 0, 0);
  const double p_SpVm = P(+hS, -hv, 0, 0);
  const double p_SmVp = P(-hS, +hv, 0, 0);
  const double p_SmVm = P(-hS, -hv, 0, 0);
  const double p_SpTp = P(+hS, 0, 0, +hT);
  const double p_SmTp = P(-hS, 0, 0, +hT);
  const double p_SpTm = near_expiry ? p_Sp : P(+hS, 0, 0, -hT);
  const double p_SmTm = near_expiry ? p_Sm : P(-hS, 0, 0, -hT);
  const double dT_den = near_expiry ? hT : (2.0 * hT);
  AmericanGreeks g;
  g.price = p0;
  g.delta = (p_Sp - p_Sm) / (2.0 * hS);
  g.gamma = (p_Sp - 2.0 * p0 + p_Sm) / (hS * hS);
  g.vega = (p_vp - p_vm) / (2.0 * hv);
  g.volga = (p_vp - 2.0 * p0 + p_vm) / (hv * hv);
  g.rho = (p_rp - p_rm) / (2.0 * hr);
  g.theta = -(p_Tp - p_Tm) / dT_den;
  g.vanna = (p_SpVp - p_SpVm - p_SmVp + p_SmVm) / (4.0 * hS * hv);
  g.charm = -(p_SpTp - p_SpTm - p_SmTp + p_SmTm) / (2.0 * hS * dT_den);
  return g;
}

// ── The 17-solve cold FD reference ────────────────────────────────────────
void BM_FdRef(benchmark::State& state) {
  const std::vector<Pt> grid = make_grid();
  double sink = 0.0;
  for (auto _ : state) {
    for (const Pt& p : grid) {
      const AmericanGreeks g =
          greeks_fd_reference(kS, p.K, p.T, p.sigma, kR, kQ, Side::Put);
      sink += g.delta + g.vega + g.gamma;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(grid.size()));
}

// ── Fast FD: 7 cold boundary solves (P1a) ─────────────────────────────────
void BM_FdFast(benchmark::State& state) {
  const std::vector<Pt> grid = make_grid();
  const std::optional<AlOpts> opts = std::nullopt;
  double sink = 0.0;
  for (auto _ : state) {
    for (const Pt& p : grid) {
      const auto g =
          american_greeks_fd(kS, p.K, p.T, p.sigma, kR, kQ, Side::Put,
                             AmericanMethod::AndersenLake, opts,
                             /*warm_start=*/false);
      sink += g ? g->delta + g->vega + g->gamma : 0.0;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(grid.size()));
}

// ── Warm FD: 6 bumped boundaries seeded from the base boundary (P1b) ───────
void BM_FdWarm(benchmark::State& state) {
  const std::vector<Pt> grid = make_grid();
  const std::optional<AlOpts> opts = std::nullopt;
  double sink = 0.0;
  for (auto _ : state) {
    for (const Pt& p : grid) {
      const auto g =
          american_greeks_fd(kS, p.K, p.T, p.sigma, kR, kQ, Side::Put,
                             AmericanMethod::AndersenLake, opts,
                             /*warm_start=*/true);
      sink += g ? g->delta + g->vega + g->gamma : 0.0;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(grid.size()));
}

// ── Analytic Andersen-Lake greeks: ~5 solves (P2) ─────────────────────────
void BM_AndersenLake(benchmark::State& state) {
  const std::vector<Pt> grid = make_grid();
  double sink = 0.0;
  for (auto _ : state) {
    for (const Pt& p : grid) {
      const auto g = american_greeks_al(kS, p.K, p.T, p.sigma, kR, kQ, Side::Put);
      sink += g ? g->delta + g->vega + g->gamma : 0.0;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(grid.size()));
}

// ── K3 laned analytic greeks bundle (batched), scalar vs AVX2 A/B ─────────
// The whole 40-put grid priced in one american_put_greeks_batch call: ForceScalar is
// the per-contract american_greeks_al oracle; ForceAvx2 lanes 4 puts/pack through the
// K3 kernel. laned_scalar / laned = the honest laned-bundle speedup at the shipping
// tier (composes with K1's preset ladder for the >=5x greeks close). PROVISIONAL.
void run_laned_greeks(benchmark::State& state, atx::vol::simd::SimdIsa isa,
                      bool need_vega, bool need_rho, bool need_charm) {
  const std::vector<Pt> grid = make_grid();
  const std::size_t n = grid.size();
  std::vector<double> S(n, kS), K(n), T(n), sig(n), r(n, kR), q(n, kQ);
  for (std::size_t i = 0; i < n; ++i) {
    K[i] = grid[i].K; T[i] = grid[i].T; sig[i] = grid[i].sigma;
  }
  std::vector<AmericanGreeks> out(n);
  double sink = 0.0;
  for (auto _ : state) {
    atx::vol::simd::american_put_greeks_batch(S.data(), K.data(), T.data(), sig.data(),
                                              r.data(), q.data(), n, std::nullopt,
                                              out.data(), isa, need_vega, need_rho,
                                              need_charm);
    for (const auto& g : out) sink += g.delta + g.gamma;
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
void BM_LanedScalar(benchmark::State& state) {
  run_laned_greeks(state, atx::vol::simd::SimdIsa::ForceScalar, true, true, true);
}
void BM_LanedAvx2(benchmark::State& state) {
  if (!atx::vol::simd::have_avx2()) {
    state.SkipWithError("no AVX2 on host");
    return;
  }
  run_laned_greeks(state, atx::vol::simd::SimdIsa::ForceAvx2, true, true, true);
}
// K4 first-order tier: {delta,gamma,theta} only -> 1 boundary solve/pack (skips the
// sigma+/-, r+/- and speed solves). The hedge/risk path's win over the full bundle.
void BM_LanedFirstOrder(benchmark::State& state) {
  if (!atx::vol::simd::have_avx2()) {
    state.SkipWithError("no AVX2 on host");
    return;
  }
  run_laned_greeks(state, atx::vol::simd::SimdIsa::ForceAvx2, false, false, false);
}

// Register with the mandated common knobs (warm-up, 5 reps, p95 + CV).
const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("american_greeks/fd_ref", BM_FdRef))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american_greeks/fd_fast", BM_FdFast))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american_greeks/fd_warm", BM_FdWarm))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american_greeks/andersen_lake",
                                            BM_AndersenLake))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american_greeks/laned_scalar",
                                            BM_LanedScalar))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american_greeks/laned", BM_LanedAvx2))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american_greeks/laned_first_order",
                                            BM_LanedFirstOrder))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

}  // namespace

}  // namespace atx::vol::bench
