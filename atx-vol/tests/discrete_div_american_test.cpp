#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/dividend.hpp"

// Coverage for the Vellekoop-Nieuwenhuis spliced CRR lattice
// (api/pricing/american.hpp, src/pricing/american_discrete_div.cpp).
//
// Every numeric anchor below is REPRODUCED from a validated Python reference
// implementation of the same scheme, not invented here. The anchors are quoted
// at 17 significant digits where the reference produced them that way, and each
// test names which reference check it mirrors (V0-V3 in the reference's own
// numbering).

namespace {

using atx::vol::american_discrete_div_greek_bundle;
using atx::vol::american_discrete_div_greeks;
using atx::vol::american_discrete_div_price;
using atx::vol::black76_price;
using atx::vol::CashDividend;
using atx::vol::discrete_div_sigma_bump;
using atx::vol::DiscreteDivGreekBundle;
using atx::vol::DiscreteDivGreeks;
using atx::vol::ErrorCode;
using atx::vol::ExerciseStyle;
using atx::vol::kDiscreteDivMaxSteps;
using atx::vol::kDiscreteDivRateBump;
using atx::vol::kDiscreteDivSigmaBumpRel;
using atx::vol::kDiscreteDivThetaSecantHorizon;
using atx::vol::kDiscreteDivYieldBump;
using atx::vol::Side;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Black-Scholes with a continuous yield, expressed through the shipped Black-76
// kernel: F = S*exp((r-q)*T), df = exp(-r*T).
[[nodiscard]] double bs_price(double S, double K, double T, double sigma, double r, double q,
                              Side side) {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T), side);
}

// The CLOSED-FORM d^2P/dsigma^2, volga = vega*d1*d2/sigma. Side-independent, as
// every second-order Black-Scholes greek is. Needed wherever a stencil's own
// bump is the variable under test: a same-bump reference moves with it and
// would score a wider bump against a target that had shifted underneath it.
[[nodiscard]] double bs_analytic_volga(double S, double K, double T, double sigma, double r,
                                       double q) {
  const double sq = sigma * std::sqrt(T);
  const double F = S * std::exp((r - q) * T);
  const double d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / sq;
  const double d2 = d1 - sq;
  const double pdf = std::exp(-0.5 * d1 * d1) / std::sqrt(2.0 * std::acos(-1.0));
  const double vega = std::exp(-r * T) * F * pdf * std::sqrt(T);
  return vega * d1 * d2 / sigma;
}

// An INDEPENDENT plain Cox-Ross-Rubinstein lattice, written out here so
// "the empty dividend span reduces exactly to the plain CRR path" is a claim
// about two implementations rather than a tautology about one.
[[nodiscard]] double plain_crr(double S, double K, double T, double sigma, double r, double q,
                               Side side, int steps, bool american) {
  const double dt = T / static_cast<double>(steps);
  const double sqrt_dt = std::sqrt(dt);
  const double u = std::exp(sigma * sqrt_dt);
  const double d = 1.0 / u;
  const double disc = std::exp(-r * dt);
  const double p = (std::exp((r - q) * dt) - d) / (u - d);
  const double pu = disc * p;
  const double pd = disc * (1.0 - p);
  const double sgn = (side == Side::Call) ? 1.0 : -1.0;

  const std::size_t nodes = static_cast<std::size_t>(steps) + 1U;
  std::vector<double> level(nodes);
  std::vector<double> value(nodes);
  for (std::size_t i = 0; i < nodes; ++i) {
    const int rung = 2 * static_cast<int>(i) - steps;
    level[i] = S * std::exp(sigma * sqrt_dt * static_cast<double>(rung));
    value[i] = std::max(sgn * (level[i] - K), 0.0);
  }
  for (int k = steps - 1; k >= 0; --k) {
    const std::size_t last = static_cast<std::size_t>(k);
    for (std::size_t i = 0; i <= last; ++i) {
      level[i] *= u;
    }
    for (std::size_t i = 0; i <= last; ++i) {
      value[i] = pu * value[i + 1U] + pd * value[i];
    }
    if (american) {
      for (std::size_t i = 0; i <= last; ++i) {
        value[i] = std::max(value[i], std::max(sgn * (level[i] - K), 0.0));
      }
    }
  }
  return value[0];
}

[[nodiscard]] double price_or_fail(double S, double K, double T, double sigma, double r, double q,
                                   Side side, std::span<const CashDividend> divs, int steps,
                                   ExerciseStyle exercise) {
  const auto out = american_discrete_div_price(S, K, T, sigma, r, q, side, divs, steps, exercise);
  EXPECT_TRUE(out.has_value());
  return out.has_value() ? *out : kNaN;
}

// ── An EXACT reference for one dividend, by a method that is not a lattice ────
//
// With a SINGLE cash dividend at t1 and no continuous yield, the American call
// under this engine's own (spot-shift) dividend model has a closed-form
// decomposition, because early exercise is optimal at exactly one instant:
//
//   * before t1  — never. Committing to exercise at t1^- is already worth
//                  S_t - K*exp(-r*(t1 - t)) > S_t - K, so exercising earlier
//                  strictly gives up interest on the strike.
//   * after t1   — never. Nothing is paid after t1, so the residual claim is a
//                  call on a non-dividend-paying stock with r > 0.
//   * AT t1^-    — the only place it can bind, and where the exercise boundary
//                  is effectively vertical (Roll 1977 / Geske 1979 / Whaley
//                  1981; re-confirmed numerically by Itkin, arXiv:2510.18159
//                  section 6.4).
//
// So  V(0) = exp(-r*t1) * E[ max( S_t1 - K, C_BS(S_t1 - D, K, T - t1) ) ],
// one normal expectation over a closed form. That is an INDEPENDENT reference
// in the sense that matters — a different numerical method (quadrature plus
// Black-Scholes) for the SAME model the lattice implements — and it is exact up
// to the quadrature, so it pins the lattice's value rather than its structure.
[[nodiscard]] double vn_american_call_one_dividend(double S, double K, double T, double sigma,
                                                   double r, double D, double t1) {
  constexpr int kNodes = 40000; // even: composite Simpson
  constexpr double kZ = 10.0;   // +/-10 sigma; the tail beyond is < 1e-23
  const double h = 2.0 * kZ / static_cast<double>(kNodes);
  const double drift = (r - 0.5 * sigma * sigma) * t1;
  const double diffusion = sigma * std::sqrt(t1);
  const double inv_sqrt_2pi = 1.0 / std::sqrt(2.0 * std::acos(-1.0));
  const auto integrand = [&](double z) {
    const double s_cum = S * std::exp(drift + diffusion * z);
    const double hold = bs_price(std::max(s_cum - D, 0.0), K, T - t1, sigma, r, 0.0, Side::Call);
    const double exercise_now = std::max(s_cum - K, 0.0);
    return inv_sqrt_2pi * std::exp(-0.5 * z * z) * std::max(exercise_now, hold);
  };
  double sum = integrand(-kZ) + integrand(kZ);
  for (int i = 1; i < kNodes; ++i) {
    sum += ((i % 2) != 0 ? 4.0 : 2.0) * integrand(-kZ + h * static_cast<double>(i));
  }
  return std::exp(-r * t1) * sum * h / 3.0;
}

// ── Roll-Geske-Whaley, the classical closed form ─────────────────────────────
//
// RGW is exact for ONE cash dividend on an American call under the ESCROWED
// model (the stochastic part is S - PV(D) and stays lognormal throughout). This
// engine is spot-shift, not escrowed, so RGW is a CROSS-MODEL check: it must
// agree in shape and to within the two models' own difference, and the test
// that uses it pins that difference rather than pretending it is zero.
[[nodiscard]] double norm_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

// M(a, b; rho) = P(X <= a, Y <= b) for standard bivariates with correlation rho,
// by composite Simpson on the conditional decomposition
//   M = int_{-inf}^{a} phi(x) * N((b - rho*x)/sqrt(1 - rho^2)) dx.
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

[[nodiscard]] double rgw_american_call(double S, double K, double T, double sigma, double r,
                                       double D, double t1) {
  const double s_adj = S - D * std::exp(-r * t1);
  // Merton's no-early-exercise condition: the cash cannot beat the interest
  // saved by deferring the strike payment over the stub (t1, T].
  if (D <= K * (1.0 - std::exp(-r * (T - t1)))) {
    return bs_price(s_adj, K, T, sigma, r, 0.0, Side::Call);
  }
  // S*: the CUM-dividend spot at which exercising at t1^- exactly ties with
  // holding. g(x) = C_BS(x, K, T - t1) - (x + D - K) is positive at x -> 0 and
  // negative at x -> inf precisely when the condition above fails, so bisection
  // is well posed.
  double lo = 1.0e-8;
  double hi = 100.0 * (S + K);
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (bs_price(mid, K, T - t1, sigma, r, 0.0, Side::Call) - (mid + D - K) > 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
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

// ── The four reference V0/V1 scenarios, verbatim ─────────────────────────────
struct Scenario {
  double S;
  double K;
  double r;
  double q;
  double sigma;
  double T;
};

constexpr Scenario kV0[] = {
    {100.0, 90.0, 0.04, 0.0, 0.20, 0.5},
    {100.0, 100.0, 0.04, 0.005, 0.35, 1.0},
    {100.0, 115.0, 0.04, 0.0, 0.15, 0.25},
    {775.8, 780.0, 0.04, 0.012, 0.14, 0.0968},
};

// ── The reference's 6-dividend SPY-shaped schedule (V2) ──────────────────────
constexpr double kParityS = 775.8;
constexpr double kParityR = 0.041;
constexpr double kParityQ = 0.008;
constexpr double kParitySigma = 0.18;
constexpr double kParityT = 1.3407;

[[nodiscard]] std::vector<CashDividend> parity_schedule() {
  return {{0.096768, 1.98}, {0.347905, 2.15}, {0.592603, 1.95},
          {0.838835, 2.05}, {1.089590, 2.15}, {1.340730, 2.35}};
}

// ═══════════════════════════════════════════════════════════════════════════
//  The empty span, and everything that must behave like it
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivAmerican, EmptyDividendSpan_IsBitExactPlainCrr) {
  for (const Scenario &s : kV0) {
    for (const Side side : {Side::Call, Side::Put}) {
      for (const bool american : {false, true}) {
        const ExerciseStyle style = american ? ExerciseStyle::American : ExerciseStyle::European;
        const double got =
            price_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, side, {}, 301, style);
        const double want = plain_crr(s.S, s.K, s.T, s.sigma, s.r, s.q, side, 301, american);
        EXPECT_EQ(got, want) << "S=" << s.S << " K=" << s.K << " american=" << american;
      }
    }
  }
}

TEST(DiscreteDivAmerican, DividendsOutsideTheWindow_AreBitExactToTheEmptySpan) {
  const Scenario s = kV0[1];
  const std::vector<CashDividend> outside{
      {-0.25, 1.0},   // already paid
      {0.0, 1.0},     // exactly at valuation: not inside (0, T]
      {s.T * 1.5, 1.0}, // after expiry
      {0.5, 0.0},     // inside the window, but a zero amount is a no-op
  };
  const double with = price_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Put,
                                    outside, 301, ExerciseStyle::American);
  const double without =
      price_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Put, {}, 301, ExerciseStyle::American);
  EXPECT_EQ(with, without);
}

TEST(DiscreteDivAmerican, UnsortedDividendInput_MatchesTheSortedSchedule) {
  const std::vector<CashDividend> sorted{{0.25, 1.0}, {0.50, 1.1}, {0.75, 1.2}};
  const std::vector<CashDividend> shuffled{{0.50, 1.1}, {0.75, 1.2}, {0.25, 1.0}};
  const double a =
      price_or_fail(775.8, 780.0, 1.0, 0.20, 0.04, 0.01, Side::Put, sorted, 301,
                    ExerciseStyle::American);
  const double b =
      price_or_fail(775.8, 780.0, 1.0, 0.20, 0.04, 0.01, Side::Put, shuffled, 301,
                    ExerciseStyle::American);
  EXPECT_EQ(a, b);
  // Reference value for this schedule (scalar clone of the validated Python).
  EXPECT_NEAR(a, 55.348974035261527, 1.0e-9);
}

// ═══════════════════════════════════════════════════════════════════════════
//  V1 — a dividend landing exactly at expiry (the terminal-payoff-kink fix)
// ═══════════════════════════════════════════════════════════════════════════

// The reference's V1: for a EUROPEAN call, a cash dividend D paid at tau == T is
// exactly a strike shift to K + D, because the terminal payoff is
// max(S - D - K, 0). Interpolating that kinked payoff back onto the grid loses
// accuracy; applying the payoff ANALYTICALLY at the terminal step is what makes
// the identity hold to the last bit. The reference measured 0.00e+00 here at
// N = 301, 1201 and 4801; 4801 is left out only because it buys nothing this
// suite does not already have and costs ~16x the rollback.
TEST(DiscreteDivAmerican, DividendExactlyAtExpiry_IsBitExactStrikeShift) {
  constexpr double kD = 2.15;
  for (const int steps : {301, 1201}) {
    for (const Scenario &s : kV0) {
      const std::vector<CashDividend> at_expiry{{s.T, kD}};
      const double spliced = price_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Call,
                                           at_expiry, steps, ExerciseStyle::European);
      const double shifted = price_or_fail(s.S, s.K + kD, s.T, s.sigma, s.r, s.q, Side::Call, {},
                                           steps, ExerciseStyle::European);
      EXPECT_EQ(spliced, shifted) << "steps=" << steps << " S=" << s.S << " K=" << s.K;
    }
  }
}

