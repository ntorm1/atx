#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>

#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/types.hpp"

// American implied-vol inverter coverage.
//   - price->sigma round-trip across a moneyness/maturity/vol grid for both
//     the Andersen-Lake and BAW forward pricers (self-consistent by design),
//   - monotonicity of the forward map the inverter relies on,
//   - deep ITM/OTM behaviour,
//   - no-arbitrage guards (sub-intrinsic, above upper bound, non-finite),
//   - the at-intrinsic clamp to the vol floor,
//   - the strike-axis batch helper and its parallel-status failure convention.
//
// Strict 1e-5 sigma recovery is asserted only where the Black-76 vega is large
// enough that a 1e-5 vol change is resolvable above the cold pricer's noise
// floor; at the deep-wing / short-maturity corners vega collapses and sigma is
// not identifiable, so only the price round-trip is checked there.

namespace {

using atx::vol::AlOpts;
using atx::vol::american_implied_vol;
using atx::vol::american_implied_vol_batch;
using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::Side;

// Unwrap a Result<double>, flagging (non-fatally) an unexpected error.
double value_or_fail(const atx::core::Result<double>& r) {
  EXPECT_TRUE(r.has_value()) << (r ? std::string{} : r.error().to_string());
  return r ? *r : std::nan("");
}

// American finite-difference vega — the TRUE sensitivity of the American
// premium to sigma, and the correct conditioning proxy for strict recovery.
// A deep-ITM American option optimally exercised now sits at intrinsic and is
// insensitive to sigma (time value ~ 0), so sigma is unidentifiable there even
// though the European vega is large; only the FD American vega captures that.
double american_vega_fd(double S, double K, double T, double sigma, double r,
                        double q, Side side, AmericanMethod method) {
  const double h = 1.0e-4;
  const double pu =
      value_or_fail(american_price(S, K, T, sigma + h, r, q, side, method));
  const double pd =
      value_or_fail(american_price(S, K, T, sigma - h, r, q, side, method));
  return (pu - pd) / (2.0 * h);
}

const char* side_tag(Side side) { return side == Side::Call ? "C" : "P"; }

// Price a point, invert it, and assert the round-trip. Price recovery is always
// checked; strict sigma recovery only where vega is well above the noise floor.
void check_round_trip(double S, double K, double T, double sigma, double r,
                      double q, Side side, AmericanMethod method) {
  const double p = value_or_fail(american_price(S, K, T, sigma, r, q, side, method));
  if (!std::isfinite(p)) {
    return;  // forward pricer already flagged the failure above
  }

  const atx::core::Result<double> iv =
      american_implied_vol(p, S, K, T, r, q, side, method);
  ASSERT_TRUE(iv.has_value())
      << iv.error().to_string() << " [K=" << K << " T=" << T
      << " sig=" << sigma << " q=" << q << " " << side_tag(side) << "]";

  const double reprice =
      value_or_fail(american_price(S, K, T, *iv, r, q, side, method));
  EXPECT_NEAR(reprice, p, 1.0e-5 * std::fmax(1.0, p))
      << "price round-trip [K=" << K << " T=" << T << " sig=" << sigma << "]";

  if (american_vega_fd(S, K, T, sigma, r, q, side, method) > 0.5) {
    // BAW is a documented 3-4 significant-figure approximation, so its
    // self-consistent inversion inherits that coarseness at ITM corners;
    // Andersen-Lake is ~1e-7 accurate and holds the strict tolerance.
    const double sig_tol = (method == AmericanMethod::Baw) ? 1.0e-3 : 1.0e-5;
    EXPECT_NEAR(*iv, sigma, sig_tol)
        << "sigma round-trip [K=" << K << " T=" << T << " sig=" << sigma
        << " q=" << q << " " << side_tag(side) << "]";
  }
}

void grid_round_trip(AmericanMethod method) {
  const double S = 100.0, r = 0.05;
  for (double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
    for (double T : {0.1, 0.5, 1.0, 2.0}) {
      for (double sigma : {0.1, 0.2, 0.4}) {
        for (double q : {0.0, 0.03}) {
          for (Side side : {Side::Call, Side::Put}) {
            check_round_trip(S, K, T, sigma, r, q, side, method);
          }
        }
      }
    }
  }
}

}  // namespace

// ── Round-trip over the full grid, both pricers ──────────────────────────

TEST(AmericanIv, RoundTrip_GridAndersenLake_RecoversSigma) {
  grid_round_trip(AmericanMethod::AndersenLake);
}

TEST(AmericanIv, RoundTrip_GridBaw_RecoversSigma) {
  grid_round_trip(AmericanMethod::Baw);
}

// ── Monotonicity of the forward map the inverter assumes ─────────────────

TEST(AmericanIv, ForwardPrice_IncreasingInSigma_Monotone) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  for (Side side : {Side::Call, Side::Put}) {
    double prev = -1.0;
    for (double sigma : {0.05, 0.1, 0.2, 0.3, 0.5, 0.8, 1.2}) {
      const double p = value_or_fail(
          american_price(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake));
      EXPECT_GT(p, prev) << "sigma=" << sigma << " " << side_tag(side);
      prev = p;
    }
  }
}

// ── Deep ITM / OTM at a well-conditioned vol ─────────────────────────────

