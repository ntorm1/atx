#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "atx/vol/api/analytics/deriv_pnl.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceSet / SurfaceRef
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "pricing/deriv_ref_bridge.hpp" // I-3: a PRICED smile rotation
#include "support/analytics_fixture.hpp"

// Task F-8 / GK-G5: two-date P&L attribution for a swap position.
//
// ── ON THE ORACLE, BECAUSE THIS TEST FILE IS EASY TO WRITE BADLY ───────────
//
// `residual` is DEFINED as `d_pv - sum(components)`, so "sum of components
// minus dPV equals residual" is a tautology. Asserting it proves the assembly
// has no sign error and nothing more; `IdentityIsExactByConstruction` says so
// in its own name and is the only test here that leans on it.
//
// Everything else is checked against a world whose answer was written out by
// hand BEFORE the library was called. `ClosedFormVarSwapStepIsFullyExplained`
// builds a variance swap whose PV is a four-term arithmetic expression --
// notional times a weighted mean of fixings, no discounting, no smile -- and
// derives every component's expected value on paper. The library never
// supplies a number that test then checks against itself: `theta_zero_fixing`,
// the marks and the fixing weight are all literals derived from the contract
// definition, so an error inside `deriv_pnl_explain` cannot move the
// expectation with it.
//
// The one test that does consume library output on both sides --
// `AgainstAPricedStepTheResidualIsSecondOrder` -- is deliberately the weakest
// claim of the file, and it bounds the residual as a FRACTION of the move
// rather than pinning it. Its two sides come from different entry points
// (`deriv_price` for the marks, `deriv_greeks` for the sensitivities), which
// is what makes a nonzero residual there informative rather than circular.

namespace {

using atx::vol::DerivContract;
using atx::vol::DerivGreeks;
using atx::vol::DerivKind;
using atx::vol::DerivPnlExplain;
using atx::vol::DerivPnlFlags;
using atx::vol::DerivPnlInputs;
using atx::vol::DerivPnlMark;
using atx::vol::deriv_pnl_explain;
using atx::vol::has_flag;
using atx::vol::var_swap_fixing_weight;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ── The hand-derived world ────────────────────────────────────────────────
//
// A 252-fixing variance swap, notional 1e6, struck at 0.04 variance, in a
// zero-rate world so every discount factor is exactly 1. Its PV is
//
//   PV = N * ( (n_done*RV_done + n_future*K_var_future) / n_total - K_strike )
//
// which is the contract, not a model. At inception-plus-100 fixings with
// everything realizing exactly at the implied rate, the swap is worth zero:
// 100*0.04 + 152*0.04 = 10.08, and 10.08/252 = 0.04 = K_strike.
constexpr double kN = 1.0e6;
constexpr double kNTotal = 252.0;
constexpr double kNDone0 = 100.0;
constexpr double kRvDone0 = 0.04;
constexpr double kKVar = 0.04;
constexpr double kKStrike = 0.04;
constexpr double kDt = 1.0 / 252.0;

[[nodiscard]] double pv_of(double n_done, double rv_done, double k_var) noexcept {
  const double n_future = kNTotal - n_done;
  return kN * ((n_done * rv_done + n_future * k_var) / kNTotal - kKStrike);
}

// One fixing at annualized rate `r2` lands: the running mean absorbs it and
// one day of future implied variance is retired.
[[nodiscard]] double pv_after_fixing(double r2) noexcept {
  const double rv_done1 = (kNDone0 * kRvDone0 + r2) / (kNDone0 + 1.0);
  return pv_of(kNDone0 + 1.0, rv_done1, kKVar);
}

// `theta_zero_fixing` for this world, derived rather than measured: a literal
// zero return replaces one day of implied variance, so the PV falls by
// N*K_var/n_total over dt, i.e. -N*K_var/(n_total*dt) per year. With
// dt = 1/252 and n_total = 252 that is exactly -N*K_var = -40000.
constexpr double kThetaZeroFixing = -kN * kKVar;

DerivGreeks flat_greeks() {
  DerivGreeks g{};
  g.theta_zero_fixing = kThetaZeroFixing;
  g.vega = 0.0;
  g.rho = 0.0;
  g.skew_vega = 0.0;
  g.convexity_vega = 0.0;
  return g;
}

DerivPnlMark mark_at(double pv) {
  DerivPnlMark m{};
  m.pv = pv;
  m.sigma_atm = 0.20;
  m.skew_slope = -0.30;
  m.smile_curvature = 0.50;
  m.zero_rate = 0.0;
  return m;
}

}  // namespace

// ── The tautology, named as one ────────────────────────────────────────────

