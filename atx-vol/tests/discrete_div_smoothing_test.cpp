#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"

// Coverage for the opt-in lattice schemes of the dated-cash route
// (api/pricing/american.hpp, DiscreteDivScheme; src/pricing/american_discrete_div.cpp).
//
// What the schemes are FOR is strike shape: a chain priced strike by strike at
// per-strike vols must pass AmericanPriceSlice certification. So the central
// tests build a synthetic chain and hand its prices to the certifier with its
// real, fixed tolerance. The rest pins the contract: the default is the
// historical rollback bit for bit, every scheme converges to an independent
// reference for its own model, the BBS final step handles cash on the last two
// steps analytically, the European legs keep put-call parity, and the refusals
// are typed. Allocation is pinned in discrete_div_smoothing_alloc_test.cpp.

namespace {

using atx::vol::AmericanPriceBounds;
using atx::vol::AmericanPriceNode;
using atx::vol::AmericanPriceSlice;
using atx::vol::american_discrete_div_greeks;
using atx::vol::american_discrete_div_price;
using atx::vol::black76_price;
using atx::vol::CashDividend;
using atx::vol::DiscreteDivScheme;
using atx::vol::discrete_div_scheme_admits;
using atx::vol::ErrorCode;
using atx::vol::ExerciseStyle;
using atx::vol::Side;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

constexpr DiscreteDivScheme kAllSchemes[] = {
    DiscreteDivScheme::SplicedCrr, DiscreteDivScheme::SplicedBbs, DiscreteDivScheme::SplicedBbsr,
    DiscreteDivScheme::EscrowedBbs, DiscreteDivScheme::EscrowedLeisenReimer};

[[nodiscard]] bool escrowed(DiscreteDivScheme scheme) {
  return scheme == DiscreteDivScheme::EscrowedBbs ||
         scheme == DiscreteDivScheme::EscrowedLeisenReimer;
}

[[nodiscard]] double price(double S, double K, double T, double sigma, double r, double q,
                           Side side, std::span<const CashDividend> divs, int steps,
                           ExerciseStyle exercise, DiscreteDivScheme scheme) {
  const auto out = american_discrete_div_price(S, K, T, sigma, r, q, side, divs, steps, exercise,
                                               scheme);
  EXPECT_TRUE(out.has_value()) << atx::vol::to_string(scheme) << " K=" << K;
  return out.has_value() ? *out : kNaN;
}

// Black-Scholes with a continuous yield through the shipped Black-76 kernel.
[[nodiscard]] double bs_price(double S, double K, double T, double sigma, double r, double q,
                              Side side) {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T), side);
}

[[nodiscard]] double norm_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

// EXACT European value of the spliced (spot-shift) model with ONE cash dividend
// at t1: exp(-r t1) E[V_BS(max(S_t1 - D, 0), K, T - t1)], composite Simpson over
// the lognormal S_t1 (the reference discrete_div_american_test uses, extended to
// a continuous yield q).
[[nodiscard]] double spliced_european_one_dividend(double S, double K, double T, double sigma,
                                                   double r, double q, double D, double t1,
                                                   Side side) {
  constexpr int kNodes = 40000;
  constexpr double kZ = 10.0;
  const double h = 2.0 * kZ / static_cast<double>(kNodes);
  const double drift = (r - q - 0.5 * sigma * sigma) * t1;
  const double diffusion = sigma * std::sqrt(t1);
  const double inv_sqrt_2pi = 1.0 / std::sqrt(2.0 * std::acos(-1.0));
  const auto integrand = [&](double z) {
    const double post = std::max(S * std::exp(drift + diffusion * z) - D, 0.0);
    const double value = post > 0.0 ? bs_price(post, K, T - t1, sigma, r, q, side)
                                    : (side == Side::Put ? K * std::exp(-r * (T - t1)) : 0.0);
    return inv_sqrt_2pi * std::exp(-0.5 * z * z) * value;
  };
  double sum = integrand(-kZ) + integrand(kZ);
  for (int i = 1; i < kNodes; ++i) {
    sum += ((i % 2) != 0 ? 4.0 : 2.0) * integrand(-kZ + h * static_cast<double>(i));
  }
  return std::exp(-r * t1) * sum * h / 3.0;
}

// M(a, b; rho) by composite Simpson on the conditional decomposition.
[[nodiscard]] double bivariate_norm_cdf(double a, double b, double rho) {
  constexpr double kLo = -10.0;
  if (a <= kLo) {
    return 0.0;
  }
  constexpr int kNodes = 20000;
  const double s = std::sqrt(1.0 - rho * rho);
  const double h = (a - kLo) / static_cast<double>(kNodes);
  const double inv_sqrt_2pi = 1.0 / std::sqrt(2.0 * std::acos(-1.0));
  const auto integrand = [&](double x) {
    return inv_sqrt_2pi * std::exp(-0.5 * x * x) * norm_cdf((b - rho * x) / s);
  };
  double sum = integrand(kLo) + integrand(a);
  for (int i = 1; i < kNodes; ++i) {
    sum += ((i % 2) != 0 ? 4.0 : 2.0) * integrand(kLo + h * static_cast<double>(i));
  }
  return sum * h / 3.0;
}

