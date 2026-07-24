#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/portfolio_risk.hpp"
#include "atx/vol/projection.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_surface.hpp"

// Theoretical portfolio-RISK engine, ported from the C ats-vol
// test_portfolio_risk.c (plan / resolve / price / PnL-explain) plus the
// scenario/shock engine (ats_greeks_scenario). Oracles come from the already-
// ported projection spine (surface_eval_ex) and the Black-76 kernels.

namespace {

using atx::vol::AggMode;
using atx::vol::black76_price;
using atx::vol::CoordKind;
using atx::vol::CorrectionCache;
using atx::vol::CurveSet;
using atx::vol::EssviParams;
using atx::vol::ExpiryId;
using atx::vol::ForwardPoint;
using atx::vol::LaneStatus;
using atx::vol::LegKind;
using atx::vol::make_contract_id;
using atx::vol::MarketBinding;
using atx::vol::Parametrization;
using atx::vol::PortfolioLeg;
using atx::vol::PortfolioRiskMode;
using atx::vol::PricingPlan;
using atx::vol::RoutePolicy;
using atx::vol::Shock;
using atx::vol::ShockKind;
using atx::vol::Side;
using atx::vol::TheoreticalLeg;
using atx::vol::Uid;
using atx::vol::Underlying;
using atx::vol::UnderlyingMarket;
using atx::vol::Universe;
using atx::vol::VolSurface;

constexpr std::array<double, 4> kTs{0.10, 0.25, 0.50, 1.00};
constexpr std::int64_t kNowNs = 1'700'000'000LL * 1'000'000'000LL;

// ── 4-slice fixture for the theoretical-leg path (bound via set_market) ──

[[nodiscard]] CurveSet make_cs4() {
  CurveSet cs;
  cs.spot = 100.0;
  const std::array<double, 11> t{1.0 / 365.25, 7.0 / 365.25, 14.0 / 365.25,
                                 1.0 / 12.0,    2.0 / 12.0,   3.0 / 12.0,
                                 6.0 / 12.0,    9.0 / 12.0,   1.0,
                                 1.5,           2.0};
  const std::array<double, 11> r{0.0405, 0.0410, 0.0415, 0.0420, 0.0425, 0.0430,
                                 0.0440, 0.0450, 0.0455, 0.0460, 0.0465};
  (void)cs.set_yield(t, r);
  std::array<ForwardPoint, 4> pts{};
  for (std::size_t i = 0; i < kTs.size(); ++i) {
    pts[i].T = kTs[i];
    pts[i].F = 100.0 * std::exp(0.04 * kTs[i]);
    pts[i].q_eff = 0.0;
  }
  cs.forward.set(pts);
  return cs;
}

[[nodiscard]] VolSurface make_surf4() {
  VolSurface surf = VolSurface::create(1u, Parametrization::Essvi, 4).value();
  for (std::uint16_t i = 0; i < 4; ++i) {
    EssviParams sl{};
    sl.theta = 0.04 + 0.02 * static_cast<double>(i);
    sl.phi = 1.0;
    sl.rho = -0.2;
    sl.T = kTs[i];
    sl.F = 100.0 * std::exp(0.04 * kTs[i]);
    sl.expiry_id = i;
    (void)surf.set_slice_essvi(i, sl);
  }
  return surf;
}

[[nodiscard]] TheoreticalLeg theo_strike(std::uint32_t input_ix, Uid uid,
                                         Side side, double T, double K,
                                         double qty) {
  TheoreticalLeg leg;
  leg.input_ix = input_ix;
  leg.uid = uid;
  leg.side = side;
  leg.T_clock = T;
  leg.x = K;
  leg.coord_kind = CoordKind::Strike;
  leg.qty = qty;
  leg.multiplier = 100.0;
  return leg;
}

// ── Universe fixture for the scenario/shock path ─────────────────────────

[[nodiscard]] std::int64_t expiry_ns_for(double T) {
  return kNowNs + static_cast<std::int64_t>(T * 365.25 * 86400.0 * 1e9);
}

[[nodiscard]] Uid add_underlying(Universe& u, std::string_view ticker,
                                 double spot, std::span<const double> strikes,
                                 double T, ExpiryId& eid_out) {
  const Uid uid = u.intern_ticker(ticker).value();
  eid_out = u.add_expiry(uid, expiry_ns_for(T)).value();
  for (double k : strikes) {
    (void)u.add_strike(uid, eid_out, k).value();
  }
  Underlying* under = u.get_underlying(uid).value();
  under->spot = spot;
  under->chains[eid_out].T = T;
  return uid;
}

[[nodiscard]] CurveSet make_curves(double spot, double r, double T) {
  CurveSet cs;
  cs.spot = spot;
  const std::array<double, 3> tp{0.05, 1.0, 3.0};
  const std::array<double, 3> rr{r, r, r};
  (void)cs.set_yield(tp, rr);
  ForwardPoint fp{};
  fp.expiry_ns = expiry_ns_for(T);
  fp.T = T;
  fp.F = spot * std::exp(r * T);
  const std::array<ForwardPoint, 1> fps{fp};
  cs.forward.set(fps);
  return cs;
}

[[nodiscard]] VolSurface make_surface1(Uid uid, double T, double F,
                                       ExpiryId eid) {
  VolSurface surf = VolSurface::create(uid, Parametrization::Essvi, 1).value();
  EssviParams sl{};
  sl.theta = 0.20 * 0.20 * T;
  sl.phi = 0.45;
  sl.rho = -0.20;
  sl.T = T;
  sl.F = F;
  sl.expiry_id = eid;
  (void)surf.set_slice_essvi(0, sl);
  return surf;
}

}  // namespace