// `residual` is the leftover by definition, so this asserts arithmetic and
// nothing else -- specifically that the six components are summed in the order
// the header publishes and subtracted the right way round. It is exact on the
// bits because a caller reproducing the residual from the published components
// must land on the same number.
TEST(DerivPnlExplain, IdentityIsExactByConstruction) {
  DerivGreeks g{};
  g.theta_zero_fixing = -4.0e4;
  g.vega = 7.5e5;
  g.rho = -1.3e5;
  g.skew_vega = -2.2e5;
  g.convexity_vega = 9.1e4;

  DerivPnlInputs in{};
  in.greeks = g;
  in.dt_years = kDt;
  in.realized_var_dec = 0.031;
  in.fixing_weight = kN / kNTotal;
  in.from = mark_at(1234.5);
  in.to = mark_at(-987.25);
  in.to.sigma_atm = 0.213;
  in.to.skew_slope = -0.34;
  in.to.smile_curvature = 0.44;
  in.to.zero_rate = 0.0007;

  const auto e = deriv_pnl_explain(in);
  ASSERT_TRUE(e.has_value()) << e.error().to_string();

  const double summed = e->carry + e->realized + e->vol_level + e->skew + e->convexity +
                        e->discount;
  EXPECT_EQ(std::bit_cast<std::uint64_t>(e->residual),
            std::bit_cast<std::uint64_t>(e->d_pv - summed));
  EXPECT_EQ(e->flags, DerivPnlFlags::None);

  // Every component is its own sensitivity times its own move -- checked
  // against hand-written products, so a transposed pairing (skew_vega against
  // the curvature move, say) fails here rather than hiding inside a residual
  // that still sums correctly.
  EXPECT_DOUBLE_EQ(e->carry, -4.0e4 * kDt);
  EXPECT_DOUBLE_EQ(e->realized, (kN / kNTotal) * 0.031);
  EXPECT_DOUBLE_EQ(e->vol_level, 7.5e5 * (0.213 - 0.20));
  EXPECT_DOUBLE_EQ(e->skew, -2.2e5 * (-0.34 - -0.30));
  EXPECT_DOUBLE_EQ(e->convexity, 9.1e4 * (0.44 - 0.50));
  EXPECT_DOUBLE_EQ(e->discount, -1.3e5 * (0.0007 - 0.0));
}

// ── The closed-form world ─────────────────────────────────────────────────

// A big up-move day on a fair-struck swap. Every number below is derived from
// the contract definition at the top of this file:
//
//   PV0  = 0                       (fair-struck, everything realized at implied)
//   PV1  = 1e6 * (10.13/252 - 0.04) = 50000/252   with r^2 = 0.09
//   carry    = -40000/252          (one zero fixing retires one implied day)
//   realized = 1e6 * 0.09 / 252    (the actual fixing, at the linear weight)
//
// and -40000/252 + 90000/252 = 50000/252 = dPV exactly. The residual is
// therefore floating-point noise on a world where the explain is COMPLETE, not
// a tolerance hiding a missing term.
TEST(DerivPnlExplain, ClosedFormVarSwapStepIsFullyExplained) {
  const double r2 = 0.09;
  const double pv0 = pv_of(kNDone0, kRvDone0, kKVar);
  const double pv1 = pv_after_fixing(r2);
  ASSERT_DOUBLE_EQ(pv0, 0.0);
  // The running-mean route and the closed form are algebraically the same
  // number but not the same rounding (~16 ULP apart at this magnitude), so the
  // hand-derived value is asserted relatively rather than on the bits.
  ASSERT_NEAR(pv1, 50000.0 / kNTotal, 1.0e-12 * (50000.0 / kNTotal));

  DerivPnlInputs in{};
  in.greeks = flat_greeks();
  in.dt_years = kDt;
  in.realized_var_dec = r2;
  in.fixing_weight = kN / kNTotal;  // df = 1 in this world
  in.from = mark_at(pv0);
  in.to = mark_at(pv1);

  const auto e = deriv_pnl_explain(in);
  ASSERT_TRUE(e.has_value()) << e.error().to_string();

  EXPECT_DOUBLE_EQ(e->carry, -40000.0 / kNTotal);
  EXPECT_DOUBLE_EQ(e->realized, 90000.0 / kNTotal);
  EXPECT_DOUBLE_EQ(e->vol_level, 0.0);
  EXPECT_DOUBLE_EQ(e->skew, 0.0);
  EXPECT_DOUBLE_EQ(e->convexity, 0.0);
  EXPECT_DOUBLE_EQ(e->discount, 0.0);
  EXPECT_NEAR(e->d_pv, 50000.0 / kNTotal, 1.0e-12 * (50000.0 / kNTotal));
  EXPECT_LT(std::abs(e->residual), 1.0e-9 * std::abs(pv1));
}

// The brief's zero-return case, stated as it states it: carry IS the move, to
// the last bit the two expressions can agree on, and every other component is
// exactly zero rather than merely small.
TEST(DerivPnlExplain, ZeroReturnDayIsPureCarry) {
  const double pv0 = pv_of(kNDone0, kRvDone0, kKVar);
  const double pv1 = pv_after_fixing(0.0);
  ASSERT_LT(pv1, 0.0);  // retiring an implied day for nothing is a loss

  DerivPnlInputs in{};
  in.greeks = flat_greeks();
  in.dt_years = kDt;
  in.realized_var_dec = 0.0;
  in.fixing_weight = kN / kNTotal;
  in.from = mark_at(pv0);
  in.to = mark_at(pv1);

  const auto e = deriv_pnl_explain(in);
  ASSERT_TRUE(e.has_value()) << e.error().to_string();
  EXPECT_DOUBLE_EQ(e->carry, kThetaZeroFixing * kDt);
  EXPECT_EQ(e->realized, 0.0);
  EXPECT_EQ(e->vol_level, 0.0);
  EXPECT_EQ(e->skew, 0.0);
  EXPECT_EQ(e->convexity, 0.0);
  EXPECT_EQ(e->discount, 0.0);
  EXPECT_LT(std::abs(e->residual), 1.0e-8 * std::abs(pv1));
}

