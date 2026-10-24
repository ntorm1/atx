#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "pricing/american_discrete_div_reference.hpp"

// Fast suite of the independent cash-dividend reference (lane D0,
// src/pricing/american_discrete_div_reference.hpp): the engine MECHANICS, each
// against a value that does not come from a lattice or from the engine itself —
// the cold Andersen-Lake price (no cash), closed forms (cash at expiry, the
// escrowed European), and exact one-dividend decompositions by quadrature. The
// data-driven matrix and the kernel comparison are the slow suite
// (american_discrete_div_reference_matrix_test.cpp).

namespace {

using atx::vol::AlOpts;
using atx::vol::andersen_lake;
using atx::vol::black76_price;
using atx::vol::CashDividend;
using atx::vol::ErrorCode;
using atx::vol::ExerciseStyle;
using atx::vol::Side;
using atx::vol::ddiv_ref::Axis;
using atx::vol::ddiv_ref::CashProcess;
using atx::vol::ddiv_ref::Estimate;
using atx::vol::ddiv_ref::event_schedule;
using atx::vol::ddiv_ref::EventSchedule;
using atx::vol::ddiv_ref::interp_uniform_cubic;
using atx::vol::ddiv_ref::kMaxGridIntervals;
using atx::vol::ddiv_ref::pde_price;
using atx::vol::ddiv_ref::pde_reference;
using atx::vol::ddiv_ref::PdeGrid;
using atx::vol::ddiv_ref::PdeLadder;
using atx::vol::ddiv_ref::Problem;
using atx::vol::ddiv_ref::reference_price;
using atx::vol::ddiv_ref::ReferenceOpts;
using atx::vol::ddiv_ref::richardson_axis;
using atx::vol::ddiv_ref::segment_steps;
using atx::vol::ddiv_ref::time_grid;
using atx::vol::ddiv_ref::trinomial_price;
using atx::vol::ddiv_ref::trinomial_reference;
using atx::vol::ddiv_ref::TrinomialGrid;
using atx::vol::ddiv_ref::TrinomialLadder;
using atx::vol::ddiv_ref::validate;

constexpr double kInf = std::numeric_limits<double>::infinity();

// The fast-suite ladders: 4 levels, finest PDE grid 3,200 log-space intervals x 800
// nominal steps, finest lattice 2,000 steps. Every assertion below is against the
// uncertainty THESE ladders report, never a hand-widened tolerance.
[[nodiscard]] PdeLadder fast_pde() {
  PdeLadder ladder;
  ladder.base.space_base = 400;
  ladder.base.time_base = 100;
  ladder.levels = 4;
  return ladder;
}

[[nodiscard]] TrinomialLadder fast_lattice() {
  TrinomialLadder ladder;
  ladder.base.steps_base = 250;
  ladder.levels = 4;
  return ladder;
}

[[nodiscard]] Problem make(double S, double K, double T, double sigma, double r, double q,
                           Side side, ExerciseStyle exercise,
                           std::span<const CashDividend> divs,
                           CashProcess process = CashProcess::SpotLiquidator) {
  Problem p;
  p.S = S;
  p.K = K;
  p.T = T;
  p.sigma = sigma;
  p.r = r;
  p.q = q;
  p.side = side;
  p.exercise = exercise;
  p.process = process;
  p.dividends = divs;
  return p;
}

[[nodiscard]] Estimate pde_or_fail(const Problem &p, const PdeLadder &ladder = fast_pde()) {
  const auto out = pde_reference(p, ladder);
  EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().to_string());
  return out.has_value() ? *out : Estimate{};
}

[[nodiscard]] double bs_price(double S, double K, double T, double sigma, double r, double q,
                              Side side) {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T), side);
}

// ── Exact one-dividend references by quadrature (not a lattice, not a PDE) ──
//
// Composite 8-point Gauss-Legendre in the standard-normal variable z of S(t1),
// split exactly at every kink of the integrand, so each piece is smooth and the
// rule converges far below any tolerance used here.
constexpr std::array<double, 8> kGlX = {-0.9602898564975363, -0.7966664774136267,
                                        -0.5255324099163290, -0.1834346424956498,
                                        0.1834346424956498,  0.5255324099163290,
                                        0.7966664774136267,  0.9602898564975363};
constexpr std::array<double, 8> kGlW = {0.1012285362903763, 0.2223810344533745,
                                        0.3137066458778873, 0.3626837833783620,
                                        0.3626837833783620, 0.3137066458778873,
                                        0.2223810344533745, 0.1012285362903763};

template <class F> [[nodiscard]] double gl_integrate(F &&f, double a, double b, int panels) {
  double sum = 0.0;
  const double w = (b - a) / static_cast<double>(panels);
  for (int k = 0; k < panels; ++k) {
    const double lo = a + w * static_cast<double>(k);
    const double mid = lo + 0.5 * w;
    for (std::size_t i = 0; i < kGlX.size(); ++i) {
      sum += kGlW[i] * 0.5 * w * f(mid + 0.5 * w * kGlX[i]);
    }
  }
  return sum;
}

// E[f(S_t1)] with S_t1 lognormal from S at drift r - q, split at the given stock
// levels (each a kink of f).
template <class F>
[[nodiscard]] double lognormal_expectation(double S, double t1, double sigma, double r, double q,
                                           std::vector<double> kinks, F &&f) {
  constexpr double kZ = 12.0;
  const double drift = (r - q - 0.5 * sigma * sigma) * t1;
  const double diffusion = sigma * std::sqrt(t1);
  const double inv_sqrt_2pi = 1.0 / std::sqrt(2.0 * std::acos(-1.0));
  std::vector<double> cuts{-kZ, kZ};
  for (const double level : kinks) {
    if (level > 0.0) {
      const double z = (std::log(level / S) - drift) / diffusion;
      if (z > -kZ && z < kZ) {
        cuts.push_back(z);
      }
    }
  }
  std::sort(cuts.begin(), cuts.end());
  const auto integrand = [&](double z) {
    return inv_sqrt_2pi * std::exp(-0.5 * z * z) * f(S * std::exp(drift + diffusion * z));
  };
  double total = 0.0;
  for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
    total += gl_integrate(integrand, cuts[i], cuts[i + 1], 400);
  }
  return total;
}

