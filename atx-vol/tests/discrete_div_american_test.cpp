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

using atx::vol::american_discrete_div_greeks;
using atx::vol::american_discrete_div_price;
using atx::vol::black76_price;
using atx::vol::CashDividend;
using atx::vol::DiscreteDivGreeks;
using atx::vol::ErrorCode;
using atx::vol::ExerciseStyle;
using atx::vol::kDiscreteDivMaxSteps;
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

} // namespace