// The task brief specified `realized = (r^2 - K_var_future/n) * leg_weight`.
// Paired with a ZERO-fixing carry that has already retired the implied day,
// that subtracts the implied leg twice. This measures the size of the mistake
// rather than asserting it away: on the closed-form world the leftover is
// exactly `fixing_weight * K_var_future`, and the shipped (unpaired) form has
// no leftover at all.
TEST(DerivPnlExplain, RealizedTermPairsWithZeroFixingCarryNotWithImplied) {
  const double r2 = 0.09;
  const double weight = kN / kNTotal;

  DerivPnlInputs in{};
  in.greeks = flat_greeks();
  in.dt_years = kDt;
  in.fixing_weight = weight;
  in.from = mark_at(pv_of(kNDone0, kRvDone0, kKVar));
  in.to = mark_at(pv_after_fixing(r2));

  in.realized_var_dec = r2;
  const auto shipped = deriv_pnl_explain(in);
  ASSERT_TRUE(shipped.has_value()) << shipped.error().to_string();

  in.realized_var_dec = r2 - kKVar;  // the brief's paired form
  const auto paired = deriv_pnl_explain(in);
  ASSERT_TRUE(paired.has_value()) << paired.error().to_string();

  EXPECT_LT(std::abs(shipped->residual), 1.0e-9 * std::abs(in.to.pv));
  EXPECT_NEAR(paired->residual, weight * kKVar, 1.0e-9 * weight * kKVar);
  // 158.73 on a 198.41 move -- 80% of the day, left unexplained.
  EXPECT_GT(std::abs(paired->residual), 0.5 * std::abs(shipped->d_pv));
}

// A day where only the smile's SLOPE moved. The skew term must carry it and
// the level term must be exactly zero -- not small, zero -- because the ATM
// vol did not move at all. A `skew_vega` accidentally multiplied by the ATM
// move, or a `vega` reading the slope, fails on the second assertion even
// though the sum would still reconcile.
TEST(DerivPnlExplain, SkewMoveIsAttributedToSkewNotLevel) {
  DerivGreeks g = flat_greeks();
  g.theta_zero_fixing = 0.0;  // isolate the smile move
  g.vega = 8.0e5;
  g.skew_vega = -2.4e5;

  // Both slopes and their difference are exact in binary, so the expectation
  // below is an exact product rather than a tolerance -- the point of this
  // test is WHICH sensitivity was multiplied, and a rounding allowance would
  // blunt that.
  const double slope_from = -0.25;
  const double slope_to = -0.375;                     // steepens by exactly 0.125
  const double expected_skew = -2.4e5 * -0.125;       // = +30000, hand-computed

  DerivPnlInputs in{};
  in.greeks = g;
  in.dt_years = kDt;
  in.realized_var_dec = 0.0;
  in.fixing_weight = kN / kNTotal;
  in.from = mark_at(0.0);
  in.to = mark_at(expected_skew);
  in.from.skew_slope = slope_from;
  in.to.skew_slope = slope_to;

  const auto e = deriv_pnl_explain(in);
  ASSERT_TRUE(e.has_value()) << e.error().to_string();
  EXPECT_DOUBLE_EQ(e->skew, expected_skew);
  EXPECT_EQ(e->vol_level, 0.0);
  EXPECT_EQ(e->convexity, 0.0);
  EXPECT_DOUBLE_EQ(e->residual, 0.0);
  EXPECT_GT(std::abs(e->skew), 0.9 * std::abs(e->d_pv));
}

// ── "Not computed" must not read as "measured zero" ───────────────────────

// `skew_vega`/`convexity_vega` are NaN unless `smile_greeks` was on, and
// `theta_zero_fixing` is NaN on any of five documented conditions. A component
// built from one of those must come back NaN with its flag raised, and must
// poison the residual -- a partially-attributed day that reported a clean
// residual would be the worst possible output of this function.
TEST(DerivPnlExplain, UnavailableSensitivitiesFlagAndPoisonRatherThanZero) {
  DerivPnlInputs in{};
  in.greeks = flat_greeks();
  in.greeks.theta_zero_fixing = kNaN;  // e.g. carry_theta off, or T <= dt
  in.greeks.skew_vega = kNaN;          // smile_greeks off
  in.dt_years = kDt;
  in.realized_var_dec = 0.01;
  in.fixing_weight = kN / kNTotal;
  in.from = mark_at(0.0);
  in.to = mark_at(100.0);

  const auto e = deriv_pnl_explain(in);
  ASSERT_TRUE(e.has_value()) << e.error().to_string();
  EXPECT_TRUE(std::isnan(e->carry));
  EXPECT_TRUE(std::isnan(e->skew));
  EXPECT_TRUE(std::isnan(e->residual));
  EXPECT_TRUE(has_flag(e->flags, DerivPnlFlags::CarryUnavailable));
  EXPECT_TRUE(has_flag(e->flags, DerivPnlFlags::SkewUnavailable));
  EXPECT_FALSE(has_flag(e->flags, DerivPnlFlags::VolLevelUnavailable));
  EXPECT_FALSE(has_flag(e->flags, DerivPnlFlags::RealizedUnavailable));
  // The components that WERE measurable are still measured, so a report can
  // print what it knows alongside the gap.
  EXPECT_DOUBLE_EQ(e->realized, (kN / kNTotal) * 0.01);

  // A step with no measurable realized variance (a stale or holiday date) is
  // flagged, not priced as a zero return -- those are different days.
  DerivPnlInputs stale = in;
  stale.greeks = flat_greeks();
  stale.realized_var_dec = kNaN;
  const auto s = deriv_pnl_explain(stale);
  ASSERT_TRUE(s.has_value()) << s.error().to_string();
  EXPECT_TRUE(has_flag(s->flags, DerivPnlFlags::RealizedUnavailable));
  EXPECT_TRUE(std::isnan(s->realized));

  // An unusable mark on either date flags rather than silently differencing.
  DerivPnlInputs no_mark = in;
  no_mark.greeks = flat_greeks();
  no_mark.to.pv = kNaN;
  const auto m = deriv_pnl_explain(no_mark);
  ASSERT_TRUE(m.has_value()) << m.error().to_string();
  EXPECT_TRUE(has_flag(m->flags, DerivPnlFlags::MarkUnavailable));
  EXPECT_TRUE(std::isnan(m->d_pv));
}

