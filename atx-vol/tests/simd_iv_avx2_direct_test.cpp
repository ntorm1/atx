// K4 / R-24 — direct parity gate for the retained AVX2 IV batch kernel.
//
// R-24 routed the public simd::implied_vol_batch to the scalar loop (the AVX2
// batch measured ~parity, not ≥1.2× scalar on this ISA), so the dispatch no
// longer exercises detail::implied_vol_batch_avx2. That kernel is kept for the IV
// shootout bench and a future AVX-512 batch, so this test calls it DIRECTLY to
// keep its 4-lane path covered: accepted lanes must track the scalar inverter and
// degenerate/failure lanes must flag ok == 0 exactly as scalar errors.

#include "atx/vol/black76.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol::simd::detail {
// Defined in src/simd/iv_batch_avx2.cpp; the AVX2 4-lane IV batch kernel.
void implied_vol_batch_avx2(const double *price, const double *F, const double *K, const double *T,
                            const double *df, const Side *side, double *iv_out,
                            std::uint8_t *ok_out, std::size_t n) noexcept;
} // namespace atx::vol::simd::detail

namespace atx::vol {
namespace {

TEST(SimdIvAvx2Direct, RoundTripMatchesScalar) {
  if (!simd::have_avx2()) GTEST_SKIP() << "AVX2 required for the direct kernel";
  std::vector<double> price, F, K, T, df, sigma_in;
  std::vector<Side> side;
  const double forwards[] = {50.0, 100.0, 250.0};
  const double moneyness[] = {0.85, 0.95, 1.0, 1.05, 1.15};
  const double tenors[] = {0.1, 0.5, 1.0, 2.0};
  const double vols[] = {0.15, 0.25, 0.40};
  for (double Fv : forwards)
    for (double m : moneyness)
      for (double Tv : tenors)
        for (double v : vols)
          for (Side sd : {Side::Call, Side::Put}) {
            const double Kv = Fv * m;
            const double d = std::exp(-0.03 * Tv);
            F.push_back(Fv); K.push_back(Kv); T.push_back(Tv);
            df.push_back(d); side.push_back(sd); sigma_in.push_back(v);
            price.push_back(black76_price(Fv, Kv, Tv, v, d, sd));
          }
  const std::size_t n = F.size();
  std::vector<double> iv(n, 0.0);
  std::vector<std::uint8_t> ok(n, 0);
  simd::detail::implied_vol_batch_avx2(price.data(), F.data(), K.data(), T.data(), df.data(),
                                       side.data(), iv.data(), ok.data(), n);
  double max_vs_scalar = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_EQ(ok[i], 1u) << "i=" << i;
    const Result<double> r = implied_vol(price[i], F[i], K[i], T[i], df[i], side[i]);
    ASSERT_TRUE(r.has_value()) << "i=" << i;
    max_vs_scalar = std::max(max_vs_scalar, std::fabs(iv[i] - *r));
    EXPECT_LE(std::fabs(iv[i] - *r), 2e-8 + 1e-8 * std::fabs(*r)) << "i=" << i;
  }
  // The AVX2 accept gate admits accepted lanes within ~1e-8 of scalar (2 Halley
  // steps); the worst observed on this grid is ~1.2e-8 — far inside the 1e-4 vol
  // economic bound. (This looser-than-scalar accuracy is exactly why R-24 keeps
  // the kernel off the default IV dispatch.)
  EXPECT_LT(max_vs_scalar, 5e-8);
}

TEST(SimdIvAvx2Direct, DegenerateLanesFlaggedLikeScalar) {
  if (!simd::have_avx2()) GTEST_SKIP() << "AVX2 required for the direct kernel";
  const double d = std::exp(-0.03);
  const std::vector<double> price = {1.20 * 100.0 * d, -0.5, 5.0, 5.0};
  const std::vector<double> F = {100.0, 100.0, 100.0, 100.0};
  const std::vector<double> K = {95.0, 100.0, 100.0, 100.0};
  const std::vector<double> T = {1.0, 1.0, 0.0, -1.0};
  const std::vector<double> df = {d, d, 1.0, 1.0};
  const std::vector<Side> side = {Side::Call, Side::Call, Side::Call, Side::Put};
  const std::size_t n = price.size();
  std::vector<double> iv(n, 0.0);
  std::vector<std::uint8_t> ok(n, 7);
  simd::detail::implied_vol_batch_avx2(price.data(), F.data(), K.data(), T.data(), df.data(),
                                       side.data(), iv.data(), ok.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const Result<double> r = implied_vol(price[i], F[i], K[i], T[i], df[i], side[i]);
    EXPECT_EQ(ok[i] != 0, r.has_value()) << "i=" << i;
  }
}

} // namespace
} // namespace atx::vol