// Roll-Geske-Whaley: EXACT for one cash dividend on an American call under the
// ESCROWED model (q = 0) — the escrowed schemes' own model, so this is an
// independent closed-form reference for them (it is only a cross-model check
// for the spliced lattice in discrete_div_american_test.cpp).
[[nodiscard]] double rgw_american_call(double S, double K, double T, double sigma, double r,
                                       double D, double t1) {
  const double s_adj = S - D * std::exp(-r * t1);
  if (D <= K * (1.0 - std::exp(-r * (T - t1)))) {
    return bs_price(s_adj, K, T, sigma, r, 0.0, Side::Call);
  }
  double lo = 1.0e-8;
  double hi = 100.0 * (S + K);
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    (bs_price(mid, K, T - t1, sigma, r, 0.0, Side::Call) - (mid + D - K) > 0.0 ? lo : hi) = mid;
  }
  const double s_star = 0.5 * (lo + hi);
  const double sq_t = sigma * std::sqrt(T);
  const double sq_t1 = sigma * std::sqrt(t1);
  const double a1 = (std::log(s_adj / K) + (r + 0.5 * sigma * sigma) * T) / sq_t;
  const double a2 = a1 - sq_t;
  const double b1 = (std::log(s_adj / s_star) + (r + 0.5 * sigma * sigma) * t1) / sq_t1;
  const double b2 = b1 - sq_t1;
  const double rho = -std::sqrt(t1 / T);
  return s_adj * (norm_cdf(b1) + bivariate_norm_cdf(a1, -b1, rho)) -
         K * std::exp(-r * T) * bivariate_norm_cdf(a2, -b2, rho) -
         (K - D) * std::exp(-r * t1) * norm_cdf(b2);
}

// The dated-cash forward the escrowed schemes' European converges to.
[[nodiscard]] double dated_forward(double S, double T, double r, double q,
                                   std::span<const CashDividend> divs) {
  double f = S * std::exp((r - q) * T);
  for (const CashDividend &dv : divs) {
    if (dv.tau > 0.0 && dv.tau <= T && dv.amount > 0.0) {
      f -= dv.amount * std::exp((r - q) * (T - dv.tau));
    }
  }
  return f;
}

// Black-76 implied vol by bisection, for building a curve that is convex in
// Black76 price space by construction.
[[nodiscard]] double black_iv(double price_target, double F, double K, double T, double df,
                              Side side) {
  double lo = 1.0e-4;
  double hi = 5.0;
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    (black76_price(F, K, T, mid, df, side) < price_target ? lo : hi) = mid;
  }
  return 0.5 * (lo + hi);
}

// ═══════════════════════════════════════════════════════════════════════════
//  The default is the historical rollback; names; determinism
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivScheme, DefaultArgumentIsSplicedCrrBitForBit) {
  const std::vector<CashDividend> divs{{0.10, 1.50}, {0.35, 1.60}, {0.60, 1.70}};
  for (const Side side : {Side::Call, Side::Put}) {
    for (const ExerciseStyle ex : {ExerciseStyle::American, ExerciseStyle::European}) {
      for (const double K : {60.0, 95.0, 100.0, 105.0, 160.0}) {
        const auto defaulted = american_discrete_div_price(100.0, K, 0.75, 0.30, 0.045, 0.01,
                                                           side, divs, 301, ex);
        const auto explicit_default = american_discrete_div_price(
            100.0, K, 0.75, 0.30, 0.045, 0.01, side, divs, 301, ex, DiscreteDivScheme::SplicedCrr);
        ASSERT_TRUE(defaulted.has_value());
        ASSERT_TRUE(explicit_default.has_value());
        EXPECT_EQ(*defaulted, *explicit_default);
      }
    }
  }
}

TEST(DiscreteDivScheme, GreeksPriceStaysTheSplicedCrrPrice) {
  const std::vector<CashDividend> divs{{0.20, 2.15}};
  const auto greeks =
      american_discrete_div_greeks(100.0, 100.0, 0.5, 0.25, 0.04, 0.0, Side::Put, divs, 301);
  ASSERT_TRUE(greeks.has_value());
  EXPECT_EQ(greeks->price, price(100.0, 100.0, 0.5, 0.25, 0.04, 0.0, Side::Put, divs, 301,
                                 ExerciseStyle::American, DiscreteDivScheme::SplicedCrr));
}

