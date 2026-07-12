#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/adjusted_greeks.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/portfolio.hpp"

// Portfolio pricing engine + portfolio-Greeks aggregation, ported from the C
// ats-vol tests test_portfolio_pricing.c, test_portfolio_unification.c, and
// test_greeks_portfolio.c. The oracle prices come straight from the already-
// ported Black-76 kernels evaluated at the surface IV.

namespace {

using atx::vol::AggMode;
using atx::vol::black76_greeks;
using atx::vol::black76_price;
using atx::vol::cid_side;
using atx::vol::cid_strike_idx;
using atx::vol::CurveSet;
using atx::vol::EssviParams;
using atx::vol::essvi_natural_to_reparam;
using atx::vol::ExpiryId;
using atx::vol::ForwardPoint;
using atx::vol::GreeksAggregate;
using atx::vol::kInvalidUid;
using atx::vol::LaneStatus;
using atx::vol::LegKind;
using atx::vol::make_contract_id;
using atx::vol::MarketBinding;
using atx::vol::Parametrization;
using atx::vol::PortfolioLeg;
using atx::vol::PortfolioRiskMode;
using atx::vol::price_portfolio;
using atx::vol::Side;
using atx::vol::StickyParams;
using atx::vol::Uid;
using atx::vol::Underlying;
using atx::vol::UnderlyingMarket;
using atx::vol::Universe;
using atx::vol::VolSurface;

constexpr std::int64_t kNowNs = 1'700'000'000LL * 1'000'000'000LL;

[[nodiscard]] std::int64_t expiry_ns_for(double T) {
  return kNowNs + static_cast<std::int64_t>(T * 365.25 * 86400.0 * 1e9);
}

// Register an underlying + expiry + strikes into `u`; set spot and chain T.
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

[[nodiscard]] VolSurface make_surface(Uid uid, double T, double F, ExpiryId eid,
                                      double theta, double phi, double rho) {
  auto res = VolSurface::create(uid, Parametrization::Essvi, 2);
  VolSurface surf = std::move(res).value();
  EssviParams sl{};
  sl.theta = theta;
  sl.phi = phi;
  sl.rho = rho;
  sl.T = T;
  sl.F = F;
  sl.expiry_id = eid;
  sl.expiry_ns = expiry_ns_for(T);
  const auto cube = essvi_natural_to_reparam(theta, phi, rho, T);
  sl.psi = cube.psi;
  sl.p = cube.p;
  sl.lambda = cube.lambda;
  (void)surf.set_slice_essvi(0, sl);
  return surf;
}

constexpr std::array<double, 5> kRelStrikes{0.85, 0.95, 1.0, 1.05, 1.15};

[[nodiscard]] std::array<double, 5> scaled_strikes(double spot) {
  std::array<double, 5> out{};
  for (std::size_t i = 0; i < kRelStrikes.size(); ++i) {
    out[i] = kRelStrikes[i] * spot;
  }
  return out;
}

[[nodiscard]] double leg_option_price(const VolSurface& surf, double F, double K,
                                      double T, double r, Side side) {
  const double sigma = surf.iv(std::log(K / F), T);
  return black76_price(F, K, T, sigma, std::exp(-r * T), side);
}

}  // namespace

// ── Price-only per-lane parity vs scalar Black-76 ────────────────────────

TEST(Portfolio, PriceOnlyMatchesScalarB76PerLane) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  const auto strikes = scaled_strikes(100.0);
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  const double F = 100.0 * std::exp(r * T);
  CurveSet cs = make_curves(100.0, r, T);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  UnderlyingMarket m{};
  m.surface = &surf;
  m.curves = &cs;
  b.set_market(uid, m);

  std::vector<PortfolioLeg> book;
  for (std::uint16_t k = 0; k < 5; ++k) {
    for (int s = 0; s < 2; ++s) {
      book.push_back(PortfolioLeg{LegKind::Option, uid,
                                  make_contract_id(uid, eid, k,
                                                   static_cast<Side>(s)),
                                  1.0, 100.0, 0.0, 0u});
    }
  }

  auto res = price_portfolio(book, b, PortfolioRiskMode::PriceOnly,
                             AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& val = res.value();
  ASSERT_EQ(val.legs.size(), book.size());

  const Underlying* under = u.get_underlying(uid).value();
  for (std::size_t i = 0; i < book.size(); ++i) {
    EXPECT_EQ(val.legs[i].status, LaneStatus::Ok);
    const std::uint16_t sx = cid_strike_idx(book[i].contract_id);
    const Side side = cid_side(book[i].contract_id);
    const double K = under->chains[eid].strikes[sx];
    EXPECT_NEAR(val.legs[i].price, leg_option_price(surf, F, K, T, r, side),
                1.0e-9);
  }
}

