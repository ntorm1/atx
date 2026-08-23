#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/dividend.hpp"
#include "atx/vol/api/pricing/rates_curve.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/core/vol_time.hpp"      // TimeSpec, time_to_expiry_years,
                                              // kCalendarYearNs (the two clocks)
#include "atx/vol/api/marketdata/data.hpp"    // iso_to_ns, expiry_instant_ns

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


// ── The clock the dividend WINDOW is decided on (SR-DIVS) ───────────────────
//
// `forward_div_corrected` computes each dividend's `t_i` on the CALENDAR clock
// (it discounts cash, which no vol clock changes) but is handed the board's own
// `T`, which under `TimeConvention::VolTime` is on a DIFFERENT clock. A
// `t_i > T` screen therefore compares two clocks, and vol time compresses a
// weekend while a calendar `t_i` does not — so a Monday ex-date off a Friday
// snapshot was silently filtered out. Under Calendar365 that screen only ever
// duplicated the `ex_date_ns > expiry_ns` instant test above it, which is why
// nothing ever caught it: the code was dead until a discrete schedule and a
// vol-time board met. These two cases pin the live combination.
//
// Real instants, because the defect is a calendar property: 2026-08-14 is a
// FRIDAY and 2026-08-17 the following MONDAY, which SPY/QQQ/IWM list a weekly
// expiry on — so a Monday upper-bracket ex-date is an ordinary output of the
// schedule emitter, not a contrived one.

namespace voltime_window {

using atx::vol::expiry_instant_ns;
using atx::vol::iso_to_ns;
using atx::vol::kCalendarYearNs;
using atx::vol::TimeConvention;
using atx::vol::TimeSpec;
using atx::vol::time_to_expiry_years;

struct WeekendCase {
  std::int64_t now_ns = iso_to_ns("2026-08-14T19:55:00Z");     // Friday pre-close
  std::int64_t expiry_ns = expiry_instant_ns("2026-08-17");    // Monday weekly, 16:00 ET
  std::int64_t ex_ns = iso_to_ns("2026-08-17");                // Monday, midnight UTC
  double S = 100.0;
  double r = 0.04;
  double amount = 1.25;
};

// `T` on the vol clock, plus the calendar `t_i` of the Monday dividend. Both are
// ASSERTed by the callers, not merely computed: the case only bites while
// `t_vol < t_i`, and if the clock is ever retuned so that stops holding, the
// tests must say so rather than quietly passing on a scenario that no longer
// reproduces anything.
struct Clocks {
  double t_vol = 0.0;
  double t_cal = 0.0;
  double t_i = 0.0;
};

[[nodiscard]] Clocks clocks_for(const WeekendCase &c) {
  TimeSpec vol;
  vol.convention = TimeConvention::VolTime;
  const auto years = time_to_expiry_years(c.now_ns, c.expiry_ns, vol);
  EXPECT_TRUE(years.has_value())
      << "vol-time calendar must cover 2026: " << (years.has_value() ? "" : years.error().to_string());
  Clocks out;
  out.t_vol = years.value_or(0.0);
  out.t_cal = static_cast<double>(c.expiry_ns - c.now_ns) / kCalendarYearNs;
  out.t_i = static_cast<double>(c.ex_ns - c.now_ns) / kCalendarYearNs;
  return out;
}

} // namespace voltime_window

TEST(DividendForward, VolTimeShortTKeepsAMondayExDateOffAFridaySnapshot) {
  const voltime_window::WeekendCase c;
  const voltime_window::Clocks clk = voltime_window::clocks_for(c);

  // The dividend is INSIDE the option window on instants...
  ASSERT_GE(c.ex_ns, c.now_ns);
  ASSERT_LE(c.ex_ns, c.expiry_ns);
  // ...and OUTSIDE it on the mixed-clock comparison the old screen made. Without
  // this the case proves nothing.
  ASSERT_LT(clk.t_vol, clk.t_i) << "vol-time T no longer compresses this weekend "
                                   "below the Monday ex-date; pick another case";
  ASSERT_LE(clk.t_i, clk.t_cal);

  const std::vector<DividendEvent> divs{{c.ex_ns, c.amount}};
  const double f = forward_div_corrected(c.S, c.r, clk.t_vol, divs, c.expiry_ns, c.now_ns);

  const double expected =
      (c.S - c.amount * std::exp(-c.r * clk.t_i)) * std::exp(c.r * clk.t_vol);
  EXPECT_NEAR(f, expected, 1.0e-12);
  // Stated as a property too, so a future rewrite that reintroduces the filter
  // fails on meaning rather than on an arithmetic identity: the forward of a
  // dividend-paying name must sit BELOW the pure-carry forward.
  EXPECT_LT(f, c.S * std::exp(c.r * clk.t_vol));
}

TEST(DividendForward, VolTimeShortTJacobianKeepsTheSameMondayExDate) {
  const voltime_window::WeekendCase c;
  const voltime_window::Clocks clk = voltime_window::clocks_for(c);
  ASSERT_LT(clk.t_vol, clk.t_i);

  // The analytic ∂F/∂D must zero exactly the events the forward drops and no
  // others, or it stops being the derivative of the thing it is paired with.
  const std::vector<DividendEvent> divs{{c.ex_ns, c.amount}};
  const HybridDivParams hyb{/*prop_div_yield=*/0.0, /*blend=*/0.0};
  double jac[1] = {0.0};
  atx::vol::hybrid_forward_div_jacobian(c.r, /*borrow=*/0.0, clk.t_vol, divs, c.expiry_ns,
                                        c.now_ns, hyb, jac);
  EXPECT_NEAR(jac[0], -std::exp(c.r * (clk.t_vol - clk.t_i)), 1.0e-12);

  // And it agrees with a central-difference bump of the forward it differentiates.
  const double h = 1.0e-6;
  const std::vector<DividendEvent> up{{c.ex_ns, c.amount + h}};
  const std::vector<DividendEvent> down{{c.ex_ns, c.amount - h}};
  const double f_up = hybrid_forward(c.S, c.r, 0.0, clk.t_vol, up, c.expiry_ns, c.now_ns, hyb);
  const double f_dn = hybrid_forward(c.S, c.r, 0.0, clk.t_vol, down, c.expiry_ns, c.now_ns, hyb);
  EXPECT_NEAR(jac[0], (f_up - f_dn) / (2.0 * h), 1.0e-6);
}

} // namespace