TEST(DiscreteDivAmerican, DividendExactlyAtExpiry_ReproducesReferencePrices) {
  constexpr double kD = 2.15;
  constexpr double kWant[] = {11.532978116700168, 14.408104751743615, 0.072331831877589306,
                              11.490520570596777};
  for (std::size_t i = 0; i < std::size(kV0); ++i) {
    const Scenario &s = kV0[i];
    const std::vector<CashDividend> at_expiry{{s.T, kD}};
    const double got = price_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Call, at_expiry, 301,
                                     ExerciseStyle::European);
    EXPECT_NEAR(got, kWant[i], 1.0e-10) << "scenario " << i;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  V2 — European put-call parity with six discrete cash dividends
// ═══════════════════════════════════════════════════════════════════════════

// The parity reference is MODEL-CONSISTENT, and getting it wrong is the single
// easiest way to fake a passing parity test: under a residual continuous yield
// `q` the yield accrues on the removed cash too, so the present value of the
// schedule is  sum_i D_i * exp(-r*tau_i) * exp(-q*(T - tau_i)),  NOT the plain
// sum_i D_i * exp(-r*tau_i). The difference reads as a constant offset across
// every strike, which is exactly the shape a parity test is blind to if the
// reference is derived from the same mistake.
TEST(DiscreteDivAmerican, EuropeanPutCallParity_HoldsWithSixDiscreteDividends) {
  const std::vector<CashDividend> schedule = parity_schedule();
  double pv = 0.0;
  for (const CashDividend &dv : schedule) {
    // The SAME window the lattice applies. Worth spelling out rather than
    // summing the whole schedule: this reference expiry is 1.3407 and the
    // schedule's last ex-date is 1.340730, so that quarter falls 3e-5 years
    // AFTER the option dies and is not in its life. Summing it anyway shifts
    // the reference by 2.22 dollars and turns a passing parity check into a
    // failing one — which is the correct outcome, and exactly why the
    // reference is derived here instead of being pasted as a number.
    if (dv.tau > kParityT * (1.0 + 1.0e-12)) {
      continue;
    }
    pv += dv.amount * std::exp(-kParityR * dv.tau) * std::exp(-kParityQ * (kParityT - dv.tau));
  }
  EXPECT_NEAR(pv, 9.9719482001835704, 1.0e-12);

  double worst = 0.0;
  for (const double K : {600.0, 700.0, 775.0, 850.0, 950.0}) {
    const double call = price_or_fail(kParityS, K, kParityT, kParitySigma, kParityR, kParityQ,
                                      Side::Call, schedule, 301, ExerciseStyle::European);
    const double put = price_or_fail(kParityS, K, kParityT, kParitySigma, kParityR, kParityQ,
                                     Side::Put, schedule, 301, ExerciseStyle::European);
    const double parity = kParityS * std::exp(-kParityQ * kParityT) - pv -
                          K * std::exp(-kParityR * kParityT);
    worst = std::max(worst, std::abs((call - put) - parity));
  }
  // The reference's own PASS bar is 1e-3 dollars; it measured 5.222871e-05
  // (0.0052 ticks), a residual that is dividend-step TIME rounding and decays
  // with the step count. Both are pinned: the bar, and the exact value, so a
  // regression that stays under the bar still fails here.
  EXPECT_LT(worst, 1.0e-3);
  // 5.222871e-05 before the grid-bottom extrapolation (see `splice_dividend`);
  // the earliest ex-date of this schedule lands on step 22, where the corner
  // node still carries ~(1-p)^22 of weight and the flat clamp was visible.
  EXPECT_NEAR(worst, 5.1822980566385013e-05, 1.0e-9);
  EXPECT_LT(worst * 100.0, 0.0053) << "parity residual in ticks";
}

TEST(DiscreteDivAmerican, EuropeanWithDividends_ReproducesReferencePriceAndGreeks) {
  const std::vector<CashDividend> schedule = parity_schedule();
  const auto call = american_discrete_div_greeks(kParityS, 775.0, kParityT, kParitySigma, kParityR,
                                                 kParityQ, Side::Call, schedule, 301,
                                                 ExerciseStyle::European);
  ASSERT_TRUE(call.has_value()) << call.error().to_string();
  // All five anchors moved with the grid-bottom extrapolation, in the directions
  // the geometry requires: the CALL value curve is increasing, so extrapolating
  // below level[0] LOWERS the continuation the flat clamp held up
  // (75.222227622486884 -> 75.222227558392603); the PUT curve is decreasing, so
  // the same edit RAISES it (51.21952913918814 -> 51.219529480822089). The pair
  // moves toward each other, which is the parity improvement above.
  EXPECT_NEAR(call->price, 75.222227558392603, 1.0e-9);
  EXPECT_NEAR(call->delta, 0.59491124565685172, 1.0e-9);
  EXPECT_NEAR(call->gamma, 0.0023741849955113002, 1.0e-11);

  const auto put = american_discrete_div_greeks(kParityS, 775.0, kParityT, kParitySigma, kParityR,
                                                kParityQ, Side::Put, schedule, 301,
                                                ExerciseStyle::European);
  ASSERT_TRUE(put.has_value()) << put.error().to_string();
  EXPECT_NEAR(put->price, 51.219529480822089, 1.0e-9);
  EXPECT_NEAR(put->delta, -0.39445572220346636, 1.0e-9);
}

// ═══════════════════════════════════════════════════════════════════════════
//  V0 — the control: plain CRR European converges to Black-Scholes as O(1/N)
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivAmerican, PlainCrrEuropean_MatchesReferenceBlackScholesGap) {
  constexpr double kWant301[] = {-0.002490, 0.010891, -0.000310, 0.010521};
  for (std::size_t i = 0; i < std::size(kV0); ++i) {
    const Scenario &s = kV0[i];
    const double tree = price_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Call, {}, 301,
                                      ExerciseStyle::European);
    const double closed = bs_price(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Call);
    EXPECT_NEAR(tree - closed, kWant301[i], 1.0e-6) << "scenario " << i;
  }
}

// The discretisation error of a plain CRR European is O(1/N), so N * error is
// the invariant, not the error. The reference measured 3.278 for this scenario
// at every one of N = 301, 601, 1201, 4801 — pinning the CONSTANT is what makes
// this a statement about the convergence ORDER rather than four separate
// magnitudes that a second-order scheme would also satisfy.
TEST(DiscreteDivAmerican, PlainCrrEuropean_ConvergesToBlackScholesAtOrderOneOverN) {
  const Scenario &s = kV0[1];
  const double closed = bs_price(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Call);
  for (const int steps : {301, 601, 1201, 4801}) {
    const double tree = price_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Call, {}, steps,
                                      ExerciseStyle::European);
    const double scaled = static_cast<double>(steps) * std::abs(tree - closed);
    EXPECT_NEAR(scaled, 3.278, 0.01) << "steps=" << steps;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  V2c / V3 — the American path
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivAmerican, AmericanPutWithOneDividend_ReproducesTheReferenceLattice) {
  const std::vector<CashDividend> one{{0.20, 2.15}};
  constexpr double kStrike[] = {700.0, 780.0, 860.0};
  constexpr double kWant301[] = {5.7858156035716881, 31.469268684567247, 87.098325528011372};
  constexpr double kWant601[] = {5.7826533, 31.4648338, 87.1007917};
  for (std::size_t i = 0; i < 3U; ++i) {
    const double at301 = price_or_fail(775.8, kStrike[i], 0.35, 0.18, 0.041, 0.0, Side::Put, one,
                                       301, ExerciseStyle::American);
    EXPECT_NEAR(at301, kWant301[i], 1.0e-9) << "K=" << kStrike[i];
    const double at601 = price_or_fail(775.8, kStrike[i], 0.35, 0.18, 0.041, 0.0, Side::Put, one,
                                       601, ExerciseStyle::American);
    EXPECT_NEAR(at601, kWant601[i], 1.0e-6) << "K=" << kStrike[i];
  }
}

TEST(DiscreteDivAmerican, AmericanPutWithOneDividend_ReproducesReferenceGreeks) {
  const std::vector<CashDividend> one{{0.20, 2.15}};
  const auto got = american_discrete_div_greeks(775.8, 780.0, 0.35, 0.18, 0.041, 0.0, Side::Put,
                                                one, 301, ExerciseStyle::American);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_NEAR(got->price, 31.469268684567247, 1.0e-9);
  EXPECT_NEAR(got->delta, -0.4737691229637162, 1.0e-9);
  EXPECT_NEAR(got->gamma, 0.0050665053305024733, 1.0e-11);
}

TEST(DiscreteDivAmerican, GreeksPriceIsBitIdenticalToThePriceEntry) {
  const std::vector<CashDividend> one{{0.20, 2.15}};
  const auto greeks = american_discrete_div_greeks(775.8, 780.0, 0.35, 0.18, 0.041, 0.0, Side::Put,
                                                   one, 301, ExerciseStyle::American);
  ASSERT_TRUE(greeks.has_value()) << greeks.error().to_string();
  const double price = price_or_fail(775.8, 780.0, 0.35, 0.18, 0.041, 0.0, Side::Put, one, 301,
                                     ExerciseStyle::American);
  EXPECT_EQ(greeks->price, price);
}

TEST(DiscreteDivAmerican, AmericanNoDividend_ReproducesReferencePriceDeltaGamma) {
  const auto got = american_discrete_div_greeks(100.0, 100.0, 1.0, 0.25, 0.05, 0.02, Side::Put, {},
                                                301, ExerciseStyle::American);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_NEAR(got->price, 8.5728165799167684, 1.0e-9);
  EXPECT_NEAR(got->delta, -0.41905816279378388, 1.0e-9);
  EXPECT_NEAR(got->gamma, 0.016818257302131094, 1.0e-11);
  EXPECT_GT(got->gamma, 0.0);
  EXPECT_LT(got->delta, 0.0);
  EXPECT_GT(got->delta, -1.0);
}

// V3: deep-OTM, short-dated, zero yield and no dividends. Early exercise is
// worthless here, so the American lattice must land on Black-Scholes; the
// reference measured max|tree - BS| = 1.4e-05 across these six.
TEST(DiscreteDivAmerican, DeepOtmShortDatedAmerican_MatchesBlackScholes) {
  constexpr double kStrike[] = {850.0, 900.0, 950.0, 700.0, 650.0, 600.0};
  constexpr Side kSide[] = {Side::Call, Side::Call, Side::Call, Side::Put, Side::Put, Side::Put};
  double worst = 0.0;
  for (std::size_t i = 0; i < 6U; ++i) {
    const double tree = price_or_fail(775.8, kStrike[i], 0.0208, 0.16, 0.041, 0.0, kSide[i], {},
                                      301, ExerciseStyle::American);
    const double closed = bs_price(775.8, kStrike[i], 0.0208, 0.16, 0.041, 0.0, kSide[i]);
    EXPECT_GE(tree, 0.0);
    worst = std::max(worst, std::abs(tree - closed));
  }
  EXPECT_LT(worst, 5.0e-3);
  EXPECT_NEAR(worst, 1.428e-05, 1.0e-7);
}


// ═══════════════════════════════════════════════════════════════════════════
//  Cum-dividend exercise — the ONLY place an American call exercises early
// ═══════════════════════════════════════════════════════════════════════════
//
// The classical result (Roll 1977 / Geske 1979 / Whaley 1981, re-confirmed
// numerically by Itkin, arXiv:2510.18159 section 6.4) is that an American call
// on a stock paying discrete cash exercises early ONLY at the instant before an
// ex-date, where the exercise boundary is effectively vertical. A lattice that
// tests exercise against the POST-dividend level only therefore excludes the
// one place American call exercise happens, and understates the exercise value
// by exactly the dividend at every node where exercise binds.
//
// The put is the mirror image and does NOT change: exercising at t^- pays
// K - S, exercising at t^+ pays K - (S - D), and the second is larger, so a put
// holder never exercises cum-dividend. The engine takes the better of the two
// levels, which is why the same edit is a no-op on every put anchor above.

// The hard floor. Exercising UNCONDITIONALLY at the instant before the ex-date
// is a feasible strategy, and under the engine's own measure its time-0 value is
// exp(-r*t1)*E[S_t1 - K] = S - K*exp(-r*t1) EXACTLY (the dividend-free process
// is a martingale up to the jump). An American call cannot be worth less than a
// strategy its holder may follow, so this bound is model-free within the engine
// and needs no reference implementation at all.
TEST(DiscreteDivAmerican, AmericanCallBeforeAnExDate_ClearsTheUnconditionalExerciseFloor) {
  constexpr double kS = 100.0, kK = 90.0, kT = 0.5, kSigma = 0.20, kR = 0.04;
  constexpr double kTau = 0.25, kD = 5.00;
  const std::vector<CashDividend> one{{kTau, kD}};
  const double floor_value = kS - kK * std::exp(-kR * kTau);
  EXPECT_NEAR(floor_value, 10.895514962574865, 1.0e-12);

  const double amer =
      price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, one, 301, ExerciseStyle::American);
  EXPECT_GT(amer, floor_value)
      << "an American call must beat committing to exercise at the ex-date";

  // ... and it must still beat its own European sibling, which cannot exercise
  // at all. The two together bracket where the early-exercise premium lives.
  const double euro =
      price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, one, 301, ExerciseStyle::European);
  EXPECT_GT(amer, euro);
  EXPECT_LT(euro, floor_value) << "the European sibling is BELOW the floor — which is exactly "
                                  "why excluding cum-dividend exercise is visible";
}

// The value, against the exact quadrature-plus-Black-Scholes decomposition of
// the SAME model (see `vn_american_call_one_dividend`). This is what turns the
// bound above into a number: the lattice must land on the reference to its own
// O(1/steps) truncation, and that error must fall as the step count rises.
TEST(DiscreteDivAmerican, AmericanCallWithOneDividend_MatchesTheExactDecomposition) {
  constexpr double kS = 100.0, kK = 90.0, kT = 0.5, kSigma = 0.20, kR = 0.04;
  constexpr double kTau = 0.25, kD = 5.00;
  const std::vector<CashDividend> one{{kTau, kD}};
  const double exact = vn_american_call_one_dividend(kS, kK, kT, kSigma, kR, kD, kTau);
  EXPECT_NEAR(exact, 11.66391384621064, 1.0e-6);

  const double at301 =
      price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, one, 301, ExerciseStyle::American);
  const double at1201 =
      price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, one, 1201, ExerciseStyle::American);
  // Measured, and one-SIDED in both regimes: the lattice can only ever miss
  // exercise, never invent it, so the error is negative at every step count.
  // Admitting cum-dividend exercise took it from -1.4539e-2 / -6.0211e-3 /
  // -2.7829e-3 at 301 / 601 / 1201 steps to -2.1011e-3 / -1.3008e-3 /
  // -9.4645e-4 -- 6.9x at the shipped step count.
  EXPECT_LT(at301, exact);
  EXPECT_NEAR(at301, exact, 3.0e-3);
  EXPECT_NEAR(at1201, exact, 1.2e-3);
  EXPECT_LT(std::abs(at1201 - exact), std::abs(at301 - exact))
      << "301: " << (at301 - exact) << "  1201: " << (at1201 - exact);
}