// ── Mixed option/stock/cash TOTAL value is the dollar sum ────────────────

TEST(Portfolio, MixedLegsAggregateTotalValueIsDollarSum) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  const auto strikes_a = scaled_strikes(100.0);
  const auto strikes_b = scaled_strikes(50.0);
  ExpiryId eid_a = 0;
  ExpiryId eid_b = 0;
  const Uid uid_a = add_underlying(u, "AAA", 100.0, strikes_a, T, eid_a);
  const Uid uid_b = add_underlying(u, "BBB", 50.0, strikes_b, T, eid_b);
  const double f_a = 100.0;
  const double f_b = 50.0;
  CurveSet cs_a = make_curves(100.0, r, T);
  CurveSet cs_b = make_curves(50.0, r, T);
  VolSurface surf_a = make_surface(uid_a, T, f_a, eid_a, 0.20 * 0.20 * T, 0.45, -0.20);
  VolSurface surf_b = make_surface(uid_b, T, f_b, eid_b, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid_a, UnderlyingMarket{&surf_a, &cs_a, nullptr, nullptr});
  b.set_market(uid_b, UnderlyingMarket{&surf_b, &cs_b, nullptr, nullptr});

  std::vector<PortfolioLeg> book{
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 2, Side::Call), 1.0, 100.0,
                   0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b,
                   make_contract_id(uid_b, eid_b, 2, Side::Put), -1.0, 100.0,
                   0.0, 0u},
      PortfolioLeg{LegKind::Stock, uid_a, 0, 10.0, 1.0, 0.0, 0u},
      PortfolioLeg{LegKind::Cash, kInvalidUid, 0, 1.0, 1.0, 1234.5, 0u},
  };

  auto res = price_portfolio(book, b, PortfolioRiskMode::PriceOnly,
                             AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& val = res.value();
  ASSERT_EQ(val.aggregates.size(), 1u);

  const double opt0 =
      1.0 * 100.0 * leg_option_price(surf_a, f_a, strikes_a[2], T, r, Side::Call);
  const double opt1 =
      -1.0 * 100.0 * leg_option_price(surf_b, f_b, strikes_b[2], T, r, Side::Put);
  const double stock = 10.0 * 100.0;  // spot 100, qty 10, mult 1
  const double cash = 1.0 * 1234.5;
  EXPECT_NEAR(val.aggregates[0].value, opt0 + opt1 + stock + cash, 1.0e-6);
}

// ── BY_UID groups per underlying and sums back to the leg total ──────────

TEST(Portfolio, AggByUidGroupsPerUnderlying) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  const auto strikes_a = scaled_strikes(100.0);
  const auto strikes_b = scaled_strikes(50.0);
  ExpiryId eid_a = 0;
  ExpiryId eid_b = 0;
  const Uid uid_a = add_underlying(u, "AAA", 100.0, strikes_a, T, eid_a);
  const Uid uid_b = add_underlying(u, "BBB", 50.0, strikes_b, T, eid_b);
  CurveSet cs_a = make_curves(100.0, r, T);
  CurveSet cs_b = make_curves(50.0, r, T);
  VolSurface surf_a = make_surface(uid_a, T, 100.0, eid_a, 0.20 * 0.20 * T, 0.45, -0.20);
  VolSurface surf_b = make_surface(uid_b, T, 50.0, eid_b, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid_a, UnderlyingMarket{&surf_a, &cs_a, nullptr, nullptr});
  b.set_market(uid_b, UnderlyingMarket{&surf_b, &cs_b, nullptr, nullptr});

  std::vector<PortfolioLeg> book{
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 2, Side::Call), 1.0, 100.0,
                   0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 3, Side::Put), 1.0, 100.0,
                   0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b,
                   make_contract_id(uid_b, eid_b, 2, Side::Call), 1.0, 100.0,
                   0.0, 0u},
  };

  auto res =
      price_portfolio(book, b, PortfolioRiskMode::PriceOnly, AggMode::ByUid);
  ASSERT_TRUE(res.has_value());
  const auto& val = res.value();
  ASSERT_EQ(val.aggregates.size(), 2u);

  double expected = 0.0;
  for (const auto& lv : val.legs) {
    expected += 1.0 * 100.0 * lv.price;
  }
  double got = 0.0;
  for (const auto& a : val.aggregates) {
    got += a.value;
  }
  EXPECT_NEAR(got, expected, 1.0e-6);
}