// ── Plan grouping ────────────────────────────────────────────────────────

TEST(PortfolioRisk, PlanCreate_Empty_IsOk) {
  auto plan = PricingPlan::create({});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->n_theoretical_legs(), 0u);
  EXPECT_EQ(plan->n_theoretical_groups(), 0u);
}

TEST(PortfolioRisk, PlanCreate_TwoLegsSameT_CollapseToOneGroup) {
  const std::array<TheoreticalLeg, 2> legs{
      theo_strike(0, 1u, Side::Call, 0.30, 105.0, 1.0),
      theo_strike(1, 1u, Side::Call, 0.30, 110.0, 1.0)};
  auto plan = PricingPlan::create(legs);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->n_theoretical_legs(), 2u);
  EXPECT_EQ(plan->n_theoretical_groups(), 1u);
}

TEST(PortfolioRisk, PlanCreate_DistinctT_SplitIntoDistinctGroups) {
  const std::array<TheoreticalLeg, 3> legs{
      theo_strike(0, 1u, Side::Call, 0.25, 100.0, 1.0),
      theo_strike(1, 1u, Side::Call, 0.50, 100.0, 1.0),
      theo_strike(2, 1u, Side::Call, 0.75, 100.0, 1.0)};
  auto plan = PricingPlan::create(legs);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->n_theoretical_groups(), 3u);
}

// ── Resolver pipeline ────────────────────────────────────────────────────

TEST(PortfolioRisk, Bind_ThreeUniqueT_BuildsOneInsertedSliceCtxPerT) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();

  // 3x3 (T, K) grid: 3 unique T -> 3 inserted-slice ctxs, 3 groups, 9 legs.
  std::array<TheoreticalLeg, 9> legs{};
  const std::array<double, 3> ts{0.30, 0.40, 0.60};
  const std::array<double, 3> ks{95.0, 100.0, 110.0};
  std::uint32_t k = 0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      legs[k] = theo_strike(k, 1u, Side::Call, ts[static_cast<std::size_t>(i)],
                            ks[static_cast<std::size_t>(j)], 1.0);
      ++k;
    }
  }
  auto plan = PricingPlan::create(legs);
  ASSERT_TRUE(plan.has_value());

  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  ASSERT_TRUE(plan->bind_market(b, atx::vol::time_model_clock()).has_value());

  EXPECT_EQ(plan->n_inserted_slice_ctxs(), 3u);
  EXPECT_EQ(plan->n_theoretical_groups(), 3u);
  EXPECT_EQ(plan->n_theoretical_legs(), 9u);
}