// European, one dividend D at t1, liquidator: exp(-r t1) E[ BS(max(S_t1 - D, 0)) ].
[[nodiscard]] double exact_european_one_dividend(double S, double K, double T, double sigma,
                                                 double r, double q, double D, double t1,
                                                 Side side) {
  const double tail = T - t1;
  const auto value_after = [&](double s_cum) {
    const double post = std::max(s_cum - D, 0.0);
    if (!(post > 0.0)) {
      return (side == Side::Call) ? 0.0 : K * std::exp(-r * tail);
    }
    return bs_price(post, K, tail, sigma, r, q, side);
  };
  return std::exp(-r * t1) * lognormal_expectation(S, t1, sigma, r, q, {D}, value_after);
}

// American CALL, one dividend at t1, q = 0, r > 0: exercise can bind only the
// instant before t1, so V = exp(-r t1) E[ max(S_t1 - K, C_BS(S_t1 - D)) ] with
// the max's kink located exactly by bisection.
[[nodiscard]] double exact_american_call_one_dividend(double S, double K, double T, double sigma,
                                                      double r, double D, double t1) {
  const double tail = T - t1;
  const auto hold = [&](double s_cum) {
    const double post = std::max(s_cum - D, 0.0);
    return (post > 0.0) ? bs_price(post, K, tail, sigma, r, 0.0, Side::Call) : 0.0;
  };
  // s* solves s - K = hold(s): below it holding wins, above it exercise does.
  double lo = K;
  double hi = 100.0 * (K + D);
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    ((mid - K) > hold(mid) ? hi : lo) = mid;
  }
  const double s_star = 0.5 * (lo + hi);
  const auto value = [&](double s_cum) { return std::max(s_cum - K, hold(s_cum)); };
  return std::exp(-r * t1) * lognormal_expectation(S, t1, sigma, r, 0.0, {D, s_star}, value);
}

// ── The event schedule and the time grid ──────────────────────────────────

TEST(DiscreteDivReferenceSchedule, WindowMergeAndEveryExDateBitExactOnTheGrid) {
  constexpr double kT = 1.0;
  const std::vector<CashDividend> divs{
      {0.25, 1.0},  {0.25, 0.5},           {-0.1, 9.0},    {0.0, 9.0}, {0.6, 2.0},
      {kT, 0.75},   {kT * (1.0 + 5e-13), 0.25}, {kT + 0.1, 9.0}, {0.4, 0.0}};
  const EventSchedule ev = event_schedule(kT, divs);
  ASSERT_EQ(ev.interior.size(), 2U);
  EXPECT_EQ(ev.interior[0].t, 0.25);
  EXPECT_DOUBLE_EQ(ev.interior[0].amount, 1.5);
  EXPECT_EQ(ev.interior[1].t, 0.6);
  EXPECT_DOUBLE_EQ(ev.interior[1].amount, 2.0);
  EXPECT_DOUBLE_EQ(ev.terminal_amount, 1.0);

  for (int level = 0; level < 3; ++level) {
    const std::vector<double> grid = time_grid(ev, kT, 37, level);
    ASSERT_GE(grid.size(), 3U);
    EXPECT_EQ(grid.front(), 0.0);
    EXPECT_EQ(grid.back(), kT);
    EXPECT_NE(std::find(grid.begin(), grid.end(), 0.25), grid.end()) << "level " << level;
    EXPECT_NE(std::find(grid.begin(), grid.end(), 0.6), grid.end()) << "level " << level;
    EXPECT_TRUE(std::is_sorted(grid.begin(), grid.end()));
    EXPECT_EQ(std::adjacent_find(grid.begin(), grid.end()), grid.end()) << "duplicate level";
  }
}

TEST(DiscreteDivReferenceSchedule, LevelsAreExactlyNestedEvenForTinySegments) {
  constexpr double kT = 2.0;
  // A segment far shorter than one nominal step (the first-step ex-date case).
  const std::vector<CashDividend> divs{{1.0e-4, 1.0}, {0.7, 1.0}, {1.9999, 1.0}};
  const EventSchedule ev = event_schedule(kT, divs);
  const std::vector<int> base = segment_steps(ev, kT, 50, 0);
  ASSERT_EQ(base.size(), 4U);
  for (const int n : base) {
    EXPECT_GE(n, 1);
  }
  for (int level = 1; level < 4; ++level) {
    const std::vector<int> fine = segment_steps(ev, kT, 50, level);
    ASSERT_EQ(fine.size(), base.size());
    for (std::size_t j = 0; j < base.size(); ++j) {
      EXPECT_EQ(fine[j], base[j] << level) << "segment " << j << " level " << level;
    }
  }
}

TEST(DiscreteDivReferenceValidate, RefusesMalformedInputsWithTypedErrors) {
  const std::vector<CashDividend> none;
  const std::vector<CashDividend> negative{{0.5, -1.0}};
  const std::vector<CashDividend> nan_tau{{std::nan(""), 1.0}};
  const std::vector<CashDividend> ruinous{{0.5, 150.0}};
  const auto code = [](const Problem &p) {
    const auto s = validate(p);
    return s.has_value() ? ErrorCode::Unknown : s.error().code();
  };
  const Problem ok = make(100, 100, 1, 0.2, 0.03, 0, Side::Put, ExerciseStyle::American, none);
  EXPECT_TRUE(validate(ok).has_value());
  Problem bad = ok;
  bad.S = 0.0;
  EXPECT_EQ(code(bad), ErrorCode::InvalidArgument);
  bad = ok;
  bad.K = kInf;
  EXPECT_EQ(code(bad), ErrorCode::InvalidArgument);
  bad = ok;
  bad.T = -1.0;
  EXPECT_EQ(code(bad), ErrorCode::InvalidArgument);
  bad = ok;
  bad.sigma = 0.0;
  EXPECT_EQ(code(bad), ErrorCode::InvalidArgument);
  bad = ok;
  bad.r = std::nan("");
  EXPECT_EQ(code(bad), ErrorCode::InvalidArgument);
  bad = ok;
  bad.dividends = negative;
  EXPECT_EQ(code(bad), ErrorCode::InvalidArgument);
  bad = ok;
  bad.dividends = nan_tau;
  EXPECT_EQ(code(bad), ErrorCode::InvalidArgument);
  // The spot conventions price ruinous cash (that is what liquidator/survivor
  // are for); the escrowed state refuses it exactly as the Escrowed* lattices do.
  bad = ok;
  bad.dividends = ruinous;
  EXPECT_TRUE(validate(bad).has_value());
  bad.process = CashProcess::Escrowed;
  EXPECT_EQ(code(bad), ErrorCode::OutOfRange);
  EXPECT_FALSE(pde_price(bad, PdeGrid{}).has_value());
  PdeGrid grid;
  grid.space_base = 4;
  EXPECT_FALSE(pde_price(ok, grid).has_value());
}