TEST(DerivPnlExplain, RejectsAnUndefinedStepLength) {
  DerivPnlInputs in{};
  in.greeks = flat_greeks();
  in.from = mark_at(0.0);
  in.to = mark_at(1.0);
  in.dt_years = -kDt;
  EXPECT_FALSE(deriv_pnl_explain(in).has_value());
  in.dt_years = kNaN;
  EXPECT_FALSE(deriv_pnl_explain(in).has_value());
  in.dt_years = 0.0;  // two marks on the same instant is a legitimate query
  EXPECT_TRUE(deriv_pnl_explain(in).has_value());
}

// ── The fixing weight ─────────────────────────────────────────────────────

// Only the uncapped variance leg is linear in one fixing. Returning a linear
// weight for a vol swap or a capped kind would be a wrong measurement dressed
// as a right one, so those come back NaN and the caller has to supply its own.
TEST(DerivPnlExplain, FixingWeightIsLinearOnlyForTheVarianceLeg) {
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.35;
  c.notional = kN;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 252u;

  // df * N / n_total, hand-computed at df = 1 and at a real discount factor.
  EXPECT_DOUBLE_EQ(var_swap_fixing_weight(c, 1.0), kN / kNTotal);
  EXPECT_DOUBLE_EQ(var_swap_fixing_weight(c, 0.985), 0.985 * kN / kNTotal);

  for (const DerivKind kind : {DerivKind::VolSwap, DerivKind::CappedVarSwap,
                               DerivKind::CappedVolSwap, DerivKind::GammaSwap}) {
    DerivContract other = c;
    other.kind = kind;
    EXPECT_TRUE(std::isnan(var_swap_fixing_weight(other, 1.0)))
        << "kind " << static_cast<int>(kind) << " has no constant fixing weight";
  }

  DerivContract no_schedule = c;
  no_schedule.rv_spec.n_obs_total = 0u;
  EXPECT_TRUE(std::isnan(var_swap_fixing_weight(no_schedule, 1.0)));
  EXPECT_TRUE(std::isnan(var_swap_fixing_weight(c, kNaN)));
}

// ── The one test whose two sides both come from the library ───────────────

// Marks from `deriv_price`, sensitivities from `deriv_greeks`: two different
// entry points, so the residual measures whether the greeks predict the mark
// rather than whether one expression equals itself. A zero-vol-move step over
// one day on a real fitted surface should be explained by carry plus the
// realized fixing to within the second-order terms this attribution
// deliberately leaves out.
//
// The bound is stated as a fraction of the MOVE, and is roughly two orders of
// magnitude looser than the measured residual -- it is a regression guard on
// the attribution staying first-order-correct, not a pin on a number.
TEST(DerivPnlExplain, AgainstAPricedStepTheResidualIsSecondOrder) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(70, 100.0, 100.0, 0.22);

  const double dt = 1.0 / 252.0;
  const double r2 = 0.05;  // the step realizes 5 variance points, annualized

  DerivContract c0{};
  c0.kind = DerivKind::VarSwap;
  c0.maturity_t = 0.35;
  c0.notional = kN;
  c0.rv_spec.annualization = 252.0;
  c0.rv_spec.n_obs_total = 63u;
  c0.rv_spec.n_obs_done = 20u;
  c0.rv_spec.rv_done_dec = 0.048;

  atx::vol::DerivGreekBumps bumps{};
  bumps.time_years = dt;      // the carry roll must match the step being explained
  bumps.smile_greeks = true;  // otherwise the two smile terms are "not computed"
  const auto g0 = atx::vol::deriv_greeks(ps, c0, atx::vol::deriv_default_config(), bumps);
  ASSERT_TRUE(g0.has_value()) << g0.error().to_string();
  const auto q0 = atx::vol::deriv_price(ps, c0);
  ASSERT_TRUE(q0.has_value()) << q0.error().to_string();

  // One day later: the calendar rolls and one fixing lands at r2. The surface
  // is unchanged, so every smile component is exactly zero by construction.
  DerivContract c1 = c0;
  c1.maturity_t = c0.maturity_t - dt;
  c1.rv_spec.n_obs_done = c0.rv_spec.n_obs_done + 1u;
  c1.rv_spec.rv_done_dec =
      (static_cast<double>(c0.rv_spec.n_obs_done) * c0.rv_spec.rv_done_dec + r2) /
      (static_cast<double>(c0.rv_spec.n_obs_done) + 1.0);
  const auto q1 = atx::vol::deriv_price(ps, c1);
  ASSERT_TRUE(q1.has_value()) << q1.error().to_string();

  const double df1 = std::exp(-atx::vol::testkit::kFixtureRate * c1.maturity_t);

  DerivPnlInputs in{};
  in.greeks = *g0;
  in.dt_years = dt;
  in.realized_var_dec = r2;
  in.fixing_weight = var_swap_fixing_weight(c1, df1);
  ASSERT_TRUE(std::isfinite(in.fixing_weight));
  in.from = mark_at(q0->pv);
  in.to = mark_at(q1->pv);

  const auto e = deriv_pnl_explain(in);
  ASSERT_TRUE(e.has_value()) << e.error().to_string();
  ASSERT_EQ(e->flags, DerivPnlFlags::None) << "the fixture must leave nothing unmeasured";

  EXPECT_EQ(e->vol_level, 0.0);
  EXPECT_EQ(e->skew, 0.0);
  EXPECT_EQ(e->convexity, 0.0);
  EXPECT_EQ(e->discount, 0.0);
  EXPECT_NE(e->carry, 0.0);
  EXPECT_NE(e->realized, 0.0);

  // MEASURED: residual is 0.0 exactly (carry -748.79 + realized +781.93 =
  // d_pv 33.136444326308265, to the last bit). It is exact rather than merely
  // small because a var swap's PV is LINEAR in the realized leg and the
  // surface did not move, so this fixture has no second-order term to leave
  // behind. What it therefore establishes is narrow but real: the fixing
  // weight, the sign of carry, and that the two do not double-count the
  // implied day. The genuinely approximate case is the next test.
  const double rel = std::abs(e->residual) / std::abs(e->d_pv);
  EXPECT_LT(rel, 1.0e-12) << "carry=" << e->carry << " realized=" << e->realized
                          << " d_pv=" << e->d_pv << " residual=" << e->residual;
}

