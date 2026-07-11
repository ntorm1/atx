// Parity gate for the vectorized (AVX2) Black-76 Greeks batch kernel.
//
// simd::black76_greeks_batch dispatches to the 4-lane AVX2 path when the host
// supports it (this CI/dev box does — Alder Lake). These tests assert that the
// vectorized result reproduces the scalar per-contract atx::vol::black76_greeks
// to full risk accuracy across all eight sensitivities + price, including the
// awkward cases the SIMD path must special-case: n not a multiple of 4 (scalar
// tail), degenerate lanes (T ≤ 0 or σ ≤ 0), and deep-wing lanes (|d| large) that
// the kernel patches through the exact scalar path. If AVX2 is absent the batch
// runs the scalar loop and these become identity checks — still valid, just
// trivially exact.

#include "atx/vol/simd/greeks_batch.hpp"

#include "atx/vol/greeks.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// A broad, deterministic grid of contracts spanning ITM/OTM/ATM, short/long
// tenors, low/high vol, both sides, plus explicit degenerate and deep-wing rows.
struct Batch {
  std::vector<double> F, K, T, sigma, r, df;
  std::vector<Side> side;
  [[nodiscard]] std::size_t size() const { return F.size(); }
  void push(double f, double k, double t, double s, double rr, double d,
            Side sd) {
    F.push_back(f); K.push_back(k); T.push_back(t);
    sigma.push_back(s); r.push_back(rr); df.push_back(d); side.push_back(sd);
  }
};

Batch make_grid() {
  Batch b;
  const double forwards[] = {10.0, 50.0, 100.0, 250.0, 500.0};
  const double moneyness[] = {0.5, 0.8, 0.95, 1.0, 1.05, 1.25, 2.0};
  const double tenors[] = {1.0 / 365, 0.05, 0.25, 1.0, 2.5};
  const double vols[] = {0.08, 0.20, 0.45, 0.90};
  const double rate = 0.03;
  for (double F : forwards)
    for (double m : moneyness)
      for (double T : tenors)
        for (double v : vols) {
          const double df = std::exp(-rate * T);
          b.push(F, F * m, T, v, rate, df, Side::Call);
          b.push(F, F * m, T, v, rate, df, Side::Put);
        }
  // Degenerate lanes: expired and zero-vol.
  b.push(100.0, 95.0, 0.0, 0.20, 0.03, 1.0, Side::Call);
  b.push(100.0, 105.0, -1.0, 0.20, 0.03, 1.0, Side::Put);
  b.push(100.0, 100.0, 0.5, 0.0, 0.03, 0.98, Side::Call);
  // Deep-wing lanes: |d| far beyond the Chebyshev interior (patched to scalar).
  b.push(100.0, 5.0, 2.0, 0.10, 0.03, 0.95, Side::Call);
  b.push(100.0, 5000.0, 2.0, 0.10, 0.03, 0.95, Side::Put);
  return b;
}

// Combined abs+rel tolerance check, tracking the worst absolute error seen.
void expect_close(double got, double want, double abs_tol, double rel_tol,
                  const char* field, std::size_t i, double& max_abs) {
  const double e = std::abs(got - want);
  max_abs = std::max(max_abs, e);
  EXPECT_LE(e, abs_tol + rel_tol * std::abs(want))
      << field << " i=" << i << " got=" << got << " want=" << want;
}