TEST(DiscreteDivReferenceValidate, RefusesDriftDominatedCenteredStencils) {
  const std::vector<CashDividend> none;
  for (const auto rates : {std::array{.10, .0}, std::array{.0, .10}}) {
    const Problem p = make(100, 200, 1, .005, rates[0], rates[1], Side::Put,
                           ExerciseStyle::American, none);
    ASSERT_TRUE(validate(p)); // valid model; this particular grid cannot price it
    const auto result = pde_price(p, PdeGrid{});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(result.error().to_string().find("centered stencil"), std::string::npos);
  }
  // A small volatility alone is not a refusal: the centered drift can be admissible.
  const Problem admitted = make(100, 100, 1, .005, 0, 0, Side::Put,
                                ExerciseStyle::American, none);
  const auto price = pde_price(admitted, PdeGrid{});
  ASSERT_TRUE(price);
  EXPECT_TRUE(std::isfinite(price->price));
}

TEST(DiscreteDivReferenceValidate, RefusesNonfiniteCenteredOperatorFromFiniteInputs) {
  const std::vector<CashDividend> none;
  const double large = std::numeric_limits<double>::max();
  const Problem p = make(100, 100, 1, .2, large, -large, Side::Put,
                         ExerciseStyle::American, none);
  ASSERT_TRUE(validate(p));
  const auto result = pde_price(p, PdeGrid{});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
  EXPECT_NE(result.error().to_string().find("centered stencil"), std::string::npos);
}

TEST(DiscreteDivReferenceValidate, RefusesUnsupportedNegativeRateImplicitRows) {
  const std::vector<CashDividend> none;
  // Near-zero log drift gives nonnegative centered off-diagonals. A sufficiently
  // negative discount rate still destroys diagonal dominance of later theta rows.
  const Problem p = make(100, 100, 1, .2, -300, -300.02, Side::Put,
                         ExerciseStyle::American, none);
  ASSERT_TRUE(validate(p));
  const auto refused = pde_price(p, PdeGrid{});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
  EXPECT_NE(refused.error().to_string().find("implicit rows"), std::string::npos);
  const Problem admitted = make(100, 100, 1, .2, -.02, -.04, Side::Put,
                                ExerciseStyle::American, none);
  const auto price = pde_price(admitted, PdeGrid{});
  ASSERT_TRUE(price);
  EXPECT_TRUE(std::isfinite(price->price));
  EXPECT_GE(price->price, 0);
}

// Review m3: every grid count reaches an int through a cast or a shift. Inputs that
// would overflow one are refused with a typed error BEFORE the cast — previously
// a sigma tiny against the cash cast ~7e9 to int (UB), and an underflowing sigma
// cast lround(NaN).
TEST(DiscreteDivReferenceValidate, GridsBeyondIntRangeAreRefusedBeforeAnyCast) {
  const std::vector<CashDividend> none;
  const std::vector<CashDividend> fifth{{0.5, 20.0}};
  const auto code_of = [](const auto &result) {
    return result.has_value() ? ErrorCode::Unknown : result.error().code();
  };
  const Problem ok = make(100, 100, 1, 0.2, 0.03, 0, Side::Put, ExerciseStyle::American, none);
  PdeGrid pde;
  pde.space_base = kMaxGridIntervals + 1;
  EXPECT_EQ(code_of(pde_price(ok, pde)), ErrorCode::InvalidArgument);
  pde = PdeGrid{};
  pde.space_base = (kMaxGridIntervals >> 3) + 1;
  pde.space_level = 3;
  EXPECT_EQ(code_of(pde_price(ok, pde)), ErrorCode::InvalidArgument);
  pde = PdeGrid{};
  pde.time_base = std::numeric_limits<int>::max();
  EXPECT_EQ(code_of(pde_price(ok, pde)), ErrorCode::InvalidArgument);
  TrinomialGrid lattice;
  lattice.steps_base = (kMaxGridIntervals >> 2) + 1;
  lattice.level = 2;
  EXPECT_EQ(code_of(trinomial_price(ok, lattice)), ErrorCode::InvalidArgument);
  // sigma 1e-9 with r = q = 0 keeps every branch probability in [0, 1], so the
  // padding band (ln(100 / 80) / dx ~ 7e9 nodes) is what must refuse it.
  const Problem thin =
      make(100, 100, 1, 1.0e-9, 0.0, 0.0, Side::Put, ExerciseStyle::American, fifth);
  EXPECT_EQ(code_of(trinomial_price(thin, TrinomialGrid{})), ErrorCode::OutOfRange);
  // A sigma whose 8 sigma sqrt(T) underflows the spacing to 0.
  const Problem flat = make(100, 100, 1, std::numeric_limits<double>::denorm_min(), 0.03, 0.0,
                            Side::Put, ExerciseStyle::American, none);
  EXPECT_EQ(code_of(pde_price(flat, PdeGrid{})), ErrorCode::OutOfRange);
  // A direct segment_steps call never overflows: counts saturate at the cap.
  const EventSchedule events = event_schedule(1.0, none);
  const std::vector<int> steps = segment_steps(events, 1.0, std::numeric_limits<int>::max(), 7);
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_EQ(steps[0], kMaxGridIntervals);
}

// ── The uncertainty rule ──────────────────────────────────────────────────