// The cross-MODEL check. Roll-Geske-Whaley is exact for one cash dividend on an
// American call under the ESCROWED model; this engine is spot-shift. The two
// are different models and their gap is NOT zero, so the number that matters is
// the gap itself — pinned here so a future edit that silently switches dividend
// models is caught by its size.
TEST(DiscreteDivAmerican, AmericanCallWithOneDividend_SitsBesideRollGeskeWhaley) {
  constexpr double kS = 100.0, kK = 90.0, kT = 0.5, kSigma = 0.20, kR = 0.04;
  constexpr double kTau = 0.25, kD = 5.00;
  const std::vector<CashDividend> one{{kTau, kD}};
  const double rgw = rgw_american_call(kS, kK, kT, kSigma, kR, kD, kTau);
  const double lattice =
      price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, one, 1201, ExerciseStyle::American);
  EXPECT_NEAR(rgw, 11.559265746648435, 1.0e-6);
  EXPECT_NEAR(lattice - rgw, 0.10370165364757611, 5.0e-4)
      << "escrowed vs spot-shift model gap, not a lattice error";
}

// The boundary of the same rule, and the guard that the fix does not fire where
// it must not. Under Merton's condition D <= K*(1 - exp(-r*(T - t1))) early
// exercise is never optimal, so admitting the cum-dividend test must leave the
// American price BIT-IDENTICAL to the European one.
TEST(DiscreteDivAmerican, SmallDividendUnderMertonsBound_LeavesTheAmericanCallEuropean) {
  constexpr double kS = 100.0, kK = 90.0, kT = 0.5, kSigma = 0.20, kR = 0.04;
  constexpr double kTau = 0.25;
  const double merton_bound = kK * (1.0 - std::exp(-kR * (kT - kTau)));
  EXPECT_NEAR(merton_bound, 0.8955149625748704, 1.0e-12);
  const std::vector<CashDividend> small{{kTau, 0.5 * merton_bound}};
  const double amer =
      price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, small, 301, ExerciseStyle::American);
  const double euro =
      price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, small, 301, ExerciseStyle::European);
  EXPECT_EQ(amer, euro);
}

// The put side of the same edit: cum-dividend exercise is DOMINATED for a put,
// so every American put anchor in this file must be untouched by it. Pinned as
// an equality against the reference lattice value that predates the change.
TEST(DiscreteDivAmerican, AmericanPutWithOneDividend_IsUnchangedByCumDividendExercise) {
  const std::vector<CashDividend> one{{0.20, 2.15}};
  const double put = price_or_fail(775.8, 780.0, 0.35, 0.18, 0.041, 0.0, Side::Put, one, 301,
                                   ExerciseStyle::American);
  EXPECT_EQ(put, 31.469268684567247);
}

// ═══════════════════════════════════════════════════════════════════════════
//  The splice below the grid bottom — extrapolate, do not flat-clamp
// ═══════════════════════════════════════════════════════════════════════════
//
// `post = level[i] - amount` is BELOW level[0] for every low node at every
// splice with amount > 0, so the bottom-edge branch is not a corner case: it
// fires on every dividend. Holding the continuation value FLAT there says the
// option stops responding to spot below the grid, which for an American put is
// hidden by its own exercise floor and for a EUROPEAN put — the side every
// validating parity identity is written on — is not hidden at all.
//
// The exact statement below is the cleanest possible witness: with a dividend
// that certainly exceeds the stock, the stock is worth 0 from the ex-date on,
// so a European put IS a zero-coupon claim on the strike, worth K*exp(-r*T) and
// nothing else. The lattice reproduces that only if the continuation value is
// extrapolated to post = 0 rather than clamped at value[0].
TEST(DiscreteDivAmerican, EuropeanPutOnACertainlyWorthlessStock_IsTheDiscountedStrike) {
  const std::vector<CashDividend> ruinous{{0.9, 1.0e6}};
  const double exact = 100.0 * std::exp(-0.03 * 1.0);
  EXPECT_NEAR(exact, 97.044553354850808, 1.0e-12);
  const double euro_put = price_or_fail(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, Side::Put, ruinous, 301,
                                        ExerciseStyle::European);
  // The flat clamp lost exactly level[0]*exp(-r*tau_grid) of this — 0.897723 —
  // and the loss is a lattice artefact, not a model statement, so the bar is
  // machine precision and not a tolerance for it.
  EXPECT_NEAR(euro_put, exact, 1.0e-9);
}

// The same defect with the zero floor NEVER binding, so the two clamps cannot be
// confused. European put-call parity is exactly the identity that sees it: C - P
// is AFFINE in the stock level, so interpolation reproduces it exactly and
// EXTRAPOLATION does too, while a flat clamp does not.
//
// WHERE it bites is the part worth stating, because a mid-tree ex-date hides it
// completely. The bottom-edge branch fires on every splice, but the nodes it
// fires on carry binomial weight ~(1-p)^k at step k, so a late ex-date multiplies
// the error by ~1e-30 and a parity check there reads clean at machine precision
// both before and after. An EARLY ex-date is where the corner has weight: the
// same identity at tau = 0.05, worst over K in {60 .. 140}, measured
//   D = 6  : 3.2922e-03 -> 1.9966e-12
//   D = 12 : 9.9558e-02 -> 4.9613e-05   (residual: the ZERO floor now binds)
// Both ex-dates are on-grid at 300 steps, so no part of this is ex-date rounding.
TEST(DiscreteDivAmerican, EuropeanParityWithAnEarlyDividend_HoldsAtTheGridBottom) {
  constexpr double kS = 100.0, kT = 1.0, kSigma = 0.30, kR = 0.03;
  constexpr double kTau = 0.05; // == 15 * dt at 300 steps: on-grid, no rounding
  const auto worst_residual = [&](double D) {
    const std::vector<CashDividend> one{{kTau, D}};
    const double pv = D * std::exp(-kR * kTau);
    double worst = 0.0;
    for (const double K : {60.0, 80.0, 100.0, 120.0, 140.0}) {
      const double call =
          price_or_fail(kS, K, kT, kSigma, kR, 0.0, Side::Call, one, 300, ExerciseStyle::European);
      const double put =
          price_or_fail(kS, K, kT, kSigma, kR, 0.0, Side::Put, one, 300, ExerciseStyle::European);
      worst = std::max(worst, std::abs((call - put) - (kS - pv - K * std::exp(-kR * kT))));
    }
    return worst;
  };
  EXPECT_LT(worst_residual(6.0), 1.0e-11) << "parity must hold exactly when only the edge binds";
  EXPECT_LT(worst_residual(12.0), 1.0e-4) << "the zero floor is a model statement, not an artefact";
}

// ═══════════════════════════════════════════════════════════════════════════
//  Per-event dP/dD_i — a real derivative, not the European forward chain
// ═══════════════════════════════════════════════════════════════════════════
//
// The escrowed route folds the whole cash schedule into ONE scalar carry
// (q_eff = r - ln(F/S)/T) before any early-exercise boundary is touched, so the
// only thing an individual event can do is move F. `american_dividend_
// sensitivities` (american.cpp) therefore computes
//     dP/dD_i = (-dP/dq / (F*T)) * dF/dD_i
// which is a FIXED scalar times dF/dD_i = -(1 - blend)*exp(r*(T - t_i)). Every
// event's answer is then the same number scaled by exp(-r*t_i): the ratio
// between two events depends on NOTHING but the gap between their ex-dates and
// the rate. Two schedules with the same present value and different timing get
// identical dividend sensitivities, and a call that would be exercised ahead of
// a late ex-date is indistinguishable from one that would not.
//
// The lattice knows the difference because it prices each event where it lands.

// First: the shipped chain-rule entry really does collapse to that ratio, for
// ANY dP/dq. Asserted rather than asserted-about, so the demonstration below is
// a comparison against measured behaviour and not against a paraphrase.
TEST(DiscreteDivAmerican, DividendSensitivities_TheForwardChainRuleRatioIsTimingOnly) {
  constexpr double kT = 1.0, kR = 0.05, kF = 100.0;
  constexpr double kT1 = 0.10, kT2 = 0.90;
  const double dF_dDiv[2] = {-std::exp(kR * (kT - kT1)), -std::exp(kR * (kT - kT2))};
  const double timing_ratio = std::exp(kR * (kT2 - kT1));
  for (const double dP_dq : {-40.0, -12.5, 3.75}) {
    double out[2] = {0.0, 0.0};
    atx::vol::american_dividend_sensitivities(dP_dq, kF, kT, dF_dDiv, out);
    EXPECT_NEAR(out[0] / out[1], timing_ratio, 1.0e-12) << "dP_dq=" << dP_dq;
  }
  EXPECT_NEAR(timing_ratio, 1.0408107741923882, 1.0e-12);
}

// The lattice's own per-event sensitivities, validated against a naive two-sided
// bump of the shipped PRICE entry — a different caller, the same model.
TEST(DiscreteDivAmerican, DividendSensitivities_MatchADirectBumpOfThePriceEntry) {
  constexpr double kS = 100.0, kK = 100.0, kT = 1.0, kSigma = 0.25, kR = 0.05;
  const std::vector<CashDividend> two{{0.10, 1.50}, {0.90, 1.50}};
  double got[2] = {kNaN, kNaN};
  const auto status = atx::vol::american_discrete_div_dividend_sensitivities(
      kS, kK, kT, kSigma, kR, 0.0, Side::Call, two, got, 301, ExerciseStyle::American);
  ASSERT_TRUE(status.has_value()) << status.error().to_string();

  for (std::size_t i = 0; i < 2U; ++i) {
    const double h = atx::vol::discrete_div_amount_bump(two[i].amount);
    std::vector<CashDividend> up = two;
    std::vector<CashDividend> dn = two;
    up[i].amount += h;
    dn[i].amount -= h;
    const double p_up =
        price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, up, 301, ExerciseStyle::American);
    const double p_dn =
        price_or_fail(kS, kK, kT, kSigma, kR, 0.0, Side::Call, dn, 301, ExerciseStyle::American);
    EXPECT_NEAR(got[i], (p_up - p_dn) / (2.0 * h), 1.0e-12) << "event " << i;
  }
}

// The demonstration. Same schedule, same present value question, and the two
// routes disagree by far more than either one's own numerical noise.
TEST(DiscreteDivAmerican, DividendSensitivities_DoNotCollapseToTheForwardChainRule) {
  constexpr double kS = 100.0, kK = 100.0, kT = 1.0, kSigma = 0.25, kR = 0.05;
  constexpr double kT1 = 0.10, kT2 = 0.90;
  // A CALL, and a schedule big enough that the late ex-date is an exercise
  // event while the early one is only a spot reduction. That asymmetry is
  // exactly what a single scalar carry cannot represent.
  const std::vector<CashDividend> two{{kT1, 6.0}, {kT2, 6.0}};
  double lattice[2] = {kNaN, kNaN};
  const auto status = atx::vol::american_discrete_div_dividend_sensitivities(
      kS, kK, kT, kSigma, kR, 0.0, Side::Call, two, lattice, 301, ExerciseStyle::American);
  ASSERT_TRUE(status.has_value()) << status.error().to_string();

  // Both must be negative: more cash off the stock is worth less to a call.
  EXPECT_LT(lattice[0], 0.0);
  EXPECT_LT(lattice[1], 0.0);

  const double lattice_ratio = lattice[0] / lattice[1];
  const double chain_rule_ratio = std::exp(kR * (kT2 - kT1));
  EXPECT_NEAR(chain_rule_ratio, 1.0408107741923882, 1.0e-12);
  // Measured 18.4581 against the chain rule's 1.0408. The late ex-date is very
  // nearly free to a call because the holder exercises the instant before it;
  // the early one is a straight spot reduction with most of the life still to
  // run. An escrowed carry can only ever put those 4% apart.
  EXPECT_GT(lattice_ratio, 10.0) << "measured 18.4581, chain rule says 1.0408";
  EXPECT_NEAR(lattice_ratio, 18.458057364197490, 1.0e-3);

  // And the structural point, stated as its own measurement: the PUT on the
  // same schedule ranks the two events the OTHER way round (0.9298 < 1), which
  // no scalar multiple of dF/dD_i can do — the chain rule hands every side, and
  // every strike, the identical 1.0408.
  double put[2] = {kNaN, kNaN};
  const auto put_status = atx::vol::american_discrete_div_dividend_sensitivities(
      kS, kK, kT, kSigma, kR, 0.0, Side::Put, two, put, 301, ExerciseStyle::American);
  ASSERT_TRUE(put_status.has_value()) << put_status.error().to_string();
  EXPECT_GT(put[0], 0.0) << "more cash off the stock is worth MORE to a put";
  EXPECT_GT(put[1], 0.0);
  EXPECT_LT(put[0] / put[1], chain_rule_ratio);
  EXPECT_NEAR(put[0] / put[1], 0.92979744455616498, 1.0e-6) << "measured 0.9298";
}

// Out-of-window events contribute nothing and must report exactly 0, the same
// convention `hybrid_forward_div_jacobian` uses for its own window.
TEST(DiscreteDivAmerican, DividendSensitivities_OutOfWindowEventsAreExactlyZero) {
  const std::vector<CashDividend> mixed{{-0.10, 2.0}, {0.50, 2.0}, {1.90, 2.0}};
  double got[3] = {kNaN, kNaN, kNaN};
  const auto status = atx::vol::american_discrete_div_dividend_sensitivities(
      100.0, 100.0, 1.0, 0.25, 0.05, 0.0, Side::Put, mixed, got, 301, ExerciseStyle::American);
  ASSERT_TRUE(status.has_value()) << status.error().to_string();
  EXPECT_EQ(got[0], 0.0);
  EXPECT_GT(got[1], 0.0);
  EXPECT_EQ(got[2], 0.0);
}

TEST(DiscreteDivAmerican, DividendSensitivities_RejectAMismatchedOutputSpan) {
  const std::vector<CashDividend> two{{0.10, 1.0}, {0.90, 1.0}};
  double one_slot[1] = {kNaN};
  const auto status = atx::vol::american_discrete_div_dividend_sensitivities(
      100.0, 100.0, 1.0, 0.25, 0.05, 0.0, Side::Put, two, one_slot, 301,
      ExerciseStyle::American);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);

  // And the scalar contract is the price entry's, unchanged.
  double slot[2] = {kNaN, kNaN};
  const auto bad_sigma = atx::vol::american_discrete_div_dividend_sensitivities(
      100.0, 100.0, 1.0, 0.0, 0.05, 0.0, Side::Put, two, slot, 301, ExerciseStyle::American);
  ASSERT_FALSE(bad_sigma.has_value());
  EXPECT_EQ(bad_sigma.error().code(), ErrorCode::InvalidArgument);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Degenerate inputs — every one handled explicitly, none silently
