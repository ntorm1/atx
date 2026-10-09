// CStar (C16M "modal") calibration + no-arb hot-path microbenchmarks.
//
// Two families:
//   fit/cstar/normal_eq/{legacy,fused}  — the per-observation work of the
//     vol/price-domain LM normal-equations build (SPRINT W5.2). `legacy` models
//     the pre-S2 cost (a redundant cstar_slice_w evaluation + the analytic
//     gradient + the central-FD f'(z) evaluations the old cstar_slice_grad_w
//     paid); `fused` is the shipped single-pass cstar_slice_w_and_grad. The
//     ratio is the S2 speedup on the normal-equations build.
//   arb/cstar/{project,min_roper_g}     — the no-arb butterfly projection
//     (SPRINT W5.1). Baseline for the S3 table-driven rewrite; the recorded
//     ratio is legacy-projection / table-driven-projection.
//
// All numbers are provisional (measured on a concurrently-loaded host); the
// definitive figures land on a quiet host at Sprint I.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "fitting/cstar.hpp"

#include "bench_util.hpp"

namespace {

using atx::vol::CStarParams;
using atx::vol::CStarTier;
using atx::vol::cstar_arb_project;
using atx::vol::cstar_base;
using atx::vol::cstar_basis;
using atx::vol::cstar_min_roper_g;
using atx::vol::cstar_slice_grad_w;
using atx::vol::cstar_slice_w;
using atx::vol::cstar_slice_w_and_grad;
using atx::vol::cstar_tier_mask;
using atx::vol::kCStarNBase;
using atx::vol::kCStarNModes;
using atx::vol::kCStarNParams;
using atx::vol::bench::apply_common;

// A representative C16 SPY-class slice: curvature + skew + all 11 modes carrying
// small amplitudes (an eSSVI-seeded surface after modal fitting looks like this).
[[nodiscard]] CStarParams bench_slice() {
  CStarParams s{};
  s.T = 0.08;
  s.F = 100.0;
  s.theta = 0.04;
  s.s2 = -0.06;
  s.c2 = 0.20;
  s.C_left = 0.22;
  s.C_right = 0.16;
  s.active_modes = cstar_tier_mask(CStarTier::C16);
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    s.beta[j] = 0.004 * std::sin(0.7 * static_cast<double>(j) + 0.3);
  }
  return s;
}

// A ~40-strike board's log-moneyness grid (z ∈ [-3, +3] in √theta units).
[[nodiscard]] std::vector<double> bench_k_grid(const CStarParams& s) {
  constexpr int kN = 40;
  const double sqrt_theta = std::sqrt(s.theta);
  std::vector<double> ks;
  ks.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    const double z =
        -3.0 + 6.0 * static_cast<double>(i) / static_cast<double>(kN - 1);
    ks.push_back(z * sqrt_theta);
  }
  return ks;
}

// Accumulate one Gauss-Newton row (dim = kCStarNParams) into a symmetric H and g
// — identical between the two variants, so the timing difference is the shape
// evaluation the S2 fusion removed, not the linear algebra.
inline void accumulate(std::array<double, kCStarNParams>& g,
                       std::array<double, kCStarNParams * kCStarNParams>& H,
                       const std::array<double, kCStarNParams>& row, double r) {
  for (std::size_t j = 0; j < kCStarNParams; ++j) {
    g[j] += row[j] * r;
    for (std::size_t kk = 0; kk <= j; ++kk) {
      H[j * kCStarNParams + kk] += row[j] * row[kk];
    }
  }
}

void BM_CStar_normal_eq_legacy(benchmark::State& state) {
  const CStarParams s = bench_slice();
  const std::vector<double> ks = bench_k_grid(s);
  const double sqrt_theta = std::sqrt(s.theta);
  for (auto _ : state) {
    std::array<double, kCStarNParams> g{};
    std::array<double, kCStarNParams * kCStarNParams> H{};
    for (const double k : ks) {
      const double w = cstar_slice_w(s, k);  // redundant w evaluation (pre-S2)
      const auto grad = cstar_slice_grad_w(s, k);
      // Model the pre-S2 central-FD f'(z): two extra base evals + a per-mode
      // pair — the cost the old FD gradient paid on top of the analytic terms.
      const double z = k / sqrt_theta;
      constexpr double h = 1.0e-4;
      double fd = cstar_base(z + h, s.s2, s.c2, s.C_left, s.C_right) -
                  cstar_base(z - h, s.s2, s.c2, s.C_left, s.C_right);
      for (std::size_t j = 0; j < kCStarNModes; ++j) {
        fd += cstar_basis(static_cast<int>(j), z + h) -
              cstar_basis(static_cast<int>(j), z - h);
      }
      benchmark::DoNotOptimize(fd);
      accumulate(g, H, *grad, w);
    }
    benchmark::DoNotOptimize(g);
    benchmark::DoNotOptimize(H);
  }
}

void BM_CStar_normal_eq_fused(benchmark::State& state) {
  const CStarParams s = bench_slice();
  const std::vector<double> ks = bench_k_grid(s);
  for (auto _ : state) {
    std::array<double, kCStarNParams> g{};
    std::array<double, kCStarNParams * kCStarNParams> H{};
    for (const double k : ks) {
      const auto wg = cstar_slice_w_and_grad(s, k);  // single fused pass
      accumulate(g, H, wg->grad, wg->w);
    }
    benchmark::DoNotOptimize(g);
    benchmark::DoNotOptimize(H);
  }
}

// An arb-violating C16 slice: a large ATM modal bump forces the projection to
// run its full group-damping bisection ladder (30 min-Roper-g sweeps/group).
[[nodiscard]] CStarParams arb_slice() {
  CStarParams s = bench_slice();
  s.beta[5] = 0.35;   // strong ATM convexity spike => butterfly violation
  s.beta[4] = -0.20;
  s.beta[6] = -0.20;
  return s;
}

void BM_CStar_min_roper_g(benchmark::State& state) {
  const CStarParams s = arb_slice();
  for (auto _ : state) {
    benchmark::DoNotOptimize(cstar_min_roper_g(s));
  }
}

void BM_CStar_arb_project(benchmark::State& state) {
  const CStarParams s = arb_slice();
  for (auto _ : state) {
    CStarParams work = s;  // fresh copy: projection mutates in place
    benchmark::DoNotOptimize(cstar_arb_project(work));
  }
}

// apply_common(RegisterBenchmark(...)) is the house idiom: it applies the shared
// knobs (warm-up, Repetitions(5), p95/CV statistics) and returns the Benchmark*.
[[maybe_unused]] const auto* const kReg1 = apply_common(
    benchmark::RegisterBenchmark("fit/cstar/normal_eq/legacy",
                                 BM_CStar_normal_eq_legacy));
[[maybe_unused]] const auto* const kReg2 = apply_common(
    benchmark::RegisterBenchmark("fit/cstar/normal_eq/fused",
                                 BM_CStar_normal_eq_fused));
[[maybe_unused]] const auto* const kReg3 = apply_common(
    benchmark::RegisterBenchmark("arb/cstar/min_roper_g", BM_CStar_min_roper_g));
[[maybe_unused]] const auto* const kReg4 = apply_common(
    benchmark::RegisterBenchmark("arb/cstar/project", BM_CStar_arb_project));

}  // namespace
