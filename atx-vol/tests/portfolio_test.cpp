#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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
using atx::vol::essvi_natural_to_reparam;
using atx::vol::EssviParams;
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
[[nodiscard]] Uid add_underlying(Universe &u, std::string_view ticker, double spot,
                                 std::span<const double> strikes, double T, ExpiryId &eid_out) {
  const Uid uid = u.intern_ticker(ticker).value();
  eid_out = u.add_expiry(uid, expiry_ns_for(T)).value();
  for (double k : strikes) {
    (void)u.add_strike(uid, eid_out, k).value();
  }
  Underlying *under = u.get_underlying(uid).value();
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

[[nodiscard]] VolSurface make_surface(Uid uid, double T, double F, ExpiryId eid, double theta,
                                      double phi, double rho) {
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

[[nodiscard]] double leg_option_price(const VolSurface &surf, double F, double K, double T,
                                      double r, Side side) {
  const double sigma = surf.iv(std::log(K / F), T);
  return black76_price(F, K, T, sigma, std::exp(-r * T), side);
}

void expect_greeks_aggregates_bit_identical(std::span<const GreeksAggregate> lhs,
                                            std::span<const GreeksAggregate> rhs) {
  ASSERT_EQ(lhs.size(), rhs.size());
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    EXPECT_EQ(lhs[i].group_key, rhs[i].group_key);
    EXPECT_EQ(lhs[i].uid, rhs[i].uid);
    EXPECT_EQ(lhs[i].expiry_id, rhs[i].expiry_id);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].greeks.delta),
              std::bit_cast<std::uint64_t>(rhs[i].greeks.delta));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].greeks.gamma),
              std::bit_cast<std::uint64_t>(rhs[i].greeks.gamma));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].greeks.vega),
              std::bit_cast<std::uint64_t>(rhs[i].greeks.vega));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].greeks.theta),
              std::bit_cast<std::uint64_t>(rhs[i].greeks.theta));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].greeks.rho),
              std::bit_cast<std::uint64_t>(rhs[i].greeks.rho));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].greeks.vanna),
              std::bit_cast<std::uint64_t>(rhs[i].greeks.vanna));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].greeks.volga),
              std::bit_cast<std::uint64_t>(rhs[i].greeks.volga));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].greeks.charm),
              std::bit_cast<std::uint64_t>(rhs[i].greeks.charm));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs[i].net_qty),
              std::bit_cast<std::uint64_t>(rhs[i].net_qty));
  }
}