// ═══════════════════════════════════════════════════════════════════════════

// A cash amount larger than the stock level at a LOW node. The lattice floors
// the post-dividend level at 0 rather than pricing a negative stock: without
// that floor a put reads back a payoff ABOVE the strike at those nodes.
TEST(DiscreteDivAmerican, DividendLargerThanTheLowestNode_ClampsInsteadOfGoingNegative) {
  // At 301 steps this lattice's lowest level at the ex-step is ~7.47, well
  // below the 20.00 dividend, so the floor genuinely binds.
  const std::vector<CashDividend> big{{0.5, 20.0}};
  const auto put = american_discrete_div_greeks(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, Side::Put, big,
                                                301, ExerciseStyle::American);
  ASSERT_TRUE(put.has_value()) << put.error().to_string();
  EXPECT_TRUE(std::isfinite(put->price));
  EXPECT_GE(put->price, 0.0);
  EXPECT_LE(put->price, 100.0) << "a put can never be worth more than its strike";
  EXPECT_NEAR(put->price, 22.743716471511213, 1.0e-9);

  // 9.3443758393719278 before cum-dividend exercise was admitted at the ex-step:
  // a 20.00 dividend on a 100 stock is exactly the regime where an American call
  // exercises the instant BEFORE the ex-date rather than after it.
  const double call = price_or_fail(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, Side::Call, big, 301,
                                    ExerciseStyle::American);
  EXPECT_GE(call, 0.0);
  EXPECT_NEAR(call, 9.3833648543233199, 1.0e-9);
}

// The floor binds at EVERY node here: 1e6 of cash on a 100 stock, so the stock
// is certainly worth 0 from the ex-date on. This is where an unfloored lattice
// stops producing prices and starts producing signed nonsense.
TEST(DiscreteDivAmerican, DividendLargerThanEveryNode_StaysFiniteAndNonNegative) {
  const std::vector<CashDividend> ruinous{{0.9, 1.0e6}};
  // A European call on a certainly-worthless stock is worth exactly nothing.
  const double euro_call = price_or_fail(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, Side::Call, ruinous,
                                         301, ExerciseStyle::European);
  EXPECT_EQ(euro_call, 0.0);

  const double euro_put = price_or_fail(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, Side::Put, ruinous, 301,
                                        ExerciseStyle::European);
  EXPECT_TRUE(std::isfinite(euro_put));
  EXPECT_GT(euro_put, 0.0);
  EXPECT_LE(euro_put, 100.0) << "a put can never be worth more than its strike";
  // Once the stock is certainly worth 0 this put IS a zero-coupon claim on the
  // strike, so the only defensible value is K*exp(-r*T). The flat continuation
  // clamp at the grid bottom pinned 96.146830494183135 here instead, short by
  // exactly level[0]*exp(-r*tau_grid) -- the PINNED VALUE was the artefact.
  // EuropeanPutOnACertainlyWorthlessStock_IsTheDiscountedStrike states it as the
  // identity rather than as a number.
  EXPECT_NEAR(euro_put, 100.0 * std::exp(-0.03), 1.0e-9);

  for (const Side side : {Side::Call, Side::Put}) {
    const double amer = price_or_fail(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, side, ruinous, 301,
                                      ExerciseStyle::American);
    EXPECT_TRUE(std::isfinite(amer));
    EXPECT_GE(amer, 0.0);
  }
  // The American put is still capped by its strike; the American call keeps
  // only the early-exercise value available BEFORE the ex-date.
  const double amer_put = price_or_fail(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, Side::Put, ruinous, 301,
                                        ExerciseStyle::American);
  EXPECT_LE(amer_put, 100.0);
  EXPECT_NEAR(amer_put, 97.335154029783297, 1.0e-9);
  const double amer_call = price_or_fail(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, Side::Call, ruinous,
                                         301, ExerciseStyle::American);
  // 12.514058272121554 before cum-dividend exercise: the holder used to have to
  // exercise a whole lattice step ahead of the ex-date to capture anything.
  EXPECT_GT(amer_call, euro_call);
  EXPECT_NEAR(amer_call, 12.55950705605116, 1.0e-9);
}

