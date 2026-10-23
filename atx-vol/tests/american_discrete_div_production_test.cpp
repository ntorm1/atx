// The D1 production cash-dividend path (api/pricing/american_discrete_div_kernel.hpp
// section 4, api/pricing/american_discrete_div_batch.hpp) — the fast suite.
//
//   * the production rollback with every correction OFF is the historical rollback
//     bit for bit, and the kill switch is the historical lattice call for call;
//   * the route rule (events -> steps, pre-steps, flags, refusals);
//   * accuracy pins against D0's independent reference (matrix.csv cc89bc89);
//   * the production inversion's root, the event-aware sensitivities and their
//     unsupported marking, and the batch's identity with the per-contract entries.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_batch.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"
#include "pricing/american_discrete_div_detail.hpp"

namespace atx::vol {
namespace {

namespace dd = ddiv_detail;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] std::vector<CashDividend> quarterly(double first, double amount, double T) {
  std::vector<CashDividend> out;
  for (double t = first; t <= T + 1e-12; t += 0.25) {
    out.push_back({t, amount});
  }
  return out;
}

[[nodiscard]] DiscreteDivProductionOpts opts_of(DiscreteDivAccuracy accuracy,
                                                int base = kDiscreteDivDefaultSteps,
                                                bool kill = false) {
  DiscreteDivProductionOpts o;
  o.accuracy = accuracy;
  o.base_steps = base;
  o.kill_switch = kill;
  return o;
}

[[nodiscard]] DiscreteDivWorkspace production_workspace() {
  return DiscreteDivWorkspace(kDiscreteDivMaxRouteSteps, kDiscreteDivMaxPreSteps);
}

// ── The historical rollback is untouched ────────────────────────────────────

TEST(DiscreteDivProductionLattice, AllCorrectionsOffIsTheHistoricalRollbackBitForBit) {
  const double dt = 1.0 / 301.0;
  const std::vector<std::vector<CashDividend>> schedules = {
      {},
      {{0.5, 3.0}},
      {{0.7 * dt, 3.0}},
      {{1.0, 3.0}},
      quarterly(0.1, 1.0, 1.0),
      {{0.25, 6.0}},
  };
  dd::LatticeBuffers a;
  dd::LatticeBuffers b;
  int compared = 0;
  for (const auto &divs : schedules) {
    for (const Side side : {Side::Call, Side::Put}) {
      for (const ExerciseStyle ex : {ExerciseStyle::American, ExerciseStyle::European}) {
        for (const int steps : {101, 301}) {
          for (const bool bbs : {false, true}) {
            dd::ProductionSpec spec;
            spec.splice = dd::SpliceInterp::Linear;
            spec.pre_steps = 0;
            spec.align_forward = false;
            spec.align_exercise = false;
            spec.bbs = bbs;
            const auto hist =
                dd::run_lattice(100.0, 95.0, 1.0, 0.3, 0.04, 0.01, side, divs, steps, ex, bbs, a);
            const auto prod = dd::run_production_lattice(100.0, 95.0, 1.0, 0.3, 0.04, 0.01, side,
                                                         divs, steps, ex, spec, b);
            ASSERT_TRUE(hist.has_value() && prod.has_value());
            EXPECT_EQ(hist->price, prod->price);
            EXPECT_EQ(hist->delta, prod->delta);
            EXPECT_EQ(hist->gamma, prod->gamma);
            EXPECT_EQ(hist->theta, prod->theta);
            EXPECT_EQ(hist->charm, prod->charm);
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_EQ(compared, 6 * 2 * 2 * 2 * 2);
}

TEST(DiscreteDivProductionLattice, KillSwitchIsTheHistoricalLatticeCallForCall) {
  DiscreteDivWorkspace ws = production_workspace();
  const auto divs = quarterly(0.1, 1.0, 2.0);
  for (const Side side : {Side::Call, Side::Put}) {
    const auto hist = american_discrete_div_price(100.0, 100.0, 2.0, 0.25, 0.04, 0.0, side, divs,
                                                  301, ExerciseStyle::American,
                                                  DiscreteDivScheme::SplicedCrr, ws);
    const auto prod = american_discrete_div_production_price(
        100.0, 100.0, 2.0, 0.25, 0.04, 0.0, side, divs, ExerciseStyle::American,
        opts_of(DiscreteDivAccuracy::Standard, 301, true), ws);
    ASSERT_TRUE(hist.has_value() && prod.has_value());
    EXPECT_EQ(*hist, prod->price);
    EXPECT_EQ(prod->route.method, DiscreteDivMethod::HistoricalSpliced);
    EXPECT_TRUE(prod->route.has(DiscreteDivRouteFlag::KillSwitch));
    EXPECT_EQ(prod->route.pre_steps, 0);
    EXPECT_EQ(prod->convention.ex_dates, DiscreteDivExDateGrid::NearestStep);

    DiscreteDivIvOpts iv;
    const auto hist_iv = american_discrete_div_implied_vol(*hist * 1.01, 100.0, 100.0, 2.0, 0.04,
                                                           0.0, side, divs, 301,
                                                           DiscreteDivScheme::SplicedCrr, iv, ws);
    const auto prod_iv = american_discrete_div_production_iv(
        *hist * 1.01, 100.0, 100.0, 2.0, 0.04, 0.0, side, divs,
        opts_of(DiscreteDivAccuracy::Standard, 301, true), iv, ws);
    ASSERT_TRUE(hist_iv.has_value() && prod_iv.has_value());
    EXPECT_EQ(hist_iv->sigma, prod_iv->iv.sigma);
    EXPECT_EQ(hist_iv->accurate_lattices, prod_iv->iv.accurate_lattices);
    EXPECT_EQ(hist_iv->screen_lattices, prod_iv->iv.screen_lattices);
  }
}

TEST(DiscreteDivProductionLattice, SplicedCubicSchemeIsTheStandardRoute) {
  DiscreteDivWorkspace ws = production_workspace();
  const auto divs = quarterly(0.1, 1.0, 3.0);
  for (const Side side : {Side::Call, Side::Put}) {
    const auto alloc = american_discrete_div_price(100.0, 95.0, 3.0, 0.25, 0.04, 0.0, side, divs,
                                                   301, ExerciseStyle::American,
                                                   DiscreteDivScheme::SplicedCubic);
    const auto on_ws = american_discrete_div_price(100.0, 95.0, 3.0, 0.25, 0.04, 0.0, side, divs,
                                                   301, ExerciseStyle::American,
                                                   DiscreteDivScheme::SplicedCubic, ws);
    const auto prod = american_discrete_div_production_price(
        100.0, 95.0, 3.0, 0.25, 0.04, 0.0, side, divs, ExerciseStyle::American,
        opts_of(DiscreteDivAccuracy::Standard), ws);
    ASSERT_TRUE(alloc.has_value() && on_ws.has_value() && prod.has_value());
    EXPECT_EQ(*alloc, prod->price);
    EXPECT_EQ(*on_ws, prod->price);
    EXPECT_EQ(prod->route.method, DiscreteDivMethod::CubicCrr);
  }
  EXPECT_STREQ(to_string(DiscreteDivScheme::SplicedCubic), "spliced-cubic");
  const DiscreteDivConvention c = discrete_div_convention(DiscreteDivScheme::SplicedCubic);
  EXPECT_EQ(c.ex_dates, DiscreteDivExDateGrid::NearestStepRetimed);
}

TEST(DiscreteDivProductionLattice, OneArgumentWorkspaceRefusesAPreSteppedRoute) {
  const std::vector<CashDividend> divs{{0.5, 3.0}};
  const auto route = discrete_div_lattice_route(100.0, 1.0, divs, opts_of(DiscreteDivAccuracy::Standard));
  ASSERT_TRUE(route.has_value());
  DiscreteDivWorkspace legacy(kDiscreteDivMaxRouteSteps);
  EXPECT_EQ(legacy.max_pre_steps(), -1);
  const auto refused = american_discrete_div_route_price(*route, 100.0, 100.0, 1.0, 0.25, 0.04,
                                                         0.0, Side::Put, divs,
                                                         ExerciseStyle::American, legacy);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  DiscreteDivWorkspace ws = production_workspace();
  EXPECT_TRUE(american_discrete_div_route_price(*route, 100.0, 100.0, 1.0, 0.25, 0.04, 0.0,
                                                Side::Put, divs, ExerciseStyle::American, ws)
                  .has_value());
  // The reservation: two step rows of N + 2, three node rows of N + 2 + max_pre.
  DiscreteDivWorkspace sized(301, 64);
  EXPECT_EQ(sized.max_pre_steps(), 64);
  EXPECT_TRUE(sized.admits_production(301, 64));
  EXPECT_FALSE(sized.admits_production(301, 66));
  EXPECT_FALSE(sized.admits_production(303, 0));
  EXPECT_EQ(sized.reserved_bytes(), (2U * 303U + 3U * 367U) * sizeof(double));
}

// ── The route rule ──────────────────────────────────────────────────────────

TEST(DiscreteDivProductionRoute, EventsSetTheStepCountWithoutAGlobalRaise) {
  const auto std_opts = opts_of(DiscreteDivAccuracy::Standard);
  {
    const auto r = discrete_div_lattice_route(100.0, 1.0, {}, std_opts);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->steps, 301);
    EXPECT_EQ(r->events, 0);
    EXPECT_EQ(r->first_ex_step, -1);
    EXPECT_EQ(r->pre_steps, 2);
    EXPECT_TRUE(r->has(DiscreteDivRouteFlag::NoInLifeEvent));
  }
  {
    // Twelve quarterly events: 12 x 25 = 300 < 301, no raise.
    const auto r = discrete_div_lattice_route(100.0, 3.0, quarterly(0.1, 1.0, 3.0), std_opts);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->events, 12);
    EXPECT_EQ(r->steps, 301);
    EXPECT_FALSE(r->has(DiscreteDivRouteFlag::StepsRaisedForEvents));
  }
  {
    // Thirteen: 325.
    const auto r = discrete_div_lattice_route(100.0, 3.25, quarterly(0.1, 1.0, 3.2), std_opts);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->events, 13);
    EXPECT_EQ(r->steps, 325);
    EXPECT_TRUE(r->has(DiscreteDivRouteFlag::StepsRaisedForEvents));
    EXPECT_FALSE(r->has(DiscreteDivRouteFlag::StepsCapped));
  }
  {
    // 200 weekly events: 5000 wanted, capped at 2401.
    std::vector<CashDividend> weekly;
    for (int i = 1; i <= 200; ++i) {
      weekly.push_back({static_cast<double>(i) / 52.0, 0.05});
    }
    const auto r = discrete_div_lattice_route(100.0, 4.0, weekly, std_opts);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->events, 200);
    EXPECT_EQ(r->steps, kDiscreteDivMaxRouteSteps);
    EXPECT_TRUE(r->has(DiscreteDivRouteFlag::StepsCapped));
  }
  {
    // Refined: CubicBbsRichardson, the same-parity pair (1201, 601).
    const auto r = discrete_div_lattice_route(100.0, 3.25, quarterly(0.1, 1.0, 3.2),
                                              opts_of(DiscreteDivAccuracy::Refined));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->method, DiscreteDivMethod::CubicBbsRichardson);
    EXPECT_EQ(r->steps, 1201);
    EXPECT_EQ(r->coarse_steps, 601);
  }
}

TEST(DiscreteDivProductionRoute, EarlyExDatesGetPreStepsAndCashDominatedOnesAreFlagged) {
  const auto std_opts = opts_of(DiscreteDivAccuracy::Standard);
  const double dt = 1.0 / 301.0;
  {
    // D0 B1: 3.00 at 0.7 of the first step (step 1).
    const auto r = discrete_div_lattice_route(100.0, 1.0, std::vector<CashDividend>{{0.7 * dt, 3.0}},
                                              std_opts);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first_ex_step, 1);
    EXPECT_GT(r->pre_steps, 2);
    EXPECT_EQ(r->pre_steps % 2, 0);
    EXPECT_TRUE(r->has(DiscreteDivRouteFlag::PreStepsForEarlyExDate));
    EXPECT_FALSE(r->has(DiscreteDivRouteFlag::ExDateOnStepZero));
  }
  {
    // 0.3 of a step: spliced ON t = 0 (>= 4 pre-steps).
    const auto r = discrete_div_lattice_route(100.0, 1.0, std::vector<CashDividend>{{0.3 * dt, 3.0}},
                                              std_opts);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first_ex_step, 0);
    EXPECT_GE(r->pre_steps, 4);
    EXPECT_TRUE(r->has(DiscreteDivRouteFlag::ExDateOnStepZero));
  }
  {
    // D0 E64 (review m1): 4.10 cash on 6.375 spot, 5.9 days out.
    const std::vector<CashDividend> m1{{0.016272795852273703, 4.0999999046325684}};
    const auto r = discrete_div_lattice_route(6.375, 0.34715297818183899, m1, std_opts);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->pre_steps, kDiscreteDivMaxPreSteps);
    EXPECT_TRUE(r->has(DiscreteDivRouteFlag::GridBottomExtrapolated));
    EXPECT_GT(r->grid_bottom_depth, 0.0);
    EXPECT_NEAR(r->max_cash_ratio, 4.0999999046325684 / 6.375, 1e-15);
  }
}

