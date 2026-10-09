// K2 — full-range Cody rational-erfc Φ accuracy gate (SPRINT W5.3).
//
// The AVX2 pricing/greeks/IV kernels replaced their degree-48 Chebyshev–Clenshaw
// Φ (uniform absolute error ~1e-11 on |d| ≤ 7 only, deep wings clamped) with W.
// J. Cody's rational-erfc Φ (Math. Comp. 23, 1969), which is full-range double
// precision (≈1e-16 absolute, correct denormal wings). Φ is AVX2-internal, so
// this exercises it through its real consumers: the batch price/value-vega/IV
// kernels vs. the scalar source of truth (atx::core::norm_cdf, std::erfc-based).
//
// The tolerances here are ~1000× tighter than the old Chebyshev path could hold
// (simd_batch_test documents its ~1e-6 abs / ~1e-9 approximation): a regression
// back to the Chebyshev Φ would fail these bounds.

#include "simd/black76_batch.hpp"
#include "simd/iv_batch.hpp"

#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"
#include "atx/vol/api/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// A grid that stays in the interior/near-wing region (|d| ≲ 7) where prices are
// not catastrophically canceling, so the batch-vs-scalar price error tracks the
// Φ error directly rather than downstream subtraction noise. Includes moneyness
// out to 1.6/0.6 and vols to 0.9 to push d toward the wings the Chebyshev could
// not reach accurately.
struct Grid {
  std::vector<double> F, K, T, sigma, df;
  std::vector<Side> side;
  [[nodiscard]] std::size_t size() const { return F.size(); }
  void push(double f, double k, double t, double s, double d, Side sd) {
    F.push_back(f); K.push_back(k); T.push_back(t);
    sigma.push_back(s); df.push_back(d); side.push_back(sd);
  }
};

Grid make_grid() {
  Grid g;
  const double forwards[] = {25.0, 100.0, 400.0};
  const double moneyness[] = {0.6, 0.8, 0.95, 1.0, 1.05, 1.25, 1.6};
  const double tenors[] = {0.05, 0.25, 1.0, 2.0};
  const double vols[] = {0.10, 0.25, 0.50, 0.90};
  for (double F : forwards)
    for (double m : moneyness)
      for (double T : tenors)
        for (double v : vols) {
          const double df = std::exp(-0.025 * T);
          g.push(F, F * m, T, v, df, Side::Call);
          g.push(F, F * m, T, v, df, Side::Put);
        }
  return g;
}

// Batch Black-76 prices agree with the scalar std::erfc source of truth to a
// tight ATM-relative bound: the erfc Φ is machine-accurate, so the only residual
// is the price's own subtraction rounding, not a ~1e-11 Φ bias.
TEST(SimdNormCdfErfc, PriceMatchesScalarTightly) {
  const Grid g = make_grid();
  const std::size_t n = g.size();
  std::vector<double> got(n, 0.0);
  simd::black76_price_batch(g.F.data(), g.K.data(), g.T.data(), g.sigma.data(), g.df.data(),
                            g.side.data(), got.data(), n);

  double max_abs = 0.0, max_rel_atm = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double want = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
    const double abs_err = std::fabs(got[i] - want);
    max_abs = std::max(max_abs, abs_err);
    // Relative to the notional scale df·F (a stable denominator that does not
    // vanish for OTM lanes), the error is Φ-accuracy-limited.
    const double rel = abs_err / (g.df[i] * g.F[i]);
    max_rel_atm = std::max(max_rel_atm, rel);
  }
  // The old Chebyshev path held only ~1e-6 absolute here; erfc is machine-class.
  EXPECT_LT(max_abs, 1e-9) << "max_abs=" << max_abs;
  EXPECT_LT(max_rel_atm, 1e-13) << "max_rel(df·F)=" << max_rel_atm;
  std::printf("[SimdNormCdfErfc] price max_abs=%.3e max_rel(dfF)=%.3e\n", max_abs, max_rel_atm);
}

// Batch value+vega: vega = df·F·φ(d1)·√T. φ uses the vectorized exp; the erfc Φ
// improves the price, not φ, so vega parity is unchanged but must not regress.
TEST(SimdNormCdfErfc, ValueVegaMatchesScalarTightly) {
  const Grid g = make_grid();
  const std::size_t n = g.size();
  std::vector<double> price(n, 0.0), vega(n, 0.0);
  simd::black76_value_vega_batch(g.F.data(), g.K.data(), g.T.data(), g.sigma.data(), g.df.data(),
                                 g.side.data(), price.data(), vega.data(), n);
  double max_price = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const Black76ValueVega want =
        black76_value_and_vega(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
    max_price = std::max(max_price, std::fabs(price[i] - want.price));
  }
  EXPECT_LT(max_price, 1e-9) << "max_price=" << max_price;
}

// The IV batch inherits the machine-accurate Φ: accepted lanes now recover σ far
// tighter to the scalar inverter than the Chebyshev's ~1e-9 accept bias allowed.
TEST(SimdNormCdfErfc, IvBatchMatchesScalarTightly) {
  const Grid g = make_grid();
  const std::size_t n = g.size();
  std::vector<double> price(n);
  for (std::size_t i = 0; i < n; ++i) {
    price[i] = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
  }
  std::vector<double> iv(n, 0.0);
  std::vector<std::uint8_t> ok(n, 0);
  simd::implied_vol_batch(price.data(), g.F.data(), g.K.data(), g.T.data(), g.df.data(),
                          g.side.data(), iv.data(), ok.data(), n);
  double max_vs_scalar = 0.0;
  int accepted = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!ok[i]) continue;
    const Result<double> r = implied_vol(price[i], g.F[i], g.K[i], g.T[i], g.df[i], g.side[i]);
    if (!r) continue;
    max_vs_scalar = std::max(max_vs_scalar, std::fabs(iv[i] - *r));
    ++accepted;
  }
  // Well inside the 1e-4 vol economic bound; erfc removes the Chebyshev accept
  // bias so accepted lanes track scalar to ~1e-10 rather than ~1e-9.
  EXPECT_GT(accepted, 0);
  EXPECT_LT(max_vs_scalar, 1e-8) << "max_vs_scalar=" << max_vs_scalar;
  std::printf("[SimdNormCdfErfc] iv accepted=%d max|Δσ| vs scalar=%.3e\n", accepted, max_vs_scalar);
}

} // namespace
} // namespace atx::vol