TEST(DiscreteDivAmerican, NonPositiveScalarInputs_AreInvalidArgument) {
  const auto bad = [](double S, double K, double T, double sigma, double r, double q) {
    return american_discrete_div_price(S, K, T, sigma, r, q, Side::Put, {}, 301,
                                       ExerciseStyle::American);
  };
  for (const auto &res : {bad(0.0, 100.0, 1.0, 0.2, 0.03, 0.0),   // S == 0
                          bad(-1.0, 100.0, 1.0, 0.2, 0.03, 0.0),  // S < 0
                          bad(100.0, 0.0, 1.0, 0.2, 0.03, 0.0),   // K == 0
                          bad(100.0, 100.0, 0.0, 0.2, 0.03, 0.0), // T == 0
                          bad(100.0, 100.0, -1.0, 0.2, 0.03, 0.0),// T < 0
                          bad(100.0, 100.0, 1.0, 0.0, 0.03, 0.0), // sigma == 0
                          bad(100.0, 100.0, 1.0, -0.2, 0.03, 0.0),// sigma < 0
                          bad(kNaN, 100.0, 1.0, 0.2, 0.03, 0.0),  // non-finite S
                          bad(100.0, 100.0, 1.0, 0.2, kNaN, 0.0), // non-finite r
                          bad(100.0, 100.0, 1.0, 0.2, 0.03, kNaN)}) {
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(DiscreteDivAmerican, StepCountOutsideItsRange_IsInvalidArgument) {
  for (const int steps : {0, -1, kDiscreteDivMaxSteps + 1}) {
    const auto res = american_discrete_div_price(100.0, 100.0, 1.0, 0.2, 0.03, 0.0, Side::Put, {},
                                                 steps, ExerciseStyle::American);
    ASSERT_FALSE(res.has_value()) << "steps=" << steps;
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }
  // One step is a legal (if useless) lattice for a PRICE.
  const auto one = american_discrete_div_price(100.0, 100.0, 1.0, 0.2, 0.03, 0.0, Side::Put, {}, 1,
                                               ExerciseStyle::American);
  EXPECT_TRUE(one.has_value());
}

TEST(DiscreteDivAmerican, MalformedDividendFields_AreInvalidArgument) {
  const std::vector<std::vector<CashDividend>> malformed{
      {{kNaN, 1.0}},
      {{0.5, kNaN}},
      {{std::numeric_limits<double>::infinity(), 1.0}},
      {{0.5, -1.0}}, // a negative amount is malformed, not merely out of window
  };
  for (const std::vector<CashDividend> &divs : malformed) {
    const auto res = american_discrete_div_price(100.0, 100.0, 1.0, 0.2, 0.03, 0.0, Side::Put,
                                                 divs, 301, ExerciseStyle::American);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(DiscreteDivAmerican, LatticeGreeksNeedAtLeastTwoSteps) {
  const auto res = american_discrete_div_greeks(100.0, 100.0, 1.0, 0.2, 0.03, 0.0, Side::Put, {}, 1,
                                                ExerciseStyle::American);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);

  // Two steps is the smallest lattice that carries a gamma, and it must be a
  // number rather than 0/0 — step 2 is the TERMINAL step there, which the
  // rollback never revisits.
  const auto two = american_discrete_div_greeks(100.0, 100.0, 1.0, 0.2, 0.03, 0.0, Side::Put, {}, 2,
                                                ExerciseStyle::American);
  ASSERT_TRUE(two.has_value()) << two.error().to_string();
  EXPECT_TRUE(std::isfinite(two->gamma));
  EXPECT_TRUE(std::isfinite(two->delta));
}

TEST(DiscreteDivAmerican, RiskNeutralProbabilityOutsideZeroOne_IsOutOfRange) {
  // A 500% carry over a single one-year step outruns a 5% vol's up-move, so the
  // CRR probability leaves (0, 1). The lattice refuses rather than rolling back
  // negative probabilities.
  const auto res = american_discrete_div_price(100.0, 100.0, 1.0, 0.05, 5.0, 0.0, Side::Call, {}, 1,
                                               ExerciseStyle::European);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

// ═══════════════════════════════════════════════════════════════════════════
//  The nine-greek oracle bundle
// ═══════════════════════════════════════════════════════════════════════════

// ── An independent lattice reference for the five FREE greeks ────────────────
//
// `plain_crr` above exists so "the empty span reduces to the no-dividend path"
// is a claim about two implementations rather than a tautology about one. The
// bundle reads four more outputs off the same rollback, so the same claim needs
// the same second implementation extended to carry them.
struct PlainCrrGreeks {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double theta = 0.0;
  double charm = 0.0;
};

[[nodiscard]] PlainCrrGreeks plain_crr_greeks(double S, double K, double T, double sigma, double r,
                                              double q, Side side, int steps, bool american) {
  const double dt = T / static_cast<double>(steps);
  const double sqrt_dt = std::sqrt(dt);
  const double u = std::exp(sigma * sqrt_dt);
  const double d = 1.0 / u;
  const double disc = std::exp(-r * dt);
  const double p = (std::exp((r - q) * dt) - d) / (u - d);
  const double pu = disc * p;
  const double pd = disc * (1.0 - p);
  const double sgn = (side == Side::Call) ? 1.0 : -1.0;

  const std::size_t nodes = static_cast<std::size_t>(steps) + 1U;
  std::vector<double> level(nodes);
  std::vector<double> value(nodes);
  for (std::size_t i = 0; i < nodes; ++i) {
    const int rung = 2 * static_cast<int>(i) - steps;
    level[i] = S * std::exp(sigma * sqrt_dt * static_cast<double>(rung));
    value[i] = std::max(sgn * (level[i] - K), 0.0);
  }

  double lv1[2] = {0.0, 0.0};
  double vv1[2] = {0.0, 0.0};
  double lv2[3] = {0.0, 0.0, 0.0};
  double vv2[3] = {0.0, 0.0, 0.0};
  double lv3[4] = {0.0, 0.0, 0.0, 0.0};
  double vv3[4] = {0.0, 0.0, 0.0, 0.0};
  if (steps == 2) {
    for (std::size_t i = 0; i < 3U; ++i) {
      lv2[i] = level[i];
      vv2[i] = value[i];
    }
  } else if (steps == 3) {
    for (std::size_t i = 0; i < 4U; ++i) {
      lv3[i] = level[i];
      vv3[i] = value[i];
    }
  }

  for (int k = steps - 1; k >= 0; --k) {
    const std::size_t last = static_cast<std::size_t>(k);
    for (std::size_t i = 0; i <= last; ++i) {
      level[i] *= u;
    }
    for (std::size_t i = 0; i <= last; ++i) {
      value[i] = pu * value[i + 1U] + pd * value[i];
    }
    if (american) {
      for (std::size_t i = 0; i <= last; ++i) {
        value[i] = std::max(value[i], std::max(sgn * (level[i] - K), 0.0));
      }
    }
    if (k == 3) {
      for (std::size_t i = 0; i < 4U; ++i) {
        lv3[i] = level[i];
        vv3[i] = value[i];
      }
    } else if (k == 2) {
      for (std::size_t i = 0; i < 3U; ++i) {
        lv2[i] = level[i];
        vv2[i] = value[i];
      }
    } else if (k == 1) {
      for (std::size_t i = 0; i < 2U; ++i) {
        lv1[i] = level[i];
        vv1[i] = value[i];
      }
    }
  }

  PlainCrrGreeks g;
  g.price = value[0];
  g.delta = (vv1[1] - vv1[0]) / (lv1[1] - lv1[0]);
  const double slope_up = (vv2[2] - vv2[1]) / (lv2[2] - lv2[1]);
  const double slope_dn = (vv2[1] - vv2[0]) / (lv2[1] - lv2[0]);
  g.gamma = (slope_up - slope_dn) / (0.5 * (lv2[2] - lv2[0]));
  g.theta = (vv2[1] - g.price) / (2.0 * dt);
  const double delta_3 = (vv3[2] - vv3[1]) / (lv3[2] - lv3[1]);
  g.charm = (delta_3 - g.delta) / (2.0 * dt);
  return g;
}

// The whole bundle rebuilt on the independent lattice, using the bump rule and
// the stencils the engine PUBLISHES rather than a second guess at them — the
// point of this reference is the rollback, not the arithmetic around it.
[[nodiscard]] DiscreteDivGreekBundle plain_crr_bundle(double S, double K, double T, double sigma,
                                                      double r, double q, Side side, int steps,
                                                      bool american, double horizon) {
  const double hv = discrete_div_sigma_bump(sigma);
  const double hr = kDiscreteDivRateBump;
  const double hq = kDiscreteDivYieldBump;
  const PlainCrrGreeks base = plain_crr_greeks(S, K, T, sigma, r, q, side, steps, american);
  const PlainCrrGreeks vol_up = plain_crr_greeks(S, K, T, sigma + hv, r, q, side, steps, american);
  const PlainCrrGreeks vol_dn = plain_crr_greeks(S, K, T, sigma - hv, r, q, side, steps, american);
  const double rate_up = plain_crr(S, K, T, sigma, r + hr, q, side, steps, american);
  const double rate_dn = plain_crr(S, K, T, sigma, r - hr, q, side, steps, american);
  const double yield_up = plain_crr(S, K, T, sigma, r, q + hq, side, steps, american);
  const double yield_dn = plain_crr(S, K, T, sigma, r, q - hq, side, steps, american);
  const double sgn = (side == Side::Call) ? 1.0 : -1.0;
  const double bumped_T = T - horizon;
  const double bumped = (bumped_T > 0.0)
                            ? plain_crr(S, K, bumped_T, sigma, r, q, side, steps, american)
                            : std::max(sgn * (S - K), 0.0);

  DiscreteDivGreekBundle b;
  b.price = base.price;
  b.delta = base.delta;
  b.gamma = base.gamma;
  b.theta = base.theta;
  b.charm = base.charm;
  b.vega = (vol_up.price - vol_dn.price) / (2.0 * hv);
  b.volga = (vol_up.price - 2.0 * base.price + vol_dn.price) / (hv * hv);
  b.vanna = (vol_up.delta - vol_dn.delta) / (2.0 * hv);
  b.rho = (rate_up - rate_dn) / (2.0 * hr);
  b.phi = (yield_up - yield_dn) / (2.0 * hq);
  b.theta_secant = base.price - bumped;
  return b;
}

// The same bundle off the CLOSED-FORM Black-Scholes price, with the engine's own
// bump sizes on the sigma/r/q/T axes so the comparison isolates lattice error
// instead of mixing in a stencil difference. Spot stencils are the library's
// 1e-3*S; the time stencils for the analytic theta/charm are a tight 1e-4
// because a closed form has no grid to fall off.
[[nodiscard]] DiscreteDivGreekBundle bs_stencil_bundle(double S, double K, double T, double sigma,
                                                       double r, double q, Side side,
                                                       double horizon) {
  const double hv = discrete_div_sigma_bump(sigma);
  const double hr = kDiscreteDivRateBump;
  const double hq = kDiscreteDivYieldBump;
  const double hs = 1.0e-3 * S;
  const double ht = 1.0e-4;
  const auto P = [&](double ds, double dsig, double dr, double dq, double dT) {
    return bs_price(S + ds, K, T + dT, sigma + dsig, r + dr, q + dq, side);
  };
  const double p0 = P(0.0, 0.0, 0.0, 0.0, 0.0);
  const double p_sp = P(hs, 0.0, 0.0, 0.0, 0.0);
  const double p_sm = P(-hs, 0.0, 0.0, 0.0, 0.0);
  const double p_vp = P(0.0, hv, 0.0, 0.0, 0.0);
  const double p_vm = P(0.0, -hv, 0.0, 0.0, 0.0);

  DiscreteDivGreekBundle b;
  b.price = p0;
  b.delta = (p_sp - p_sm) / (2.0 * hs);
  b.gamma = (p_sp - 2.0 * p0 + p_sm) / (hs * hs);
  b.vega = (p_vp - p_vm) / (2.0 * hv);
  b.volga = (p_vp - 2.0 * p0 + p_vm) / (hv * hv);
  b.vanna = (P(hs, hv, 0.0, 0.0, 0.0) - P(-hs, hv, 0.0, 0.0, 0.0) - P(hs, -hv, 0.0, 0.0, 0.0) +
             P(-hs, -hv, 0.0, 0.0, 0.0)) /
            (4.0 * hs * hv);
  b.rho = (P(0.0, 0.0, hr, 0.0, 0.0) - P(0.0, 0.0, -hr, 0.0, 0.0)) / (2.0 * hr);
  b.phi = (P(0.0, 0.0, 0.0, hq, 0.0) - P(0.0, 0.0, 0.0, -hq, 0.0)) / (2.0 * hq);
  // Calendar convention: dP/dt = -dP/dT, and charm = -d^2P/dS dT.
  b.theta = -(P(0.0, 0.0, 0.0, 0.0, ht) - P(0.0, 0.0, 0.0, 0.0, -ht)) / (2.0 * ht);
  b.charm = -(P(hs, 0.0, 0.0, 0.0, ht) - P(hs, 0.0, 0.0, 0.0, -ht) - P(-hs, 0.0, 0.0, 0.0, ht) +
              P(-hs, 0.0, 0.0, 0.0, -ht)) /
            (4.0 * hs * ht);
  b.theta_secant = p0 - P(0.0, 0.0, 0.0, 0.0, -horizon);
  return b;
}

// Named field access so a loop can report WHICH greek drifted.
struct NamedGreek {
  const char *name;
  double DiscreteDivGreekBundle::*field;
};

constexpr NamedGreek kNamedGreeks[] = {
    {"price", &DiscreteDivGreekBundle::price},
    {"delta", &DiscreteDivGreekBundle::delta},
    {"gamma", &DiscreteDivGreekBundle::gamma},
    {"vega", &DiscreteDivGreekBundle::vega},
    {"theta", &DiscreteDivGreekBundle::theta},
    {"rho", &DiscreteDivGreekBundle::rho},
    {"phi", &DiscreteDivGreekBundle::phi},
    {"vanna", &DiscreteDivGreekBundle::vanna},
    {"volga", &DiscreteDivGreekBundle::volga},
    {"charm", &DiscreteDivGreekBundle::charm},
    {"theta_secant", &DiscreteDivGreekBundle::theta_secant},
};

[[nodiscard]] DiscreteDivGreekBundle bundle_or_fail(double S, double K, double T, double sigma,
                                                    double r, double q, Side side,
                                                    std::span<const CashDividend> divs, int steps,
                                                    ExerciseStyle exercise) {
  const auto out = american_discrete_div_greek_bundle(S, K, T, sigma, r, q, side, divs, steps,
                                                      exercise);
  EXPECT_TRUE(out.has_value());
  return out.has_value() ? *out : DiscreteDivGreekBundle{};
}

// ── The empty span: every greek, not just the price ─────────────────────────

TEST(DiscreteDivAmerican, GreekBundle_EmptyDividendSpan_MatchesAnIndependentPlainCrrBundle) {
  for (const Scenario &s : kV0) {
    for (const Side side : {Side::Call, Side::Put}) {
      for (const bool american : {false, true}) {
        const ExerciseStyle style = american ? ExerciseStyle::American : ExerciseStyle::European;
        const DiscreteDivGreekBundle got =
            bundle_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, side, {}, 301, style);
        const DiscreteDivGreekBundle want =
            plain_crr_bundle(s.S, s.K, s.T, s.sigma, s.r, s.q, side, 301, american,
                             kDiscreteDivThetaSecantHorizon);
        for (const NamedGreek &g : kNamedGreeks) {
          EXPECT_DOUBLE_EQ(got.*(g.field), want.*(g.field))
              << g.name << " K=" << s.K << " american=" << american;
        }
      }
    }
  }
}

TEST(DiscreteDivAmerican, GreekBundle_OutOfWindowDividends_AreBitExactToTheEmptySpan) {
  const Scenario s = kV0[1];
  const std::vector<CashDividend> outside{{-0.25, 1.0}, {0.0, 1.0}, {s.T * 1.5, 1.0}, {0.5, 0.0}};
  const DiscreteDivGreekBundle with = bundle_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Put,
                                                     outside, 301, ExerciseStyle::American);
  const DiscreteDivGreekBundle without = bundle_or_fail(s.S, s.K, s.T, s.sigma, s.r, s.q, Side::Put,
                                                        {}, 301, ExerciseStyle::American);
  EXPECT_EQ(with, without);
}

// ── The cheap tier is a strict prefix of the bundle ─────────────────────────

TEST(DiscreteDivAmerican, GreekBundle_PriceDeltaGamma_AreBitIdenticalToTheCheapTier) {
  const std::vector<CashDividend> one{{0.20, 2.15}};
  const auto cheap = american_discrete_div_greeks(775.8, 780.0, 0.35, 0.18, 0.041, 0.0, Side::Put,
                                                  one, 301, ExerciseStyle::American);
  ASSERT_TRUE(cheap.has_value()) << cheap.error().to_string();
  const DiscreteDivGreekBundle wide = bundle_or_fail(775.8, 780.0, 0.35, 0.18, 0.041, 0.0,
                                                     Side::Put, one, 301, ExerciseStyle::American);
  EXPECT_EQ(wide.price, cheap->price);
  EXPECT_EQ(wide.delta, cheap->delta);
  EXPECT_EQ(wide.gamma, cheap->gamma);
}

// ── vanna is d(delta)/d(sigma), and must survive being computed that way twice ─

TEST(DiscreteDivAmerican, GreekBundle_Vanna_MatchesAnIndependentDeltaOverSigmaDifference) {
  const std::vector<CashDividend> schedule = parity_schedule();
  constexpr double kStrike[] = {700.0, 775.0, 850.0};
  for (const double K : kStrike) {
    for (const Side side : {Side::Call, Side::Put}) {
      const DiscreteDivGreekBundle wide = bundle_or_fail(kParityS, K, kParityT, kParitySigma,
                                                         kParityR, kParityQ, side, schedule, 301,
                                                         ExerciseStyle::American);
      // A fully independent central difference of the LATTICE delta over the
      // same sigma bump, built from whole `american_discrete_div_greeks` solves
      // rather than from anything the bundle produced.
      const double hv = discrete_div_sigma_bump(kParitySigma);
      const auto up = american_discrete_div_greeks(kParityS, K, kParityT, kParitySigma + hv,
                                                   kParityR, kParityQ, side, schedule, 301,
                                                   ExerciseStyle::American);
      const auto dn = american_discrete_div_greeks(kParityS, K, kParityT, kParitySigma - hv,
                                                   kParityR, kParityQ, side, schedule, 301,
                                                   ExerciseStyle::American);
      ASSERT_TRUE(up.has_value() && dn.has_value());
      const double want = (up->delta - dn->delta) / (2.0 * hv);
      EXPECT_DOUBLE_EQ(wide.vanna, want) << "K=" << K;
    }
  }
}

// ── The European no-dividend control: every greek against closed-form BS ────

// The European no-dividend control at 1201 steps. Every tolerance below is the
// measured lattice error rounded up to about 2x, and the SPREAD across the
// bundle is the finding, not any single number: eight of the ten greeks land
// within 1e-3 relative of the closed form, and volga does not — it gets its own
// two tests below because it is the only member whose accuracy claim has to be
// hedged. Bump sizes are the engine's own on every axis, so what is measured
// here is lattice error and not a stencil difference.
TEST(DiscreteDivAmerican, GreekBundle_EuropeanNoDividend_MatchesTheBlackScholesStencil) {
  constexpr double kS = 100.0;
  constexpr double kT = 1.0;
  constexpr double kSigma = 0.25;
  constexpr double kR = 0.04;
  constexpr double kQ = 0.01;
  for (const double K : {97.0, 100.0, 103.0}) {
    for (const Side side : {Side::Call, Side::Put}) {
      const DiscreteDivGreekBundle got = bundle_or_fail(kS, K, kT, kSigma, kR, kQ, side, {}, 1201,
                                                        ExerciseStyle::European);
      const DiscreteDivGreekBundle want =
          bs_stencil_bundle(kS, K, kT, kSigma, kR, kQ, side, kDiscreteDivThetaSecantHorizon);
      EXPECT_NEAR(got.price, want.price, 4.0e-3) << "K=" << K;
      EXPECT_NEAR(got.delta, want.delta, 5.0e-5) << "K=" << K;
      EXPECT_NEAR(got.gamma, want.gamma, 2.0e-5) << "K=" << K;
      EXPECT_NEAR(got.vega, want.vega, 8.0e-2) << "K=" << K;
      EXPECT_NEAR(got.theta, want.theta, 3.0e-3) << "K=" << K;
      EXPECT_NEAR(got.rho, want.rho, 1.0e-2) << "K=" << K;
      EXPECT_NEAR(got.phi, want.phi, 5.0e-3) << "K=" << K;
      EXPECT_NEAR(got.vanna, want.vanna, 1.0e-3) << "K=" << K;
      EXPECT_NEAR(got.charm, want.charm, 2.0e-4) << "K=" << K;
      EXPECT_NEAR(got.theta_secant, want.theta_secant, 1.0e-4) << "K=" << K;
    }
  }
}

// AT the money volga is the one place the second difference is clean, and the
// reason is geometric rather than lucky: the terminal grid is
// S*exp(sigma*sqrt(dt)*(2i - N)), so ln(K/S) = 0 sits at a FIXED position
// between nodes no matter what sigma does, and the node-quantization ripple the
// sigma bump would otherwise sweep through never moves. Measured error here is
// 3.8e-3 on a value of -0.169 (2.3%).
TEST(DiscreteDivAmerican, GreekBundle_AtTheMoneyVolga_MatchesTheClosedFormAndIsNegative) {
  constexpr double kS = 100.0;
  constexpr double kT = 1.0;
  constexpr double kSigma = 0.25;
  constexpr double kR = 0.04;
  constexpr double kQ = 0.01;
  for (const Side side : {Side::Call, Side::Put}) {
    const DiscreteDivGreekBundle got = bundle_or_fail(kS, kS, kT, kSigma, kR, kQ, side, {}, 1201,
                                                      ExerciseStyle::European);
    const DiscreteDivGreekBundle want =
        bs_stencil_bundle(kS, kS, kT, kSigma, kR, kQ, side, kDiscreteDivThetaSecantHorizon);
    // Sign is a statement in its own right: volga = vega*d1*d2/sigma, and just
    // above the forward-at-the-money point d1*d2 < 0. A lattice that took the
    // second difference with the wrong sign would still pass a magnitude band.
    EXPECT_LT(want.volga, 0.0);
    EXPECT_LT(got.volga, 0.0);
    EXPECT_NEAR(got.volga, want.volga, 2.0e-2);
  }
}

// AWAY from the money the same second difference divides the ripple by h^2, and
// this is the honest bound: at 1201 steps on a 100-spot option the volga error
// is ~2.2 ABSOLUTE — an order of magnitude worse than any other member of the
// bundle. It is NOT O(1/steps) (the next test measures that, and an earlier
// version of this comment claimed it was), so `steps` is not a lever at all.
// The claim that survives is the SIGN plus an order of magnitude.
TEST(DiscreteDivAmerican, GreekBundle_NearTheMoneyVolga_KeepsItsSignAndOrderOfMagnitude) {
  constexpr double kS = 100.0;
  constexpr double kT = 1.0;
  constexpr double kSigma = 0.25;
  constexpr double kR = 0.04;
  constexpr double kQ = 0.01;
  for (const Side side : {Side::Call, Side::Put}) {
    const DiscreteDivGreekBundle got = bundle_or_fail(kS, 97.0, kT, kSigma, kR, kQ, side, {}, 1201,
                                                      ExerciseStyle::European);
    const DiscreteDivGreekBundle want =
        bs_stencil_bundle(kS, 97.0, kT, kSigma, kR, kQ, side, kDiscreteDivThetaSecantHorizon);
    EXPECT_GT(want.volga, 0.0) << "below the forward-at-the-money point d1*d2 > 0";
    EXPECT_GT(got.volga, 0.0);
    EXPECT_GT(got.volga, 0.3 * want.volga);
    EXPECT_LT(got.volga, 2.0 * want.volga);
    EXPECT_LT(std::abs(got.volga - want.volga), 3.0) << "the measured bound is 2.21";
  }
}

// ── volga does NOT converge in the step count, and the bump is not a knob ────
//
// Both of these are NEGATIVE results, pinned because each one is a plausible
// fix that costs real money to try and does not work.
//
// The off-the-money volga error is O(1), not O(1/steps). The mechanism says it
// must be: the CRR node-quantisation ripple has amplitude O(1/n) and, read as a
// function of sigma, its period is sigma/|p|, where
//     p = ln(K/S) / (sigma*sqrt(dt)) == ln(K/S)*sqrt(n) / (sigma*sqrt(T))
// is the strike's offset in TERMINAL NODES. A second difference in sigma
// multiplies the ripple by (dp/dsigma)^2 = (p/sigma)^2, which grows like n. The
// two powers of n cancel EXACTLY, so refining the lattice buys nothing here — it
// only re-randomises the ripple's phase. The price off the very same lattices
// does fall like 1/steps, and asserting both in one test is what makes this a
// statement about the second difference rather than about the lattice.
TEST(DiscreteDivAmerican, GreekBundle_OffTheMoneyVolgaError_IsOrderOneInTheStepCount) {
  constexpr double kS = 100.0;
  constexpr double kT = 1.0;
  constexpr double kSigma = 0.25;
  constexpr double kR = 0.04;
  constexpr double kQ = 0.01;
  constexpr int kCoarse = 301;
  constexpr int kFine = 2401;
  for (const double K : {97.0, 103.0}) {
    // The SAME-bump Black-Scholes stencil, so what is measured is the lattice's
    // own error and not the central difference's O(h^2) truncation.
    const DiscreteDivGreekBundle want =
        bs_stencil_bundle(kS, K, kT, kSigma, kR, kQ, Side::Call, kDiscreteDivThetaSecantHorizon);
    double volga_err[3] = {0.0, 0.0, 0.0};
    double price_err[3] = {0.0, 0.0, 0.0};
    std::size_t slot = 0;
    for (const int steps : {kCoarse, 1201, kFine}) {
      const DiscreteDivGreekBundle got =
          bundle_or_fail(kS, K, kT, kSigma, kR, kQ, Side::Call, {}, steps,
                         ExerciseStyle::European);
      volga_err[slot] = std::abs(got.volga - want.volga);
      price_err[slot] = std::abs(got.price - want.price);
      // Measured 2.221 / 2.212 / 2.215 at K=97 and 2.234 / 2.220 / 2.213 at
      // K=103 — one band holds every step count, which is the whole point.
      EXPECT_GT(volga_err[slot], 2.0) << "K=" << K << " steps=" << steps;
      EXPECT_LT(volga_err[slot], 2.5) << "K=" << K << " steps=" << steps;
      ++slot;
    }
    // An O(1/steps) error would shrink EIGHTFOLD over this range. Measured 0.997
    // and 0.990 — it does not shrink at all.
    EXPECT_GT(volga_err[2], 0.8 * volga_err[0])
        << "K=" << K << " volga error must NOT decay with the step count";
    // The price off the same eight solves does decay: measured 0.128 (K=97) and
    // 0.115 (K=103) of its 301-step value, against the 0.125 a 1/steps law asks
    // for. Without this leg the test above would also pass on a lattice that had
    // simply stopped converging altogether.
    EXPECT_LT(price_err[2], 0.25 * price_err[0])
        << "K=" << K << " the PRICE error is O(1/steps) on the same lattices";
  }
}

// The sigma bump is a fixed constant and not a tuning knob, and this pins WHY:
// the error-versus-bump curve is not the U-shape a truncation/noise trade-off
// would give, so there is no bump that is right everywhere. Widening it from
// 10% to 30% of sigma makes the AT-THE-MONEY volga — the one place the bundle
// is accurate — 11.6x worse, while off the money it moves in whichever
// direction the ripple's phase happens to send it (worse at K=103, better at
// K=97, on the same lattice). Anything read off this axis is a coincidence of
// one (K, T, sigma, steps) and does not transfer.
TEST(DiscreteDivAmerican, GreekBundle_AWiderSigmaBump_TradesTheAtTheMoneyVolgaForNothing) {
  constexpr double kS = 100.0;
  constexpr double kT = 1.0;
  constexpr double kSigma = 0.25;
  constexpr double kR = 0.04;
  constexpr double kQ = 0.01;
  constexpr double kWideBumpRel = 0.30;
  // An independent second difference of the shipped PRICE entry at an arbitrary
  // bump — the bundle's own stencil, with the constant replaced.
  const auto volga_at = [&](double K, double bump_rel, int steps) {
    const double h = bump_rel * kSigma;
    const double up = price_or_fail(kS, K, kT, kSigma + h, kR, kQ, Side::Call, {}, steps,
                                    ExerciseStyle::European);
    const double base = price_or_fail(kS, K, kT, kSigma, kR, kQ, Side::Call, {}, steps,
                                      ExerciseStyle::European);
    const double dn = price_or_fail(kS, K, kT, kSigma - h, kR, kQ, Side::Call, {}, steps,
                                    ExerciseStyle::European);
    return (up - 2.0 * base + dn) / (h * h);
  };
  const auto rel_err = [&](double K, double bump_rel, int steps) {
    // Against the ANALYTIC curvature, not a same-bump stencil: a wider bump
    // changes its OWN truncation, so a same-bump reference would score the 30%
    // stencil against a target that had moved with it.
    const double exact = bs_analytic_volga(kS, K, kT, kSigma, kR, kQ);
    return std::abs(volga_at(K, bump_rel, steps) - exact) / std::abs(exact);
  };
  // AT the money the shipped bump is the accurate one, by an order of magnitude.
  const double atm_narrow = rel_err(100.0, kDiscreteDivSigmaBumpRel, 1201);
  const double atm_wide = rel_err(100.0, kWideBumpRel, 1201);
  EXPECT_LT(atm_narrow, 0.15) << "measured 0.098";
  EXPECT_GT(atm_wide, 5.0 * atm_narrow) << "measured 11.6x worse: " << atm_wide;
  // One strike up it is worse at the wider bump, one strike down it is better —
  // same lattice, same bump pair. That is a phase, not an optimum.
  EXPECT_GT(rel_err(103.0, kWideBumpRel, 301), rel_err(103.0, kDiscreteDivSigmaBumpRel, 301))
      << "measured 0.992 vs 0.913";
  EXPECT_LT(rel_err(97.0, kWideBumpRel, 301), rel_err(97.0, kDiscreteDivSigmaBumpRel, 301))
      << "measured 0.226 vs 0.338 — the opposite direction at the adjacent strike";
}

// ── The price is the vendor-validated number: pin it against ANY volga work ──
//
// `price` is measured against SpiderRock's `srPrc` and against the Python
// reference. A scheme change made to improve volga — a wider bump, a step-count
// extrapolation, a strike-aligned tree — is only a bug fix while these four
// literals hold; the moment one moves it is a NEW vendor measurement and has to
// be re-validated before it ships. The literals were produced by the reference
// implementation of this same scheme, so they also re-check the port.
TEST(DiscreteDivAmerican, GreekBundle_Price_IsPinnedAgainstAnyGreekSchemeChange) {
  const std::vector<CashDividend> schedule = parity_schedule();
  const DiscreteDivGreekBundle call = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma,
                                                     kParityR, kParityQ, Side::Call, schedule,
                                                     301, ExerciseStyle::American);
  const DiscreteDivGreekBundle put = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma,
                                                    kParityR, kParityQ, Side::Put, schedule, 301,
                                                    ExerciseStyle::American);
  // 75.222227622486997 before the grid-bottom extrapolation in `splice_dividend`;
  // the earliest of these six ex-dates lands on step 22, where the corner node
  // the flat clamp mishandled still carries ~(1-p)^22 of probability. The PUT is
  // untouched to the last bit, which is what says the move is the edge and not
  // the exercise rule.
  EXPECT_DOUBLE_EQ(call.price, 75.22222755839276);
  EXPECT_DOUBLE_EQ(put.price, 54.128579425756222);

  const DiscreteDivGreekBundle euro_call = bundle_or_fail(100.0, 97.0, 1.0, 0.25, 0.04, 0.01,
                                                          Side::Call, {}, 1201,
                                                          ExerciseStyle::European);
  const DiscreteDivGreekBundle euro_put = bundle_or_fail(100.0, 103.0, 1.0, 0.25, 0.04, 0.01,
                                                         Side::Put, {}, 1201,
                                                         ExerciseStyle::European);
  EXPECT_DOUBLE_EQ(euro_call.price, 12.742321369885115);
  EXPECT_DOUBLE_EQ(euro_put.price, 9.8266034504230042);
}