// ── First-order price/delta/vega parity (no-correction, r == 0) ──────────

TEST(Portfolio, FirstOrderMatchesB76NoCorrection) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  const auto strikes = scaled_strikes(100.0);
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  const double F = 100.0;
  CurveSet cs = make_curves(100.0, r, T);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  std::vector<PortfolioLeg> book;
  for (std::uint16_t k = 0; k < 5; ++k) {
    book.push_back(PortfolioLeg{LegKind::Option, uid,
                                make_contract_id(uid, eid, k, Side::Call), 1.0,
                                100.0, 0.0, 0u});
  }

  auto res =
      price_portfolio(book, b, PortfolioRiskMode::FirstOrder, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& val = res.value();

  const Underlying* under = u.get_underlying(uid).value();
  for (std::size_t k = 0; k < book.size(); ++k) {
    const double K = under->chains[eid].strikes[k];
    const double sigma = surf.iv(std::log(K / F), T);
    const auto bg = black76_greeks(F, K, T, sigma, r, std::exp(-r * T),
                                   Side::Call);
    // r == q == 0 => m == 1, so spot delta == B76 forward delta.
    EXPECT_NEAR(val.legs[k].price, bg.price, 1.0e-9);
    EXPECT_NEAR(val.legs[k].delta, bg.greeks.delta, 1.0e-8);
    EXPECT_NEAR(val.legs[k].vega, bg.greeks.vega, 1.0e-8);
  }
}

// ── Unification: BY_UID and TOTAL aggregates are consistent ──────────────

TEST(Portfolio, ByUidAndTotalAggregatesAreConsistent) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  const auto strikes_a = scaled_strikes(100.0);
  const auto strikes_b = scaled_strikes(50.0);
  ExpiryId eid_a = 0;
  ExpiryId eid_b = 0;
  const Uid uid_a = add_underlying(u, "AAA", 100.0, strikes_a, T, eid_a);
  const Uid uid_b = add_underlying(u, "BBB", 50.0, strikes_b, T, eid_b);
  CurveSet cs_a = make_curves(100.0, r, T);
  CurveSet cs_b = make_curves(50.0, r, T);
  VolSurface surf_a = make_surface(uid_a, T, 100.0, eid_a, 0.20 * 0.20 * T, 0.45, -0.20);
  VolSurface surf_b = make_surface(uid_b, T, 50.0, eid_b, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid_a, UnderlyingMarket{&surf_a, &cs_a, nullptr, nullptr});
  b.set_market(uid_b, UnderlyingMarket{&surf_b, &cs_b, nullptr, nullptr});

  std::vector<PortfolioLeg> book{
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 2, Side::Call), 1.0, 100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 3, Side::Call), -1.0, 100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 1, Side::Put), 2.0, 100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b,
                   make_contract_id(uid_b, eid_b, 2, Side::Call), 3.0, 100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Stock, uid_a, 0, 100.0, 1.0, 0.0, 0u},
      PortfolioLeg{LegKind::Stock, uid_b, 0, -50.0, 1.0, 0.0, 0u},
      PortfolioLeg{LegKind::Cash, kInvalidUid, 0, 12345.67, 1.0, 1.0, 0u},
  };

  auto total = price_portfolio(book, b, PortfolioRiskMode::FirstOrder,
                               AggMode::Total);
  auto by_uid = price_portfolio(book, b, PortfolioRiskMode::FirstOrder,
                                AggMode::ByUid);
  ASSERT_TRUE(total.has_value());
  ASSERT_TRUE(by_uid.has_value());
  ASSERT_EQ(total.value().aggregates.size(), 1u);

  double sum_value = 0.0;
  double sum_delta = 0.0;
  for (const auto& a : by_uid.value().aggregates) {
    sum_value += a.value;
    sum_delta += a.delta;
  }
  EXPECT_NEAR(total.value().aggregates[0].value, sum_value, 1.0e-6);
  EXPECT_NEAR(total.value().aggregates[0].delta, sum_delta, 1.0e-6);
}