TEST(PortfolioRisk, RouteB76AlCache_NoCorrection_FallsBackToB76) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  TheoreticalLeg leg = theo_strike(0, 1u, Side::Call, 0.50, 105.0, 1.0);
  leg.route_policy = RoutePolicy::B76AlCache;

  auto plan = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});  // no corr
  ASSERT_TRUE(plan->bind_market(b, atx::vol::time_model_clock()).has_value());
  auto res = plan->price(PortfolioRiskMode::PriceOnly, AggMode::Total);
  ASSERT_TRUE(res.has_value());

  EXPECT_EQ(res->legs[0].route, RoutePolicy::B76Only);
  EXPECT_NE(res->legs[0].resolver_flags & atx::vol::kResolverRouteFallbackB76,
            0u);
  EXPECT_NE(res->legs[0].resolver_flags & atx::vol::kFlagRouteB76Only, 0u);
}

TEST(PortfolioRisk, RouteAlCorrection_ReturnsDeferred) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  TheoreticalLeg leg = theo_strike(0, 1u, Side::Call, 0.30, 100.0, 1.0);
  leg.route_policy = RoutePolicy::AlCorrection;

  auto plan = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  ASSERT_TRUE(plan->bind_market(b, atx::vol::time_model_clock()).has_value());
  auto res = plan->price(PortfolioRiskMode::PriceOnly, AggMode::Total);
  ASSERT_TRUE(res.has_value());

  EXPECT_NE(res->legs[0].resolver_flags & atx::vol::kResolverAmericanDeferred,
            0u);
  EXPECT_EQ(res->legs[0].status, LaneStatus::ModelUnavailable);
}

// ── Theoretical pricing matches the Stage I scalar evaluator ─────────────

TEST(PortfolioRisk, StrikePrice_NonPillarT_MatchesStage1EvalEx) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  const double T = 0.30;
  const double K = 105.0;
  TheoreticalLeg leg = theo_strike(0, 1u, Side::Call, T, K, 1.0);

  auto plan = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  const auto tm = atx::vol::time_model_clock();
  ASSERT_TRUE(plan->bind_market(b, tm).has_value());
  auto res = plan->price(PortfolioRiskMode::PriceOnly, AggMode::Total);
  ASSERT_TRUE(res.has_value());

  atx::vol::EvalRequest req = atx::vol::eval_request_default();
  req.T_clock = T;
  req.x = K;
  req.coord_kind = CoordKind::Strike;
  req.side = Side::Call;
  auto ref = atx::vol::surface_eval_ex(sf, cs, nullptr, tm, req);
  ASSERT_TRUE(ref.has_value());

  EXPECT_NEAR(res->legs[0].iv, ref->iv, 1e-12);
  EXPECT_NEAR(res->legs[0].price, ref->price, 1e-9);
}

TEST(PortfolioRisk, LogMoneynessGrid_ReusesInsertedSliceCtx_MatchesEvalEx) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  constexpr int kN = 5;
  std::array<TheoreticalLeg, kN> legs{};
  for (int i = 0; i < kN; ++i) {
    TheoreticalLeg leg =
        theo_strike(static_cast<std::uint32_t>(i), 1u, Side::Call, 0.40, 100.0,
                    1.0);
    leg.coord_kind = CoordKind::LogMoneyness;
    leg.x = -0.20 + 0.10 * static_cast<double>(i);
    legs[static_cast<std::size_t>(i)] = leg;
  }
  auto plan = PricingPlan::create(legs);
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  const auto tm = atx::vol::time_model_clock();
  ASSERT_TRUE(plan->bind_market(b, tm).has_value());
  EXPECT_EQ(plan->n_theoretical_groups(), 1u);
  EXPECT_EQ(plan->n_inserted_slice_ctxs(), 1u);

  auto res = plan->price(PortfolioRiskMode::PriceOnly, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  for (int i = 0; i < kN; ++i) {
    atx::vol::EvalRequest req = atx::vol::eval_request_default();
    req.T_clock = 0.40;
    req.coord_kind = CoordKind::LogMoneyness;
    req.x = legs[static_cast<std::size_t>(i)].x;
    req.side = Side::Call;
    auto ref = atx::vol::surface_eval_ex(sf, cs, nullptr, tm, req);
    ASSERT_TRUE(ref.has_value());
    EXPECT_NEAR(res->legs[static_cast<std::size_t>(i)].iv, ref->iv, 1e-12);
    EXPECT_NEAR(res->legs[static_cast<std::size_t>(i)].price, ref->price, 1e-9);
  }
}

// ── DELTA coordinate ─────────────────────────────────────────────────────