// ── Convergence: 301 vs 1201 steps ─────────────────────────────────────────

TEST(DiscreteDivAmerican, GreekBundle_IsStableBetween301And1201Steps) {
  const std::vector<CashDividend> schedule = parity_schedule();
  // Tolerances are RELATIVE to the 1201-step value except where the greek can
  // sit at zero. They were measured, not guessed, and they are not uniform:
  // the price/delta/gamma/theta/charm family comes off ONE lattice, so the
  // O(1/steps) ripple largely cancels inside it, while every bumped greek
  // differences two DIFFERENT lattices and keeps the ripple. volga divides that
  // residue by h^2 and is the loosest member of the bundle by two orders of
  // magnitude -- see the accuracy note at the bottom of this file.
  struct Tol {
    const char *name;
    double DiscreteDivGreekBundle::*field;
    double rel;
  };
  const Tol kTols[] = {
      {"price", &DiscreteDivGreekBundle::price, 5.0e-3},        // measured 2.8e-3
      {"delta", &DiscreteDivGreekBundle::delta, 1.0e-3},        // measured 3.4e-4
      {"gamma", &DiscreteDivGreekBundle::gamma, 5.0e-3},        // measured 1.2e-3
      {"vega", &DiscreteDivGreekBundle::vega, 2.0e-3},          // measured 7.2e-4
      {"theta", &DiscreteDivGreekBundle::theta, 1.0e-2},        // measured 3.5e-3
      {"rho", &DiscreteDivGreekBundle::rho, 5.0e-3},            // measured 2.1e-3
      {"phi", &DiscreteDivGreekBundle::phi, 5.0e-3},            // measured 1.6e-3
      {"vanna", &DiscreteDivGreekBundle::vanna, 5.0e-2},        // measured 1.9e-2
      {"volga", &DiscreteDivGreekBundle::volga, 5.0e-1},        // measured 1.9e-1 -- the outlier
      {"charm", &DiscreteDivGreekBundle::charm, 2.0e-2},        // measured 7.5e-3
      {"theta_secant", &DiscreteDivGreekBundle::theta_secant, 1.0e-1}, // measured 4.6e-2
  };
  for (const Side side : {Side::Call, Side::Put}) {
    const DiscreteDivGreekBundle coarse = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma,
                                                         kParityR, kParityQ, side, schedule, 301,
                                                         ExerciseStyle::American);
    const DiscreteDivGreekBundle fine = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma,
                                                       kParityR, kParityQ, side, schedule, 1201,
                                                       ExerciseStyle::American);
    for (const Tol &t : kTols) {
      const double a = coarse.*(t.field);
      const double b = fine.*(t.field);
      ASSERT_TRUE(std::isfinite(a) && std::isfinite(b)) << t.name;
      EXPECT_LE(std::abs(a - b), t.rel * std::abs(b))
          << t.name << " 301=" << a << " 1201=" << b;
    }
  }
}

// ── The two thetas are two different numbers, on purpose ───────────────────

TEST(DiscreteDivAmerican, GreekBundle_SecantTheta_IsAOneDayQuantityAndNotTheTangent) {
  const std::vector<CashDividend> schedule = parity_schedule();
  const DiscreteDivGreekBundle g = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma, kParityR,
                                                  kParityQ, Side::Put, schedule, 301,
                                                  ExerciseStyle::American);
  // The secant is EXACTLY P(T) - P(T - 1/252) off the same engine, with the
  // schedule re-anchored by the engine's own window.
  const double at_T = price_or_fail(kParityS, 775.0, kParityT, kParitySigma, kParityR, kParityQ,
                                    Side::Put, schedule, 301, ExerciseStyle::American);
  const double at_bumped =
      price_or_fail(kParityS, 775.0, kParityT - kDiscreteDivThetaSecantHorizon, kParitySigma,
                    kParityR, kParityQ, Side::Put, schedule, 301, ExerciseStyle::American);
  EXPECT_EQ(g.theta_secant, at_T - at_bumped);

  // Decay: the secant is reported POSITIVE, the calendar tangent NEGATIVE.
  EXPECT_GT(g.theta_secant, 0.0);
  EXPECT_LT(g.theta, 0.0);

  // THE DOUBLE-SCALING TRAP. The secant is already a one-day dollar amount; the
  // per-year tangent divided by 252 is the number it is closest to, and the two
  // must not be confused, but dividing the secant AGAIN by 252 puts it two
  // orders of magnitude away from both.
  const double tangent_per_day = -g.theta / 252.0;
  EXPECT_NEAR(g.theta_secant, tangent_per_day, 0.25 * std::abs(tangent_per_day));
  EXPECT_NE(g.theta_secant, tangent_per_day);
  EXPECT_GT(std::abs(g.theta_secant - g.theta_secant / 252.0),
            0.5 * std::abs(g.theta_secant))
      << "a re-divided secant would be ~252x too small";
}

// WITHOUT dividends the option value depends only on the time REMAINING, so the
// two theta forms are two estimates of one number: the secant's slope is
// dP/dT + O(h) and the calendar tangent is -dP/dT, so |slope + theta| is pure
// truncation and must halve when h halves. Measured 4.74e-3 -> 2.19e-3 ->
// 9.13e-4 (ratios 0.46 and 0.42), so the bar below is 0.6 per halving.
TEST(DiscreteDivAmerican, GreekBundle_SecantSlope_ApproachesTheTangentAsTheHorizonShrinks) {
  constexpr int kSteps = 601;
  constexpr double kS = 100.0;
  constexpr double kT = 1.0;
  constexpr double kSigma = 0.25;
  constexpr double kR = 0.04;
  constexpr double kQ = 0.01;
  for (const ExerciseStyle style : {ExerciseStyle::European, ExerciseStyle::American}) {
    const DiscreteDivGreekBundle base =
        bundle_or_fail(kS, kS, kT, kSigma, kR, kQ, Side::Put, {}, kSteps, style);
    double previous = std::numeric_limits<double>::infinity();
    for (const double horizon : {1.0 / 252.0, 1.0 / 504.0, 1.0 / 1008.0}) {
      const auto res = american_discrete_div_greek_bundle(kS, kS, kT, kSigma, kR, kQ, Side::Put, {},
                                                          kSteps, style, horizon);
      ASSERT_TRUE(res.has_value()) << res.error().to_string();
      const double gap = std::abs(res->theta_secant / horizon + base.theta);
      EXPECT_LT(gap, 0.6 * previous) << "horizon=1/" << 1.0 / horizon << " gap=" << gap;
      previous = gap;
    }
    // Truncation that shrinks is still truncation that EXISTS: at the horizon
    // that ships, the secant is not the tangent.
    EXPECT_NE(base.theta_secant, -base.theta * kDiscreteDivThetaSecantHorizon);
  }
}