TEST(DiscreteDivReferenceAxis, RecoversAKnownOrderAndBoundsTheResidual) {
  // v_l = 1 + 0.3 h^2 + 0.05 h^3 with h = 2^-l: second order, a higher-order tail.
  std::array<double, 4> v{};
  for (int l = 0; l < 4; ++l) {
    const double h = std::ldexp(1.0, -l);
    v[static_cast<std::size_t>(l)] = 1.0 + 0.3 * h * h + 0.05 * h * h * h;
  }
  const Axis axis = richardson_axis(v, 2.0);
  EXPECT_EQ(axis.n, 4);
  EXPECT_TRUE(axis.asymptotic);
  EXPECT_NEAR(axis.observed_order, 2.0, 0.15);
  const double extrapolated = v[3] + axis.correction;
  EXPECT_LE(std::abs(extrapolated - 1.0), axis.uncertainty) << "the bound must hold";
  EXPECT_LT(std::abs(extrapolated - 1.0), std::abs(v[3] - 1.0) / 10.0);
}

TEST(DiscreteDivReferenceAxis, ObservedOrderIsNotAGenericErrorBound) {
  const auto sequence = [](double h) {
    const double h2 = h * h;
    return h2 + 100.0 * (h2 - 1.0) * (h2 - .25) * (h2 - .0625);
  };
  const std::array<double, 3> values{sequence(1), sequence(.5), sequence(.25)};
  const Axis axis = richardson_axis(values, 2);
  ASSERT_TRUE(axis.asymptotic); // the implemented empirical observed-order test
  EXPECT_DOUBLE_EQ(axis.observed_order, 2);
  const double extrapolated = values.back() + axis.correction;
  EXPECT_DOUBLE_EQ(extrapolated, 0);
  EXPECT_DOUBLE_EQ(axis.uncertainty, .0625);
  EXPECT_DOUBLE_EQ(sequence(0), -1.5625);
  EXPECT_GT(std::abs(extrapolated - sequence(0)), axis.uncertainty);
}

TEST(DiscreteDivReferenceAxis, OscillationIsNotAsymptoticAndGetsTheEnvelope) {
  const std::array<double, 4> v{1.0, 1.01, 0.995, 1.004};
  const Axis axis = richardson_axis(v, 2.0);
  EXPECT_FALSE(axis.asymptotic);
  EXPECT_EQ(axis.correction, 0.0);
  EXPECT_DOUBLE_EQ(axis.uncertainty, std::max(std::abs(0.995 - 1.01), std::abs(1.004 - 0.995)));
  const std::array<double, 2> two{1.0, 1.001};
  EXPECT_FALSE(richardson_axis(two, 2.0).asymptotic);
  EXPECT_NEAR(richardson_axis(two, 2.0).uncertainty, 0.001, 1e-15);
}

// Review m1: an exact repeat on the last level is not convergence. It used to
// report |d1| eps (~0) as the uncertainty; the previous difference bounds it.
TEST(DiscreteDivReferenceAxis, AnExactRepeatOnTheLastLevelKeepsThePreviousDifference) {
  const std::array<double, 4> repeat{1.0, 1.1, 1.15, 1.15};
  const Axis axis = richardson_axis(repeat, 2.0);
  EXPECT_FALSE(axis.asymptotic);
  EXPECT_EQ(axis.correction, 0.0);
  EXPECT_DOUBLE_EQ(axis.uncertainty, std::abs(1.15 - 1.1));
  const std::array<double, 4> flat{2.5, 2.5, 2.5, 2.5};
  const Axis still = richardson_axis(flat, 2.0);
  EXPECT_TRUE(still.asymptotic);
  EXPECT_EQ(still.uncertainty, 0.0);
}

TEST(DiscreteDivReferenceInterp, CubicLagrangeIsExactOnCubicsIncludingTheEnds) {
  constexpr double kX0 = -1.3, kH = 0.125;
  std::vector<double> v(12);
  const auto f = [](double x) { return 2.0 - 0.5 * x + 0.75 * x * x - 0.2 * x * x * x; };
  for (std::size_t i = 0; i < v.size(); ++i) {
    v[i] = f(kX0 + kH * static_cast<double>(i));
  }
  for (const double x : {-1.3, -1.29, -1.17, -0.5, 0.0, 0.07, 0.07 + 1e-13, 0.0625}) {
    EXPECT_NEAR(interp_uniform_cubic(v, kX0, kH, x), f(x), 1e-13) << x;
  }
  const double top = kX0 + kH * 11.0;
  EXPECT_NEAR(interp_uniform_cubic(v, kX0, kH, top), f(top), 1e-13);
  EXPECT_NEAR(interp_uniform_cubic(v, kX0, kH, top - 0.01), f(top - 0.01), 1e-13);
  // Below its four-node precondition (or with h <= 0) it answers NaN, never a read
  // outside the span.
  const std::vector<double> three{1.0, 2.0, 3.0};
  EXPECT_TRUE(std::isnan(interp_uniform_cubic(three, 0.0, 1.0, 1.0)));
  EXPECT_TRUE(std::isnan(interp_uniform_cubic(v, kX0, 0.0, 0.0)));
}

// ── A. The PDE against values that are not lattices ───────────────────────

