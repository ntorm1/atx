// Parity gate for the vectorized (AVX2) Black-76 batch kernels.
//
// The batch entry points dispatch to the 4-lane AVX2 path when the host
// supports it (this CI/dev box does — Alder Lake). These tests assert that the
// vectorized result reproduces the scalar per-contract kernels in
// atx/vol/black76.hpp to full pricing accuracy, including the awkward cases the
// SIMD path must special-case: n not a multiple of 4 (scalar tail) and
// degenerate lanes (T ≤ 0 or σ ≤ 0) that the kernel patches through the exact
// scalar path. Deep-wing lanes (|d| large) are NO LONGER patched (K2): the Cody
// rational-erfc Φ prices them on the vector path to machine accuracy. If AVX2 is
// absent the batch runs the scalar loop and these become identity checks — still
// valid, just trivially exact.

#include "atx/vol/simd/black76_batch.hpp"

#include "atx/vol/black76.hpp"
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
  std::vector<double> F, K, T, sigma, df;
  std::vector<Side> side;
  [[nodiscard]] std::size_t size() const { return F.size(); }
  void push(double f, double k, double t, double s, double d, Side sd) {
    F.push_back(f); K.push_back(k); T.push_back(t);
    sigma.push_back(s); df.push_back(d); side.push_back(sd);
  }
};

Batch make_grid() {
  Batch b;
  const double forwards[] = {10.0, 50.0, 100.0, 250.0, 500.0};
  const double moneyness[] = {0.5, 0.8, 0.95, 1.0, 1.05, 1.25, 2.0};
  const double tenors[] = {1.0 / 365, 0.05, 0.25, 1.0, 2.5};
  const double vols[] = {0.08, 0.20, 0.45, 0.90};
  for (double F : forwards)
    for (double m : moneyness)
      for (double T : tenors)
        for (double v : vols) {
          const double df = std::exp(-0.03 * T);
          b.push(F, F * m, T, v, df, Side::Call);
          b.push(F, F * m, T, v, df, Side::Put);
        }
  // Degenerate lanes: expired and zero-vol.
  b.push(100.0, 95.0, 0.0, 0.20, 1.0, Side::Call);
  b.push(100.0, 105.0, -1.0, 0.20, 1.0, Side::Put);
  b.push(100.0, 100.0, 0.5, 0.0, 0.98, Side::Call);
  // Deep-wing lanes: |d| far beyond the old Chebyshev interior. Post-K2 these
  // price on the vector Cody-erfc Φ (no wing patch) to machine accuracy.
  b.push(100.0, 5.0, 2.0, 0.10, 0.95, Side::Call);
  b.push(100.0, 5000.0, 2.0, 0.10, 0.95, Side::Put);
  return b;
}

TEST(SimdBlack76Batch, PriceMatchesScalarAcrossGrid) {
  const Batch b = make_grid();
  const std::size_t n = b.size();
  std::vector<double> got(n, 0.0);
  simd::black76_price_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(),
                            b.df.data(), b.side.data(), got.data(), n);

  // The vector Φ is an absolute-accuracy (~1e-9) approximation, so the price
  // matches scalar to a combined abs+rel tolerance: sub-cent OTM prices where
  // F·Φ(d1)-K·Φ(d2) cancels keep tiny *absolute* error (nanodollars) even
  // though their *relative* error can reach ~1e-6. Economically exact.
  constexpr double kAbs = 1e-6;
  constexpr double kRel = 1e-7;
  double max_abs = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double want =
        black76_price(b.F[i], b.K[i], b.T[i], b.sigma[i], b.df[i], b.side[i]);
    const double abs_err = std::abs(got[i] - want);
    max_abs = std::max(max_abs, abs_err);
    EXPECT_LE(abs_err, kAbs + kRel * std::abs(want))
        << "i=" << i << " got=" << got[i] << " want=" << want;
  }
  // Absolute error stays sub-microdollar across the whole grid.
  EXPECT_LT(max_abs, kAbs);
}

TEST(SimdBlack76Batch, ValueVegaMatchesScalarAcrossGrid) {
  const Batch b = make_grid();
  const std::size_t n = b.size();
  std::vector<double> price(n, 0.0);
  std::vector<double> vega(n, 0.0);
  simd::black76_value_vega_batch(b.F.data(), b.K.data(), b.T.data(),
                                 b.sigma.data(), b.df.data(), b.side.data(),
                                 price.data(), vega.data(), n);

  double max_price = 0.0;
  double max_vega = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const Black76ValueVega want = black76_value_and_vega(
        b.F[i], b.K[i], b.T[i], b.sigma[i], b.df[i], b.side[i]);
    max_price = std::max(max_price, std::abs(price[i] - want.price));
    max_vega = std::max(max_vega, std::abs(vega[i] - want.vega));
  }
  EXPECT_LT(max_price, 1e-6);
  // Vega scales with F·√T·φ; φ uses the vectorized exp (~1e-13 rel). Bound
  // generously against the largest F (500) and T (2.5).
  EXPECT_LT(max_vega, 1e-5);
}

// The scalar tail (n % 4 != 0) must be handled for every residue class.
TEST(SimdBlack76Batch, HandlesEveryTailResidue) {
  const Batch full = make_grid();
  for (std::size_t n = 1; n <= 11; ++n) {
    std::vector<double> got(n, 0.0);
    simd::black76_price_batch(full.F.data(), full.K.data(), full.T.data(),
                              full.sigma.data(), full.df.data(),
                              full.side.data(), got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      const double want = black76_price(full.F[i], full.K[i], full.T[i],
                                        full.sigma[i], full.df[i], full.side[i]);
      EXPECT_LT(std::abs(got[i] - want), 1e-6) << "n=" << n << " i=" << i;
    }
  }
}

TEST(SimdBlack76Batch, ZeroLengthIsNoOp) {
  double sentinel = 42.0;
  simd::black76_price_batch(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                            &sentinel, 0);
  EXPECT_EQ(sentinel, 42.0);
}

} // namespace
} // namespace atx::vol
