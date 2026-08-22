#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"

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
  EXPECT_NEAR(worst, 5.222871e-05, 1.0e-9);
  EXPECT_LT(worst * 100.0, 0.0053) << "parity residual in ticks";
}

TEST(DiscreteDivAmerican, EuropeanWithDividends_ReproducesReferencePriceAndGreeks) {
  const std::vector<CashDividend> schedule = parity_schedule();
  const auto call = american_discrete_div_greeks(kParityS, 775.0, kParityT, kParitySigma, kParityR,
                                                 kParityQ, Side::Call, schedule, 301,
                                                 ExerciseStyle::European);
  ASSERT_TRUE(call.has_value()) << call.error().to_string();
  EXPECT_NEAR(call->price, 75.222227622486884, 1.0e-9);
  EXPECT_NEAR(call->delta, 0.59491123873539919, 1.0e-9);
  EXPECT_NEAR(call->gamma, 0.002374185751928035, 1.0e-11);

  const auto put = american_discrete_div_greeks(kParityS, 775.0, kParityT, kParitySigma, kParityR,
                                                kParityQ, Side::Put, schedule, 301,
                                                ExerciseStyle::European);
  ASSERT_TRUE(put.has_value()) << put.error().to_string();
  EXPECT_NEAR(put->price, 51.21952913918814, 1.0e-9);
  EXPECT_NEAR(put->delta, -0.39445568531088704, 1.0e-9);
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

  const double call = price_or_fail(100.0, 100.0, 1.0, 0.30, 0.03, 0.0, Side::Call, big, 301,
                                    ExerciseStyle::American);
  EXPECT_GE(call, 0.0);
  EXPECT_NEAR(call, 9.3443758393719278, 1.0e-9);
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
  EXPECT_NEAR(euro_put, 96.146830494183135, 1.0e-9);

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
  EXPECT_GT(amer_call, euro_call);
  EXPECT_NEAR(amer_call, 12.514058272121554, 1.0e-9);
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
// bundle, and O(1/steps) like every other lattice error, so `steps` is the only
// lever. The claim that survives is the SIGN plus an order of magnitude.
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
      // option no longer lives to receive.
      const double clamped =
          price_or_fail(100.0, 100.0, kBumped, 0.25, 0.04, 0.01, side, clipped, 301, style);
      EXPECT_NE(dropped, clamped);
    }
  }

  // How big the mistake is, side by side. The EUROPEAN legs show it at full
  // size (1.03 for the call, 1.27 for the put on a ~7 premium) because a
  // European holder simply eats the terminal drop. The AMERICAN CALL is the one
  // place it compresses -- to 0.024 -- and for a real reason rather than a
  // cancellation: facing a 2.35 ex-dividend at expiry the holder exercises
  // just before it, so most of the clipped cash never touches the value. That
  // is exactly why this test does not assert one magnitude for all four legs.
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
// 2.2 ABSOLUTE: 35% of the K=97 value, 90% of the K=103 value. It is O(1/steps)
// like every other lattice error, so `steps` is the only lever inside this
// scheme; making volga materially better needs a strike-aligned tree
// (Leisen-Reimer / Tian), which is a different PRICE and therefore a different
// measurement against the vendor, not a tuning change here.
//
// STEP-COUNT STABILITY, 301 vs 1201, on the six-dividend American parity
// scenario, relative: price 2.8e-3, delta 3.4e-4, gamma 1.2e-3, vega 7.2e-4,
// theta 3.5e-3, rho 2.1e-3, phi 1.6e-3, charm 7.5e-3, vanna 1.9e-2,
// theta_secant 4.6e-2, volga 1.9e-1. Same ordering, same reason.

} // namespace