// The no-dividend sanity case of the lane brief: the reference must agree with
// the existing continuous-carry American reference (cold Andersen-Lake at the
// high-precision scheme of AndersenLake.CanonicalAtmZeroCarry_PremiumMatchesReference,
// whose tested accuracy is 1e-7 against QuantLib FP-B) within the PDE's OWN
// stated uncertainty plus that 1e-7.
TEST(DiscreteDivReferencePde, NoDividendAmericanAgreesWithAndersenLakeWithinTheStatedError) {
  struct Case {
    double S, K, T, sigma, r, q;
    Side side;
  };
  const std::array<Case, 6> cases{{{100, 100, 1.0, 0.25, 0.04, 0.0, Side::Put},
                                   {100, 110, 0.5, 0.30, 0.06, 0.02, Side::Put},
                                   {100, 95, 1.0, 0.20, 0.03, 0.05, Side::Call},
                                   {100, 90, 0.25, 0.35, 0.05, 0.0, Side::Put},
                                   {100, 100, 1.0, 0.25, 0.0, 0.0, Side::Put},
                                   {100, 120, 3.0, 0.30, 0.04, 0.03, Side::Put}}};
  constexpr double kAlStatedError = 1.0e-7;
  const AlOpts high{.n_collocation = 32, .n_quadrature = 64, .max_newton_iter = 16, .tol = 1e-13};
  const std::vector<CashDividend> none;
  for (const Case &c : cases) {
    const Problem p =
        make(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, ExerciseStyle::American, none);
    const Estimate est = pde_or_fail(p);
    const auto al = andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, high);
    ASSERT_TRUE(al.has_value());
    const auto al_accurate = andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side);
    ASSERT_TRUE(al_accurate.has_value());
    const double gap = std::abs(est.value - *al);
    std::printf("[ddiv-ref] no-div %s K %.0f T %.2f: pde %.10f +- %.2e  AL %.10f  |gap| %.2e"
                "  (AL accurate preset %.10f)\n"
                "[ddiv-ref]   space %.10f %.10f %.10f %.10f p_x %.2f u_x %.2e\n"
                "[ddiv-ref]   time  %.10f %.10f %.10f %.10f p_t %.2f u_t %.2e\n",
                c.side == Side::Put ? "put " : "call", c.K, c.T, est.value, est.uncertainty, *al,
                gap, *al_accurate, est.space.values[0], est.space.values[1], est.space.values[2],
                est.space.values[3], est.space.observed_order, est.space.uncertainty,
                est.time.values[0], est.time.values[1], est.time.values[2], est.time.values[3],
                est.time.observed_order, est.time.uncertainty);
    EXPECT_LE(gap, est.uncertainty + kAlStatedError) << "K " << c.K << " T " << c.T;
    // Resolution: the fast ladder's own uncertainty is below 0.01 bp of IV
    // (1e-6 x the Black-Scholes vega of the contract).
    const double sqrt_t = std::sqrt(c.T);
    const double d1 = (std::log(c.S / c.K) + (c.r - c.q + 0.5 * c.sigma * c.sigma) * c.T) /
                      (c.sigma * sqrt_t);
    const double vega = c.S * std::exp(-c.q * c.T) * std::exp(-0.5 * d1 * d1) /
                        std::sqrt(2.0 * std::acos(-1.0)) * sqrt_t;
    EXPECT_LT(est.uncertainty, 1.0e-6 * vega) << "the fast ladder must resolve a plain American";
  }
}

TEST(DiscreteDivReferencePde, EuropeanOneDividendMatchesTheExactQuadrature) {
  struct Case {
    double K, tau, D, q;
    Side side;
  };
  // Mid-life cash, an ex-date 0.3 % into the year (the first lattice step of a
  // 301-step tree: LEDGER L522), and cash of 20 % of spot.
  const std::array<Case, 6> cases{{{100, 0.4, 3.0, 0.0, Side::Put},
                                   {100, 0.4, 3.0, 0.0, Side::Call},
                                   {95, 0.003, 6.0, 0.01, Side::Put},
                                   {105, 0.003, 6.0, 0.01, Side::Call},
                                   {100, 0.5, 20.0, 0.0, Side::Put},
                                   {100, 0.5, 20.0, 0.0, Side::Call}}};
  for (const Case &c : cases) {
    const std::vector<CashDividend> one{{c.tau, c.D}};
    const Problem p =
        make(100, c.K, 1.0, 0.3, 0.03, c.q, c.side, ExerciseStyle::European, one);
    const Estimate est = pde_or_fail(p);
    const double exact =
        exact_european_one_dividend(100, c.K, 1.0, 0.3, 0.03, c.q, c.D, c.tau, c.side);
    std::printf("[ddiv-ref] euro-1div %s tau %.3f D %.1f: pde %.10f +- %.2e exact %.10f |gap| "
                "%.2e\n",
                c.side == Side::Put ? "put " : "call", c.tau, c.D, est.value, est.uncertainty,
                exact, std::abs(est.value - exact));
    EXPECT_LE(std::abs(est.value - exact), est.uncertainty) << "tau " << c.tau << " D " << c.D;
  }
}

TEST(DiscreteDivReferencePde, AmericanCallOneDividendMatchesTheExactDecomposition) {
  // The anchor of DiscreteDivAmerican.AmericanCallWithOneDividend_MatchesTheExactDecomposition
  // (11.66391384621064 by Simpson there, 1e-6), recomputed here with the kink split.
  constexpr double kS = 100.0, kK = 90.0, kT = 0.5, kSigma = 0.20, kR = 0.04;
  const std::vector<CashDividend> one{{0.25, 5.0}};
  const double exact = exact_american_call_one_dividend(kS, kK, kT, kSigma, kR, 5.0, 0.25);
  EXPECT_NEAR(exact, 11.66391384621064, 1.0e-6);
  const Problem p = make(kS, kK, kT, kSigma, kR, 0.0, Side::Call, ExerciseStyle::American, one);
  const Estimate est = pde_or_fail(p);
  std::printf("[ddiv-ref] amer-call-1div: pde %.10f +- %.2e exact %.10f |gap| %.2e\n",
              est.value, est.uncertainty, exact, std::abs(est.value - exact));
  EXPECT_LE(std::abs(est.value - exact), est.uncertainty);
  // A deep-in-the-money twin where cum-dividend exercise binds on most paths.
  const std::vector<CashDividend> late{{0.45, 8.0}};
  const double exact_itm = exact_american_call_one_dividend(kS, 70.0, kT, 0.3, kR, 8.0, 0.45);
  const Problem itm = make(kS, 70.0, kT, 0.3, kR, 0.0, Side::Call, ExerciseStyle::American, late);
  const Estimate est_itm = pde_or_fail(itm);
  std::printf("[ddiv-ref] amer-call-1div-itm: pde %.10f +- %.2e exact %.10f |gap| %.2e\n",
              est_itm.value, est_itm.uncertainty, exact_itm, std::abs(est_itm.value - exact_itm));
  EXPECT_LE(std::abs(est_itm.value - exact_itm), est_itm.uncertainty);
}