void add_european_raw_qty_oracle(GreeksAggregate &target, const Universe &universe,
                                 const VolSurface &surface, const CurveSet &curves,
                                 const PortfolioLeg &leg) {
  const Uid uid = atx::vol::cid_uid(leg.contract_id);
  const ExpiryId expiry_id = atx::vol::cid_expiry(leg.contract_id);
  const Underlying *underlying = universe.get_underlying(uid).value();
  const auto &chain = underlying->chains[static_cast<std::size_t>(expiry_id)];
  const std::uint16_t strike_index = cid_strike_idx(leg.contract_id);
  const double strike = chain.strikes[static_cast<std::size_t>(strike_index)];
  const double forward = curves.forward.forward_at(static_cast<std::size_t>(expiry_id));
  const double rate = curves.yield.zero(chain.T);
  const double sigma = surface.iv(std::log(strike / forward), chain.T);
  const auto greeks = black76_greeks(forward, strike, chain.T, sigma, rate,
                                     std::exp(-rate * chain.T), cid_side(leg.contract_id))
                          .greeks;
  target.greeks.delta += leg.qty * greeks.delta;
  target.greeks.gamma += leg.qty * greeks.gamma;
  target.greeks.vega += leg.qty * greeks.vega;
  target.greeks.theta += leg.qty * greeks.theta;
  target.greeks.rho += leg.qty * greeks.rho;
  target.greeks.vanna += leg.qty * greeks.vanna;
  target.greeks.volga += leg.qty * greeks.volga;
  target.greeks.charm += leg.qty * greeks.charm;
  target.net_qty += leg.qty;
}

} // namespace

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
                                  make_contract_id(uid, eid, k, static_cast<Side>(s)), 1.0, 100.0,
                                  0.0, 0u});
    }
  }

  auto res = price_portfolio(book, b, PortfolioRiskMode::PriceOnly, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto &val = res.value();
  ASSERT_EQ(val.legs.size(), book.size());

  const Underlying *under = u.get_underlying(uid).value();
  for (std::size_t i = 0; i < book.size(); ++i) {
    EXPECT_EQ(val.legs[i].status, LaneStatus::Ok);
    const std::uint16_t sx = cid_strike_idx(book[i].contract_id);
    const Side side = cid_side(book[i].contract_id);
    const double K = under->chains[eid].strikes[sx];
    EXPECT_NEAR(val.legs[i].price, leg_option_price(surf, F, K, T, r, side), 1.0e-9);
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
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 2, Side::Call), 1.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b, make_contract_id(uid_b, eid_b, 2, Side::Put), -1.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Stock, uid_a, 0, 10.0, 1.0, 0.0, 0u},
      PortfolioLeg{LegKind::Cash, kInvalidUid, 0, 1.0, 1.0, 1234.5, 0u},
  };

  auto res = price_portfolio(book, b, PortfolioRiskMode::PriceOnly, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto &val = res.value();
  ASSERT_EQ(val.aggregates.size(), 1u);

  const double opt0 = 1.0 * 100.0 * leg_option_price(surf_a, f_a, strikes_a[2], T, r, Side::Call);
  const double opt1 = -1.0 * 100.0 * leg_option_price(surf_b, f_b, strikes_b[2], T, r, Side::Put);
  const double stock = 10.0 * 100.0; // spot 100, qty 10, mult 1
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
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 2, Side::Call), 1.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 3, Side::Put), 1.0, 100.0,
                   0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b, make_contract_id(uid_b, eid_b, 2, Side::Call), 1.0,
                   100.0, 0.0, 0u},
  };

  auto res = price_portfolio(book, b, PortfolioRiskMode::PriceOnly, AggMode::ByUid);
  ASSERT_TRUE(res.has_value());
  const auto &val = res.value();
  ASSERT_EQ(val.aggregates.size(), 2u);

  double expected = 0.0;
  for (const auto &lv : val.legs) {
    expected += 1.0 * 100.0 * lv.price;
  }
  double got = 0.0;
  for (const auto &a : val.aggregates) {
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
    book.push_back(PortfolioLeg{LegKind::Option, uid, make_contract_id(uid, eid, k, Side::Call),
                                1.0, 100.0, 0.0, 0u});
  }

  auto res = price_portfolio(book, b, PortfolioRiskMode::FirstOrder, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto &val = res.value();

  const Underlying *under = u.get_underlying(uid).value();
  for (std::size_t k = 0; k < book.size(); ++k) {
    const double K = under->chains[eid].strikes[k];
    const double sigma = surf.iv(std::log(K / F), T);
    const auto bg = black76_greeks(F, K, T, sigma, r, std::exp(-r * T), Side::Call);
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
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 2, Side::Call), 1.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 3, Side::Call), -1.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 1, Side::Put), 2.0, 100.0,
                   0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b, make_contract_id(uid_b, eid_b, 2, Side::Call), 3.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Stock, uid_a, 0, 100.0, 1.0, 0.0, 0u},
      PortfolioLeg{LegKind::Stock, uid_b, 0, -50.0, 1.0, 0.0, 0u},
      PortfolioLeg{LegKind::Cash, kInvalidUid, 0, 12345.67, 1.0, 1.0, 0u},
  };

  auto total = price_portfolio(book, b, PortfolioRiskMode::FirstOrder, AggMode::Total);
  auto by_uid = price_portfolio(book, b, PortfolioRiskMode::FirstOrder, AggMode::ByUid);
  ASSERT_TRUE(total.has_value());
  ASSERT_TRUE(by_uid.has_value());
  ASSERT_EQ(total.value().aggregates.size(), 1u);

  double sum_value = 0.0;
  double sum_delta = 0.0;
  for (const auto &a : by_uid.value().aggregates) {
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
    book.push_back(PortfolioLeg{LegKind::Option, uid, make_contract_id(uid, eid, sx, side),
                                qtys[static_cast<std::size_t>(i)], 100.0, 0.0, 0u});
  }

  auto res = atx::vol::aggregate_greeks(book, b, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res.value().size(), 1u);
  const GreeksAggregate &agg = res.value()[0];

  const Underlying *under = u.get_underlying(uid).value();
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
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 0, Side::Call), 100.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 2, Side::Put), -50.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a, 1, Side::Call), 25.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b, make_contract_id(uid_b, eid_b, 1, Side::Call), 200.0,
                   100.0, 0.0, 0u},
      PortfolioLeg{LegKind::Option, uid_b, make_contract_id(uid_b, eid_b, 0, Side::Put), -75.0,
                   100.0, 0.0, 0u},
  };

  auto res = atx::vol::aggregate_greeks(book, b, AggMode::ByUid);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res.value().size(), 2u);

  double qty_a = 0.0;
  double qty_b = 0.0;
  for (const auto &a : res.value()) {
    if (a.uid == uid_a) {
      qty_a = a.net_qty;
    } else if (a.uid == uid_b) {
      qty_b = a.net_qty;
    }
  }
  EXPECT_DOUBLE_EQ(qty_a, 100.0 - 50.0 + 25.0);
  EXPECT_DOUBLE_EQ(qty_b, 200.0 - 75.0);
}