TEST(PortfolioRisk, DeltaLeg_Call_RoundTripsQuoteDelta) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  TheoreticalLeg leg = theo_strike(0, 1u, Side::Call, 0.50, 0.25, 1.0);
  leg.coord_kind = CoordKind::Delta;  // 0.25-delta call

  auto plan = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  ASSERT_TRUE(plan->bind_market(b, atx::vol::time_model_clock()).has_value());
  auto res = plan->price(PortfolioRiskMode::PriceOnly, AggMode::Total);
  ASSERT_TRUE(res.has_value());

  EXPECT_NEAR(res->legs[0].quote_delta, 0.25, 1e-7);
  EXPECT_EQ(res->legs[0].status, LaneStatus::Ok);
}

TEST(PortfolioRisk, DeltaLeg_Unbracketed_MarksInvalid) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  TheoreticalLeg leg = theo_strike(0, 1u, Side::Put, 0.50, 0.5, 1.0);
  leg.coord_kind = CoordKind::Delta;  // +0.5 for a put is impossible

  auto plan = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  ASSERT_TRUE(plan->bind_market(b, atx::vol::time_model_clock()).has_value());
  auto res = plan->price(PortfolioRiskMode::PriceOnly, AggMode::Total);
  ASSERT_TRUE(res.has_value());

  EXPECT_NE(res->legs[0].resolver_flags & atx::vol::kFlagDeltaNotBracketed, 0u);
  EXPECT_EQ(res->legs[0].status, LaneStatus::ModelUnavailable);
}

// ── First-order Greeks ───────────────────────────────────────────────────

TEST(PortfolioRisk, FirstOrderGreeks_AtmLeg_AreFiniteAndSensible) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  TheoreticalLeg leg = theo_strike(0, 1u, Side::Call, 0.50, 100.0, 1.0);

  auto plan = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  ASSERT_TRUE(plan->bind_market(b, atx::vol::time_model_clock()).has_value());
  auto res = plan->price(PortfolioRiskMode::FirstOrder, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& lv = res->legs[0];

  EXPECT_EQ(lv.status, LaneStatus::Ok);
  EXPECT_TRUE(std::isfinite(lv.delta));
  EXPECT_TRUE(std::isfinite(lv.vega));
  EXPECT_TRUE(std::isfinite(lv.theta));
  EXPECT_TRUE(std::isfinite(lv.rho));
  EXPECT_GT(lv.delta, 0.3);
  EXPECT_LT(lv.delta, 0.8);
  EXPECT_GT(lv.vega, 0.0);
}

// ── Risk aggregation vs qty-weighted per-leg risk ────────────────────────

TEST(PortfolioRisk, Aggregate_Total_EqualsQtyWeightedPerLegRisk) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  std::array<TheoreticalLeg, 4> legs{
      theo_strike(0, 1u, Side::Call, 0.40, 95.0, 2.0),
      theo_strike(1, 1u, Side::Put, 0.40, 100.0, -1.0),
      theo_strike(2, 1u, Side::Call, 0.40, 105.0, 3.0),
      theo_strike(3, 1u, Side::Call, 0.40, 110.0, 1.0)};

  auto plan = PricingPlan::create(legs);
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  ASSERT_TRUE(plan->bind_market(b, atx::vol::time_model_clock()).has_value());
  auto res = plan->price(PortfolioRiskMode::FirstOrder, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->aggregates.size(), 1u);

  double value = 0.0;
  double delta = 0.0;
  double vega = 0.0;
  for (std::size_t i = 0; i < legs.size(); ++i) {
    const double qxm = legs[i].qty * legs[i].multiplier;
    value += qxm * res->legs[i].price;
    delta += qxm * res->legs[i].delta;
    vega += qxm * res->legs[i].vega;
  }
  EXPECT_NEAR(res->aggregates[0].value, value, 1e-9);
  EXPECT_NEAR(res->aggregates[0].delta, delta, 1e-9);
  EXPECT_NEAR(res->aggregates[0].vega, vega, 1e-9);
  EXPECT_EQ(res->aggregates[0].n_legs, 4u);
}

// ── project_compare PnL identity ─────────────────────────────────────────