// WITH discrete dividends the two forms stop being two estimates of one number,
// and this is why the bundle ships both rather than picking one.
//
// The value depends on `t` and `T` SEPARATELY once the ex-dates are pinned to
// absolute calendar time: writing V = f(T - t; {tau_i - t}),
//   dV/dt + dV/dT = -sum_i dV/dtau_i,
// which is zero only when there are no ex-dates. The calendar tangent advances
// the CLOCK and carries the schedule with it (each ex-date keeps its distance to
// expiry); the secant moves EXPIRY and leaves the schedule where it is (each
// ex-date lands closer to expiry, and any ex-date past T - h is dropped
// entirely). Measured on the schedule below: the gap is 0.146 European and
// 0.419 American -- 4% and 12% of theta -- and unlike the dividend-free case it
// does NOT shrink when the horizon shrinks, because it is not truncation.
TEST(DiscreteDivAmerican, GreekBundle_WithDividends_TheTwoThetaFormsDivergeAndStayDiverged) {
  constexpr int kSteps = 601;
  constexpr double kS = 100.0;
  constexpr double kT = 1.0;
  constexpr double kSigma = 0.25;
  constexpr double kR = 0.04;
  constexpr double kQ = 0.01;
  const std::vector<CashDividend> schedule{{0.30, 0.9}, {0.80, 0.9}};
  for (const ExerciseStyle style : {ExerciseStyle::European, ExerciseStyle::American}) {
    const DiscreteDivGreekBundle base =
        bundle_or_fail(kS, kS, kT, kSigma, kR, kQ, Side::Put, schedule, kSteps, style);
    // 1/252 and 1/504 are the asserted pair. 1/1008 is deliberately NOT
    // asserted at the same bar: below about 1/500 the dividend STEP INDEX
    // becomes the noise floor -- `nearbyint(tau/dt)` snaps each ex-date to a
    // lattice step, dt moves with T, and the effective ex-date therefore jumps
    // by up to dt/2 between the two legs while the price difference the secant
    // is measuring has shrunk to a few thousandths of a dollar. The measured
    // European gap drops to 0.053 there for that reason, not because the
    // definitional divergence went away.
    for (const double horizon : {1.0 / 252.0, 1.0 / 504.0}) {
      const auto res = american_discrete_div_greek_bundle(kS, kS, kT, kSigma, kR, kQ, Side::Put,
                                                          schedule, kSteps, style, horizon);
      ASSERT_TRUE(res.has_value()) << res.error().to_string();
      const double gap = std::abs(res->theta_secant / horizon + base.theta);
      EXPECT_GT(gap, 0.10) << "horizon=1/" << 1.0 / horizon << " gap=" << gap;
      // The dividend-free truncation at the SAME horizons is 4.7e-3 and 2.2e-3
      // (test above). This gap is twenty times larger and does not halve.
      EXPECT_GT(gap, 20.0 * 4.74e-3);
    }
    const auto fine = american_discrete_div_greek_bundle(kS, kS, kT, kSigma, kR, kQ, Side::Put,
                                                         schedule, kSteps, style, 1.0 / 1008.0);
    ASSERT_TRUE(fine.has_value()) << fine.error().to_string();
    EXPECT_GT(std::abs(fine->theta_secant * 1008.0 + base.theta), 0.03)
        << "even at the noise floor the two forms have not converged on each other";
  }
}

// ── The expiration-day leg ─────────────────────────────────────────────────

TEST(DiscreteDivAmerican, GreekBundle_ExpirationDayHorizon_TakesTheIntrinsicBumpedLeg) {
  // T < 1/252, so `T - horizon <= 0` and the bumped leg is the intrinsic.
  constexpr double kT = 1.0 / 504.0;
  const std::vector<CashDividend> schedule{{kT, 0.5}};

  // At the money the kink resolves to the OUT-of-the-money side: intrinsic 0,
  // for BOTH sides, so the secant is the whole premium.
  for (const Side side : {Side::Call, Side::Put}) {
    const DiscreteDivGreekBundle g =
        bundle_or_fail(100.0, 100.0, kT, 0.25, 0.04, 0.01, side, schedule, 301,
                       ExerciseStyle::American);
    EXPECT_EQ(g.theta_secant, g.price) << "the ATM intrinsic leg must be exactly 0";
    EXPECT_GT(g.price, 0.0);
  }

  // In the money the leg is the intrinsic itself, exactly.
  const DiscreteDivGreekBundle itm_call =
      bundle_or_fail(110.0, 100.0, kT, 0.25, 0.04, 0.01, Side::Call, schedule, 301,
                     ExerciseStyle::American);
  EXPECT_EQ(itm_call.theta_secant, itm_call.price - 10.0);
  const DiscreteDivGreekBundle itm_put =
      bundle_or_fail(90.0, 100.0, kT, 0.25, 0.04, 0.01, Side::Put, schedule, 301,
                     ExerciseStyle::American);
  EXPECT_EQ(itm_put.theta_secant, itm_put.price - 10.0);

  // Out of the money the leg is 0, so the secant is again the whole premium.
  const DiscreteDivGreekBundle otm_call =
      bundle_or_fail(90.0, 100.0, kT, 0.25, 0.04, 0.01, Side::Call, schedule, 301,
                     ExerciseStyle::American);
  EXPECT_EQ(otm_call.theta_secant, otm_call.price);

  // Exactly at the boundary T == horizon the leg is still the intrinsic, not an
  // epsilon-maturity lattice.
  const DiscreteDivGreekBundle at_boundary =
      bundle_or_fail(100.0, 100.0, kDiscreteDivThetaSecantHorizon, 0.25, 0.04, 0.01, Side::Call, {},
                     301, ExerciseStyle::American);
  EXPECT_EQ(at_boundary.theta_secant, at_boundary.price);
}

// ── Dividend re-anchoring under the maturity bump ──────────────────────────

TEST(DiscreteDivAmerican, GreekBundle_MaturityBump_DropsDividendsOutsideTheBumpedWindow) {
  constexpr double kT = 0.50;
  constexpr double kH = 1.0 / 252.0;
  constexpr double kBumped = kT - kH;
  // Two ex-dates: one comfortably inside the bumped window, one ON expiry and
  // therefore OUTSIDE it once the maturity is bumped down by a day.
  const std::vector<CashDividend> schedule{{0.25, 1.10}, {kT, 2.35}};
  const std::vector<CashDividend> survivors{{0.25, 1.10}};
  const std::vector<CashDividend> clipped{{0.25, 1.10}, {kBumped, 2.35}};

  for (const Side side : {Side::Call, Side::Put}) {
    for (const ExerciseStyle style : {ExerciseStyle::European, ExerciseStyle::American}) {
      const DiscreteDivGreekBundle g =
          bundle_or_fail(100.0, 100.0, kT, 0.25, 0.04, 0.01, side, schedule, 301, style);
      const double dropped =
          price_or_fail(100.0, 100.0, kBumped, 0.25, 0.04, 0.01, side, survivors, 301, style);
      EXPECT_EQ(g.theta_secant, g.price - dropped) << "the ex-date past T-h must be DROPPED";

      // Clipping it onto the bumped terminal step instead prices a cash flow the
      // option no longer lives to receive -- with ONE exception, and it is an
      // economic fact rather than a numerical accident: an AMERICAN CALL facing
      // an ex-date ON its own expiry exercises the instant before it and takes
      // max(S - K, 0) either way, so a terminal dividend is invisible to it and
      // the two prices agree to the LAST BIT. Before cum-dividend exercise was
      // admitted this leg read a gap of 0.0241, an artefact of the holder having
      // to exercise a whole lattice step early to escape the drop.
      const double clamped =
          price_or_fail(100.0, 100.0, kBumped, 0.25, 0.04, 0.01, side, clipped, 301, style);
      if (side == Side::Call && style == ExerciseStyle::American) {
        EXPECT_EQ(dropped, clamped) << "a terminal ex-date cannot move an American call";
      } else {
        EXPECT_NE(dropped, clamped);
      }
    }
  }

  // How big the mistake is, side by side. The EUROPEAN legs show it at full
  // size (1.03 for the call, 1.27 for the put on a ~7 premium) because a
  // European holder simply eats the terminal drop. The AMERICAN PUT keeps
  // 1.13 of it, because cum-dividend exercise is dominated for a put. The
  // AMERICAN CALL is the one place it vanishes ENTIRELY -- measured 0.024
  // while the exercise test ran one lattice step early, exactly 0 once it runs
  // at the ex-step itself. That is why this test does not assert one magnitude
  // for all four legs.
  const double euro_call_dropped =
      price_or_fail(100.0, 100.0, kBumped, 0.25, 0.04, 0.01, Side::Call, survivors, 301,
                    ExerciseStyle::European);
  const double euro_call_clipped =
      price_or_fail(100.0, 100.0, kBumped, 0.25, 0.04, 0.01, Side::Call, clipped, 301,
                    ExerciseStyle::European);
  EXPECT_GT(std::abs(euro_call_dropped - euro_call_clipped), 0.5) << "measured 1.03";
  const double euro_put_dropped =
      price_or_fail(100.0, 100.0, kBumped, 0.25, 0.04, 0.01, Side::Put, survivors, 301,
                    ExerciseStyle::European);
  const double euro_put_clipped =
      price_or_fail(100.0, 100.0, kBumped, 0.25, 0.04, 0.01, Side::Put, clipped, 301,
                    ExerciseStyle::European);
  EXPECT_GT(std::abs(euro_put_dropped - euro_put_clipped), 0.5) << "measured 1.27";
}

// ── Degenerate inputs ──────────────────────────────────────────────────────

TEST(DiscreteDivAmerican, GreekBundle_DegenerateInputs_AreInvalidArgument) {
  const auto bad = [](double S, double K, double T, double sigma, double r, double q) {
    return american_discrete_div_greek_bundle(S, K, T, sigma, r, q, Side::Put, {}, 301,
                                              ExerciseStyle::American);
  };
  for (const auto &res : {bad(0.0, 100.0, 1.0, 0.2, 0.03, 0.0),
                          bad(-1.0, 100.0, 1.0, 0.2, 0.03, 0.0),
                          bad(100.0, 0.0, 1.0, 0.2, 0.03, 0.0),
                          bad(100.0, 100.0, 0.0, 0.2, 0.03, 0.0),
                          bad(100.0, 100.0, -1.0, 0.2, 0.03, 0.0),
                          bad(100.0, 100.0, 1.0, 0.0, 0.03, 0.0),
                          bad(100.0, 100.0, 1.0, -0.2, 0.03, 0.0),
                          bad(kNaN, 100.0, 1.0, 0.2, 0.03, 0.0),
                          bad(100.0, 100.0, 1.0, 0.2, kNaN, 0.0),
                          bad(100.0, 100.0, 1.0, 0.2, 0.03, kNaN)}) {
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }

  // Charm reads a step-3 delta, so the bundle's floor is one step above the
  // cheap tier's: 2 steps price and carry a gamma, and still refuse here.
  for (const int steps : {0, -1, 1, 2, kDiscreteDivMaxSteps + 1}) {
    const auto res = american_discrete_div_greek_bundle(100.0, 100.0, 1.0, 0.2, 0.03, 0.0,
                                                        Side::Put, {}, steps,
                                                        ExerciseStyle::American);
    ASSERT_FALSE(res.has_value()) << "steps=" << steps;
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument) << "steps=" << steps;
  }
  const auto three = american_discrete_div_greek_bundle(100.0, 100.0, 1.0, 0.2, 0.03, 0.0,
                                                        Side::Put, {}, 3,
                                                        ExerciseStyle::American);
  ASSERT_TRUE(three.has_value()) << three.error().to_string();
  EXPECT_TRUE(std::isfinite(three->charm));

  for (const double horizon : {0.0, -1.0 / 252.0, kNaN,
                               std::numeric_limits<double>::infinity()}) {
    const auto res = american_discrete_div_greek_bundle(100.0, 100.0, 1.0, 0.2, 0.03, 0.0,
                                                        Side::Put, {}, 301,
                                                        ExerciseStyle::American, horizon);
    ASSERT_FALSE(res.has_value()) << "horizon=" << horizon;
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument) << "horizon=" << horizon;
  }

  const std::vector<CashDividend> malformed{{0.5, -1.0}};
  const auto bad_div = american_discrete_div_greek_bundle(100.0, 100.0, 1.0, 0.2, 0.03, 0.0,
                                                          Side::Put, malformed, 301,
                                                          ExerciseStyle::American);
  ASSERT_FALSE(bad_div.has_value());
  EXPECT_EQ(bad_div.error().code(), ErrorCode::InvalidArgument);
}

// ── Signs, on the dividend path that motivated the engine ──────────────────