// The case with something left over. Same one-day step, but the whole surface
// lifts by one vol point, so `vega * dsigma` is a first-order approximation to
// a move that is genuinely curved. The residual must then be SMALL BUT NOT
// ZERO, and must be accounted for by the second-order term the attribution
// deliberately omits: `0.5 * volga * dsigma^2`.
//
// That last comparison is the part worth having. `volga` is produced by a
// different stencil than `vega` (a second difference rather than a first), so
// a residual that lands on 0.5*volga*dsigma^2 is two independent measurements
// agreeing, not one quantity checked against itself. A first-order explain
// with a transposed or mis-scaled vega would still reconcile its own sum and
// would still leave a "small" residual -- but not one shaped like volga.
TEST(DerivPnlExplain, VolLevelMoveLeavesOnlyTheSecondOrderTerm) {
  const double sigma0 = 0.22;
  const double d_sigma = 0.01;
  const atx::vol::PricedSurface ps0 =
      atx::vol::testkit::make_flat_surface(71, 100.0, 100.0, sigma0);
  const atx::vol::PricedSurface ps1 =
      atx::vol::testkit::make_flat_surface(71, 100.0, 100.0, sigma0 + d_sigma);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.35;
  c.notional = kN;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 20u;
  c.rv_spec.rv_done_dec = 0.048;

  atx::vol::DerivGreekBumps bumps{};
  bumps.smile_greeks = true;
  const auto g0 = atx::vol::deriv_greeks(ps0, c, atx::vol::deriv_default_config(), bumps);
  ASSERT_TRUE(g0.has_value()) << g0.error().to_string();
  const auto q0 = atx::vol::deriv_price(ps0, c);
  ASSERT_TRUE(q0.has_value()) << q0.error().to_string();
  const auto q1 = atx::vol::deriv_price(ps1, c);
  ASSERT_TRUE(q1.has_value()) << q1.error().to_string();

  // No time passes and no fixing lands: this step is the vol move alone.
  DerivPnlInputs in{};
  in.greeks = *g0;
  in.dt_years = 0.0;
  in.realized_var_dec = 0.0;
  in.fixing_weight = var_swap_fixing_weight(c, 1.0);
  in.from = mark_at(q0->pv);
  in.to = mark_at(q1->pv);
  in.from.sigma_atm = sigma0;
  in.to.sigma_atm = sigma0 + d_sigma;

  const auto e = deriv_pnl_explain(in);
  ASSERT_TRUE(e.has_value()) << e.error().to_string();
  ASSERT_EQ(e->flags, DerivPnlFlags::None);
  EXPECT_EQ(e->carry, 0.0);
  EXPECT_EQ(e->realized, 0.0);
  EXPECT_EQ(e->skew, 0.0);
  EXPECT_EQ(e->convexity, 0.0);

  // The move is dominated by the level term, but not entirely explained by it.
  EXPECT_GT(std::abs(e->vol_level), 0.9 * std::abs(e->d_pv));
  EXPECT_NE(e->residual, 0.0);
  EXPECT_LT(std::abs(e->residual), 0.05 * std::abs(e->d_pv));

  // ... and what is left over is the omitted second-order term, within 5% of
  // it. A var swap's PV is exactly quadratic in a flat vol, so `volga` is a
  // constant here and the Taylor remainder is exact up to the strip's own
  // quadrature: the tolerance is for the quadrature, not for the shape.
  const double second_order = 0.5 * g0->volga * d_sigma * d_sigma;
  EXPECT_NEAR(e->residual, second_order, 0.05 * std::abs(second_order))
      << "residual=" << e->residual << " 0.5*volga*dsigma^2=" << second_order
      << " vol_level=" << e->vol_level << " d_pv=" << e->d_pv;
}