// ── Explicit European/raw-quantity aggregation contract ─────────────────

TEST(Portfolio, NamedEuropeanRawQtyGreeksApiMatchesCompatibilityApiForEveryMode) {
  Universe u;
  const double T0 = 0.25;
  const double T1 = 0.75;
  const double r = 0.04;
  const auto strikes_a = scaled_strikes(100.0);
  const auto strikes_b = scaled_strikes(50.0);
  ExpiryId eid_a0 = 0;
  ExpiryId eid_b = 0;
  const Uid uid_a = add_underlying(u, "AAA", 100.0, strikes_a, T0, eid_a0);
  const ExpiryId eid_a1 = u.add_expiry(uid_a, expiry_ns_for(T1)).value();
  for (const double strike : strikes_a) {
    (void)u.add_strike(uid_a, eid_a1, strike).value();
  }
  u.get_underlying(uid_a).value()->chains[eid_a1].T = T1;
  const Uid uid_b = add_underlying(u, "BBB", 50.0, strikes_b, T0, eid_b);

  CurveSet cs_a = make_curves(100.0, r, T0);
  const std::array<ForwardPoint, 2> forwards_a{
      ForwardPoint{expiry_ns_for(T0), T0, 100.0 * std::exp(r * T0)},
      ForwardPoint{expiry_ns_for(T1), T1, 100.0 * std::exp(r * T1)},
  };
  cs_a.forward.set(forwards_a);
  CurveSet cs_b = make_curves(50.0, r, T0);
  VolSurface surf_a =
      make_surface(uid_a, T0, forwards_a[0].F, eid_a0, 0.20 * 0.20 * T0, 0.45, -0.20);
  EssviParams second_slice{};
  second_slice.theta = 0.24 * 0.24 * T1;
  second_slice.phi = 0.40;
  second_slice.rho = -0.15;
  second_slice.T = T1;
  second_slice.F = forwards_a[1].F;
  second_slice.expiry_id = eid_a1;
  second_slice.expiry_ns = expiry_ns_for(T1);
  const auto second_cube =
      essvi_natural_to_reparam(second_slice.theta, second_slice.phi, second_slice.rho, T1);
  second_slice.psi = second_cube.psi;
  second_slice.p = second_cube.p;
  second_slice.lambda = second_cube.lambda;
  (void)surf_a.set_slice_essvi(1u, second_slice);
  VolSurface surf_b =
      make_surface(uid_b, T0, 50.0 * std::exp(r * T0), eid_b, 0.30 * 0.30 * T0, 0.35, -0.10);

  MarketBinding binding;
  binding.universe = &u;
  binding.set_market(uid_a, UnderlyingMarket{&surf_a, &cs_a, nullptr, nullptr});
  binding.set_market(uid_b, UnderlyingMarket{&surf_b, &cs_b, nullptr, nullptr});

  const std::vector<PortfolioLeg> book{
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a1, 1, Side::Call), 3.0, 1.0,
                   0.0, 17u},
      PortfolioLeg{LegKind::Option, uid_b, make_contract_id(uid_b, eid_b, 2, Side::Put), -2.0, 10.0,
                   0.0, 9u},
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a0, 0, Side::Put), 5.0,
                   100.0, 0.0, 9u},
      PortfolioLeg{LegKind::Option, uid_a, make_contract_id(uid_a, eid_a1, 3, Side::Call), 7.0,
                   1000.0, 0.0, 42u},
  };

  for (const AggMode mode :
       {AggMode::Total, AggMode::ByUid, AggMode::ByUidExpiry, AggMode::ByGroupId}) {
    const auto compatibility = atx::vol::aggregate_greeks(book, binding, mode);
    const auto explicit_semantics =
        atx::vol::aggregate_european_b76_greeks_raw_qty(book, binding, mode);
    ASSERT_TRUE(compatibility.has_value());
    ASSERT_TRUE(explicit_semantics.has_value());

    std::vector<GreeksAggregate> expected;
    std::array<std::size_t, 4> bucket_for_leg{};
    switch (mode) {
    case AggMode::Total:
      expected.push_back(GreeksAggregate{0u, kInvalidUid, atx::vol::kInvalidExpiry, {}, 0.0});
      bucket_for_leg = {0u, 0u, 0u, 0u};
      break;
    case AggMode::ByUid:
      expected.push_back(GreeksAggregate{
          static_cast<std::uint64_t>(uid_a), uid_a, atx::vol::kInvalidExpiry, {}, 0.0});
      expected.push_back(GreeksAggregate{
          static_cast<std::uint64_t>(uid_b), uid_b, atx::vol::kInvalidExpiry, {}, 0.0});
      bucket_for_leg = {0u, 1u, 0u, 0u};
      break;
    case AggMode::ByUidExpiry:
      expected.push_back(GreeksAggregate{
          (static_cast<std::uint64_t>(uid_a) << 16u) | eid_a1, uid_a, eid_a1, {}, 0.0});
      expected.push_back(GreeksAggregate{
          (static_cast<std::uint64_t>(uid_b) << 16u) | eid_b, uid_b, eid_b, {}, 0.0});
      expected.push_back(GreeksAggregate{
          (static_cast<std::uint64_t>(uid_a) << 16u) | eid_a0, uid_a, eid_a0, {}, 0.0});
      bucket_for_leg = {0u, 1u, 2u, 0u};
      break;
    case AggMode::ByGroupId:
      expected.push_back(GreeksAggregate{17u, uid_a, atx::vol::kInvalidExpiry, {}, 0.0});
      expected.push_back(GreeksAggregate{9u, uid_b, atx::vol::kInvalidExpiry, {}, 0.0});
      expected.push_back(GreeksAggregate{42u, uid_a, atx::vol::kInvalidExpiry, {}, 0.0});
      bucket_for_leg = {0u, 1u, 1u, 2u};
      break;
    }

    for (std::size_t i = 0; i < book.size(); ++i) {
      if (atx::vol::cid_uid(book[i].contract_id) == uid_a) {
        add_european_raw_qty_oracle(expected[bucket_for_leg[i]], u, surf_a, cs_a, book[i]);
      } else {
        add_european_raw_qty_oracle(expected[bucket_for_leg[i]], u, surf_b, cs_b, book[i]);
      }
    }

    expect_greeks_aggregates_bit_identical(*explicit_semantics, expected);
    expect_greeks_aggregates_bit_identical(*compatibility, *explicit_semantics);
  }
}

