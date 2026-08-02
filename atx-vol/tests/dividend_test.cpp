#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/dividend.hpp"
#include "atx/vol/rates_curve.hpp"
#include "atx/vol/types.hpp"

// Coverage for the hybrid dividend forward + European put-call-parity borrow
// implication (atx/vol/dividend.hpp). The escrowed-cash reference is
// forward_div_corrected (rates_curve.hpp); Black-76 supplies co-terminal
// call/put prices that satisfy European PCP exactly for the inversion test.

namespace {

using atx::vol::black76_price;
using atx::vol::CoTermQuote;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::forward_div_corrected;
using atx::vol::hybrid_forward;
using atx::vol::hybrid_forward_base;
using atx::vol::hybrid_forward_from_base;
using atx::vol::HybridDivParams;
using atx::vol::imply_borrow_european_pcp;
using atx::vol::imply_borrow_european_pcp_from_base;
using atx::vol::imply_forward_atm_pcp;
using atx::vol::Side;

// Year-fraction → epoch-ns (365.25-day year, matching forward_div_corrected).
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;
[[nodiscard]] std::int64_t years_to_ns(double y) { return static_cast<std::int64_t>(y * kYearNs); }

// A canonical scenario: spot 100, 3% rate, 1y expiry, one 1.20 cash dividend
// ex-6-months (inside the option window).
struct Scenario {
  double S = 100.0;
  double r = 0.03;
  double T = 1.0;
  std::int64_t now_ns = 0;
  std::int64_t expiry_ns = years_to_ns(1.0);
  std::vector<DividendEvent> divs{{years_to_ns(0.5), 1.20}};
};

[[nodiscard]] double imply_borrow_bisection_reference(double call_price, double put_price, double S,
                                                      double K, double T, double r,
                                                      const std::vector<DividendEvent> &cash_divs,
                                                      std::int64_t expiry_ns,
                                                      std::int64_t now_ts_ns,
                                                      const HybridDivParams &hyb) {
  constexpr int kIterations = 80;
  double lo = -0.5;
  double hi = 0.5;
  const double lhs = call_price - put_price;
  const double df = std::exp(-r * T);
  const auto objective = [&](double borrow) {
    const double forward = hybrid_forward(S, r, borrow, T, cash_divs, expiry_ns, now_ts_ns, hyb);
    return df * (forward - K) - lhs;
  };
  double g_lo = objective(lo);
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const double mid = 0.5 * (lo + hi);
    const double g_mid = objective(mid);
    if ((g_mid > 0.0) == (g_lo > 0.0)) {
      lo = mid;
      g_lo = g_mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

// ── Hybrid forward ────────────────────────────────────────────────────────

TEST(HybridDiv, BlendZeroBorrowZero_ReproducesForwardDivCorrected) {
  const Scenario sc;
  // prop_div_yield deliberately non-zero: at blend == 0 it must be inert.
  const HybridDivParams hyb{/*prop_div_yield=*/0.05, /*blend=*/0.0};

  const double f_hyb =
      hybrid_forward(sc.S, sc.r, /*borrow=*/0.0, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
  const double f_ref = forward_div_corrected(sc.S, sc.r, sc.T, sc.divs, sc.expiry_ns, sc.now_ns);

  EXPECT_NEAR(f_hyb, f_ref, 1e-12);
}

TEST(HybridDiv, BorrowIncrease_StrictlyLowersForward) {
  const Scenario sc;
  const HybridDivParams hyb{/*prop_div_yield=*/0.02, /*blend=*/0.5};

  double prev = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= 40; ++i) {
    const double borrow = -0.10 + 0.005 * static_cast<double>(i); // −0.10 … +0.10
    const double f =
        hybrid_forward(sc.S, sc.r, borrow, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
    EXPECT_LT(f, prev) << "not strictly decreasing at borrow=" << borrow;
    prev = f;
  }
}

TEST(HybridDiv, BlendOne_EqualsProportionalForward) {
  const Scenario sc;
  const double borrow = 0.01;
  const HybridDivParams hyb{/*prop_div_yield=*/0.02, /*blend=*/1.0};

  const double f = hybrid_forward(sc.S, sc.r, borrow, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
  // Pure proportional: cash dividends drop out entirely.
  const double expected = sc.S * std::exp((sc.r - hyb.prop_div_yield - borrow) * sc.T);
  EXPECT_NEAR(f, expected, 1e-9);
}

TEST(HybridDiv, CashDivsOutsideWindow_Ignored) {
  const Scenario sc;
  const HybridDivParams hyb{/*prop_div_yield=*/0.015, /*blend=*/0.3};
  const double borrow = 0.01;

  // One dividend already paid (ex < now), one inside the window, one after
  // expiry. Only the middle one may affect the forward.
  const std::vector<DividendEvent> all{
      {years_to_ns(-0.20), 2.00}, // paid before now
      {years_to_ns(0.50), 1.20},  // inside window
      {years_to_ns(1.50), 3.00},  // after expiry
  };
  const std::vector<DividendEvent> mid{{years_to_ns(0.50), 1.20}};

  const double f_all = hybrid_forward(sc.S, sc.r, borrow, sc.T, all, sc.expiry_ns, sc.now_ns, hyb);
  const double f_mid = hybrid_forward(sc.S, sc.r, borrow, sc.T, mid, sc.expiry_ns, sc.now_ns, hyb);
  EXPECT_NEAR(f_all, f_mid, 1e-9);
}

TEST(HybridDiv, BadInputs_ReturnNaN) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.5};
  EXPECT_TRUE(
      std::isnan(hybrid_forward(-1.0, sc.r, 0.0, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb)));
  EXPECT_TRUE(std::isnan(
      hybrid_forward(sc.S, sc.r, 0.0, /*T=*/0.0, sc.divs, sc.expiry_ns, sc.now_ns, hyb)));
}

// ── European PCP borrow implication ───────────────────────────────────────

TEST(ImplyBorrowPcp, RecoversInjectedBorrow) {
  const Scenario sc;
  const HybridDivParams hyb{/*prop_div_yield=*/0.02, /*blend=*/0.4};
  const double K = 100.0;
  const double sigma = 0.25;
  const double b_true = 0.0150;

  const double f_true =
      hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
  const double df = std::exp(-sc.r * sc.T);
  const double call = black76_price(f_true, K, sc.T, sigma, df, Side::Call);
  const double put = black76_price(f_true, K, sc.T, sigma, df, Side::Put);

  const auto res = imply_borrow_european_pcp(call, put, sc.S, K, sc.T, sc.r, sc.divs, sc.expiry_ns,
                                             sc.now_ns, hyb);
  ASSERT_TRUE(res.has_value());
  EXPECT_NEAR(*res, b_true, 1e-6);
}

TEST(ImplyBorrowPcp, ClosedFormMatchesBisectionAcrossFixtureSweep) {
  const Scenario sc;
  constexpr double kSigma = 0.27;
  const std::vector<double> borrows{-0.35, -0.05, 0.0, 0.08, 0.35};
  const std::vector<double> strikes{75.0, 100.0, 130.0};
  const std::vector<HybridDivParams> hybrids{
      HybridDivParams{0.00, 0.00}, HybridDivParams{0.02, 0.40}, HybridDivParams{0.04, 1.00}};

  for (const HybridDivParams &hyb : hybrids) {
    const double base =
        hybrid_forward_base(sc.S, sc.r, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
    ASSERT_TRUE(std::isfinite(base));
    ASSERT_GT(base, 0.0);
    for (const double borrow : borrows) {
      const double forward = hybrid_forward_from_base(base, borrow, sc.T);
      EXPECT_DOUBLE_EQ(
          forward, hybrid_forward(sc.S, sc.r, borrow, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb));
      const double df = std::exp(-sc.r * sc.T);
      for (const double strike : strikes) {
        const double call = black76_price(forward, strike, sc.T, kSigma, df, Side::Call);
        const double put = black76_price(forward, strike, sc.T, kSigma, df, Side::Put);
        const double reference = imply_borrow_bisection_reference(
            call, put, sc.S, strike, sc.T, sc.r, sc.divs, sc.expiry_ns, sc.now_ns, hyb);

        const auto from_base =
            imply_borrow_european_pcp_from_base(call, put, strike, sc.T, sc.r, base);
        const auto public_api = imply_borrow_european_pcp(call, put, sc.S, strike, sc.T, sc.r,
                                                          sc.divs, sc.expiry_ns, sc.now_ns, hyb);
        ASSERT_TRUE(from_base.has_value());
        ASSERT_TRUE(public_api.has_value());
        EXPECT_NEAR(*from_base, borrow, 1.0e-8) << "K=" << strike;
        EXPECT_NEAR(*from_base, reference, 1.0e-8) << "borrow=" << borrow << " K=" << strike;
        EXPECT_NEAR(*public_api, reference, 1.0e-8) << "borrow=" << borrow << " K=" << strike;
      }
    }
  }
}

TEST(ImplyBorrowPcp, ClosedFormPreservesBracketAndDomainErrors) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.4};
  const double base = hybrid_forward_base(sc.S, sc.r, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
  ASSERT_GT(base, 0.0);

  const auto non_positive_forward =
      imply_borrow_european_pcp_from_base(-50.0, 50.0, 100.0, sc.T, /*r=*/0.0, base);
  ASSERT_FALSE(non_positive_forward.has_value());
  EXPECT_EQ(non_positive_forward.error().code(), ErrorCode::OutOfRange);

  const auto non_finite_base = imply_borrow_european_pcp_from_base(
      1.0, 1.0, 100.0, sc.T, sc.r, std::numeric_limits<double>::quiet_NaN());
  ASSERT_FALSE(non_finite_base.has_value());
  EXPECT_EQ(non_finite_base.error().code(), ErrorCode::InvalidArgument);

  const auto non_finite_tolerance = imply_borrow_european_pcp_from_base(
      1.0, 1.0, 100.0, sc.T, sc.r, base, -0.5, 0.5, std::numeric_limits<double>::infinity());
  ASSERT_FALSE(non_finite_tolerance.has_value());
  EXPECT_EQ(non_finite_tolerance.error().code(), ErrorCode::InvalidArgument);

  const auto non_finite_strike = imply_borrow_european_pcp_from_base(
      1.0, 1.0, std::numeric_limits<double>::infinity(), sc.T, sc.r, base);
  ASSERT_FALSE(non_finite_strike.has_value());
  EXPECT_EQ(non_finite_strike.error().code(), ErrorCode::InvalidArgument);

  const auto non_finite_tenor = imply_borrow_european_pcp_from_base(
      1.0, 1.0, 100.0, std::numeric_limits<double>::infinity(), -0.01, base);
  ASSERT_FALSE(non_finite_tenor.has_value());
  EXPECT_EQ(non_finite_tenor.error().code(), ErrorCode::InvalidArgument);
}

TEST(ImplyBorrowPcp, ClosedFormAcceptsBothBracketEndpoints) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.4};
  const double strike = 100.0;
  const double base = hybrid_forward_base(sc.S, sc.r, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
  ASSERT_GT(base, 0.0);

  for (const double borrow : {-0.5, 0.5}) {
    const double forward = hybrid_forward_from_base(base, borrow, sc.T);
    const double parity_difference = std::exp(-sc.r * sc.T) * (forward - strike);
    constexpr double kPut = 100.0;
    const auto result = imply_borrow_european_pcp_from_base(kPut + parity_difference, kPut, strike,
                                                            sc.T, sc.r, base);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(*result, borrow, 1.0e-12);
  }
}

TEST(ImplyBorrowPcp, ClosedFormRejectsRootOutsideBracketEvenWithinSolverTolerance) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.4};
  constexpr double kStrike = 100.0;
  constexpr double kTrueBorrow = 0.500001;
  constexpr double kPut = 100.0;
  const double base = hybrid_forward_base(sc.S, sc.r, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
  const double forward = hybrid_forward_from_base(base, kTrueBorrow, sc.T);
  const double parity_difference = std::exp(-sc.r * sc.T) * (forward - kStrike);

  const auto result = imply_borrow_european_pcp_from_base(kPut + parity_difference, kPut, kStrike,
                                                          sc.T, sc.r, base, -0.5, 0.5, 1.0e-4);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}

TEST(ImplyBorrowPcp, RootOutsideBracket_ReturnsOutOfRange) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.4};
  const double K = 100.0;
  const double sigma = 0.25;

  // Build prices at borrow = 0, then search a bracket that excludes it.
  const double f0 = hybrid_forward(sc.S, sc.r, 0.0, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, hyb);
  const double df = std::exp(-sc.r * sc.T);
  const double call = black76_price(f0, K, sc.T, sigma, df, Side::Call);
  const double put = black76_price(f0, K, sc.T, sigma, df, Side::Put);

  const auto res = imply_borrow_european_pcp(call, put, sc.S, K, sc.T, sc.r, sc.divs, sc.expiry_ns,
                                             sc.now_ns, hyb, /*b_lo=*/0.10,
                                             /*b_hi=*/0.50);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(ImplyBorrowPcp, InvalidBracket_ReturnsInvalidArgument) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.4};
  const auto res = imply_borrow_european_pcp(1.0, 1.0, sc.S, 100.0, sc.T, sc.r, sc.divs,
                                             sc.expiry_ns, sc.now_ns, hyb, /*b_lo=*/0.5,
                                             /*b_hi=*/-0.5);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── Near-ATM PCP forward strip convenience ────────────────────────────────

TEST(ImpliedForwardStrip, NearAtmMean_RecoversMarketForward) {
  const double S = 100.0, T = 0.75, r = 0.03;
  const double f_market = 101.25;
  const double df = std::exp(-r * T);

  // Each strike's mids satisfy PCP at f_market: C − P = df·(F − K).
  std::vector<CoTermQuote> strip;
  for (double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
    CoTermQuote q;
    q.strike = K;
    q.put_mid = 1.0;
    q.call_mid = 1.0 + df * (f_market - K);
    strip.push_back(q);
  }

  const auto res = imply_forward_atm_pcp(strip, S, T, r, /*n_atm=*/3);
  ASSERT_TRUE(res.has_value());
  EXPECT_NEAR(*res, f_market, 1e-9);
}

TEST(ImpliedForwardStrip, EmptyStrip_ReturnsInvalidArgument) {
  const auto res = imply_forward_atm_pcp({}, 100.0, 0.5, 0.03);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