TEST(DiscreteDivScheme, NamesAreStable) {
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivScheme::SplicedCrr), "spliced");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivScheme::SplicedBbs), "spliced-bbs");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivScheme::SplicedBbsr), "spliced-bbsr");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivScheme::EscrowedBbs), "escrowed-bbs");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivScheme::EscrowedLeisenReimer), "escrowed-lr");
}

TEST(DiscreteDivScheme, RepeatedCallsAreBitIdentical) {
  const std::vector<CashDividend> divs{{0.05, 3.0}, {0.55, 3.0}};
  for (const DiscreteDivScheme scheme : kAllSchemes) {
    for (const Side side : {Side::Call, Side::Put}) {
      const double a = price(250.0, 240.0, 1.1, 0.33, 0.045, -0.004, side, divs, 301,
                             ExerciseStyle::American, scheme);
      const double b = price(250.0, 240.0, 1.1, 0.33, 0.045, -0.004, side, divs, 301,
                             ExerciseStyle::American, scheme);
      EXPECT_EQ(a, b) << atx::vol::to_string(scheme);
    }
  }
}

TEST(DiscreteDivScheme, LeisenReimerRunsAnEvenStepCountAtTheNextOdd) {
  const std::vector<CashDividend> divs{{0.30, 1.2}};
  for (const Side side : {Side::Call, Side::Put}) {
    EXPECT_EQ(price(100.0, 97.0, 0.8, 0.27, 0.04, 0.0, side, divs, 300, ExerciseStyle::American,
                    DiscreteDivScheme::EscrowedLeisenReimer),
              price(100.0, 97.0, 0.8, 0.27, 0.04, 0.0, side, divs, 301, ExerciseStyle::American,
                    DiscreteDivScheme::EscrowedLeisenReimer));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Typed refusals and degenerate inputs
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivScheme, RichardsonRefusesASingleStepAndTheRestPriceIt) {
  const std::vector<CashDividend> divs{{0.25, 1.0}};
  for (const DiscreteDivScheme scheme : kAllSchemes) {
    const auto out = american_discrete_div_price(100.0, 100.0, 0.5, 0.2, 0.03, 0.0, Side::Put,
                                                 divs, 1, ExerciseStyle::American, scheme);
    if (scheme == DiscreteDivScheme::SplicedBbsr) {
      ASSERT_FALSE(out.has_value());
      EXPECT_EQ(out.error().code(), ErrorCode::InvalidArgument);
      EXPECT_FALSE(discrete_div_scheme_admits(scheme, 1));
    } else {
      EXPECT_TRUE(out.has_value()) << atx::vol::to_string(scheme);
      EXPECT_TRUE(discrete_div_scheme_admits(scheme, 1));
    }
  }
  EXPECT_TRUE(discrete_div_scheme_admits(DiscreteDivScheme::SplicedBbsr, 2));
}

TEST(DiscreteDivScheme, AnUnknownSchemeValueIsRefused) {
  const auto bogus = static_cast<DiscreteDivScheme>(97);
  EXPECT_FALSE(discrete_div_scheme_admits(bogus, 301));
  const auto out = american_discrete_div_price(100.0, 100.0, 0.5, 0.2, 0.03, 0.0, Side::Call, {},
                                               301, ExerciseStyle::American, bogus);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), ErrorCode::InvalidArgument);
}

TEST(DiscreteDivScheme, CashThatExhaustsTheSpotIsOutOfRangeForTheEscrowedSchemes) {
  // 50 of cash on a 40 stock: no escrowed stock exists.
  const std::vector<CashDividend> ruinous{{0.2, 50.0}};
  for (const DiscreteDivScheme scheme :
       {DiscreteDivScheme::EscrowedBbs, DiscreteDivScheme::EscrowedLeisenReimer}) {
    const auto out = american_discrete_div_price(40.0, 40.0, 0.5, 0.3, 0.03, 0.0, Side::Put,
                                                 ruinous, 301, ExerciseStyle::American, scheme);
    ASSERT_FALSE(out.has_value()) << atx::vol::to_string(scheme);
    EXPECT_EQ(out.error().code(), ErrorCode::OutOfRange);
  }
  // The spliced lattices floor the post-dividend stock at zero and still price.
  EXPECT_TRUE(american_discrete_div_price(40.0, 40.0, 0.5, 0.3, 0.03, 0.0, Side::Put, ruinous,
                                          301, ExerciseStyle::American,
                                          DiscreteDivScheme::SplicedBbs)
                  .has_value());
}