TEST(Portfolio, EuropeanRawQtyGreeksIgnoreMultiplierAndNonOptionLegs) {
  Universe u;
  const double T = 0.5;
  const auto strikes = scaled_strikes(100.0);
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  CurveSet curves = make_curves(100.0, 0.04, T);
  VolSurface surface =
      make_surface(uid, T, curves.forward.forward_at(eid), eid, 0.20 * 0.20 * T, 0.45, -0.20);
  MarketBinding binding;
  binding.universe = &u;
  binding.set_market(uid, UnderlyingMarket{&surface, &curves, nullptr, nullptr});

  std::vector<PortfolioLeg> unit_multiplier{
      PortfolioLeg{LegKind::Option, uid, make_contract_id(uid, eid, 1, Side::Call), 3.0, 1.0, 0.0,
                   4u},
      PortfolioLeg{LegKind::Option, uid, make_contract_id(uid, eid, 3, Side::Put), -2.0, 1.0, 0.0,
                   4u},
      PortfolioLeg{LegKind::Stock, uid, make_contract_id(uid, eid, 4, Side::Call), 1'000'000.0, 1.0,
                   0.0, 4u},
      PortfolioLeg{LegKind::Cash, kInvalidUid, make_contract_id(uid, eid, 0, Side::Put),
                   1'000'000.0, 1.0, 1.0, 4u},
  };
  std::vector<PortfolioLeg> varied_multiplier = unit_multiplier;
  varied_multiplier[0].multiplier = 100.0;
  varied_multiplier[1].multiplier = 10'000.0;
  varied_multiplier[2].multiplier = 500.0;
  varied_multiplier[3].multiplier = 0.01;

  const auto unit =
      atx::vol::aggregate_european_b76_greeks_raw_qty(unit_multiplier, binding, AggMode::ByGroupId);
  const auto varied = atx::vol::aggregate_european_b76_greeks_raw_qty(varied_multiplier, binding,
                                                                      AggMode::ByGroupId);
  ASSERT_TRUE(unit.has_value());
  ASSERT_TRUE(varied.has_value());
  GreeksAggregate expected{4u, uid, atx::vol::kInvalidExpiry, {}, 0.0};
  add_european_raw_qty_oracle(expected, u, surface, curves, unit_multiplier[0]);
  add_european_raw_qty_oracle(expected, u, surface, curves, unit_multiplier[1]);
  const std::array<GreeksAggregate, 1> expected_rows{expected};
  expect_greeks_aggregates_bit_identical(*unit, expected_rows);
  expect_greeks_aggregates_bit_identical(*varied, expected_rows);
  expect_greeks_aggregates_bit_identical(*unit, *varied);
}

