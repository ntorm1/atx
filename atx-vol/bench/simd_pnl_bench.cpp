// Throughput proof for the vectorized Taylor P&L-explain kernel: a scalar
// per-position loop vs the AVX2 SoA batch, on one big cache-resident book.
//
// Both cases decompose the SAME SoA book into the eight Taylor components + total
// the portfolio pnl-explain hot path emits. The scalar case is the plain
// per-position loop (the pre-vectorization path); the SIMD case calls
// simd::pnl_taylor_explain_batch (4-lane AVX2+FMA on this host). The items/s
// (positions explained per second) and ns/position Google Benchmark derives are
// the speedup evidence. A one-shot max-abs cross-check on `total` is surfaced via
// a label so the run also documents that the two paths agree.

#include "atx/vol/simd/pnl_batch.hpp"

#include "atx/vol/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

// SoA columns for a book of Greeks + per-position state moves. A big contiguous
// layout so the batch shows its cache-friendly (streaming, no gather) throughput.
struct Book {
  std::vector<double> delta, gamma, vega, volga, vanna, theta, rho, charm;
  std::vector<double> qty, dS, dSigma, dt, dr;
};

Book make_book(std::size_t n) {
  Book b;
  b.delta.reserve(n); b.gamma.reserve(n); b.vega.reserve(n);
  b.volga.reserve(n); b.vanna.reserve(n); b.theta.reserve(n);
  b.rho.reserve(n); b.charm.reserve(n); b.qty.reserve(n);
  b.dS.reserve(n); b.dSigma.reserve(n); b.dt.reserve(n); b.dr.reserve(n);
  for (std::size_t k = 0; k < n; ++k) {
    const double p = static_cast<double>(k);
    b.delta.push_back(-1.0 + std::fmod(p * 0.013, 2.0));
    b.gamma.push_back(std::fmod(p * 0.0007, 0.5));
    b.vega.push_back(std::fmod(p * 0.11, 45.0));
    b.volga.push_back(-90.0 + std::fmod(p * 0.7, 180.0));
    b.vanna.push_back(-40.0 + std::fmod(p * 0.3, 80.0));
    b.theta.push_back(-45.0 + std::fmod(p * 0.5, 45.0));
    b.rho.push_back(-80.0 + std::fmod(p * 1.1, 160.0));
    b.charm.push_back(-8.0 + std::fmod(p * 0.2, 16.0));
    b.qty.push_back(-500.0 + std::fmod(p * 3.0, 1000.0));
    b.dS.push_back(-20.0 + std::fmod(p * 0.9, 40.0));
    b.dSigma.push_back(-0.3 + std::fmod(p * 0.001, 0.6));
    b.dt.push_back(-0.04 + std::fmod(p * 0.0001, 0.05));
    b.dr.push_back(-0.015 + std::fmod(p * 0.00005, 0.02));
  }
  return b;
}

simd::PnlExplainInputs make_inputs(const Book& b) {
  simd::PnlExplainInputs in{};
  in.delta = b.delta.data();
  in.gamma = b.gamma.data();
  in.vega = b.vega.data();
  in.volga = b.volga.data();
  in.vanna = b.vanna.data();
  in.theta = b.theta.data();
  in.rho = b.rho.data();
  in.charm = b.charm.data();
  in.qty = b.qty.data();
  in.dS = b.dS.data();
  in.dSigma = b.dSigma.data();
  in.dt = b.dt.data();
  in.dr = b.dr.data();
  return in;
}

struct Cols {
  std::vector<double> delta_pnl, gamma_pnl, vega_pnl, volga_pnl, vanna_pnl;
  std::vector<double> theta_pnl, rho_pnl, charm_pnl, total;
  explicit Cols(std::size_t n)
      : delta_pnl(n), gamma_pnl(n), vega_pnl(n), volga_pnl(n), vanna_pnl(n),
        theta_pnl(n), rho_pnl(n), charm_pnl(n), total(n) {}
  simd::PnlExplainOutputs view() {
    return simd::PnlExplainOutputs{
        delta_pnl.data(), gamma_pnl.data(), vega_pnl.data(),
        volga_pnl.data(), vanna_pnl.data(), theta_pnl.data(),
        rho_pnl.data(),   charm_pnl.data(), total.data()};
  }
};