TEST(DiscreteDivReferencePde, EscrowedEuropeanIsBlack76AtTheDatedCashForward) {
  constexpr double kS = 100.0, kT = 1.5, kSigma = 0.28, kR = 0.045, kQ = 0.01;
  const std::vector<CashDividend> four{{0.2, 1.1}, {0.45, 1.1}, {0.7, 1.2}, {kT, 1.2}};
  double escrow = 0.0;
  for (const CashDividend &d : four) {
    escrow += d.amount * std::exp(-(kR - kQ) * d.tau);
  }
  const double forward = (kS - escrow) * std::exp((kR - kQ) * kT);
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double K : {85.0, 100.0, 118.0}) {
      const Problem p = make(kS, K, kT, kSigma, kR, kQ, side, ExerciseStyle::European, four,
                             CashProcess::Escrowed);
      const Estimate est = pde_or_fail(p);
      const double exact = black76_price(forward, K, kT, kSigma, std::exp(-kR * kT), side);
      EXPECT_LE(std::abs(est.value - exact), est.uncertainty) << "K " << K;
    }
  }
}

TEST(DiscreteDivReferencePde, CashAtExpiryIsTheAnalyticPayoffShift) {
  constexpr double kS = 100.0, kK = 100.0, kT = 0.75, kSigma = 0.25, kR = 0.03, kD = 4.0;
  const std::vector<CashDividend> at_expiry{{kT, kD}};
  const std::vector<CashDividend> none;
  const auto value = [&](Side side, ExerciseStyle ex, std::span<const CashDividend> d) {
    return pde_or_fail(make(kS, kK, kT, kSigma, kR, 0.0, side, ex, d));
  };
  // European call: (max(S - D, 0) - K)+ == (S - (K + D))+.
  const Estimate call = value(Side::Call, ExerciseStyle::European, at_expiry);
  const double call_exact = bs_price(kS, kK + kD, kT, kSigma, kR, 0.0, Side::Call);
  EXPECT_LE(std::abs(call.value - call_exact), call.uncertainty);
  // European put: put(K + D) - put(D).
  const Estimate put = value(Side::Put, ExerciseStyle::European, at_expiry);
  const double put_exact = bs_price(kS, kK + kD, kT, kSigma, kR, 0.0, Side::Put) -
                           bs_price(kS, kD, kT, kSigma, kR, 0.0, Side::Put);
  EXPECT_LE(std::abs(put.value - put_exact), put.uncertainty);
  // An American call may exercise the instant before the cash: the cash cannot
  // move it, so it is the no-dividend American call (== European, q = 0).
  const Estimate amer = value(Side::Call, ExerciseStyle::American, at_expiry);
  const double amer_exact = bs_price(kS, kK, kT, kSigma, kR, 0.0, Side::Call);
  EXPECT_LE(std::abs(amer.value - amer_exact), amer.uncertainty);
}

TEST(DiscreteDivReferencePde, CashAtTimeZeroLimitIsTheShiftedSpot) {
  constexpr double kS = 100.0, kK = 100.0, kT = 1.0, kSigma = 0.25, kR = 0.03, kD = 5.0;
  constexpr double kTau = 1.0e-6;
  const std::vector<CashDividend> now{{kTau, kD}};
  for (const Side side : {Side::Call, Side::Put}) {
    const Estimate est =
        pde_or_fail(make(kS, kK, kT, kSigma, kR, 0.0, side, ExerciseStyle::European, now));
    const double shifted = bs_price(kS - kD, kK, kT - kTau, kSigma, kR, 0.0, side) *
                           std::exp(-kR * kTau);
    // The only gap besides the uncertainty is the O(tau) diffusion before the cash.
    EXPECT_LE(std::abs(est.value - shifted), est.uncertainty + 1.0e-4);
  }
}

TEST(DiscreteDivReferencePde, PutCallParityAndTheAmericanBounds) {
  constexpr double kS = 100.0, kK = 100.0, kT = 1.0, kSigma = 0.25, kR = 0.04, kQ = 0.0;
  const std::vector<CashDividend> two{{0.3, 2.0}, {0.8, 2.0}};
  double pv = 0.0;
  for (const CashDividend &d : two) {
    pv += d.amount * std::exp(-kR * d.tau) * std::exp(-kQ * (kT - d.tau));
  }
  const auto value = [&](Side side, ExerciseStyle ex) {
    return pde_or_fail(make(kS, kK, kT, kSigma, kR, kQ, side, ex, two));
  };
  const Estimate ce = value(Side::Call, ExerciseStyle::European);
  const Estimate pe = value(Side::Put, ExerciseStyle::European);
  const double parity = kS * std::exp(-kQ * kT) - pv - kK * std::exp(-kR * kT);
  EXPECT_LE(std::abs((ce.value - pe.value) - parity), ce.uncertainty + pe.uncertainty);
  const Estimate ca = value(Side::Call, ExerciseStyle::American);
  const Estimate pa = value(Side::Put, ExerciseStyle::American);
  const double slack = ca.uncertainty + pa.uncertainty;
  EXPECT_GE(ca.value - pa.value, kS - pv - kK - slack);
  EXPECT_LE(ca.value - pa.value, kS - kK * std::exp(-kR * kT) + slack);
  EXPECT_GE(ca.value, ce.value - slack);
  EXPECT_GE(pa.value, pe.value - slack);
}