TEST(PortfolioRisk, ProjectCompare_SameBinding_AllComponentsZero) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  std::array<TheoreticalLeg, 3> legs{
      theo_strike(0, 1u, Side::Call, 0.30, 95.0, 1.0),
      theo_strike(1, 1u, Side::Call, 0.30, 100.0, 1.0),
      theo_strike(2, 1u, Side::Call, 0.50, 105.0, 1.0)};
  auto plan = PricingPlan::create(legs);
  ASSERT_TRUE(plan.has_value());
  MarketBinding b;
  b.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});

  auto out = plan->project_compare(b, b, atx::vol::time_model_clock());
  ASSERT_TRUE(out.has_value());
  for (const auto& row : out->explain) {
    EXPECT_NEAR(row.d_from_forward, 0.0, 1e-10);
    EXPECT_NEAR(row.d_from_vol, 0.0, 1e-10);
    EXPECT_NEAR(row.d_from_route, 0.0, 1e-10);
    EXPECT_NEAR(row.d_from_interp, 0.0, 1e-10);
    EXPECT_NEAR(row.d_price, 0.0, 1e-10);
  }
}

TEST(PortfolioRisk, ProjectCompare_Components_SumToTargetMinusSource) {
  CurveSet cs1 = make_cs4();
  VolSurface sf1 = make_surf4();
  CurveSet cs2 = make_cs4();
  VolSurface sf2 = make_surf4();

  // Bump target: +10% variance level, +50 bps forward.
  for (std::size_t i = 0; i < sf2.n_slices(); ++i) {
    EssviParams sl = sf2.essvi_slices()[i];
    sl.theta *= 1.10;
    (void)sf2.set_slice_essvi(i, sl);
  }
  for (ForwardPoint& p : cs2.forward.points()) {
    p.F *= 1.005;
  }

  std::array<TheoreticalLeg, 2> legs{
      theo_strike(0, 1u, Side::Call, 0.30, 100.0, 1.0),
      theo_strike(1, 1u, Side::Call, 0.50, 105.0, 1.0)};
  auto plan = PricingPlan::create(legs);
  ASSERT_TRUE(plan.has_value());
  MarketBinding bs;
  bs.set_market(1u, UnderlyingMarket{&sf1, &cs1, nullptr, nullptr});
  MarketBinding bt;
  bt.set_market(1u, UnderlyingMarket{&sf2, &cs2, nullptr, nullptr});

  auto out = plan->project_compare(bs, bt, atx::vol::time_model_clock());
  ASSERT_TRUE(out.has_value());
  for (std::size_t i = 0; i < legs.size(); ++i) {
    const auto& row = out->explain[i];
    const double sum = row.d_from_forward + row.d_from_vol + row.d_from_route +
                       row.d_from_interp;
    const double diff = out->target.legs[i].price - out->source.legs[i].price;
    EXPECT_NEAR(sum, diff, 1e-10);
  }
}

TEST(PortfolioRisk, ProjectCompare_VolAttribution_SumsToDpriceFromVol) {
  CurveSet cs1 = make_cs4();
  VolSurface sf1 = make_surf4();
  CurveSet cs2 = make_cs4();
  VolSurface sf2 = make_surf4();

  // Move level (theta), skew (rho), curvature (phi) all at once on target.
  for (std::size_t i = 0; i < sf2.n_slices(); ++i) {
    EssviParams sl = sf2.essvi_slices()[i];
    sl.theta *= 1.20;
    sl.rho -= 0.10;
    sl.phi *= 1.15;
    (void)sf2.set_slice_essvi(i, sl);
  }

  constexpr int kN = 3;
  std::array<TheoreticalLeg, kN> legs{};
  for (int i = 0; i < kN; ++i) {
    TheoreticalLeg leg =
        theo_strike(static_cast<std::uint32_t>(i), 1u, Side::Call, 0.40, 100.0,
                    1.0);
    leg.coord_kind = CoordKind::LogMoneyness;
    leg.x = -0.10 + 0.10 * static_cast<double>(i);
    legs[static_cast<std::size_t>(i)] = leg;
  }
  auto plan = PricingPlan::create(legs);
  ASSERT_TRUE(plan.has_value());
  MarketBinding bs;
  bs.set_market(1u, UnderlyingMarket{&sf1, &cs1, nullptr, nullptr});
  MarketBinding bt;
  bt.set_market(1u, UnderlyingMarket{&sf2, &cs2, nullptr, nullptr});

  auto out = plan->project_compare(bs, bt, atx::vol::time_model_clock());
  ASSERT_TRUE(out.has_value());
  for (int i = 0; i < kN; ++i) {
    const auto& row = out->explain[static_cast<std::size_t>(i)];
    const double sum_attr = row.d_from_vol_level + row.d_from_vol_skew +
                            row.d_from_vol_curvature + row.d_from_vol_higher;
    EXPECT_NEAR(sum_attr, row.d_from_vol, 1e-10);
  }
}