TEST(DiscreteDivProductionRoute, RefusesWhatThePriceEntryRefuses) {
  const auto o = opts_of(DiscreteDivAccuracy::Standard);
  const std::vector<CashDividend> ok{{0.5, 1.0}};
  EXPECT_FALSE(discrete_div_lattice_route(0.0, 1.0, ok, o).has_value());
  EXPECT_FALSE(discrete_div_lattice_route(100.0, 0.0, ok, o).has_value());
  EXPECT_FALSE(discrete_div_lattice_route(100.0, 1.0, ok, opts_of(DiscreteDivAccuracy::Standard, 3))
                   .has_value());
  EXPECT_FALSE(discrete_div_lattice_route(100.0, 1.0, std::vector<CashDividend>{{0.5, -1.0}}, o)
                   .has_value());
  EXPECT_FALSE(discrete_div_lattice_route(
                   100.0, 1.0, std::vector<CashDividend>{{std::nan(""), 1.0}}, o)
                   .has_value());
  std::vector<CashDividend> many;
  for (int i = 1; i <= 257; ++i) {
    many.push_back({static_cast<double>(i) / 300.0, 0.01});
  }
  const auto r = discrete_div_lattice_route(100.0, 1.0, many, o);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

// ── Accuracy pins against D0's reference (matrix.csv cc89bc89) ──────────────

struct Pin {
  const char *id;
  double K, T;
  Side side;
  double ref, vega;
};

TEST(DiscreteDivProductionAccuracy, MultiEventChainsAgainstTheIndependentReference) {
  // C8 / D12: quarterly 1.00 from 0.10 at T 2 / 3 (S 100, sigma 0.25, r 0.04). The
  // historical 301 lattice misses these by +25.0 to +34.2 bp of IV.
  const Pin pins[] = {{"C8p", 100, 2, Side::Put, 13.723830866167567, 53.426364732807485},
                      {"C8c", 95, 2, Side::Call, 15.710904572133005, 51.234093699028094},
                      {"D12p", 100, 3, Side::Put, 16.411330157685374, 63.717961298950243},
                      {"D12c", 95, 3, Side::Call, 18.206733741577949, 61.350175139901708}};
  DiscreteDivWorkspace ws = production_workspace();
  for (const Pin &p : pins) {
    const auto divs = quarterly(0.1, 1.0, p.T);
    const auto hist = american_discrete_div_price(100.0, p.K, p.T, 0.25, 0.04, 0.0, p.side, divs,
                                                  301, ExerciseStyle::American,
                                                  DiscreteDivScheme::SplicedCrr, ws);
    const auto std_p = american_discrete_div_production_price(
        100.0, p.K, p.T, 0.25, 0.04, 0.0, p.side, divs, ExerciseStyle::American,
        opts_of(DiscreteDivAccuracy::Standard), ws);
    const auto ref_p = american_discrete_div_production_price(
        100.0, p.K, p.T, 0.25, 0.04, 0.0, p.side, divs, ExerciseStyle::American,
        opts_of(DiscreteDivAccuracy::Refined), ws);
    ASSERT_TRUE(hist.has_value() && std_p.has_value() && ref_p.has_value()) << p.id;
    const double hist_bp = (*hist - p.ref) / p.vega * 1e4;
    const double std_bp = (std_p->price - p.ref) / p.vega * 1e4;
    const double ref_bp = (ref_p->price - p.ref) / p.vega * 1e4;
    EXPECT_GT(hist_bp, 20.0) << p.id; // the defect, as D0 measured it
    EXPECT_LE(std::abs(std_bp), 3.0) << p.id;
    EXPECT_LE(std::abs(ref_bp), 0.05) << p.id;
  }
}

TEST(DiscreteDivProductionAccuracy, FirstStepExDateAgainstTheIndependentReference) {
  // D0 B1: the historical 301 lattice UNDERprices by 17-19 bp (a first-step splice
  // with two nodes); the pre-stepped route reads it on a full stencil.
  const double dt = 1.0 / 301.0;
  const std::vector<CashDividend> divs{{0.7 * dt, 3.0}};
  const Pin pins[] = {{"B1p", 100, 1, Side::Put, 9.6402481234589139, 37.686072665445991},
                      {"B1c", 95, 1, Side::Call, 12.502153045794337, 36.1620762144792}};
  DiscreteDivWorkspace ws = production_workspace();
  for (const Pin &p : pins) {
    const auto std_p = american_discrete_div_production_price(
        100.0, p.K, 1.0, 0.25, 0.04, 0.0, p.side, divs, ExerciseStyle::American,
        opts_of(DiscreteDivAccuracy::Standard), ws);
    const auto ref_p = american_discrete_div_production_price(
        100.0, p.K, 1.0, 0.25, 0.04, 0.0, p.side, divs, ExerciseStyle::American,
        opts_of(DiscreteDivAccuracy::Refined), ws);
    ASSERT_TRUE(std_p.has_value() && ref_p.has_value()) << p.id;
    EXPECT_LE(std::abs((std_p->price - p.ref) / p.vega * 1e4), 3.0) << p.id;
    EXPECT_LE(std::abs((ref_p->price - p.ref) / p.vega * 1e4), 0.05) << p.id;
  }
}

// ── The production inversion ────────────────────────────────────────────────

TEST(DiscreteDivProductionIv, RecoversTheRouteVolatilityWithAndWithoutASeed) {
  DiscreteDivWorkspace ws = production_workspace();
  const double dt = 1.0 / 301.0;
  const std::vector<std::vector<CashDividend>> schedules = {
      quarterly(0.1, 1.0, 1.0), {{0.7 * dt, 3.0}}, {{0.5, 5.0}}};
  for (const auto &divs : schedules) {
    for (const Side side : {Side::Call, Side::Put}) {
      for (const DiscreteDivAccuracy acc :
           {DiscreteDivAccuracy::Standard, DiscreteDivAccuracy::Refined}) {
        const double sigma = 0.27;
        const auto quote = american_discrete_div_production_price(
            100.0, 100.0, 1.0, sigma, 0.04, 0.0, side, divs, ExerciseStyle::American,
            opts_of(acc), ws);
        ASSERT_TRUE(quote.has_value());
        for (const double seed : {0.0, 0.25}) {
          DiscreteDivIvOpts iv;
          iv.seed_sigma = seed;
          const auto root = american_discrete_div_production_iv(
              quote->price, 100.0, 100.0, 1.0, 0.04, 0.0, side, divs, opts_of(acc), iv, ws);
          ASSERT_TRUE(root.has_value()) << root.error().to_string();
          // The root of the accurate route (its sigma ripple bounds the distance).
          EXPECT_NEAR(root->iv.sigma, sigma, 1.0e-5);
          EXPECT_EQ(root->route.steps, quote->route.steps);
          EXPECT_EQ(root->iv.convention.ex_dates, DiscreteDivExDateGrid::NearestStepRetimed);
          // The Screen route runs for the first-step ex-date too (pre-steps).
          EXPECT_GT(root->iv.screen_lattices, 0U);
          EXPECT_LT(root->screen.steps, root->route.steps);
        }
      }
    }
  }
}

// ── Sensitivities ───────────────────────────────────────────────────────────

TEST(DiscreteDivProductionGreeks, BundleIsConsistentWithItsOwnRoute) {
  DiscreteDivWorkspace ws = production_workspace();
  const auto divs = quarterly(0.1, 1.0, 1.0);
  for (const Side side : {Side::Call, Side::Put}) {
    std::vector<double> dpdd(divs.size(), kNaN);
    const auto g = american_discrete_div_production_greeks(
        100.0, 95.0, 1.0, 0.25, 0.04, 0.0, side, divs, ExerciseStyle::American, dpdd, 1.0 / 252.0,
        opts_of(DiscreteDivAccuracy::Standard), ws);
    ASSERT_TRUE(g.has_value());
    const auto p = american_discrete_div_production_price(100.0, 95.0, 1.0, 0.25, 0.04, 0.0, side,
                                                          divs, ExerciseStyle::American,
                                                          opts_of(DiscreteDivAccuracy::Standard),
                                                          ws);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(g->price, p->price);
    EXPECT_EQ(g->unsupported, 0U);
    // Vega is the bundle's stencil on the SAME route.
    const double h = discrete_div_sigma_bump(0.25);
    const auto up = american_discrete_div_route_price(g->route, 100.0, 95.0, 1.0, 0.25 + h, 0.04,
                                                      0.0, side, divs, ExerciseStyle::American,
                                                      ws);
    const auto dn = american_discrete_div_route_price(g->route, 100.0, 95.0, 1.0, 0.25 - h, 0.04,
                                                      0.0, side, divs, ExerciseStyle::American,
                                                      ws);
    ASSERT_TRUE(up.has_value() && dn.has_value());
    EXPECT_EQ(g->vega, (*up - *dn) / (2.0 * h));
    // Delta against a spot bump on the same route: the t = 0 stencil spacing.
    const double hs = 0.01 * 100.0;
    const auto su = american_discrete_div_route_price(g->route, 100.0 + hs, 95.0, 1.0, 0.25, 0.04,
                                                      0.0, side, divs, ExerciseStyle::American,
                                                      ws);
    const auto sd = american_discrete_div_route_price(g->route, 100.0 - hs, 95.0, 1.0, 0.25, 0.04,
                                                      0.0, side, divs, ExerciseStyle::American,
                                                      ws);
    ASSERT_TRUE(su.has_value() && sd.has_value());
    EXPECT_NEAR(g->delta, (*su - *sd) / (2.0 * hs), 2.0e-3);
    // Signs: cash lowers a call and raises a put.
    for (const double s : dpdd) {
      ASSERT_TRUE(std::isfinite(s));
      EXPECT_TRUE(side == Side::Call ? s < 0.0 : s > 0.0);
    }
    EXPECT_TRUE(side == Side::Call ? g->delta > 0.0 : g->delta < 0.0);
    EXPECT_GT(g->gamma, 0.0);
    EXPECT_GT(g->vega, 0.0);
    EXPECT_EQ(g->roll.events_crossed, 0);
    EXPECT_TRUE(std::isfinite(g->roll.roll));
  }
}

TEST(DiscreteDivProductionGreeks, ThetaNextToAnExDateIsUnsupportedAndTheRollIsAnEvent) {
  DiscreteDivWorkspace ws = production_workspace();
  const double dt = 1.0 / 301.0;
  const std::vector<CashDividend> divs{{0.7 * dt, 3.0}};
  const double horizon = 1.0 / 252.0; // > 0.7 dt: the roll crosses the ex-date
  const auto g = american_discrete_div_production_greeks(
      100.0, 100.0, 1.0, 0.25, 0.04, 0.0, Side::Put, divs, ExerciseStyle::American, {}, horizon,
      opts_of(DiscreteDivAccuracy::Standard), ws);
  ASSERT_TRUE(g.has_value());
  EXPECT_FALSE(g->supported(DiscreteDivGreek::Theta));
  EXPECT_FALSE(g->supported(DiscreteDivGreek::Charm));
  EXPECT_TRUE(std::isnan(g->theta));
  EXPECT_TRUE(std::isnan(g->charm));
  // Empty dP/dD span: not computed, so marked unsupported — never 0.
  EXPECT_FALSE(g->supported(DiscreteDivGreek::DividendAmount));
  EXPECT_TRUE(g->supported(DiscreteDivGreek::Delta));
  EXPECT_TRUE(g->supported(DiscreteDivGreek::Roll));
  EXPECT_EQ(g->roll.events_crossed, 1);
  EXPECT_EQ(g->roll.cash_crossed, 3.0);
  // The rolled option is the no-dividend contract on S - 3 with the life shortened.
  const auto rolled = american_discrete_div_production_price(
      97.0, 100.0, 1.0 - horizon, 0.25, 0.04, 0.0, Side::Put, {}, ExerciseStyle::American,
      opts_of(DiscreteDivAccuracy::Standard), ws);
  ASSERT_TRUE(rolled.has_value());
  EXPECT_EQ(g->roll.rolled_price, rolled->price);
  EXPECT_EQ(g->roll.roll, rolled->price - g->price);

  // No horizon: the roll is unsupported (NaN), not 0.
  const auto no_roll = american_discrete_div_production_greeks(
      100.0, 100.0, 1.0, 0.25, 0.04, 0.0, Side::Put, divs, ExerciseStyle::American, {}, 0.0,
      opts_of(DiscreteDivAccuracy::Standard), ws);
  ASSERT_TRUE(no_roll.has_value());
  EXPECT_FALSE(no_roll->supported(DiscreteDivGreek::Roll));
  EXPECT_TRUE(std::isnan(no_roll->roll.roll));
}

TEST(DiscreteDivProductionGreeks, DividendSlotsOutsideTheLifeAreExactlyZeroAndSizesAreChecked) {
  DiscreteDivWorkspace ws = production_workspace();
  const std::vector<CashDividend> divs{{0.5, 2.0}, {1.5, 2.0}}; // the second after expiry
  std::vector<double> slots(2, kNaN);
  const auto g = american_discrete_div_production_greeks(
      100.0, 100.0, 1.0, 0.25, 0.04, 0.0, Side::Put, divs, ExerciseStyle::American, slots,
      1.0 / 252.0, opts_of(DiscreteDivAccuracy::Standard), ws);
  ASSERT_TRUE(g.has_value());
  EXPECT_GT(slots[0], 0.0);
  EXPECT_EQ(slots[1], 0.0);
  EXPECT_TRUE(g->supported(DiscreteDivGreek::DividendAmount));
  std::vector<double> wrong(1, kNaN);
  const auto refused = american_discrete_div_production_greeks(
      100.0, 100.0, 1.0, 0.25, 0.04, 0.0, Side::Put, divs, ExerciseStyle::American, wrong,
      1.0 / 252.0, opts_of(DiscreteDivAccuracy::Standard), ws);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
}

// ── The batch ───────────────────────────────────────────────────────────────

TEST(DiscreteDivBatch, LoopsAreThePerContractEntriesBitForBit) {
  const auto c4 = quarterly(0.1, 1.0, 1.0);
  const double dt = 1.0 / 301.0;
  const std::vector<CashDividend> first{{0.7 * dt, 3.0}};
  for (const DiscreteDivAccuracy acc :
       {DiscreteDivAccuracy::Standard, DiscreteDivAccuracy::Refined}) {
    for (const bool kill : {false, true}) {
      const DiscreteDivProductionOpts o = opts_of(acc, kDiscreteDivDefaultSteps, kill);
      const std::vector<DiscreteDivBatchGroup> groups{
          {100.0, 1.0, 0.04, 0.0, c4}, {100.0, 1.0, 0.04, 0.0, first}, {50.0, 0.5, 0.03, 0.01, {}}};
      DiscreteDivBatch batch(o);
      ASSERT_EQ(batch.prepare(groups), 0U);
      std::vector<DiscreteDivBatchContract> contracts;
      for (std::uint32_t gi = 0; gi < groups.size(); ++gi) {
        for (const double m : {0.9, 1.0, 1.1}) {
          for (const Side side : {Side::Call, Side::Put}) {
            contracts.push_back({gi, groups[gi].S * m, side, 0.3, 0.0});
          }
        }
      }
      std::vector<DiscreteDivBatchResult> prices(contracts.size());
      ASSERT_TRUE(batch.price(contracts, ExerciseStyle::American, prices).has_value());
      DiscreteDivWorkspace ws = production_workspace();
      for (std::size_t i = 0; i < contracts.size(); ++i) {
        const auto &c = contracts[i];
        const auto &g = groups[c.group];
        const auto p = american_discrete_div_production_price(
            g.S, c.K, g.T, c.sigma, g.r, g.q, c.side, g.dividends, ExerciseStyle::American, o, ws);
        ASSERT_TRUE(p.has_value());
        EXPECT_EQ(prices[i].outcome, DiscreteDivBatchOutcome::Priced);
        EXPECT_EQ(prices[i].value, p->price) << i;
        contracts[i].quote = p->price * 1.02;
      }
      DiscreteDivIvOpts iv;
      std::vector<DiscreteDivBatchResult> roots(contracts.size());
      ASSERT_TRUE(batch.implied_vol(contracts, iv, roots).has_value());
      std::vector<DiscreteDivProductionGreeks> bundles(contracts.size());
      ASSERT_TRUE(
          batch.greeks(contracts, ExerciseStyle::American, 1.0 / 252.0, bundles).has_value());
      for (std::size_t i = 0; i < contracts.size(); ++i) {
        const auto &c = contracts[i];
        const auto &g = groups[c.group];
        const auto root = american_discrete_div_production_iv(
            c.quote, g.S, c.K, g.T, g.r, g.q, c.side, g.dividends, o, iv, ws);
        ASSERT_TRUE(root.has_value());
        EXPECT_EQ(roots[i].outcome, DiscreteDivBatchOutcome::Priced);
        EXPECT_EQ(roots[i].value, root->iv.sigma) << i;
        EXPECT_EQ(roots[i].accurate_lattices, root->iv.accurate_lattices) << i;
        const auto b = american_discrete_div_production_greeks(
            g.S, c.K, g.T, c.sigma, g.r, g.q, c.side, g.dividends, ExerciseStyle::American, {},
            1.0 / 252.0, o, ws);
        ASSERT_TRUE(b.has_value());
        EXPECT_EQ(bundles[i].price, b->price) << i;
        EXPECT_EQ(bundles[i].delta, b->delta) << i;
        EXPECT_EQ(bundles[i].gamma, b->gamma) << i;
        EXPECT_EQ(bundles[i].vega, b->vega) << i;
        EXPECT_EQ(bundles[i].rho, b->rho) << i;
        EXPECT_EQ(bundles[i].phi, b->phi) << i;
        EXPECT_EQ(bundles[i].unsupported, b->unsupported) << i;
        EXPECT_EQ(bundles[i].roll.roll, b->roll.roll) << i;
      }
    }
  }
}

TEST(DiscreteDivBatch, RefusalsAreTypedPerContract) {
  const std::vector<CashDividend> divs{{0.5, 1.0}};
  const std::vector<DiscreteDivBatchGroup> groups{{100.0, 1.0, 0.04, 0.0, divs},
                                                  {-1.0, 1.0, 0.04, 0.0, divs}};
  DiscreteDivBatch batch;
  EXPECT_EQ(batch.prepare(groups), 1U);
  EXPECT_TRUE(batch.group_ok(0));
  EXPECT_FALSE(batch.group_ok(1));
  EXPECT_EQ(batch.route(1).steps, 0);
  const std::vector<DiscreteDivBatchContract> contracts{
      {0, 100.0, Side::Put, 0.25, 9.0},   // a normal root
      {1, 100.0, Side::Put, 0.25, 9.0},   // refused group
      {7, 100.0, Side::Put, 0.25, 9.0},   // no such group
      {0, 150.0, Side::Put, 0.25, 50.0},  // deep ITM put at intrinsic: price-only
      {0, 100.0, Side::Put, 0.25, 500.0}, // above the maximum-volatility price
      {0, 100.0, Side::Put, 0.25, -1.0},  // not a quote
  };
  std::vector<DiscreteDivBatchResult> out(contracts.size());
  ASSERT_TRUE(batch.implied_vol(contracts, DiscreteDivIvOpts{}, out).has_value());
  EXPECT_EQ(out[0].outcome, DiscreteDivBatchOutcome::Priced);
  EXPECT_EQ(out[1].outcome, DiscreteDivBatchOutcome::RefusedGroup);
  EXPECT_EQ(out[2].outcome, DiscreteDivBatchOutcome::RefusedGroup);
  EXPECT_EQ(out[3].outcome, DiscreteDivBatchOutcome::PriceOnly);
  EXPECT_EQ(out[4].outcome, DiscreteDivBatchOutcome::RefusedRange);
  EXPECT_EQ(out[5].outcome, DiscreteDivBatchOutcome::RefusedInput);
  for (std::size_t i = 1; i < out.size(); ++i) {
    EXPECT_TRUE(std::isnan(out[i].value)) << i;
  }
  std::vector<DiscreteDivProductionGreeks> bundles(contracts.size());
  ASSERT_TRUE(batch.greeks(contracts, ExerciseStyle::American, 1.0 / 252.0, bundles).has_value());
  EXPECT_TRUE(std::isnan(bundles[1].delta));
  EXPECT_FALSE(bundles[1].supported(DiscreteDivGreek::Delta));
  EXPECT_FALSE(bundles[1].supported(DiscreteDivGreek::Roll));
  std::vector<DiscreteDivBatchResult> short_out(1);
  EXPECT_FALSE(batch.price(contracts, ExerciseStyle::American, short_out).has_value());
  EXPECT_STREQ(to_string(DiscreteDivBatchOutcome::PriceOnly), "price_only");
  EXPECT_STREQ(to_string(DiscreteDivBatchOutcome::RefusedGroup), "refused_group");
}

} // namespace
} // namespace atx::vol