// ── aggregate_greeks TOTAL matches the naive qty-weighted sum ────────────

TEST(Portfolio, AggregateGreeksTotalMatchesNaiveSum) {
  Universe u;
  const double T = 0.5;
  const auto strikes = scaled_strikes(100.0);
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  CurveSet cs = make_curves(100.0, 0.04, T);
  // Read r / df / F back from the curve so the oracle matches the engine's
  // resolved state bit-for-bit.
  const double r = cs.yield.zero(T);
  const double df = std::exp(-r * T);
  const double F = cs.forward.forward_at(eid);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  const std::array<double, 10> qtys{100, -50, 75, 25, -200, 150, -25, 60, 40, -120};
  std::vector<PortfolioLeg> book;
  for (int i = 0; i < 10; ++i) {
    const std::uint16_t sx = static_cast<std::uint16_t>(i % 5);
    const Side side = (i & 1) ? Side::Put : Side::Call;
    book.push_back(PortfolioLeg{LegKind::Option, uid,
                                make_contract_id(uid, eid, sx, side),
                                qtys[static_cast<std::size_t>(i)], 100.0, 0.0, 0u});
  }

  auto res = atx::vol::aggregate_greeks(book, b, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res.value().size(), 1u);
  const GreeksAggregate& agg = res.value()[0];

  const Underlying* under = u.get_underlying(uid).value();
  atx::vol::Greeks naive{};
  double naive_qty = 0.0;
  for (int i = 0; i < 10; ++i) {
    const std::uint16_t sx = static_cast<std::uint16_t>(i % 5);
    const Side side = (i & 1) ? Side::Put : Side::Call;
    const double K = under->chains[eid].strikes[sx];
    const double sigma = surf.iv(std::log(K / F), T);
    const auto g = black76_greeks(F, K, T, sigma, r, df, side).greeks;
    const double q = qtys[static_cast<std::size_t>(i)];
    naive.delta += q * g.delta;
    naive.gamma += q * g.gamma;
    naive.vega += q * g.vega;
    naive.theta += q * g.theta;
    naive.rho += q * g.rho;
    naive.vanna += q * g.vanna;
    naive.volga += q * g.volga;
    naive.charm += q * g.charm;
    naive_qty += q;
  }

  EXPECT_NEAR(agg.greeks.delta, naive.delta, 1.0e-9);
  EXPECT_NEAR(agg.greeks.gamma, naive.gamma, 1.0e-9);
  EXPECT_NEAR(agg.greeks.vega, naive.vega, 1.0e-9);
  EXPECT_NEAR(agg.greeks.theta, naive.theta, 1.0e-9);
  EXPECT_NEAR(agg.greeks.rho, naive.rho, 1.0e-9);
  EXPECT_NEAR(agg.greeks.vanna, naive.vanna, 1.0e-9);
  EXPECT_NEAR(agg.greeks.volga, naive.volga, 1.0e-9);
  EXPECT_NEAR(agg.greeks.charm, naive.charm, 1.0e-9);
  EXPECT_DOUBLE_EQ(agg.net_qty, naive_qty);
}

// ── aggregate_greeks skew-adjusted delta (I6) ─────────────────────────────