TEST(DiscreteDivReferencePde, SurvivorAndLiquidatorDifferOnlyWhereCashCanExceedSpot) {
  const std::vector<CashDividend> big{{0.25, 6.0}};
  const auto value = [&](Side side, CashProcess process, double sigma) {
    return pde_or_fail(
        make(10.0, 8.0, 0.5, sigma, 0.03, 0.0, side, ExerciseStyle::American, big, process));
  };
  // At sigma 0.8 the stock falls below the 6.00 of cash before the ex-date on a
  // visible share of paths: survivors keep a call alive, liquidation kills it.
  const Estimate call_l = value(Side::Call, CashProcess::SpotLiquidator, 0.8);
  const Estimate call_s = value(Side::Call, CashProcess::SpotSurvivor, 0.8);
  const Estimate put_l = value(Side::Put, CashProcess::SpotLiquidator, 0.8);
  const Estimate put_s = value(Side::Put, CashProcess::SpotSurvivor, 0.8);
  std::printf("[ddiv-ref] survivor-vs-liquidator sigma 0.8: call %.8f vs %.8f  put %.8f vs %.8f\n",
              call_s.value, call_l.value, put_s.value, put_l.value);
  EXPECT_GT(call_s.value - call_l.value, call_s.uncertainty + call_l.uncertainty);
  EXPECT_GT(put_l.value - put_s.value, put_s.uncertainty + put_l.uncertainty);
  // At sigma 0.1 that share is ~1e-20: the treatments agree to the uncertainty.
  const Estimate quiet_l = value(Side::Put, CashProcess::SpotLiquidator, 0.1);
  const Estimate quiet_s = value(Side::Put, CashProcess::SpotSurvivor, 0.1);
  EXPECT_LE(std::abs(quiet_l.value - quiet_s.value), quiet_l.uncertainty + quiet_s.uncertainty);
}

TEST(DiscreteDivReferencePde, SpaceAndTimeAreRefinedIndependentlyAndShareOnlyTheCorner) {
  const std::vector<CashDividend> two{{0.3, 2.0}, {0.8, 2.0}};
  const Problem p = make(100, 100, 1.0, 0.25, 0.04, 0.0, Side::Put, ExerciseStyle::American, two);
  const Estimate est = pde_or_fail(p);
  ASSERT_EQ(est.space.n, 4);
  ASSERT_EQ(est.time.n, 4);
  EXPECT_EQ(est.space.values[3], est.finest);
  EXPECT_EQ(est.time.values[3], est.finest);
  EXPECT_NE(est.space.values[2], est.time.values[2]) << "the axes must vary different grids";
  EXPECT_TRUE(est.space.asymptotic);
  EXPECT_TRUE(est.time.asymptotic);
  EXPECT_GT(est.max_policy_iterations, 0);
  EXPECT_DOUBLE_EQ(est.value, est.finest + est.space.correction + est.time.correction);
  // The domain truncation is not a hidden error term: 8 vs 11 sigma sqrt(T).
  PdeLadder wide = fast_pde();
  wide.base.width_sd = 11.0;
  const Estimate est_wide = pde_or_fail(p, wide);
  EXPECT_LE(std::abs(est.value - est_wide.value), est.uncertainty + est_wide.uncertainty);
}

// Regression (matrix case B2p, first run): an ex-date 0.4 of a 301-step before
// expiry leaves a 0.0013-year last segment; on the matrix's finest grid the
// implicit rows' entries are ~1e2 and the Howard policy used to cycle on a
// near-tie for its whole 200-iteration cap.
TEST(DiscreteDivReferencePde, PolicyIterationSettlesOnATinyLastSegmentAtTheFinestGrid) {
  const std::vector<CashDividend> last{{1.0 - 0.4 / 301.0, 3.0}};
  const Problem p = make(100, 100, 1.0, 0.25, 0.04, 0.0, Side::Put, ExerciseStyle::American, last);
  PdeGrid grid;
  grid.space_base = 800;
  grid.space_level = 3;
  grid.time_base = 200;
  grid.time_level = 3;
  const auto out = pde_price(p, grid);
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  EXPECT_LT(out->max_policy_iterations, 20) << "at t = " << out->max_policy_time;
}

// Regression (matrix case G4p, first official run): r = q = 0 put, four quarterly
// dividends, on the matrix's finest grid. After the last ex-date the payoff
// satisfies the discrete PDE to O(dt h^2) deep in the money — below the rows'
// rounding on that grid — so the obstacle is an exact tie there and an argmin
// policy can flip on rounding noise for its whole cap.
TEST(DiscreteDivReferencePde, PolicyIterationSettlesOnTheZeroRateTieAtTheFinestGrid) {
  const std::vector<CashDividend> four{{0.1, 1.0}, {0.35, 1.0}, {0.6, 1.0}, {0.85, 1.0}};
  const Problem p = make(100, 100, 1.0, 0.25, 0.0, 0.0, Side::Put, ExerciseStyle::American, four);
  for (const int time_level : {0, 3}) {
    PdeGrid grid;
    grid.space_base = 1200;
    grid.space_level = 3;
    grid.time_base = 300;
    grid.time_level = time_level;
    const auto out = pde_price(p, grid);
    ASSERT_TRUE(out.has_value()) << out.error().to_string();
    EXPECT_LT(out->max_policy_iterations, 20) << "at t = " << out->max_policy_time;
    EXPECT_LE(out->max_lcp_violation, 1024.0 * std::numeric_limits<double>::epsilon());
    std::printf("[ tie ] time level %d: %d policy iterations max, %d rounding-tie steps, "
                "worst accepted violation %.3e (%.0f ulps)\n",
                time_level, out->max_policy_iterations, out->lcp_rounding_ties,
                out->max_lcp_violation,
                out->max_lcp_violation / std::numeric_limits<double>::epsilon());
  }
}

TEST(DiscreteDivReferencePde, GridGreeksAreFiniteAndSigned) {
  const std::vector<CashDividend> one{{0.5, 3.0}};
  const Problem p = make(100, 100, 1.0, 0.25, 0.04, 0.0, Side::Put, ExerciseStyle::American, one);
  PdeGrid grid;
  grid.space_level = 2;
  grid.time_level = 2;
  const auto out = pde_price(p, grid);
  ASSERT_TRUE(out.has_value());
  EXPECT_LT(out->delta, 0.0);
  EXPECT_GT(out->delta, -1.0);
  EXPECT_GT(out->gamma, 0.0);
  EXPECT_EQ(out->space_intervals, 1600);
}