TEST(Portfolio, EuropeanRawQtyGreeksPreserveFirstSeenOrderAcrossManyBuckets) {
  Universe u;
  const double T = 0.5;
  const auto strikes = scaled_strikes(100.0);
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "AAA", 100.0, strikes, T, eid);
  CurveSet curves = make_curves(100.0, 0.04, T);
  VolSurface surface =
      make_surface(uid, T, curves.forward.forward_at(eid), eid, 0.20 * 0.20 * T, 0.45, -0.20);
  MarketBinding binding;
  binding.universe = &u;
  binding.set_market(uid, UnderlyingMarket{&surface, &curves, nullptr, nullptr});

  constexpr std::size_t kBuckets = 4096;
  constexpr std::uint32_t kPermutationMultiplier = 4051u;
  std::vector<PortfolioLeg> book;
  book.reserve(kBuckets * 2u);
  for (std::size_t i = 0; i < kBuckets; ++i) {
    const std::uint32_t group = (static_cast<std::uint32_t>(i) * kPermutationMultiplier) %
                                static_cast<std::uint32_t>(kBuckets);
    const double qty = 1.0 + static_cast<double>(i % 7u);
    book.push_back(PortfolioLeg{LegKind::Option, uid, make_contract_id(uid, eid, 2, Side::Call),
                                qty, 100.0, 0.0, group});
  }
  for (std::size_t i = 0; i < kBuckets; ++i) {
    const std::uint32_t group = (static_cast<std::uint32_t>(i) * kPermutationMultiplier) %
                                static_cast<std::uint32_t>(kBuckets);
    book.push_back(PortfolioLeg{LegKind::Option, uid, make_contract_id(uid, eid, 2, Side::Call),
                                -0.5, 100.0, 0.0, group});
  }

  const auto result =
      atx::vol::aggregate_european_b76_greeks_raw_qty(book, binding, AggMode::ByGroupId);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), kBuckets);
  for (std::size_t i = 0; i < kBuckets; ++i) {
    const std::uint64_t expected_group = (static_cast<std::uint32_t>(i) * kPermutationMultiplier) %
                                         static_cast<std::uint32_t>(kBuckets);
    EXPECT_EQ((*result)[i].group_key, expected_group);
    EXPECT_DOUBLE_EQ((*result)[i].net_qty, 0.5 + static_cast<double>(i % 7u));
  }
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
                                make_contract_id(uid, eid, pos[i].sx, pos[i].side), pos[i].qty,
                                100.0, 0.0, 0u});
  }

  auto res = price_portfolio(book, b, PortfolioRiskMode::FirstOrder, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res.value().aggregates.size(), 1u);
  const auto &agg = res.value().aggregates[0];

  // Book value with per-leg sigma held fixed (isolates the analytic
  // sensitivities). The forward at a bumped spot is (S + dS) * (F / S).
  auto value_at = [&](double s_bump, double vol_bump) {
    const double f = (spot + s_bump) * F / spot;
    double v = 0.0;
    for (std::size_t i = 0; i < pos.size(); ++i) {
      v +=
          pos[i].qty * 100.0 * black76_price(f, strike[i], T, sigma[i] + vol_bump, df, pos[i].side);
    }
    return v;
  };

  const double h_spot = 1.0e-3;
  const double h_vol = 1.0e-4;
  const double delta_fd = (value_at(h_spot, 0.0) - value_at(-h_spot, 0.0)) / (2.0 * h_spot);
  const double vega_fd = (value_at(0.0, h_vol) - value_at(0.0, -h_vol)) / (2.0 * h_vol);
  EXPECT_NEAR(agg.delta, delta_fd, 1.0e-2);
  EXPECT_NEAR(agg.vega, vega_fd, 1.0e-2);
}
