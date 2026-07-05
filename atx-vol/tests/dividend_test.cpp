#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/dividend.hpp"
#include "atx/vol/types.hpp"

// Coverage for the hybrid dividend forward + European put-call-parity borrow
// implication (atx/vol/dividend.hpp). The escrowed-cash reference is
// forward_div_corrected (curve.hpp); Black-76 supplies co-terminal
// call/put prices that satisfy European PCP exactly for the inversion test.

namespace {

using atx::vol::black76_price;
using atx::vol::CoTermQuote;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::forward_div_corrected;
using atx::vol::hybrid_forward;
using atx::vol::HybridDivParams;
using atx::vol::imply_borrow_european_pcp;
using atx::vol::imply_forward_atm_pcp;
using atx::vol::Side;

// Year-fraction → epoch-ns (365.25-day year, matching forward_div_corrected).
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;
[[nodiscard]] std::int64_t years_to_ns(double y) {
  return static_cast<std::int64_t>(y * kYearNs);
}

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

// ── Hybrid forward ────────────────────────────────────────────────────────

TEST(HybridDiv, BlendZeroBorrowZero_ReproducesForwardDivCorrected) {
  const Scenario sc;
  // prop_div_yield deliberately non-zero: at blend == 0 it must be inert.
  const HybridDivParams hyb{/*prop_div_yield=*/0.05, /*blend=*/0.0};

  const double f_hyb = hybrid_forward(sc.S, sc.r, /*borrow=*/0.0, sc.T, sc.divs,
                                      sc.expiry_ns, sc.now_ns, hyb);
  const double f_ref =
      forward_div_corrected(sc.S, sc.r, sc.T, sc.divs, sc.expiry_ns, sc.now_ns);

  EXPECT_NEAR(f_hyb, f_ref, 1e-12);
}

TEST(HybridDiv, BorrowIncrease_StrictlyLowersForward) {
  const Scenario sc;
  const HybridDivParams hyb{/*prop_div_yield=*/0.02, /*blend=*/0.5};

  double prev = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= 40; ++i) {
    const double borrow = -0.10 + 0.005 * static_cast<double>(i); // −0.10 … +0.10
    const double f = hybrid_forward(sc.S, sc.r, borrow, sc.T, sc.divs,
                                    sc.expiry_ns, sc.now_ns, hyb);
    EXPECT_LT(f, prev) << "not strictly decreasing at borrow=" << borrow;
    prev = f;
  }
}

TEST(HybridDiv, BlendOne_EqualsProportionalForward) {
  const Scenario sc;
  const double borrow = 0.01;
  const HybridDivParams hyb{/*prop_div_yield=*/0.02, /*blend=*/1.0};

  const double f = hybrid_forward(sc.S, sc.r, borrow, sc.T, sc.divs, sc.expiry_ns,
                                  sc.now_ns, hyb);
  // Pure proportional: cash dividends drop out entirely.
  const double expected =
      sc.S * std::exp((sc.r - hyb.prop_div_yield - borrow) * sc.T);
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

  const double f_all = hybrid_forward(sc.S, sc.r, borrow, sc.T, all,
                                      sc.expiry_ns, sc.now_ns, hyb);
  const double f_mid = hybrid_forward(sc.S, sc.r, borrow, sc.T, mid,
                                      sc.expiry_ns, sc.now_ns, hyb);
  EXPECT_NEAR(f_all, f_mid, 1e-9);
}

TEST(HybridDiv, BadInputs_ReturnNaN) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.5};
  EXPECT_TRUE(std::isnan(hybrid_forward(-1.0, sc.r, 0.0, sc.T, sc.divs,
                                        sc.expiry_ns, sc.now_ns, hyb)));
  EXPECT_TRUE(std::isnan(hybrid_forward(sc.S, sc.r, 0.0, /*T=*/0.0, sc.divs,
                                        sc.expiry_ns, sc.now_ns, hyb)));
}

// ── European PCP borrow implication ───────────────────────────────────────

TEST(ImplyBorrowPcp, RecoversInjectedBorrow) {
  const Scenario sc;
  const HybridDivParams hyb{/*prop_div_yield=*/0.02, /*blend=*/0.4};
  const double K = 100.0;
  const double sigma = 0.25;
  const double b_true = 0.0150;

  const double f_true = hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs,
                                       sc.expiry_ns, sc.now_ns, hyb);
  const double df = std::exp(-sc.r * sc.T);
  const double call = black76_price(f_true, K, sc.T, sigma, df, Side::Call);
  const double put = black76_price(f_true, K, sc.T, sigma, df, Side::Put);

  const auto res = imply_borrow_european_pcp(call, put, sc.S, K, sc.T, sc.r,
                                             sc.divs, sc.expiry_ns, sc.now_ns, hyb);
  ASSERT_TRUE(res.has_value());
  EXPECT_NEAR(*res, b_true, 1e-6);
}

TEST(ImplyBorrowPcp, RootOutsideBracket_ReturnsOutOfRange) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.4};
  const double K = 100.0;
  const double sigma = 0.25;

  // Build prices at borrow = 0, then search a bracket that excludes it.
  const double f0 = hybrid_forward(sc.S, sc.r, 0.0, sc.T, sc.divs, sc.expiry_ns,
                                   sc.now_ns, hyb);
  const double df = std::exp(-sc.r * sc.T);
  const double call = black76_price(f0, K, sc.T, sigma, df, Side::Call);
  const double put = black76_price(f0, K, sc.T, sigma, df, Side::Put);

  const auto res =
      imply_borrow_european_pcp(call, put, sc.S, K, sc.T, sc.r, sc.divs,
                                sc.expiry_ns, sc.now_ns, hyb, /*b_lo=*/0.10,
                                /*b_hi=*/0.50);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(ImplyBorrowPcp, InvalidBracket_ReturnsInvalidArgument) {
  const Scenario sc;
  const HybridDivParams hyb{0.02, 0.4};
  const auto res =
      imply_borrow_european_pcp(1.0, 1.0, sc.S, 100.0, sc.T, sc.r, sc.divs,
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