// ── The kernel's declared convention (additive metadata, lane D0) ─────────
//
// Every kernel scheme maps to exactly one reference CashProcess; the result
// structs carry the map's answer for the scheme that produced them.
TEST(DiscreteDivKernelConvention, EverySchemeDeclaresItsProcessRightsAndVolatility) {
  using atx::vol::discrete_div_convention;
  using atx::vol::DiscreteDivCashExcess;
  using atx::vol::DiscreteDivConvention;
  using atx::vol::DiscreteDivExDateGrid;
  using atx::vol::DiscreteDivProcess;
  using atx::vol::DiscreteDivScheme;
  for (const DiscreteDivScheme s : {DiscreteDivScheme::SplicedCrr, DiscreteDivScheme::SplicedBbs,
                                    DiscreteDivScheme::SplicedBbsr}) {
    const DiscreteDivConvention c = discrete_div_convention(s);
    EXPECT_EQ(c.process, DiscreteDivProcess::SpotShift);
    EXPECT_EQ(c.cash_excess, DiscreteDivCashExcess::Liquidator);
    EXPECT_TRUE(c.spot_volatility);
    EXPECT_TRUE(c.cum_and_post_exercise);
    EXPECT_EQ(c.ex_dates, DiscreteDivExDateGrid::NearestStep);
  }
  for (const DiscreteDivScheme s :
       {DiscreteDivScheme::EscrowedBbs, DiscreteDivScheme::EscrowedLeisenReimer}) {
    const DiscreteDivConvention c = discrete_div_convention(s);
    EXPECT_EQ(c.process, DiscreteDivProcess::Escrowed);
    EXPECT_EQ(c.cash_excess, DiscreteDivCashExcess::Refused);
    EXPECT_FALSE(c.spot_volatility);
    EXPECT_TRUE(c.cum_and_post_exercise);
  }
  static_assert(discrete_div_convention(DiscreteDivScheme::SplicedCrr).process ==
                DiscreteDivProcess::SpotShift);
}

TEST(DiscreteDivKernelConvention, ResultStructsCarryTheirSchemesConvention) {
  using atx::vol::american_discrete_div_converged_price;
  using atx::vol::american_discrete_div_implied_vol;
  using atx::vol::discrete_div_convention;
  using atx::vol::DiscreteDivIvOpts;
  using atx::vol::DiscreteDivProcess;
  using atx::vol::DiscreteDivScheme;
  using atx::vol::DiscreteDivTier;
  using atx::vol::DiscreteDivWorkspace;
  const std::vector<CashDividend> one{{0.5, 2.0}};
  DiscreteDivWorkspace ws(atx::vol::kDiscreteDivConvergedSteps);
  const auto conv = american_discrete_div_converged_price(100, 100, 1.0, 0.25, 0.04, 0.0,
                                                          Side::Put, one,
                                                          ExerciseStyle::American, ws);
  ASSERT_TRUE(conv.has_value());
  EXPECT_EQ(conv->convention, discrete_div_convention(DiscreteDivScheme::SplicedBbs));
  DiscreteDivIvOpts opts;
  opts.tier = DiscreteDivTier::Accurate;
  for (const DiscreteDivScheme s :
       {DiscreteDivScheme::SplicedCrr, DiscreteDivScheme::EscrowedLeisenReimer}) {
    const auto iv = american_discrete_div_implied_vol(9.0, 100, 100, 1.0, 0.04, 0.0, Side::Put,
                                                      one, 301, s, opts, ws);
    ASSERT_TRUE(iv.has_value()) << iv.error().to_string();
    EXPECT_EQ(iv->convention, discrete_div_convention(s));
  }
  // The reference's conventions name the same models: the spot schemes are the
  // liquidator, the escrowed schemes the escrowed process.
  EXPECT_STREQ(atx::vol::ddiv_ref::to_string(CashProcess::SpotLiquidator), "spot-liquidator");
  EXPECT_EQ(discrete_div_convention(DiscreteDivScheme::SplicedCrr).process,
            DiscreteDivProcess::SpotShift);
}

// ── B. The trinomial lattice cross-checks the PDE ─────────────────────────

TEST(DiscreteDivReferenceTrinomial, AgreesWithThePdeWithinBothUncertainties) {
  const std::vector<CashDividend> sched{{0.3, 2.0}, {0.55, 2.0}, {0.8, 2.0}};
  for (const Side side : {Side::Put, Side::Call}) {
    for (const CashProcess process : {CashProcess::SpotLiquidator, CashProcess::Escrowed}) {
      const Problem p =
          make(100, 100, 1.0, 0.25, 0.04, 0.0, side, ExerciseStyle::American, sched, process);
      ReferenceOpts opts;
      opts.pde = fast_pde();
      opts.lattice = fast_lattice();
      const auto ref = reference_price(p, opts);
      ASSERT_TRUE(ref.has_value()) << ref.error().to_string();
      std::printf("[ddiv-ref] cross-check %s %s: pde %.10f +- %.2e  lattice %.10f +- %.2e "
                  "(p_N %.2f)\n",
                  side == Side::Put ? "put " : "call", atx::vol::ddiv_ref::to_string(process),
                  ref->pde.value, ref->pde.uncertainty, ref->lattice.value,
                  ref->lattice.uncertainty, ref->lattice.time.observed_order);
      EXPECT_TRUE(ref->methods_agree);
      EXPECT_EQ(ref->value, ref->pde.value);
      EXPECT_EQ(ref->uncertainty, ref->pde.uncertainty);
      EXPECT_EQ(ref->lattice.space.n, 0);
      EXPECT_EQ(ref->lattice.time.n, 4);
    }
  }
}

TEST(DiscreteDivReferenceTrinomial, NoDividendAgreesWithAndersenLake) {
  const std::vector<CashDividend> none;
  const Problem p =
      make(100, 105, 0.75, 0.3, 0.05, 0.01, Side::Put, ExerciseStyle::American, none);
  const auto lat = trinomial_reference(p, fast_lattice());
  ASSERT_TRUE(lat.has_value());
  const AlOpts high{.n_collocation = 32, .n_quadrature = 64, .max_newton_iter = 16, .tol = 1e-13};
  const auto al = andersen_lake(100, 105, 0.75, 0.3, 0.05, 0.01, Side::Put, high);
  ASSERT_TRUE(al.has_value());
  EXPECT_LE(std::abs(lat->value - *al), lat->uncertainty + 1.0e-7);
}

} // namespace