// Per-position Taylor explain, one body, two codegen variants selected by the
// caller so we can separate the compiler's auto-vectorization from our explicit
// AVX2 kernel. VEC=false forces a genuinely scalar loop (the compiler otherwise
// SIMD-izes this trivial FMA loop at -O2, which would hide the vectorization
// win). VEC=true lets -O2 auto-vectorize it (the realistic "no hand-SIMD"
// baseline). Both compute the exact same 8-component decomposition.
template <bool VEC>
void explain_scalar(const Book& b, Cols& c, std::size_t n) {
#if defined(__clang__)
#  define ATXVOL_PNL_NOVEC _Pragma("clang loop vectorize(disable) interleave(disable)")
#else
#  define ATXVOL_PNL_NOVEC
#endif
  if constexpr (!VEC) {
    ATXVOL_PNL_NOVEC
    for (std::size_t i = 0; i < n; ++i) {
      const double w = b.qty[i], dS = b.dS[i], dv = b.dSigma[i], dt = b.dt[i], dr = b.dr[i];
      const double pd = b.delta[i] * dS, pg = 0.5 * b.gamma[i] * dS * dS;
      const double pv = b.vega[i] * dv, pvol = 0.5 * b.volga[i] * dv * dv;
      const double pvanna = b.vanna[i] * dS * dv, pth = b.theta[i] * dt;
      const double prho = b.rho[i] * dr, pcharm = b.charm[i] * dS * dt;
      c.delta_pnl[i] = w * pd; c.gamma_pnl[i] = w * pg; c.vega_pnl[i] = w * pv;
      c.volga_pnl[i] = w * pvol; c.vanna_pnl[i] = w * pvanna; c.theta_pnl[i] = w * pth;
      c.rho_pnl[i] = w * prho; c.charm_pnl[i] = w * pcharm;
      c.total[i] = w * (pd + pg + pv + pvol + pvanna + pth + prho + pcharm);
    }
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      const double w = b.qty[i], dS = b.dS[i], dv = b.dSigma[i], dt = b.dt[i], dr = b.dr[i];
      const double pd = b.delta[i] * dS, pg = 0.5 * b.gamma[i] * dS * dS;
      const double pv = b.vega[i] * dv, pvol = 0.5 * b.volga[i] * dv * dv;
      const double pvanna = b.vanna[i] * dS * dv, pth = b.theta[i] * dt;
      const double prho = b.rho[i] * dr, pcharm = b.charm[i] * dS * dt;
      c.delta_pnl[i] = w * pd; c.gamma_pnl[i] = w * pg; c.vega_pnl[i] = w * pv;
      c.volga_pnl[i] = w * pvol; c.vanna_pnl[i] = w * pvanna; c.theta_pnl[i] = w * pth;
      c.rho_pnl[i] = w * prho; c.charm_pnl[i] = w * pcharm;
      c.total[i] = w * (pd + pg + pv + pvol + pvanna + pth + prho + pcharm);
    }
  }
#undef ATXVOL_PNL_NOVEC
}

// Cache-resident size: 4096 positions x (13 in + 9 out) x 8B ~= 720 KiB, fits
// L2 (1.25 MiB), so this measures COMPUTE throughput, not DRAM bandwidth. At
// book scale (>~1e5 positions, several MiB) the kernel is memory-bandwidth-bound
// and scalar-autovec == avx2 == DRAM; the SIMD arithmetic win only shows when the
// working set is cache-resident. Both regimes are honest; we measure compute.
constexpr std::size_t kN = 1u << 12; // 4,096 positions (cache-resident)

template <bool VEC>
void BM_PnlExplain_Scalar(benchmark::State& state) {
  const Book b = make_book(kN);
  Cols c(kN);
  for (auto _ : state) {
    explain_scalar<VEC>(b, c, kN);
    benchmark::DoNotOptimize(c.total.data());
    benchmark::DoNotOptimize(c.delta_pnl.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));
}

void BM_PnlExplain_Avx2(benchmark::State& state) {
  const Book b = make_book(kN);
  const simd::PnlExplainInputs in = make_inputs(b);
  Cols c(kN);
  simd::PnlExplainOutputs out = c.view();
  for (auto _ : state) {
    simd::pnl_taylor_explain_batch(in, out, kN);
    benchmark::DoNotOptimize(c.total.data());
    benchmark::DoNotOptimize(c.delta_pnl.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kN));

  // One-shot parity cross-check on `total`, surfaced as a benchmark label.
  Cols ref(kN);
  explain_scalar<false>(b, ref, kN);
  double max_abs = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    max_abs = std::max(max_abs, std::abs(c.total[i] - ref.total[i]));
  }
  state.SetLabel("avx2=" + std::to_string(simd::have_avx2() ? 1 : 0) +
                 " max_abs_vs_scalar=" + std::to_string(max_abs));
}

const int kRegistered = [] {
  // scalar_novec: genuine scalar loop (auto-vectorization suppressed) — the true
  // no-SIMD baseline. scalar_autovec: what -O2 does to the same loop unaided.
  // avx2: our explicit SoA kernel. Expect avx2 ~= scalar_autovec >> scalar_novec.
  apply_common(benchmark::RegisterBenchmark("simd/pnl_explain/scalar_novec",
                                            BM_PnlExplain_Scalar<false>))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("simd/pnl_explain/scalar_autovec",
                                            BM_PnlExplain_Scalar<true>))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("simd/pnl_explain/avx2",
                                            BM_PnlExplain_Avx2))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