TEST(DiscreteDivScheme, BbsrCoarseLatticeSetsTheProbabilityWindow) {
  // sigma sits between |r - q| sqrt(dt) at 100 steps and at 50: the fine
  // lattice is admissible, the coarse one is not, so BBSR refuses OutOfRange.
  const double carry = 0.5;
  const double sigma = carry * std::sqrt(1.0 / 75.0);
  ASSERT_TRUE(american_discrete_div_price(100.0, 100.0, 1.0, sigma, carry, 0.0, Side::Put, {},
                                          100, ExerciseStyle::American,
                                          DiscreteDivScheme::SplicedBbs)
                  .has_value());
  const auto out = american_discrete_div_price(100.0, 100.0, 1.0, sigma, carry, 0.0, Side::Put,
                                               {}, 100, ExerciseStyle::American,
                                               DiscreteDivScheme::SplicedBbsr);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(atx::vol::discrete_div_coarsest_steps(100, DiscreteDivScheme::SplicedBbsr), 50);
  EXPECT_EQ(atx::vol::discrete_div_coarsest_steps(100, DiscreteDivScheme::EscrowedBbs), 100);
}

TEST(DiscreteDivScheme, LeisenReimerPricesAStrikeTooFarForItsCentredTree) {
  // sigma*sqrt(T) ~ 4e-4 puts K = 90 some 250 standard deviations in the money:
  // the Peizer-Pratt probability rounds to 1, and the CRR geometry prices it.
  const std::vector<CashDividend> divs{{0.05, 0.5}};
  const double T = 0.18;
  const double sigma = 1.0e-3;
  const double F = dated_forward(100.0, T, 0.04, 0.0, divs);
  const double got = price(100.0, 90.0, T, sigma, 0.04, 0.0, Side::Call, divs, 301,
                           ExerciseStyle::European, DiscreteDivScheme::EscrowedLeisenReimer);
  EXPECT_NEAR(got, std::exp(-0.04 * T) * (F - 90.0), 1.0e-9);
}

// ═══════════════════════════════════════════════════════════════════════════
//  The BBS final step against an independent implementation
// ═══════════════════════════════════════════════════════════════════════════

// An INDEPENDENT dividend-free Broadie-Detemple CRR: Black-Scholes over the last
// step, then the plain rollback.
[[nodiscard]] double plain_bbs(double S, double K, double T, double sigma, double r, double q,
                               Side side, int steps, bool american) {
  const double dt = T / static_cast<double>(steps);
  const double u = std::exp(sigma * std::sqrt(dt));
  const double d = 1.0 / u;
  const double p = (std::exp((r - q) * dt) - d) / (u - d);
  const double disc = std::exp(-r * dt);
  const double sgn = side == Side::Call ? 1.0 : -1.0;
  std::vector<double> value(static_cast<std::size_t>(steps));
  for (int i = 0; i < steps; ++i) {
    const double s = S * std::pow(u, static_cast<double>(2 * i - (steps - 1)));
    const auto at = static_cast<std::size_t>(i);
    value[at] = bs_price(s, K, dt, sigma, r, q, side);
    if (american) {
      value[at] = std::max(value[at], std::max(sgn * (s - K), 0.0));
    }
  }
  for (int k = steps - 2; k >= 0; --k) {
    for (int i = 0; i <= k; ++i) {
      const auto at = static_cast<std::size_t>(i);
      double v = disc * (p * value[at + 1U] + (1.0 - p) * value[at]);
      if (american) {
        const double s = S * std::pow(u, static_cast<double>(2 * i - k));
        v = std::max(v, std::max(sgn * (s - K), 0.0));
      }
      value[at] = v;
    }
  }
  return value[0];
}