TEST(SimdBlack76GreeksBatch, MatchesScalarAcrossGrid) {
  const Batch b = make_grid();
  const std::size_t n = b.size();
  std::vector<Greeks> got(n);
  std::vector<double> price(n, 0.0);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(),
                             b.r.data(), b.df.data(), b.side.data(), got.data(),
                             price.data(), n);

  // The vector Φ/exp are absolute-accuracy (~1e-11) approximations, so each
  // field matches scalar to a combined abs+rel tolerance: near-cancellation in
  // F·Φ(d1)-K·Φ(d2) keeps tiny *absolute* error even where *relative* error
  // grows. Economically exact; any formula/sign bug is order-1 relative and
  // would be caught wide.
  constexpr double kAbs = 1e-6;
  constexpr double kRel = 1e-7;
  double m_price = 0.0, m_delta = 0.0, m_gamma = 0.0, m_vega = 0.0,
         m_theta = 0.0, m_rho = 0.0, m_vanna = 0.0, m_volga = 0.0,
         m_charm = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks w = black76_greeks(b.F[i], b.K[i], b.T[i], b.sigma[i],
                                           b.r[i], b.df[i], b.side[i]);
    expect_close(price[i], w.price, kAbs, kRel, "price", i, m_price);
    expect_close(got[i].delta, w.greeks.delta, kAbs, kRel, "delta", i, m_delta);
    expect_close(got[i].gamma, w.greeks.gamma, kAbs, kRel, "gamma", i, m_gamma);
    expect_close(got[i].vega, w.greeks.vega, kAbs, kRel, "vega", i, m_vega);
    expect_close(got[i].theta, w.greeks.theta, kAbs, kRel, "theta", i, m_theta);
    expect_close(got[i].rho, w.greeks.rho, kAbs, kRel, "rho", i, m_rho);
    expect_close(got[i].vanna, w.greeks.vanna, kAbs, kRel, "vanna", i, m_vanna);
    expect_close(got[i].volga, w.greeks.volga, kAbs, kRel, "volga", i, m_volga);
    expect_close(got[i].charm, w.greeks.charm, kAbs, kRel, "charm", i, m_charm);
  }
  // Absolute error stays sub-microdollar/-microgreek across the whole grid.
  EXPECT_LT(m_price, kAbs);
  EXPECT_LT(m_delta, kAbs);
}

// Degenerate + deep-wing rows are patched through the scalar kernel, so they
// must reproduce it bit-for-bit (up to ULP). The five special rows sit at the
// tail of make_grid(), straddling a full SIMD group and the scalar tail.
TEST(SimdBlack76GreeksBatch, PatchedLanesAreBitExact) {
  const Batch b = make_grid();
  const std::size_t n = b.size();
  std::vector<Greeks> got(n);
  std::vector<double> price(n, 0.0);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(),
                             b.r.data(), b.df.data(), b.side.data(), got.data(),
                             price.data(), n);
  for (std::size_t i = n - 5; i < n; ++i) {
    const Black76Greeks w = black76_greeks(b.F[i], b.K[i], b.T[i], b.sigma[i],
                                           b.r[i], b.df[i], b.side[i]);
    EXPECT_DOUBLE_EQ(price[i], w.price) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].delta, w.greeks.delta) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].gamma, w.greeks.gamma) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].vega, w.greeks.vega) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].theta, w.greeks.theta) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].rho, w.greeks.rho) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].vanna, w.greeks.vanna) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].volga, w.greeks.volga) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].charm, w.greeks.charm) << "i=" << i;
  }
}

// The scalar tail (n % 4 != 0) must be handled for every residue class.
TEST(SimdBlack76GreeksBatch, HandlesEveryTailResidue) {
  const Batch full = make_grid();
  for (std::size_t n = 1; n <= 11; ++n) {
    std::vector<Greeks> got(n);
    std::vector<double> price(n, 0.0);
    simd::black76_greeks_batch(full.F.data(), full.K.data(), full.T.data(),
                               full.sigma.data(), full.r.data(), full.df.data(),
                               full.side.data(), got.data(), price.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      const Black76Greeks w =
          black76_greeks(full.F[i], full.K[i], full.T[i], full.sigma[i],
                         full.r[i], full.df[i], full.side[i]);
      EXPECT_LT(std::abs(price[i] - w.price), 1e-6) << "n=" << n << " i=" << i;
      EXPECT_LT(std::abs(got[i].delta - w.greeks.delta), 1e-6)
          << "n=" << n << " i=" << i;
      EXPECT_LT(std::abs(got[i].vega - w.greeks.vega), 1e-5)
          << "n=" << n << " i=" << i;
    }
  }
}

TEST(SimdBlack76GreeksBatch, ZeroLengthIsNoOp) {
  Greeks gsentinel{};
  gsentinel.delta = 42.0;
  double psentinel = 7.0;
  simd::black76_greeks_batch(nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, nullptr, &gsentinel, &psentinel, 0);
  EXPECT_EQ(gsentinel.delta, 42.0);
  EXPECT_EQ(psentinel, 7.0);
}

} // namespace
} // namespace atx::vol