// I-3 (review round 1): the vega leg of this file's oracle was attacked and
// held, but `skew_vega` was checked only against ITSELF -- multiplied by a
// slope move in `SkewMoveIsAttributedToSkewNotLevel`, never against a move the
// pricer actually produced. A skew_vega wrong by a constant factor, or carrying
// the wrong sign convention for k, would pass every test in this file.
//
// This closes it with the same instrument that made the vega leg credible: the
// move comes from a DIFFERENT entry point (`deriv_price_shocked_on_ref`, which
// reprices the contract with the smile rotated by exactly `s`), the
// sensitivity comes from `deriv_greeks`' smile stencil, and the claim is about
// the SHAPE of the residual rather than its size -- halve the rotation and the
// first-order explain must improve fourfold.
//
// The two conventions are the same object by construction and that is what
// makes the comparison legitimate rather than a coincidence: `DerivGreeks::
// skew_vega` differentiates `iv(k,T) -> max(iv(k,T) + s*k, 1e-4)`, and
// `SurfaceOverlay::skew_shift` IS that `+ s*k`, floored identically. So a
// rotation of `s` is a `skew_slope` move of exactly `s`.
TEST(DerivPnlExplain, SkewVegaExplainsAPricedSmileRotation) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_skewed_surface(72, 100.0, 100.0);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = atx::vol::SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  const atx::vol::SurfaceRef ref = ss->find(72u);
  ASSERT_NE(ref, nullptr);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.35;
  c.notional = kN;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 20u;
  c.rv_spec.rv_done_dec = 0.048;

  atx::vol::DerivGreekBumps bumps{};
  bumps.smile_greeks = true;
  const auto g0 = atx::vol::deriv_greeks(ps, c, atx::vol::deriv_default_config(), bumps);
  ASSERT_TRUE(g0.has_value()) << g0.error().to_string();
  ASSERT_TRUE(std::isfinite(g0->skew_vega));
  // Pinned on DerivGreeks::skew_vega: a long var swap loses as the skew
  // flattens, so this is negative. Asserted here because the residual-shape
  // check below is insensitive to an overall sign flip.
  ASSERT_LT(g0->skew_vega, 0.0);

  const auto q0 = atx::vol::deriv_price(ps, c);
  ASSERT_TRUE(q0.has_value()) << q0.error().to_string();

  // The residual of the first-order explain, as a function of the rotation.
  const auto residual_at = [&](double s) {
    atx::vol::detail::DerivShock shock{};
    shock.skew_shift = s;
    const auto q1 = atx::vol::detail::deriv_price_shocked_on_ref(
        ref, c, atx::vol::deriv_default_config(), std::nullopt, shock, &g0->quote);
    EXPECT_TRUE(q1.has_value());
    if (!q1.has_value()) {
      return std::pair<double, double>{kNaN, kNaN};
    }

    DerivPnlInputs in{};
    in.greeks = *g0;
    in.dt_years = 0.0;
    in.realized_var_dec = 0.0;
    in.fixing_weight = var_swap_fixing_weight(c, 1.0);
    in.from = mark_at(q0->pv);
    in.to = mark_at(q1->pv);
    in.from.skew_slope = 0.0;
    in.to.skew_slope = s;  // the rotation IS the slope move, see above

    const auto e = deriv_pnl_explain(in);
    EXPECT_TRUE(e.has_value());
    if (!e.has_value()) {
      return std::pair<double, double>{kNaN, kNaN};
    }
    EXPECT_EQ(e->flags, DerivPnlFlags::None);
    EXPECT_EQ(e->vol_level, 0.0);
    EXPECT_EQ(e->carry, 0.0);
    EXPECT_EQ(e->convexity, 0.0);
    return std::pair<double, double>{std::abs(e->residual), std::abs(e->d_pv)};
  };

  const double s = 0.01;  // one vol point of rotation per unit k
  const auto [res_s, move_s] = residual_at(s);
  const auto [res_half, move_half] = residual_at(s / 2.0);
  ASSERT_TRUE(std::isfinite(res_s) && std::isfinite(res_half));

  // The rotation must actually move the mark, or none of this measures anything.
  ASSERT_GT(move_s, 0.0);
  EXPECT_GT(move_half, 0.0);

  // The skew term carries nearly all of it -- this is what a wrong SCALE on
  // skew_vega would break, and what no previous test in this file checked.
  EXPECT_LT(res_s, 0.05 * move_s)
      << "skew_vega=" << g0->skew_vega << " residual=" << res_s << " move=" << move_s;

  // And the leftover is second-order in the rotation: halving `s` must shrink
  // it by at least 3x (a pure O(s^2) remainder gives 4x). A skew_vega scaled by
  // a constant factor leaves a FIRST-order residual, which shrinks by only 2x
  // and fails here -- the same discrimination the volga witness provides for
  // vega, and the reason this is a shape test rather than a tolerance.
  EXPECT_LT(res_half, res_s / 3.0)
      << "residual did not shrink second-order: " << res_s << " -> " << res_half;
}

// ── The daily theo-edge ledger (vrp-ml sprint, lane vrp-book) ──────────────
//
// Line 1 `collected` = 0.5*Gamma*S^2*(r^2 - sigma^2*dt), line 2 `repriced` =
// vega*dIV. The oracle here is a closed-form Black-Scholes world with r = q =
// 0, where the straddle price, delta, gamma and vega are four hand-writable
// expressions and the delta-hedged step P&L is computed from REPRICING, not
// from the ledger -- so the acceptance identity (sum of line 1 over a
// position's life == its total edge P&L up to hedging noise) is measured
// against an independent number.