TEST(PortfolioGreeks, SkewAdjustedDeltaRaisesCallDeltaOnPutSkew) {
  // One OTM call (K = 1.15F) on a strongly put-skewed eSSVI surface (rho <
  // 0, so dSigma/dk < 0 across the tested strike range -- the 07-11 sprint's
  // GlobalPutSkewRaisesAdjustedDelta sign law, now proved through the
  // aggregate_greeks production seam rather than the bare curve/Greeks pair).
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  const auto strikes = scaled_strikes(100.0);
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  const double F = 100.0;  // r == 0 => F == spot, so kRelStrikes reads as k = ln(rel)
  CurveSet cs = make_curves(100.0, r, T);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.6, -0.6);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  const std::vector<PortfolioLeg> book{PortfolioLeg{
      LegKind::Option, uid, make_contract_id(uid, eid, 4, Side::Call), 1.0, 100.0, 0.0, 0u}};

  // Flag off (both the 3-arg default call and an explicit skew_adjusted_delta =
  // false) must be bit-identical to each other on EVERY Greek -- the off-flag
  // pin this seam's acceptance bar requires.
  auto raw = aggregate_greeks(book, b, AggMode::Total);
  ASSERT_TRUE(raw.has_value());
  ASSERT_EQ(raw.value().size(), 1u);
  auto raw_pinned = aggregate_greeks(book, b, AggMode::Total, /*skew_adjusted_delta=*/false);
  ASSERT_TRUE(raw_pinned.has_value());
  const atx::vol::Greeks& r0 = raw.value()[0].greeks;
  const atx::vol::Greeks& rp = raw_pinned.value()[0].greeks;
  EXPECT_EQ(rp.delta, r0.delta);
  EXPECT_EQ(rp.gamma, r0.gamma);
  EXPECT_EQ(rp.vega, r0.vega);
  EXPECT_EQ(rp.theta, r0.theta);
  EXPECT_EQ(rp.rho, r0.rho);
  EXPECT_EQ(rp.vanna, r0.vanna);
  EXPECT_EQ(rp.volga, r0.volga);
  EXPECT_EQ(rp.charm, r0.charm);

  // omega = 0 (sticky-delta): enabled delta strictly exceeds raw.
  auto enabled0 =
      aggregate_greeks(book, b, AggMode::Total, /*skew_adjusted_delta=*/true, StickyParams{0.0});
  ASSERT_TRUE(enabled0.has_value());
  EXPECT_GT(enabled0.value()[0].greeks.delta, r0.delta);
  // The adjustment touches delta only; every other Greek passes through.
  EXPECT_EQ(enabled0.value()[0].greeks.vega, r0.vega);
  EXPECT_EQ(enabled0.value()[0].greeks.gamma, r0.gamma);

  // omega = 1 (sticky-strike): VegaSlope collapses to 0 => exactly raw.
  auto enabled1 =
      aggregate_greeks(book, b, AggMode::Total, /*skew_adjusted_delta=*/true, StickyParams{1.0});
  ASSERT_TRUE(enabled1.has_value());
  EXPECT_DOUBLE_EQ(enabled1.value()[0].greeks.delta, r0.delta);
}

// ── aggregate_greeks BY_UID splits two underlyings by net qty ────────────