TEST(DiscreteDivAmerican, GreekBundle_SignsAreSaneOnTheDividendPath) {
  const std::vector<CashDividend> schedule = parity_schedule();
  const DiscreteDivGreekBundle call = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma,
                                                     kParityR, kParityQ, Side::Call, schedule, 301,
                                                     ExerciseStyle::American);
  EXPECT_GT(call.delta, 0.0);
  EXPECT_LT(call.delta, 1.0);
  EXPECT_GT(call.gamma, 0.0);
  EXPECT_GT(call.vega, 0.0);
  EXPECT_GT(call.rho, 0.0) << "a call gains from a higher rate";
  EXPECT_LT(call.phi, 0.0) << "a call loses from a higher carry yield";

  const DiscreteDivGreekBundle put = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma,
                                                    kParityR, kParityQ, Side::Put, schedule, 301,
                                                    ExerciseStyle::American);
  EXPECT_LT(put.delta, 0.0);
  EXPECT_GT(put.delta, -1.0);
  EXPECT_GT(put.gamma, 0.0);
  EXPECT_GT(put.vega, 0.0);
  EXPECT_LT(put.rho, 0.0) << "a put loses from a higher rate";
  EXPECT_GT(put.phi, 0.0) << "a put gains from a higher carry yield";

  // Gamma parity: differentiating C - P = S*exp(-qT) - PV(div) - K*exp(-rT)
  // twice in S kills every term, so a EUROPEAN call and put at one strike must
  // report the same gamma. The lattice does, to 2.0e-6 relative. The AMERICAN
  // pair does NOT (measured 12% apart) and must not be asserted to -- early
  // exercise is not linear in S, so there is no parity relation left to
  // differentiate. This is the shape of a test that would silently pass on a
  // broken engine if it were written on the American path with a loose bar.
  const DiscreteDivGreekBundle euro_call = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma,
                                                          kParityR, kParityQ, Side::Call, schedule,
                                                          301, ExerciseStyle::European);
  const DiscreteDivGreekBundle euro_put = bundle_or_fail(kParityS, 775.0, kParityT, kParitySigma,
                                                         kParityR, kParityQ, Side::Put, schedule,
                                                         301, ExerciseStyle::European);
  EXPECT_NEAR(euro_call.gamma, euro_put.gamma, 1.0e-5 * euro_call.gamma);
  EXPECT_GT(std::abs(call.gamma - put.gamma), 1.0e-2 * call.gamma)
      << "American gamma parity does NOT hold; if it did, the exercise test is not firing";
}

// ── Measured accuracy of the bundle, collected in one place ──────────────────
//
// Every number here comes from the tests above, on the `dev` preset. Two
// families behave differently, and the split is STRUCTURAL rather than
// incidental — it follows from how many lattices each greek differences.
//
// FIVE FREE GREEKS (price, delta, gamma, theta, charm) come off ONE rollback,
// so the lattice's O(1/steps) node-quantization error is COMMON to both nodes of
// every difference and largely cancels. Against closed-form Black-Scholes at
// 1201 steps (S=100, sigma=0.25, T=1, European, no dividends), absolute:
// price 2.0e-3, delta 1.4e-5, gamma 4.0e-6, theta 1.3e-3, charm 5.8e-5.
//
// BUMPED GREEKS difference two DIFFERENT lattices and keep that error. A FIRST
// difference divides it by h once, which is enough: at the same 1201 steps,
// vega 3.8e-2 (0.1% of 38.3), rho 3.8e-3, phi 2.0e-3, vanna 2.6e-4,
// theta_secant 1.8e-5.
//
// VOLGA is the exception, and the one number this bundle does not stand behind
// away from the money: a SECOND difference divides the same error by h^2. AT the
// money there is no ripple to divide — ln(K/S) = 0 holds a fixed position in the
// terminal grid S*exp(sigma*sqrt(dt)*(2i - N)) however sigma moves — and the
// error is 3.8e-3 on -0.169 (2.3%). ONE strike either side of the money it is
// 2.2 ABSOLUTE: 35% of the K=97 value, 90% of the K=103 value.
//
// NEITHER `steps` NOR the bump size is a lever, and both were measured rather
// than argued (the two tests above pin each one):
//   - the error is O(1) in the step count, not O(1/steps). 2.221 / 2.212 /
//     2.215 at 301 / 1201 / 2401 steps at K=97; 2.234 / 2.220 / 2.213 at K=103.
//     The ripple is O(1/n) but its PERIOD in sigma is sigma/|p| with
//     p = ln(K/S)*sqrt(n)/(sigma*sqrt(T)), so the second difference multiplies
//     it by (p/sigma)^2 ~ n and the two powers of n cancel. An earlier version
//     of this note asserted O(1/steps); it was wrong, and a step-count
//     refinement is a quadratic bill for nothing.
//   - the error-versus-bump curve is not U-shaped, so there is no optimum to
//     find. Swept 2%-90% of sigma at 301 and 1201 steps across
//     K in {85, 97, 100, 103, 110}, T in {0.25, 1, 2}, sigma in {0.15, .25, .5}:
//     the best bump is 0.05 at the money, 0.30 at K=97, 0.90 at K=103, 0.20 at
//     K=110 — it is wherever the ripple's phase lands, and it does not transfer.
//     A 30% bump costs the at-the-money volga 11.6x and costs vanna (which
//     shares those two solves) 10-100x.
//   - averaging or Richardson-extrapolating the sigma legs over step counts
//     does not fix it either: (n, n+1) is worse than plain at K=103 (2.17 vs
//     0.91 relative, sign flipped) and Richardson over (n, 2n) is worse at
//     K=110 (2.28 vs 1.16, sign flipped). Even a 16-lattice average over
//     [n, 2n) — 48 solves for one greek — leaves 0.24 at K=97 and 0.47 at
//     K=103, because the residue is a bias and not an oscillation.
//
// Making volga materially better needs a strike-aligned tree (Leisen-Reimer /
// Tian). Measured what that would buy, at 301 steps European: relative volga
// error 0.0061 / 0.0133 / 0.1189 / 0.0000 / 0.0125 / 0.0043 at
// K = 85 / 97 / 100 / 103 / 110 / 130, against this lattice's
// 0.227 / 0.338 / 0.037 / 0.913 / 1.159 / 0.056 — one to two orders of
// magnitude, everywhere except at the money where LR reproduces the stencil's
// own truncation and this lattice happens to be clean. It is a different PRICE
// (LR's 301-step price error is 5e-6 against Black-Scholes where this lattice's
// is 7.4e-3) and therefore a different measurement against the vendor mark, not
// a tuning change here.
//
// STEP-COUNT STABILITY, 301 vs 1201, on the six-dividend American parity
// scenario, relative: price 2.8e-3, delta 3.4e-4, gamma 1.2e-3, vega 7.2e-4,
// theta 3.5e-3, rho 2.1e-3, phi 1.6e-3, charm 7.5e-3, vanna 1.9e-2,
// theta_secant 4.6e-2, volga 1.9e-1. Same ordering, same reason.




// ═══════════════════════════════════════════════════════════════════════════
//  The route decision — one place, explicit, switchable, and reported
// ═══════════════════════════════════════════════════════════════════════════
//
// `discrete_div_route` (api/pricing/dividend.hpp) is the seam between the
// escrowed forward and this lattice. Everything below is about the seam holding:
// the two routes must start from the SAME set of cash events, or a comparison
// between them is measuring the window and not the model.

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000'000;

[[nodiscard]] std::int64_t at_years(double years) {
  return kNow + static_cast<std::int64_t>(years * atx::vol::kCalendarYearNs);
}

} // namespace

// The switch. Escrow is not "the lattice with an empty schedule" — it is no
// route at all, so a bisection flipping this enum flips exactly one decision.
TEST(DiscreteDivRoute, EscrowPolicy_NeverProducesASchedule) {
  const std::vector<atx::vol::DividendEvent> evs{{at_years(0.10), 1.5}, {at_years(0.40), 1.5}};
  const std::int64_t expiry = at_years(0.75);
  const double T = static_cast<double>(expiry - kNow) / atx::vol::kCalendarYearNs;
  const auto escrow = atx::vol::discrete_div_route(evs, expiry, kNow, T, 0.045,
                                                   atx::vol::DiscreteDivPolicy::Escrow);
  EXPECT_FALSE(escrow.applies());
  EXPECT_TRUE(escrow.schedule.empty());
  EXPECT_EQ(escrow.pv, 0.0);

  const auto lattice = atx::vol::discrete_div_route(evs, expiry, kNow, T, 0.045,
                                                    atx::vol::DiscreteDivPolicy::Lattice);
  EXPECT_TRUE(lattice.applies());
  EXPECT_EQ(lattice.schedule.size(), 2U);
}

// The identity that makes the two routes comparable at all: the cash the lattice
// splices and the cash the escrowed forward removes are THE SAME CASH, to the
// last bit of the discounting. If this ever fails, an escrow-versus-lattice
// price difference is partly a window difference and the measurement is void.
TEST(DiscreteDivRoute, ThePvItReportsIsExactlyWhatTheEscrowedForwardRemoves) {
  const std::vector<atx::vol::DividendEvent> evs{
      {at_years(-0.05), 1.90}, // already paid: outside the instant window
      {at_years(0.10), 1.98},  {at_years(0.40), 2.05}, {at_years(0.70), 2.15},
      {at_years(0.90), 2.35}}; // after expiry: outside the instant window
  const std::int64_t expiry = at_years(0.75);
  const double T = static_cast<double>(expiry - kNow) / atx::vol::kCalendarYearNs;
  constexpr double kS = 775.8, kR = 0.041;
  const auto route = atx::vol::discrete_div_route(evs, expiry, kNow, T, kR,
                                                  atx::vol::DiscreteDivPolicy::Lattice);
  ASSERT_TRUE(route.applies());
  EXPECT_EQ(route.schedule.size(), 3U);
  EXPECT_EQ(route.n_outside_tau_window, 0U) << "a Calendar365 T loses nothing to the tau screen";

  const double escrowed =
      atx::vol::forward_div_corrected(kS, kR, T, evs, expiry, kNow);
  EXPECT_NEAR(escrowed, (kS - route.pv) * std::exp(kR * T), 1.0e-9);

  // And each tau is on the option's own clock, the conversion that discounts
  // CASH — the same one hybrid_forward_div_jacobian uses.
  for (std::size_t i = 0; i < 3U; ++i) {
    const double want =
        static_cast<double>(evs[i + 1U].ex_date_ns - kNow) / atx::vol::kCalendarYearNs;
    EXPECT_DOUBLE_EQ(route.schedule[i].tau, want);
    EXPECT_EQ(route.schedule[i].amount, evs[i + 1U].amount);
  }
}

// The one place the two windows can disagree, counted instead of hidden. Under a
// VOL-TIME clock `T` is weekend-compressed and a calendar tau is not, so an
// event the instant window admits can fall outside (0, T]. The escrowed forward
// still prices that cash and the lattice cannot, so the count is the caller's
// signal that the comparison is no longer like-for-like.
TEST(DiscreteDivRoute, AVolTimeYearFractionShorterThanCalendar_CountsWhatTheLatticeCannotSee) {
  const std::vector<atx::vol::DividendEvent> evs{{at_years(0.10), 2.0}, {at_years(0.70), 2.0}};
  const std::int64_t expiry = at_years(0.75);
  // A vol-time T that compresses 0.75 calendar years to 0.60: the second ex-date
  // is inside the instant window and outside (0, T].
  const auto route = atx::vol::discrete_div_route(evs, expiry, kNow, 0.60, 0.041,
                                                  atx::vol::DiscreteDivPolicy::Lattice);
  ASSERT_TRUE(route.applies());
  EXPECT_EQ(route.schedule.size(), 1U);
  EXPECT_EQ(route.n_outside_tau_window, 1U);
  EXPECT_NEAR(route.pv, 2.0 * std::exp(-0.041 * route.schedule[0].tau), 1.0e-12);
}

// A malformed amount is NOT swallowed here: it reaches the lattice's own
// validation and fails closed there. A zero is, because it is a no-op for both
// routes and a schedule full of zeros should not force a route.
TEST(DiscreteDivRoute, ZeroAmountsAreDroppedAndNegativeOnesFailClosedDownstream) {
  const std::int64_t expiry = at_years(0.75);
  const double T = static_cast<double>(expiry - kNow) / atx::vol::kCalendarYearNs;
  const std::vector<atx::vol::DividendEvent> zeros{{at_years(0.10), 0.0}, {at_years(0.40), 0.0}};
  EXPECT_FALSE(atx::vol::discrete_div_route(zeros, expiry, kNow, T, 0.041,
                                            atx::vol::DiscreteDivPolicy::Lattice)
                   .applies());

  const std::vector<atx::vol::DividendEvent> negative{{at_years(0.10), -1.0}};
  const auto route = atx::vol::discrete_div_route(negative, expiry, kNow, T, 0.041,
                                                  atx::vol::DiscreteDivPolicy::Lattice);
  ASSERT_TRUE(route.applies());
  const auto priced = american_discrete_div_price(100.0, 100.0, T, 0.25, 0.041, 0.0, Side::Put,
                                                  route.schedule, 301, ExerciseStyle::American);
  ASSERT_FALSE(priced.has_value());
  EXPECT_EQ(priced.error().code(), ErrorCode::InvalidArgument);
}

// The route decision is worth making, stated as a price rather than as an
// argument: the same chain, the same cash, the two routes, one number apart.
TEST(DiscreteDivRoute, TheTwoRoutesDisagreeByFarMoreThanEitherOnesNoise) {
  const std::vector<atx::vol::DividendEvent> evs{
      {at_years(0.05), 1.98}, {at_years(0.30), 2.05}, {at_years(0.55), 2.15}};
  const std::int64_t expiry = at_years(0.75);
  const double T = static_cast<double>(expiry - kNow) / atx::vol::kCalendarYearNs;
  constexpr double kS = 100.0, kK = 100.0, kSigma = 0.25, kR = 0.045;
  const auto route =
      atx::vol::discrete_div_route(evs, expiry, kNow, T, kR, atx::vol::DiscreteDivPolicy::Lattice);
  ASSERT_TRUE(route.applies());

  // The escrowed route as the library builds it today: one scalar carry that
  // reproduces the dividend-corrected forward exactly (deamer.cpp, curve_fit.cpp).
  const double F = atx::vol::forward_div_corrected(kS, kR, T, evs, expiry, kNow);
  const double q_eff = kR - std::log(F / kS) / T;
  for (const Side side : {Side::Call, Side::Put}) {
    const double escrowed = price_or_fail(kS, kK, T, kSigma, kR, q_eff, side, {}, 301,
                                          ExerciseStyle::American);
    const double lattice = price_or_fail(kS, kK, T, kSigma, kR, 0.0, side, route.schedule, 301,
                                         ExerciseStyle::American);
    // Both routes are the SAME lattice at the SAME step count here, so the gap
    // is the dividend treatment and nothing else -- no pricer difference, no
    // step-count difference, no convention difference. Measured 10.61 ticks on
    // the call and 54.59 on the put, at the money, on three ~2.00 dividends
    // inside 0.75 years of a 100 stock. The put carries the larger half because
    // escrowing removes the cash from spot on day one and hands its early-
    // exercise value back too early.
    EXPECT_GT(std::abs(lattice - escrowed) * 100.0, 10.0)
        << "route gap in ticks, side=" << (side == Side::Call ? "C" : "P") << ": "
        << (lattice - escrowed) * 100.0;
  }
}

} // namespace