namespace {

using atx::vol::TheoEdgeLedger;
using atx::vol::TheoEdgeLedgerFlags;
using atx::vol::TheoEdgeLedgerInputs;
using atx::vol::theo_edge_ledger;

[[nodiscard]] double norm_cdf(double x) noexcept {
  return 0.5 * std::erfc(-x / std::sqrt(2.0));
}
[[nodiscard]] double norm_pdf(double x) noexcept {
  return std::exp(-0.5 * x * x) / std::sqrt(2.0 * 3.14159265358979323846);
}

// Black-Scholes straddle with r = q = 0 (so American == European and every
// greek below is exact). All per one unit of the straddle.
struct BsStraddle {
  double price;
  double delta;
  double gamma;
  double vega;
};

[[nodiscard]] BsStraddle bs_straddle(double S, double K, double sigma, double T) noexcept {
  const double sqT = std::sqrt(T);
  const double d1 = (std::log(S / K) + 0.5 * sigma * sigma * T) / (sigma * sqT);
  const double d2 = d1 - sigma * sqT;
  BsStraddle out{};
  const double call = S * norm_cdf(d1) - K * norm_cdf(d2);
  const double put = call - S + K; // parity at r = q = 0
  out.price = call + put;
  out.delta = 2.0 * norm_cdf(d1) - 1.0;
  out.gamma = 2.0 * norm_pdf(d1) / (S * sigma * sqT);
  out.vega = 2.0 * S * norm_pdf(d1) * sqT;
  return out;
}

constexpr double kLedgerDt = 1.0 / 252.0;

// The controlled 5-day fixture: ATM 21-trading-day straddle, spot follows a
// planted return path, IV constant unless a test moves it.
constexpr double kLedgerS0 = 100.0;
constexpr double kLedgerK = 100.0;
constexpr double kLedgerT0 = 21.0 / 252.0;
constexpr double kLedgerSigma = 0.20;
constexpr std::array<double, 5> kLedgerReturns{0.008, -0.006, 0.004, -0.009, 0.007};

} // namespace

TEST(DerivPnlExplain, TheoEdgeLedgerSplitsCollectedFromRepriced) {
  TheoEdgeLedgerInputs in{};
  in.gamma = 2.5;
  in.spot = 50.0;
  in.r_step = 0.01;
  in.sigma_imp = 0.30;
  in.dt_years = kLedgerDt;
  in.vega = 120.0;
  in.d_iv = -0.004;

  const auto ledger = theo_edge_ledger(in);
  ASSERT_TRUE(ledger.has_value()) << ledger.error().to_string();
  EXPECT_EQ(ledger->flags, TheoEdgeLedgerFlags::None);
  // The exact published expressions, reproduced term for term.
  EXPECT_DOUBLE_EQ(ledger->collected,
                   0.5 * 2.5 * 50.0 * 50.0 * (0.01 * 0.01 - 0.30 * 0.30 * kLedgerDt));
  EXPECT_DOUBLE_EQ(ledger->repriced, 120.0 * -0.004);
}

TEST(DerivPnlExplain, TheoEdgeLedgerFlagsUnavailableInputsAndRejectsBadDt) {
  TheoEdgeLedgerInputs good{};
  good.gamma = 1.0;
  good.spot = 100.0;
  good.r_step = 0.0;
  good.sigma_imp = 0.2;
  good.dt_years = kLedgerDt;
  good.vega = 10.0;
  good.d_iv = 0.0;

  // A NaN gamma poisons line 1 only; line 2 stays a real measurement.
  TheoEdgeLedgerInputs no_gamma = good;
  no_gamma.gamma = kNaN;
  const auto l1 = theo_edge_ledger(no_gamma);
  ASSERT_TRUE(l1.has_value());
  EXPECT_TRUE(std::isnan(l1->collected));
  EXPECT_TRUE(std::isfinite(l1->repriced));
  EXPECT_TRUE(has_flag(l1->flags, TheoEdgeLedgerFlags::CollectedUnavailable));
  EXPECT_FALSE(has_flag(l1->flags, TheoEdgeLedgerFlags::RepricedUnavailable));

  // A NaN IV move poisons line 2 only.
  TheoEdgeLedgerInputs no_div = good;
  no_div.d_iv = kNaN;
  const auto l2 = theo_edge_ledger(no_div);
  ASSERT_TRUE(l2.has_value());
  EXPECT_TRUE(std::isfinite(l2->collected));
  EXPECT_TRUE(std::isnan(l2->repriced));
  EXPECT_TRUE(has_flag(l2->flags, TheoEdgeLedgerFlags::RepricedUnavailable));
  EXPECT_FALSE(has_flag(l2->flags, TheoEdgeLedgerFlags::CollectedUnavailable));

  // dt is the one caller error the ledger cannot be defined against.
  TheoEdgeLedgerInputs bad_dt = good;
  bad_dt.dt_years = -kLedgerDt;
  EXPECT_FALSE(theo_edge_ledger(bad_dt).has_value());
  bad_dt.dt_years = kNaN;
  EXPECT_FALSE(theo_edge_ledger(bad_dt).has_value());
}