TEST(DiscreteDivScheme, BbsSchemesWithoutCashMatchAnIndependentBbs) {
  for (const Side side : {Side::Call, Side::Put}) {
    for (const bool american : {false, true}) {
      const ExerciseStyle ex = american ? ExerciseStyle::American : ExerciseStyle::European;
      for (const double K : {80.0, 100.0, 125.0}) {
        const double want = plain_bbs(100.0, K, 0.8, 0.3, 0.05, 0.01, side, 200, american);
        // Without cash the escrowed stock IS the stock: both BBS lattices agree.
        for (const DiscreteDivScheme scheme :
             {DiscreteDivScheme::SplicedBbs, DiscreteDivScheme::EscrowedBbs}) {
          EXPECT_NEAR(price(100.0, K, 0.8, 0.3, 0.05, 0.01, side, {}, 200, ex, scheme), want,
                      1.0e-11)
              << atx::vol::to_string(scheme) << " K=" << K;
        }
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Cash on the expiry step, on step N-1, and on step 1 (the spliced BBS)
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivScheme, CashAtExpiryCannotMoveAnAmericanCallUnderBbs) {
  // Cum-dividend exercise at expiry makes the payoff the undiluted (S - K)+.
  const double T = 0.5;
  const std::vector<CashDividend> at_expiry{{T, 2.5}};
  for (const double K : {90.0, 100.0, 110.0}) {
    EXPECT_EQ(price(100.0, K, T, 0.25, 0.04, 0.0, Side::Call, at_expiry, 301,
                    ExerciseStyle::American, DiscreteDivScheme::SplicedBbs),
              price(100.0, K, T, 0.25, 0.04, 0.0, Side::Call, {}, 301, ExerciseStyle::American,
                    DiscreteDivScheme::SplicedBbs))
        << "K=" << K;
  }
}

TEST(DiscreteDivScheme, CashAtExpiryIsAnExactStrikeShiftForTheEuropeanBbs) {
  const double T = 0.5;
  const double D = 2.5;
  const std::vector<CashDividend> at_expiry{{T, D}};
  constexpr auto kBbs = DiscreteDivScheme::SplicedBbs;
  constexpr auto kEu = ExerciseStyle::European;
  for (const double K : {90.0, 100.0, 110.0}) {
    // call: (max(S - D, 0) - K)+ == (S - (K + D))+, the same lattice at K + D.
    EXPECT_EQ(price(100.0, K, T, 0.25, 0.04, 0.01, Side::Call, at_expiry, 301, kEu, kBbs),
              price(100.0, K + D, T, 0.25, 0.04, 0.01, Side::Call, {}, 301, kEu, kBbs));
    // put: (K - max(S - D, 0))+ == put(K + D) - put(D), on the same nodes.
    const double put = price(100.0, K, T, 0.25, 0.04, 0.01, Side::Put, at_expiry, 301, kEu, kBbs);
    const double shifted = price(100.0, K + D, T, 0.25, 0.04, 0.01, Side::Put, {}, 301, kEu, kBbs) -
                           price(100.0, D, T, 0.25, 0.04, 0.01, Side::Put, {}, 301, kEu, kBbs);
    EXPECT_NEAR(put, shifted, 1.0e-12) << "K=" << K;
  }
}

TEST(DiscreteDivScheme, CashOnStepNMinusOneIsAppliedAnalyticallyAndConverges) {
  // The BBS continuation is evaluated at each node's post-dividend level, so
  // this splice interpolates nothing.
  const double T = 0.5;
  const double D = 3.0;
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double K : {95.0, 100.0, 105.0}) {
      double previous = std::numeric_limits<double>::infinity();
      for (const int n : {300, 600, 1200}) {
        const double tau = T * static_cast<double>(n - 1) / static_cast<double>(n);
        const std::vector<CashDividend> last_step{{tau, D}};
        const double exact =
            spliced_european_one_dividend(100.0, K, T, 0.25, 0.04, 0.01, D, tau, side);
        const double err = std::abs(price(100.0, K, T, 0.25, 0.04, 0.01, side, last_step, n,
                                          ExerciseStyle::European, DiscreteDivScheme::SplicedBbs) -
                                    exact);
        EXPECT_LT(err, previous) << "K=" << K << " n=" << n;
        previous = err;
      }
      EXPECT_LT(previous, 2.0e-3) << "K=" << K;
    }
  }
}

TEST(DiscreteDivScheme, AnEarlyExDateConvergesThroughTheGridBottom) {
  // Cash on step 1 sends the low nodes below the grid: the splice's quadratic
  // grid-bottom rule prices them, BBS or not.
  const double T = 1.0;
  const double D = 6.0;
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double K : {60.0, 100.0, 140.0}) {
      double previous = std::numeric_limits<double>::infinity();
      for (const int n : {300, 600, 1200}) {
        const double tau = T / static_cast<double>(n);
        const std::vector<CashDividend> first_step{{tau, D}};
        const double exact =
            spliced_european_one_dividend(100.0, K, T, 0.3, 0.03, 0.0, D, tau, side);
        const double err = std::abs(price(100.0, K, T, 0.3, 0.03, 0.0, side, first_step, n,
                                          ExerciseStyle::European, DiscreteDivScheme::SplicedBbs) -
                                    exact);
        EXPECT_LT(err, previous) << "K=" << K << " n=" << n;
        previous = err;
      }
      EXPECT_LT(previous, 2.0e-3) << "K=" << K;
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Convergence of each scheme to an independent reference for its own model
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivScheme, SplicedBbsEuropeanConvergesMonotonicallyToTheExactValue) {
  // tau = 0.25 lands on a step at 300/600/1200, so the only error left is the
  // lattice's own: smooth, one-signed, roughly halving per doubling.
  const double T = 0.5;
  const double D = 2.0;
  const double tau = 0.25;
  const std::vector<CashDividend> one{{tau, D}};
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double K : {90.0, 100.0, 110.0}) {
      const double exact =
          spliced_european_one_dividend(100.0, K, T, 0.25, 0.04, 0.01, D, tau, side);
      double e[3];
      int idx = 0;
      for (const int n : {300, 600, 1200}) {
        e[idx++] = price(100.0, K, T, 0.25, 0.04, 0.01, side, one, n, ExerciseStyle::European,
                         DiscreteDivScheme::SplicedBbs) -
                   exact;
      }
      EXPECT_GT(e[0] * e[1], 0.0) << "one-signed, K=" << K;
      EXPECT_GT(e[1] * e[2], 0.0) << "one-signed, K=" << K;
      EXPECT_NEAR(e[0] / e[1], 2.0, 0.6) << "K=" << K;
      EXPECT_NEAR(e[1] / e[2], 2.0, 0.6) << "K=" << K;
    }
  }
}