TEST(PortfolioRisk, ProjectCompare_RouteSwap_DpriceFromRouteIsAmericanPremium) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  // Small populated put correction cache over a box covering the leg.
  auto corr = CorrectionCache::build(8u, 6u, 4u, 0.04, 0.0, -0.4, 0.4, 0.05,
                                     1.0, 0.10, 0.50, Side::Put);
  ASSERT_TRUE(corr.has_value());

  TheoreticalLeg leg = theo_strike(0, 1u, Side::Put, 0.50, 95.0, 1.0);
  leg.route_policy = RoutePolicy::B76AlCache;
  auto plan = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan.has_value());

  // Source: no correction (falls back to B76). Target: put cache present.
  MarketBinding bs;
  bs.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  MarketBinding bt;
  bt.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, &corr.value()});

  auto out = plan->project_compare(bs, bt, atx::vol::time_model_clock());
  ASSERT_TRUE(out.has_value());

  EXPECT_EQ(out->source.legs[0].route, RoutePolicy::B76Only);
  EXPECT_EQ(out->target.legs[0].route, RoutePolicy::B76AlCache);
  // Forward and IV unchanged -> dF and d_vol ~ 0; d_route == American premium.
  EXPECT_NEAR(out->explain[0].d_from_forward, 0.0, 1e-10);
  EXPECT_NEAR(out->explain[0].d_from_vol, 0.0, 1e-10);
  EXPECT_GT(out->explain[0].d_from_route, 1e-4);
  EXPECT_NEAR(out->explain[0].d_from_interp, 0.0, 1e-10);

  const double sum = out->explain[0].d_from_forward + out->explain[0].d_from_vol +
                     out->explain[0].d_from_route + out->explain[0].d_from_interp;
  EXPECT_NEAR(sum, out->target.legs[0].price - out->source.legs[0].price, 1e-10);
}

// ── Scenario / shock engine ──────────────────────────────────────────────

TEST(PortfolioRisk, Scenario_SpotAndVolShock_MatchesDirectReprice) {
  Universe u;
  const double T = 0.5;
  const double r = 0.04;
  const std::array<double, 5> strikes{85.0, 95.0, 100.0, 105.0, 115.0};
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  CurveSet cs = make_curves(100.0, r, T);
  const double F = cs.forward.forward_at(eid);
  VolSurface sf = make_surface1(uid, T, F, eid);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&sf, &cs, nullptr, nullptr});

  struct Pos {
    std::uint16_t sx;
    Side side;
    double qty;
  };
  const std::array<Pos, 4> pos{Pos{0, Side::Call, 2.0}, Pos{2, Side::Put, -1.0},
                               Pos{3, Side::Call, 1.5}, Pos{4, Side::Put, 3.0}};
  std::vector<PortfolioLeg> book;
  for (const Pos& p : pos) {
    book.push_back(PortfolioLeg{LegKind::Option, uid,
                                make_contract_id(uid, eid, p.sx, p.side), p.qty,
                                100.0, 0.0, 0u});
  }

  const std::array<Shock, 2> shocks{Shock{ShockKind::SpotPct, 0.02},
                                    Shock{ShockKind::VolRel, 0.10}};
  auto pnl = atx::vol::scenario_pnl(book, b, shocks);
  ASSERT_TRUE(pnl.has_value());

  // Direct reprice at the shocked market (same arithmetic as the engine).
  const double rr = cs.yield.zero(T);
  const double df = std::exp(-rr * T);
  const double S = 100.0;
  const double q = rr - std::log(F / S) / T;
  double expected = 0.0;
  for (const Pos& p : pos) {
    const double K = strikes[p.sx];
    const double sigma = sf.iv(std::log(K / F), T);
    const double base = black76_price(F, K, T, sigma, df, p.side);
    const double s_s = S * 1.02;
    const double sig_s = sigma * 1.10;
    const double f_s = s_s * std::exp((rr - q) * T);
    const double p_s = black76_price(f_s, K, T, sig_s, df, p.side);
    expected += p.qty * (p_s - base);
  }
  EXPECT_NEAR(pnl.value(), expected, 1e-8);
}

