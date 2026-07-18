// K4 / R-22 — adversarial NaN-d lane parity in the AVX2 price/greeks batch.
//
// A lane can produce a NaN intermediate d1/d2 from FINITE, positive inputs: an
// F/K under/overflow drives ln(F/K) to ±inf while a σ²T overflow drives ½v² to
// +inf, so d1 = (∓inf + inf)/inf = NaN. The AVX2 wing/patch mask used only
// ORDERED compares (_CMP_GT/LT_OQ), which are false for NaN, so such a lane
// escaped the scalar patch. The scalar kernel returns NaN for these inputs; the
// vector path must agree. R-22 ORs an unordered self-compare into the patch mask
// so a NaN-d lane routes to the exact scalar kernel.

#include "atx/vol/simd/black76_batch.hpp"
#include "atx/vol/simd/greeks_batch.hpp"

#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// Inputs that are finite and > 0 yet drive d1/d2 to NaN via ±inf cancellation.
struct AdvRow {
  double F, K, T, sigma, df;
  Side side;
};

std::vector<AdvRow> adversarial_rows() {
  const double df = 0.99;
  return {
      {1e-200, 1e200, 1e160, 1e160, df, Side::Call}, // F/K underflow + σ²T overflow
      {1e200, 1e-200, 1e160, 1e160, df, Side::Put},  // F/K overflow + σ²T overflow
      {100.0, 100.0, 0.5, 0.2, df, Side::Call},      // a normal lane in the same block
      {100.0, 110.0, 1.0, 0.3, df, Side::Put},       // another normal lane
  };
}

TEST(SimdBatchNaN, PriceBatchAgreesWithScalarOnNaNLanes) {
  const auto rows = adversarial_rows();
  const std::size_t n = rows.size();
  std::vector<double> F(n), K(n), T(n), sig(n), df(n);
  std::vector<Side> side(n);
  for (std::size_t i = 0; i < n; ++i) {
    F[i] = rows[i].F; K[i] = rows[i].K; T[i] = rows[i].T;
    sig[i] = rows[i].sigma; df[i] = rows[i].df; side[i] = rows[i].side;
  }
  std::vector<double> got(n, 0.0);
  simd::black76_price_batch(F.data(), K.data(), T.data(), sig.data(), df.data(), side.data(),
                            got.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const double want = black76_price(F[i], K[i], T[i], sig[i], df[i], side[i]);
    if (std::isnan(want)) {
      EXPECT_TRUE(std::isnan(got[i])) << "lane " << i << " scalar=NaN but vector=" << got[i];
    } else {
      EXPECT_LT(std::fabs(got[i] - want), 1e-9) << "lane " << i;
    }
  }
}

TEST(SimdBatchNaN, GreeksBatchAgreesWithScalarOnNaNLanes) {
  const auto rows = adversarial_rows();
  const std::size_t n = rows.size();
  std::vector<double> F(n), K(n), T(n), sig(n), r(n), df(n);
  std::vector<Side> side(n);
  for (std::size_t i = 0; i < n; ++i) {
    F[i] = rows[i].F; K[i] = rows[i].K; T[i] = rows[i].T;
    sig[i] = rows[i].sigma; r[i] = 0.02; df[i] = rows[i].df; side[i] = rows[i].side;
  }
  std::vector<Greeks> got(n);
  std::vector<double> price(n, 0.0);
  simd::black76_greeks_batch(F.data(), K.data(), T.data(), sig.data(), r.data(), df.data(),
                             side.data(), got.data(), price.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks want = black76_greeks(F[i], K[i], T[i], sig[i], r[i], df[i], side[i]);
    if (std::isnan(want.price)) {
      EXPECT_TRUE(std::isnan(price[i])) << "lane " << i << " scalar price=NaN vector=" << price[i];
      EXPECT_TRUE(std::isnan(got[i].delta)) << "lane " << i << " scalar delta=NaN";
    } else {
      EXPECT_LT(std::fabs(price[i] - want.price), 1e-9) << "lane " << i;
    }
  }
}

} // namespace
} // namespace atx::vol