TEST(Portfolio, AggregateGreeksByUidTwoUnderlyings) {
  Universe u;
  const double T = 0.5;
  const double r = 0.04;
  const auto strikes_a = scaled_strikes(100.0);
  const auto strikes_b = scaled_strikes(50.0);
  ExpiryId eid_a = 0;
  ExpiryId eid_b = 0;
  const Uid uid_a = add_underlying(u, "AAA", 100.0, strikes_a, T, eid_a);
  const Uid uid_b = add_underlying(u, "BBB", 50.0, strikes_b, T, eid_b);
  CurveSet cs_a = make_curves(100.0, r, T);
  CurveSet cs_b = make_curves(50.0, r, T);
  VolSurface surf_a =
      make_surface(uid_a, T, 100.0 * std::exp(r * T), eid_a, 0.20 * 0.20 * T, 0.45, -0.20);
  VolSurface surf_b =
      make_surface(uid_b, T, 50.0 * std::exp(r * T), eid_b, 0.30 * 0.30 * T, 0.35, -0.10);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid_a, UnderlyingMarket{&surf_a, &cs_a, nullptr, nullptr});
  b.set_market(uid_b, UnderlyingMarket{&surf_b, &cs_b, nullptr, nullptr});

  std::vector<PortfolioLeg> book{
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 0, Side::Call), 100.0, 100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 2, Side::Put), -50.0, 100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a,
                   make_contract_id(uid_a, eid_a, 1, Side::Call), 25.0, 100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b,
                   make_contract_id(uid_b, eid_b, 1, Side::Call), 200.0, 100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b,
                   make_contract_id(uid_b, eid_b, 0, Side::Put), -75.0, 100.0, 0.0, 0u},
  };

  auto res = atx::vol::aggregate_greeks(book, b, AggMode::ByUid);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res.value().size(), 2u);

  double qty_a = 0.0;
  double qty_b = 0.0;
  for (const auto& a : res.value()) {
    if (a.uid == uid_a) {
      qty_a = a.net_qty;
    } else if (a.uid == uid_b) {
      qty_b = a.net_qty;
    }
  }
  EXPECT_DOUBLE_EQ(qty_a, 100.0 - 50.0 + 25.0);
  EXPECT_DOUBLE_EQ(qty_b, 200.0 - 75.0);
}

// ── Aggregate first-order Greeks vs finite-difference of book value ──────

TEST(Portfolio, FirstOrderAggregateGreeksMatchFiniteDifference) {
  Universe u;
  const double T = 0.5;
  const double spot = 100.0;
  const auto strikes = scaled_strikes(spot);
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", spot, strikes, T, eid);
  // r != q so the spot-delta scaling m = F/S = e^{(r-q)T} bites. Read the
  // resolved r / df / F from the curve to stay consistent with the engine.
  CurveSet cs = make_curves(spot, 0.04, T);
  const double r = cs.yield.zero(T);
  const double df = std::exp(-r * T);
  const double F = cs.forward.forward_at(eid);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  struct Pos {
    std::uint16_t sx;
    Side side;
    double qty;
  };
  const std::array<Pos, 4> pos{Pos{0, Side::Call, 2.0}, Pos{1, Side::Put, -1.0},
                               Pos{3, Side::Call, 1.0}, Pos{4, Side::Put, 3.0}};

  std::vector<PortfolioLeg> book;
  std::array<double, 4> sigma{};
  std::array<double, 4> strike{};
  for (std::size_t i = 0; i < pos.size(); ++i) {
    const double K = strikes[pos[i].sx];
    strike[i] = K;
    sigma[i] = surf.iv(std::log(K / F), T);
    book.push_back(PortfolioLeg{LegKind::Option, uid,
                                make_contract_id(uid, eid, pos[i].sx, pos[i].side),
                                pos[i].qty, 100.0, 0.0, 0u});
  }

  auto res =
      price_portfolio(book, b, PortfolioRiskMode::FirstOrder, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res.value().aggregates.size(), 1u);
  const auto& agg = res.value().aggregates[0];

  // Book value with per-leg sigma held fixed (isolates the analytic
  // sensitivities). The forward at a bumped spot is (S + dS) * (F / S).
  auto value_at = [&](double s_bump, double vol_bump) {
    const double f = (spot + s_bump) * F / spot;
    double v = 0.0;
    for (std::size_t i = 0; i < pos.size(); ++i) {
      v += pos[i].qty * 100.0 *
           black76_price(f, strike[i], T, sigma[i] + vol_bump, df, pos[i].side);
    }
    return v;
  };

  const double h_spot = 1.0e-3;
  const double h_vol = 1.0e-4;
  const double delta_fd =
      (value_at(h_spot, 0.0) - value_at(-h_spot, 0.0)) / (2.0 * h_spot);
  const double vega_fd =
      (value_at(0.0, h_vol) - value_at(0.0, -h_vol)) / (2.0 * h_vol);
  EXPECT_NEAR(agg.delta, delta_fd, 1.0e-2);
  EXPECT_NEAR(agg.vega, vega_fd, 1.0e-2);
}