// The acceptance identity: over the position's life, sum(collected) equals
// the delta-hedged straddle's total P&L up to hedging noise. IV is constant
// here, so line 2 is exactly zero every day and the whole P&L must be line 1
// plus the discrete-hedge/higher-order residual. The bound below is the
// DOCUMENTED hedge-noise allowance for this fixture: the residual is
// third-order in the daily move (|r| <= 0.9%, five steps), measured at ~2e-3
// against a ~0.4 total, and the assertion allows 5x the measurement.
TEST(DerivPnlExplain, FiveDayStraddleCollectedSumsToTotalEdgePnl) {
  double spot = kLedgerS0;
  double total_hedged_pnl = 0.0;
  double sum_collected = 0.0;
  double sum_abs_collected = 0.0;
  for (std::size_t t = 0; t < kLedgerReturns.size(); ++t) {
    const double T_t = kLedgerT0 - static_cast<double>(t) * kLedgerDt;
    const BsStraddle now = bs_straddle(spot, kLedgerK, kLedgerSigma, T_t);
    const double next_spot = spot * std::exp(kLedgerReturns[t]);
    const BsStraddle next = bs_straddle(next_spot, kLedgerK, kLedgerSigma, T_t - kLedgerDt);
    total_hedged_pnl += next.price - now.price - now.delta * (next_spot - spot);

    TheoEdgeLedgerInputs in{};
    in.gamma = now.gamma;
    in.spot = spot;
    in.r_step = kLedgerReturns[t];
    in.sigma_imp = kLedgerSigma;
    in.dt_years = kLedgerDt;
    in.vega = now.vega;
    in.d_iv = 0.0;
    const auto ledger = theo_edge_ledger(in);
    ASSERT_TRUE(ledger.has_value()) << ledger.error().to_string();
    EXPECT_EQ(ledger->flags, TheoEdgeLedgerFlags::None);
    EXPECT_EQ(ledger->repriced, 0.0) << "constant IV must remark nothing";
    sum_collected += ledger->collected;
    sum_abs_collected += std::fabs(ledger->collected);
    spot = next_spot;
  }
  ASSERT_GT(sum_abs_collected, 0.0);
  const double hedge_noise_bound = 0.01; // documented: ~5x the measured 2e-3 residual
  EXPECT_NEAR(sum_collected, total_hedged_pnl, hedge_noise_bound)
      << "sum(collected)=" << sum_collected << " total=" << total_hedged_pnl;
  // And the identity is not vacuous: the position actually collected edge.
  EXPECT_LT(std::fabs(sum_collected - total_hedged_pnl), 0.2 * sum_abs_collected);
}

// The two ledger lines are the two-line reading of the EXISTING attribution:
// with the straddle's zero-fixing carry theta and per-fixing weight,
// deriv_pnl_explain's carry + realized IS line 1 and its vol_level IS line 2,
// and collected + repriced reconciles each step's delta-hedged P&L up to the
// same hedge noise. IV moves mid-fixture so line 2 is genuinely nonzero.
TEST(DerivPnlExplain, TheoEdgeLedgerReconcilesWithExplainLinesAndStepPnl) {
  const std::array<double, 6> sigma_path{0.20, 0.20, 0.21, 0.21, 0.205, 0.205};
  double spot = kLedgerS0;
  for (std::size_t t = 0; t < kLedgerReturns.size(); ++t) {
    const double T_t = kLedgerT0 - static_cast<double>(t) * kLedgerDt;
    const double sigma_t = sigma_path[t];
    const double sigma_next = sigma_path[t + 1];
    const BsStraddle now = bs_straddle(spot, kLedgerK, sigma_t, T_t);
    const double next_spot = spot * std::exp(kLedgerReturns[t]);
    const BsStraddle next = bs_straddle(next_spot, kLedgerK, sigma_next, T_t - kLedgerDt);
    const double step_pnl = next.price - now.price - now.delta * (next_spot - spot);

    TheoEdgeLedgerInputs lin{};
    lin.gamma = now.gamma;
    lin.spot = spot;
    lin.r_step = kLedgerReturns[t];
    lin.sigma_imp = sigma_t;
    lin.dt_years = kLedgerDt;
    lin.vega = now.vega;
    lin.d_iv = sigma_next - sigma_t;
    const auto ledger = theo_edge_ledger(lin);
    ASSERT_TRUE(ledger.has_value()) << ledger.error().to_string();
    EXPECT_EQ(ledger->flags, TheoEdgeLedgerFlags::None);

    // The existing explain, fed the straddle's own zero-fixing carry theta
    // (-0.5*Gamma*S^2*sigma^2 per year) and per-fixing weight
    // (0.5*Gamma*S^2 / 252) against the step's annualized realized variance.
    DerivPnlInputs ein{};
    ein.greeks = DerivGreeks{};
    ein.greeks.theta_zero_fixing = -0.5 * now.gamma * spot * spot * sigma_t * sigma_t;
    ein.greeks.vega = now.vega;
    ein.greeks.rho = 0.0;
    ein.greeks.skew_vega = 0.0;
    ein.greeks.convexity_vega = 0.0;
    ein.dt_years = kLedgerDt;
    ein.fixing_weight = 0.5 * now.gamma * spot * spot / 252.0;
    ein.realized_var_dec = 252.0 * kLedgerReturns[t] * kLedgerReturns[t];
    ein.from = mark_at(now.price);
    ein.to = mark_at(next.price);
    ein.from.sigma_atm = sigma_t;
    ein.to.sigma_atm = sigma_next;
    const auto explain = deriv_pnl_explain(ein);
    ASSERT_TRUE(explain.has_value()) << explain.error().to_string();
    EXPECT_EQ(explain->flags, DerivPnlFlags::None);

    // Line 1 == carry + realized, line 2 == vol_level (association noise only).
    const double explain_line1 = explain->carry + explain->realized;
    EXPECT_NEAR(explain_line1, ledger->collected,
                1.0e-12 * (std::fabs(ledger->collected) + 1.0));
    EXPECT_DOUBLE_EQ(explain->vol_level, ledger->repriced);

    // And the two lines reconcile the step's delta-hedged P&L up to the
    // DOCUMENTED per-step noise: on the +1-vol-pt bump step the leftover is
    // the vanna/volga cross terms a two-line ledger deliberately leaves
    // unattributed — measured 6.4e-3 on this fixture — plus the same
    // discrete-hedge noise as the life-sum test. Bound: 1.6x the measurement.
    EXPECT_NEAR(step_pnl, ledger->collected + ledger->repriced, 1.0e-2)
        << "t=" << t << " step_pnl=" << step_pnl << " collected=" << ledger->collected
        << " repriced=" << ledger->repriced;
    spot = next_spot;
  }
}