TEST(DiscreteDivScheme, EscrowedEuropeansConvergeToBlack76AtTheDatedForward) {
  const double S = 100.0;
  const double T = 0.75;
  const double r = 0.045;
  const double q = 0.01;
  const std::vector<CashDividend> three{{0.10, 2.0}, {0.35, 2.0}, {0.60, 2.0}};
  const double F = dated_forward(S, T, r, q, three);
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double K : {80.0, 100.0, 120.0}) {
      const double black = black76_price(F, K, T, 0.25, std::exp(-r * T), side);
      for (const DiscreteDivScheme scheme :
           {DiscreteDivScheme::EscrowedBbs, DiscreteDivScheme::EscrowedLeisenReimer}) {
        const double e301 =
            price(S, K, T, 0.25, r, q, side, three, 301, ExerciseStyle::European, scheme) - black;
        const double e1201 =
            price(S, K, T, 0.25, r, q, side, three, 1201, ExerciseStyle::European, scheme) - black;
        EXPECT_LT(std::abs(e1201), std::abs(e301)) << atx::vol::to_string(scheme) << " K=" << K;
        EXPECT_LT(std::abs(e301), 5.0e-3) << atx::vol::to_string(scheme) << " K=" << K;
      }
      // Leisen-Reimer's O(1/n^2): two orders below the BBS lattice at 301.
      EXPECT_LT(std::abs(price(S, K, T, 0.25, r, q, side, three, 301, ExerciseStyle::European,
                               DiscreteDivScheme::EscrowedLeisenReimer) -
                         black),
                5.0e-5)
          << "K=" << K;
    }
  }
}

TEST(DiscreteDivScheme, EscrowedAmericanCallConvergesToRollGeskeWhaley) {
  // One cash dividend, q = 0: RGW is the escrowed model's exact American call.
  // The ex-date sits on a step of both lattices compared (301 and 903 = 3*301).
  const double S = 100.0;
  const double T = 0.5;
  const double sigma = 0.20;
  const double r = 0.04;
  const double D = 5.0;
  const double t1 = 150.0 * T / 301.0;
  const std::vector<CashDividend> one{{t1, D}};
  for (const double K : {85.0, 90.0, 100.0}) {
    const double exact = rgw_american_call(S, K, T, sigma, r, D, t1);
    for (const DiscreteDivScheme scheme :
         {DiscreteDivScheme::EscrowedBbs, DiscreteDivScheme::EscrowedLeisenReimer}) {
      const double e301 = std::abs(
          price(S, K, T, sigma, r, 0.0, Side::Call, one, 301, ExerciseStyle::American, scheme) -
          exact);
      const double e903 = std::abs(
          price(S, K, T, sigma, r, 0.0, Side::Call, one, 903, ExerciseStyle::American, scheme) -
          exact);
      EXPECT_LT(e903, e301 + 1.0e-6) << atx::vol::to_string(scheme) << " K=" << K;
      EXPECT_LT(e301, 5.0e-3) << atx::vol::to_string(scheme) << " K=" << K;
    }
  }
}