TEST(PortfolioRisk, Scenario_NoShocks_IsZeroPnl) {
  Universe u;
  const double T = 0.5;
  const std::array<double, 3> strikes{95.0, 100.0, 105.0};
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  CurveSet cs = make_curves(100.0, 0.04, T);
  VolSurface sf = make_surface1(uid, T, cs.forward.forward_at(eid), eid);
  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&sf, &cs, nullptr, nullptr});

  std::vector<PortfolioLeg> book{
      PortfolioLeg{LegKind::Option, uid, make_contract_id(uid, eid, 1, Side::Call),
                   1.0, 100.0, 0.0, 0u}};
  auto pnl = atx::vol::scenario_pnl(book, b, {});
  ASSERT_TRUE(pnl.has_value());
  EXPECT_NEAR(pnl.value(), 0.0, 1e-12);
}

TEST(PortfolioRisk, Scenario_SurfaceTwist_ReturnsNotImplemented) {
  Universe u;
  const std::array<double, 1> strikes{100.0};
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, 0.5, eid);
  CurveSet cs = make_curves(100.0, 0.04, 0.5);
  VolSurface sf = make_surface1(uid, 0.5, cs.forward.forward_at(eid), eid);
  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&sf, &cs, nullptr, nullptr});

  std::vector<PortfolioLeg> book{
      PortfolioLeg{LegKind::Option, uid, make_contract_id(uid, eid, 0, Side::Call),
                   1.0, 100.0, 0.0, 0u}};
  const std::array<Shock, 1> shocks{Shock{ShockKind::SurfaceTwist, 0.01}};
  auto pnl = atx::vol::scenario_pnl(book, b, shocks);
  ASSERT_FALSE(pnl.has_value());
  EXPECT_EQ(pnl.error().code(), atx::vol::ErrorCode::NotImplemented);
}

// ── G3 (GR-P1-1): the B76AlCache theoretical plan reported the American price with
// EUROPEAN first-order greeks — a hedger off it under-hedges by the full EEP delta.
// The fix corrects delta/vega/theta/rho with the same correction jet price_option
// uses, so they must now DIFFER from the bare-European (no-cache) greeks. ──
TEST(PortfolioRisk, G3_PriceGroup_B76AlCache_GreeksCarryEEPCorrection) {
  CurveSet cs = make_cs4();
  VolSurface sf = make_surf4();
  // Populated put correction cache over a box covering the leg (proven non-trivial
  // by ProjectCompare_RouteSwap: F*c > 1e-4 at K=95, T=0.50).
  auto corr = CorrectionCache::build(8u, 6u, 4u, 0.04, 0.0, -0.4, 0.4, 0.05, 1.0,
                                     0.10, 0.50, Side::Put);
  ASSERT_TRUE(corr.has_value());

  TheoreticalLeg leg = theo_strike(0, 1u, Side::Put, 0.50, 95.0, 1.0);
  leg.route_policy = RoutePolicy::B76AlCache;

  // Cache bound -> B76AlCache route active (American mark + correction jet).
  auto plan_c = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan_c.has_value());
  MarketBinding bc;
  bc.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, &corr.value()});
  ASSERT_TRUE(plan_c->bind_market(bc, atx::vol::time_model_clock()).has_value());
  const auto rc = plan_c->price(PortfolioRiskMode::FirstOrder, AggMode::Total);
  ASSERT_TRUE(rc.has_value());
  const auto& lc = rc->legs[0];
  ASSERT_EQ(lc.route, RoutePolicy::B76AlCache);
  ASSERT_EQ(lc.status, LaneStatus::Ok);

  // No cache -> the SAME route policy falls back to B76Only (pure European greeks).
  auto plan_e = PricingPlan::create(std::span<const TheoreticalLeg>(&leg, 1));
  ASSERT_TRUE(plan_e.has_value());
  MarketBinding be;
  be.set_market(1u, UnderlyingMarket{&sf, &cs, nullptr, nullptr});
  ASSERT_TRUE(plan_e->bind_market(be, atx::vol::time_model_clock()).has_value());
  const auto re = plan_e->price(PortfolioRiskMode::FirstOrder, AggMode::Total);
  ASSERT_TRUE(re.has_value());
  const auto& le = re->legs[0];
  ASSERT_EQ(le.route, RoutePolicy::B76Only);

  // Same resolved (F, K, T, IV) in both, so the only difference is the correction
  // jet. Pre-fix the greeks were bit-identical to the European leg (they ignored the
  // correction) while only the price differed; the fix makes them American.
  EXPECT_DOUBLE_EQ(lc.iv, le.iv);
  EXPECT_NE(lc.delta, le.delta);
  EXPECT_NE(lc.vega, le.vega);
  EXPECT_NE(lc.theta, le.theta);
  EXPECT_NE(lc.rho, le.rho);
  // gamma stays the Black-76 leg on both paths (sibling price_option corrects only
  // the four first-order axes) and the American mark exceeds the European (EEP >= 0).
  EXPECT_DOUBLE_EQ(lc.gamma, le.gamma);
  EXPECT_GT(lc.price, le.price);
}