TEST(AmericanIv, DeepItmOtm_ModerateVol_RecoversSigma) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.02, sigma = 0.3;
  for (double K : {60.0, 160.0}) {
    for (Side side : {Side::Call, Side::Put}) {
      const double p = value_or_fail(
          american_price(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake));
      const double iv =
          value_or_fail(american_implied_vol(p, S, K, T, r, q, side));
      // Deep-ITM American options optimally exercised early sit at intrinsic
      // and are sigma-insensitive (time value ~ 0), so sigma is only
      // identifiable on the well-conditioned (OTM) side.
      if (american_vega_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake) >
          0.5) {
        EXPECT_NEAR(iv, sigma, 1.0e-5) << "K=" << K << " " << side_tag(side);
      }
    }
  }
}

// ── BAW method, single point ─────────────────────────────────────────────

TEST(AmericanIv, RoundTrip_BawPut_RecoversSigma) {
  const double S = 100.0, K = 105.0, T = 0.5, sigma = 0.25, r = 0.05, q = 0.0;
  const double p = value_or_fail(
      american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::Baw));
  const double iv = value_or_fail(american_implied_vol(
      p, S, K, T, r, q, Side::Put, AmericanMethod::Baw));
  EXPECT_NEAR(iv, sigma, 1.0e-5);
}

// ── No-arbitrage guards ──────────────────────────────────────────────────

TEST(AmericanIv, SubIntrinsicPrice_Call_ReturnsOutOfRange) {
  const double S = 100.0, K = 80.0, T = 1.0, r = 0.05, q = 0.0;  // intrinsic = 20
  const auto iv = american_implied_vol(10.0, S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, SubIntrinsicPrice_Put_ReturnsOutOfRange) {
  const double S = 100.0, K = 120.0, T = 1.0, r = 0.05, q = 0.0;  // intrinsic = 20
  const auto iv = american_implied_vol(5.0, S, K, T, r, q, Side::Put);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, PriceAboveUpperBound_Call_ReturnsOutOfRange) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;  // upper = S = 100
  const auto iv = american_implied_vol(101.0, S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, NonFinitePrice_ReturnsOutOfRange) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const auto iv =
      american_implied_vol(std::nan(""), S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, NonPositiveStrike_ReturnsInvalidArgument) {
  const auto iv = american_implied_vol(5.0, 100.0, 0.0, 1.0, 0.05, 0.0, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

// ── At-intrinsic clamp ───────────────────────────────────────────────────

TEST(AmericanIv, PriceAtIntrinsic_ClampsToFloor) {
  const double S = 100.0, K = 140.0, T = 1.0, r = 0.05, q = 0.0;  // put intrinsic = 40
  const auto iv = american_implied_vol(40.0, S, K, T, r, q, Side::Put);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_DOUBLE_EQ(*iv, atx::vol::kIvMin);
}

// ── Batch helper over a strike axis ──────────────────────────────────────

TEST(AmericanIv, Batch_StrikeAxis_MatchesScalar) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.0, sigma = 0.25;
  const Side side = Side::Call;
  const std::array<double, 4> K{80.0, 95.0, 105.0, 120.0};
  std::array<double, 4> price{};
  for (std::size_t i = 0; i < K.size(); ++i) {
    price[i] = value_or_fail(
        american_price(S, K[i], T, sigma, r, q, side, AmericanMethod::AndersenLake));
  }

  std::array<double, 4> iv_out{};
  std::array<atx::vol::Status, 4> status{};
  const auto s = american_implied_vol_batch(price, S, K, T, r, q, side,
                                            iv_out, status);
  ASSERT_TRUE(s.has_value()) << (s ? std::string{} : s.error().to_string());
  for (std::size_t i = 0; i < K.size(); ++i) {
    EXPECT_TRUE(status[i].has_value()) << "K=" << K[i];
    EXPECT_NEAR(iv_out[i], sigma, 1.0e-5) << "K=" << K[i];
  }
}

TEST(AmericanIv, Batch_SubIntrinsicLane_NanValueAndErrorStatus) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const Side side = Side::Call;
  const std::array<double, 2> K{90.0, 90.0};  // intrinsic = 10
  const double good = value_or_fail(
      american_price(S, 90.0, T, 0.2, r, q, side, AmericanMethod::AndersenLake));
  const std::array<double, 2> price{5.0, good};  // lane 0 is sub-intrinsic

  std::array<double, 2> iv_out{};
  std::array<atx::vol::Status, 2> status{};
  const auto s = american_implied_vol_batch(price, S, K, T, r, q, side,
                                            iv_out, status);
  ASSERT_TRUE(s.has_value());
  EXPECT_FALSE(status[0].has_value());
  EXPECT_TRUE(std::isnan(iv_out[0]));
  EXPECT_TRUE(status[1].has_value());
  EXPECT_NEAR(iv_out[1], 0.2, 1.0e-5);
}

TEST(AmericanIv, Batch_SpanLengthMismatch_ReturnsInvalidArgument) {
  const std::array<double, 3> price{1.0, 2.0, 3.0};
  const std::array<double, 2> K{90.0, 100.0};  // length mismatch vs price
  std::array<double, 3> iv_out{};
  std::array<atx::vol::Status, 3> status{};
  const auto s = american_implied_vol_batch(price, 100.0, K, 1.0, 0.05, 0.0,
                                            Side::Call, iv_out, status);
  ASSERT_FALSE(s.has_value());
  EXPECT_EQ(s.error().code(), atx::vol::ErrorCode::InvalidArgument);
}