TEST(DiscreteDivScheme, AmericanSchemesConvergeAgainstTheirOwnHighStepReference) {
  // The reference lane's three-dividend contract (S = K = 100, T = 0.75).
  const std::vector<CashDividend> three{{0.10, 2.0}, {0.35, 2.0}, {0.60, 2.0}};
  for (const DiscreteDivScheme scheme : kAllSchemes) {
    for (const Side side : {Side::Call, Side::Put}) {
      const auto at = [&](int n) {
        return price(100.0, 100.0, 0.75, 0.25, 0.045, 0.0, side, three, n,
                     ExerciseStyle::American, scheme);
      };
      const double ref = at(4801);
      EXPECT_LT(std::abs(at(1201) - ref), std::abs(at(301) - ref) + 1.0e-6)
          << atx::vol::to_string(scheme);
      EXPECT_LT(std::abs(at(301) - ref), 2.5e-2) << atx::vol::to_string(scheme);
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  European put-call parity
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivScheme, EuropeanParityHoldsOnEveryScheme) {
  const double S = 775.8;
  const double T = 1.3407;
  const double r = 0.041;
  const double q = 0.008;
  const int n = 301;
  const double dt = T / n;
  // Six interior ex-dates (none on the expiry step, where the spliced put
  // carries the zero-floor term put(D)).
  const std::vector<CashDividend> six{{0.096768, 1.98}, {0.347905, 2.15}, {0.592603, 1.95},
                                      {0.838835, 2.05}, {1.089590, 2.15}, {1.250000, 2.35}};
  // The spliced lattice pays each amount on its rounded step; the escrowed
  // schemes discount it from its exact date into the forward.
  double spliced_pv = 0.0;
  for (const CashDividend &dv : six) {
    const double t = std::nearbyint(dv.tau / dt) * dt;
    spliced_pv += dv.amount * std::exp(-r * t) * std::exp(-q * (T - t));
  }
  const double forward = dated_forward(S, T, r, q, six);
  for (const DiscreteDivScheme scheme : kAllSchemes) {
    for (const double K : {700.0, 780.0, 860.0}) {
      const double c = price(S, K, T, 0.18, r, q, Side::Call, six, n, ExerciseStyle::European,
                             scheme);
      const double p = price(S, K, T, 0.18, r, q, Side::Put, six, n, ExerciseStyle::European,
                             scheme);
      const double want = escrowed(scheme)
                              ? std::exp(-r * T) * (forward - K)
                              : S * std::exp(-q * T) - spliced_pv - K * std::exp(-r * T);
      EXPECT_NEAR(c - p, want, 1.0e-9) << atx::vol::to_string(scheme) << " K=" << K;
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Strike shape: what the schemes are for
// ═══════════════════════════════════════════════════════════════════════════

struct SyntheticChain {
  double S;
  double T;
  double r;
  double q;
  std::vector<CashDividend> divs;
  std::vector<double> strikes;
  std::vector<double> sigmas;
};

// An ASML-shaped chain (dev200, 2027-04-16): spot 1712, two 3.18 dividends,
// strikes 860..2640 by 20. The vol curve is CONVEX IN BLACK76 PRICE SPACE at the
// dated-cash forward by construction: a skewed smile, optionally with three runs
// of strikes replaced by the chord between their end points (a dense convex fit
// is tight exactly like this — zero butterflies on runs of strikes), then
// inverted back to per-strike Black vols.
[[nodiscard]] SyntheticChain asml_shaped_chain(bool tight_runs) {
  SyntheticChain c{1711.835, 0.50136, 0.047639, -0.0072149,
                   {{0.0326974, 3.1771}, {0.2806524, 3.1771}}, {}, {}};
  const double F = dated_forward(c.S, c.T, c.r, c.q, c.divs);
  const double df = std::exp(-c.r * c.T);
  std::vector<double> call;
  for (double K = 860.0; K <= 2640.0; K += 20.0) {
    const double k = std::log(K / F);
    c.strikes.push_back(K);
    call.push_back(black76_price(F, K, c.T, 0.40 - 0.10 * k + 0.25 * k * k, df, Side::Call));
  }
  if (tight_runs) {
    const auto chord = [&](double lo, double hi) {
      const auto a = static_cast<std::size_t>((lo - 860.0) / 20.0);
      const auto b = static_cast<std::size_t>((hi - 860.0) / 20.0);
      for (std::size_t i = a + 1U; i < b; ++i) {
        const double w = (c.strikes[i] - c.strikes[a]) / (c.strikes[b] - c.strikes[a]);
        call[i] = (1.0 - w) * call[a] + w * call[b];
      }
    };
    chord(1780.0, 1920.0);
    chord(2060.0, 2240.0);
    chord(2360.0, 2560.0);
  }
  for (std::size_t i = 0; i < c.strikes.size(); ++i) {
    c.sigmas.push_back(black_iv(call[i], F, c.strikes[i], c.T, df, Side::Call));
  }
  return c;
}

// The quote slice's own bounds construction (fitting/quote_slice.cpp).
[[nodiscard]] AmericanPriceBounds bounds_for(const SyntheticChain &c) {
  const double df = std::exp(-c.r * c.T);
  AmericanPriceBounds b{c.S, std::min(1.0, df), std::max(1.0, df), 0.0, 0.0};
  b.prepaid_max = c.S * std::exp(std::max(-c.q, 0.0) * c.T);
  b.prepaid_min = c.S * std::exp(-std::max(c.q, 0.0) * c.T);
  for (const CashDividend &dv : c.divs) {
    b.prepaid_min -= dv.amount * std::exp(-c.r * dv.tau + std::max(-c.q, 0.0) * (c.T - dv.tau));
  }
  b.prepaid_min = std::max(b.prepaid_min, 0.0);
  return b;
}

[[nodiscard]] bool certifies(const SyntheticChain &c, DiscreteDivScheme scheme) {
  std::vector<AmericanPriceNode> nodes;
  for (std::size_t i = 0; i < c.strikes.size(); ++i) {
    const double K = c.strikes[i];
    nodes.push_back({K,
                     price(c.S, K, c.T, c.sigmas[i], c.r, c.q, Side::Call, c.divs, 301,
                           ExerciseStyle::American, scheme),
                     price(c.S, K, c.T, c.sigmas[i], c.r, c.q, Side::Put, c.divs, 301,
                           ExerciseStyle::American, scheme)});
  }
  return AmericanPriceSlice::create(nodes, bounds_for(c)).has_value();
}

TEST(DiscreteDivSchemeStrikeShape, BbsRemovesTheLatticeStaircaseOnASmoothSmile) {
  const SyntheticChain chain = asml_shaped_chain(false);
  EXPECT_FALSE(certifies(chain, DiscreteDivScheme::SplicedCrr))
      << "the trap this lane exists for: a per-strike-vol CRR slice is refused";
  EXPECT_TRUE(certifies(chain, DiscreteDivScheme::SplicedBbs));
  EXPECT_TRUE(certifies(chain, DiscreteDivScheme::EscrowedLeisenReimer));
}

TEST(DiscreteDivSchemeStrikeShape, OnlyTheEscrowedLeisenReimerTreeSurvivesTightConvexRuns) {
  const SyntheticChain chain = asml_shaped_chain(true);
  // Smoothing alone is not enough: the spliced European differs from Black76
  // at the same forward by a smooth term that is concave across the runs.
  EXPECT_FALSE(certifies(chain, DiscreteDivScheme::SplicedCrr));
  EXPECT_FALSE(certifies(chain, DiscreteDivScheme::SplicedBbs));
  EXPECT_TRUE(certifies(chain, DiscreteDivScheme::EscrowedLeisenReimer));
}

// ═══════════════════════════════════════════════════════════════════════════
//  The quote slice carries the scheme through inversion and repricing
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivSchemeQuoteSlice, ADatedCashFitRecordsAndUsesItsScheme) {
  using atx::vol::QuoteSliceConfig;
  using atx::vol::QuoteSliceDividendModel;
  using atx::vol::QuoteSliceFitStatus;
  using atx::vol::QuoteSliceMarket;
  using atx::vol::QuoteSliceQuote;
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.0, 0.0};
  const std::vector<CashDividend> divs{{0.2, 1.25}};
  constexpr auto kScheme = DiscreteDivScheme::EscrowedLeisenReimer;
  std::vector<QuoteSliceQuote> quotes;
  for (double K = 70.0; K <= 130.0; K += 5.0) {
    const double sigma = 0.28 - 0.15 * std::log(K / 100.0);
    for (const Side side : {Side::Call, Side::Put}) {
      const double mid = price(100.0, K, 0.5, sigma, 0.04, 0.0, side, divs, 301,
                               ExerciseStyle::American, kScheme);
      const double half = std::max(0.02, 0.01 * mid);
      quotes.push_back({K, side, mid - half, mid + half, {}});
    }
  }
  QuoteSliceConfig cfg;
  cfg.dividend_model = QuoteSliceDividendModel::DatedCash;
  cfg.discrete_div_scheme = kScheme;
  const auto fit = atx::vol::fit_quote_slice(market, quotes, cfg, divs);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  EXPECT_EQ(fit->discrete_div_scheme, kScheme);
  // A certified node reprices through the scheme the fit recorded.
  const auto node = atx::vol::evaluate_quote_slice(*fit, 100.0, Side::Put);
  ASSERT_TRUE(node.has_value());
  const double direct = price(100.0, 100.0, 0.5, node->volatility, 0.04, 0.0, Side::Put,
                              fit->variance_clock_dividends, 301, ExerciseStyle::American, kScheme);
  EXPECT_NEAR(node->price, direct, 1.0e-12);
}

TEST(DiscreteDivSchemeQuoteSlice, AStepCountTheSchemeCannotCarryIsRefused) {
  atx::vol::QuoteSliceConfig cfg;
  cfg.dividend_model = atx::vol::QuoteSliceDividendModel::DatedCash;
  cfg.discrete_div_scheme = DiscreteDivScheme::SplicedBbsr;
  cfg.discrete_div_steps = 1;
  const atx::vol::QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.0, 0.0};
  const std::vector<atx::vol::QuoteSliceQuote> quotes{{100.0, Side::Call, 5.0, 5.2, {}}};
  const auto fit = atx::vol::fit_quote_slice(market, quotes, cfg);
  ASSERT_FALSE(fit.has_value());
  EXPECT_EQ(fit.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