// ── G3 (GR-P1-2): the scenario engine priced base + shocked legs in pure Black-76
// even when a per-side correction cache was bound, so an American book's scenario
// PnL silently excluded the EEP change. The fix applies the SAME overlay to both
// legs; the scenario PnL must now match an overlay-inclusive reprice, and move off
// the European value. ──
TEST(PortfolioRisk, G3_Scenario_IncludesEEPOverlayWhenCacheBound) {
  Universe u;
  const double T = 0.5;
  const double r = 0.04;
  const std::array<double, 3> strikes{95.0, 100.0, 105.0};
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  CurveSet cs = make_curves(100.0, r, T);
  const double F = cs.forward.forward_at(eid);
  VolSurface sf = make_surface1(uid, T, F, eid);
  auto corr = CorrectionCache::build(8u, 6u, 4u, 0.04, 0.0, -0.4, 0.4, 0.05, 1.0,
                                     0.10, 0.50, Side::Put);
  ASSERT_TRUE(corr.has_value());

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&sf, &cs, nullptr, &corr.value()}); // put cache

  const std::array<double, 2> qtys{2.0, -1.0};
  std::vector<PortfolioLeg> book;
  for (std::uint16_t sx = 0; sx < 2; ++sx) {
    book.push_back(PortfolioLeg{LegKind::Option, uid,
                                make_contract_id(uid, eid, sx, Side::Put),
                                qtys[sx], 100.0, 0.0, 0u});
  }

  const std::array<Shock, 1> shocks{Shock{ShockKind::SpotPct, 0.02}};
  const auto pnl = atx::vol::scenario_pnl(book, b, shocks);
  ASSERT_TRUE(pnl.has_value());

  // Independent reprice: engine arithmetic + the EEP overlay on both legs.
  const double rr = cs.yield.zero(T);
  const double df = std::exp(-rr * T);
  const double S = 100.0;
  const double q = rr - std::log(F / S) / T;
  double expected = 0.0;
  double expected_euro = 0.0;
  for (std::uint16_t sx = 0; sx < 2; ++sx) {
    const double K = strikes[sx];
    const double sigma = sf.iv(std::log(K / F), T);
    const double base_e = black76_price(F, K, T, sigma, df, Side::Put);
    const double base = base_e + F * corr->eval(std::log(K / F), T, sigma);
    const double s_s = S * 1.02;
    const double f_s = s_s * std::exp((rr - q) * T);
    const double p_s_e = black76_price(f_s, K, T, sigma, df, Side::Put);
    const double p_s = p_s_e + f_s * corr->eval(std::log(K / f_s), T, sigma);
    expected += qtys[sx] * (p_s - base);
    expected_euro += qtys[sx] * (p_s_e - base_e);
  }
  EXPECT_NEAR(pnl.value(), expected, 1e-6);
  // Non-vacuous: the overlay actually moved the scenario PnL off the European value.
  EXPECT_GT(std::fabs(expected - expected_euro), 1e-6);
  EXPECT_GT(std::fabs(pnl.value() - expected_euro), 1e-6);
}
