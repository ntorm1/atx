#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/portfolio.hpp"

// Bulk chain pricer + selection, ported from the C ats-vol test
// test_bulk_pricing.c. The European B76 and American-no-correction routes are
// exercised; the cache route (a populated CorrectionCache) is deferred (see the
// PORT NOTE in bulk.cpp).

namespace {

using atx::vol::AggMode;
using atx::vol::black76_greeks;
using atx::vol::black76_price;
using atx::vol::BulkRequest;
using atx::vol::BulkResult;
using atx::vol::BulkRiskMode;
using atx::vol::BulkSelectKind;
using atx::vol::bulk_price;
using atx::vol::cid_side;
using atx::vol::cid_strike_idx;
using atx::vol::ContractId;
using atx::vol::CurveSet;
using atx::vol::EssviParams;
using atx::vol::essvi_natural_to_reparam;
using atx::vol::ExpiryId;
using atx::vol::ForwardPoint;
using atx::vol::kInvalidUid;
using atx::vol::LaneStatus;
using atx::vol::make_contract_id;
using atx::vol::MarketBinding;
using atx::vol::Parametrization;
using atx::vol::PricingRoute;
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

constexpr std::array<double, 5> kStrikes{80.0, 90.0, 100.0, 110.0, 120.0};

[[nodiscard]] std::vector<ContractId> sorted(std::span<const ContractId> ids) {
  std::vector<ContractId> v(ids.begin(), ids.end());
  std::sort(v.begin(), v.end());
  return v;
}

}  // namespace

// ── Contract-list PRICE_ONLY parity vs scalar Black-76 ───────────────────

TEST(Bulk, ContractListPriceMatchesScalarB76) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "BULK", 100.0, kStrikes, T, eid);
  const double F = 100.0;
  CurveSet cs = make_curves(100.0, r, T);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  const std::vector<ContractId> ids{
      make_contract_id(uid, eid, 1, Side::Call),
      make_contract_id(uid, eid, 2, Side::Put),
      make_contract_id(uid, eid, 3, Side::Call),
      make_contract_id(uid, eid, 4, Side::Put),
  };
  BulkRequest req;
  req.select_kind = BulkSelectKind::ContractList;
  req.uid = uid;
  req.contract_ids = ids;
  req.risk_mode = BulkRiskMode::PriceOnly;

  auto res = bulk_price(req, b, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& out = res.value().out;
  ASSERT_EQ(out.size(), 4u);

  const Underlying* under = u.get_underlying(uid).value();
  for (std::size_t i = 0; i < 4u; ++i) {
    EXPECT_EQ(out.contract_id[i], ids[i]);
    EXPECT_EQ(out.status[i], LaneStatus::Ok);
    const std::uint16_t sx = cid_strike_idx(ids[i]);
    const Side side = cid_side(ids[i]);
    const double K = under->chains[eid].strikes[sx];
    const double sigma = surf.iv(std::log(K / F), T);
    EXPECT_NEAR(out.price[i], black76_price(F, K, T, sigma, 1.0, side), 1.0e-9);
  }
}

// ── Chain selectors pick the expected contract sets ──────────────────────

TEST(Bulk, ChainSelectorsPickExpectedContractSets) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "BULK", 100.0, kStrikes, T, eid);
  CurveSet cs = make_curves(100.0, r, T);
  VolSurface surf = make_surface(uid, T, 100.0, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  // Moneyness band F*(1 +/- 0.10) = [90, 110] -> strikes 1, 2, 3.
  BulkRequest band;
  band.select_kind = BulkSelectKind::ChainMoneynessBand;
  band.uid = uid;
  band.moneyness_pct = 0.10;
  band.risk_mode = BulkRiskMode::PriceOnly;
  auto band_res = bulk_price(band, b, AggMode::Total);
  ASSERT_TRUE(band_res.has_value());
  ASSERT_EQ(band_res.value().out.size(), 6u);
  const std::array<ContractId, 6> band_want{
      make_contract_id(uid, eid, 1, Side::Call),
      make_contract_id(uid, eid, 1, Side::Put),
      make_contract_id(uid, eid, 2, Side::Call),
      make_contract_id(uid, eid, 2, Side::Put),
      make_contract_id(uid, eid, 3, Side::Call),
      make_contract_id(uid, eid, 3, Side::Put)};
  EXPECT_EQ(sorted(band_res.value().out.contract_id), sorted(band_want));

  // Strike range [95, 115] -> strikes 2, 3.
  BulkRequest range;
  range.select_kind = BulkSelectKind::ChainStrikeRange;
  range.uid = uid;
  range.strike_lo = 95.0;
  range.strike_hi = 115.0;
  range.risk_mode = BulkRiskMode::PriceOnly;
  auto range_res = bulk_price(range, b, AggMode::Total);
  ASSERT_TRUE(range_res.has_value());
  ASSERT_EQ(range_res.value().out.size(), 4u);
  const std::array<ContractId, 4> range_want{
      make_contract_id(uid, eid, 2, Side::Call),
      make_contract_id(uid, eid, 2, Side::Put),
      make_contract_id(uid, eid, 3, Side::Call),
      make_contract_id(uid, eid, 3, Side::Put)};
  EXPECT_EQ(sorted(range_res.value().out.contract_id), sorted(range_want));
}

// ── B76_GREEKS mode matches scalar Black-76 for every field ──────────────

TEST(Bulk, B76GreeksChainBatchMatchesScalarForAllFields) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "BULK", 100.0, kStrikes, T, eid);
  const double F = 100.0;
  CurveSet cs = make_curves(100.0, r, T);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  BulkRequest req;
  req.select_kind = BulkSelectKind::ChainStrikeRange;
  req.uid = uid;
  req.strike_lo = 80.0;
  req.strike_hi = 120.0;
  req.risk_mode = BulkRiskMode::B76Greeks;

  auto res = bulk_price(req, b, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& out = res.value().out;
  ASSERT_EQ(out.size(), 10u);

  const Underlying* under = u.get_underlying(uid).value();
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint16_t sx = cid_strike_idx(out.contract_id[i]);
    const Side side = cid_side(out.contract_id[i]);
    const double K = under->chains[eid].strikes[sx];
    const double sigma = surf.iv(std::log(K / F), T);
    const auto bg = black76_greeks(F, K, T, sigma, r, 1.0, side);

    EXPECT_EQ(out.status[i], LaneStatus::Ok);
    EXPECT_EQ(out.route[i], static_cast<std::uint8_t>(PricingRoute::B76Only));
    EXPECT_NEAR(out.price[i], bg.price, 2.0e-9);
    EXPECT_NEAR(out.iv[i], sigma, 1.0e-12);
    EXPECT_NEAR(out.delta[i], bg.greeks.delta, 1.0e-8);
    EXPECT_NEAR(out.gamma[i], bg.greeks.gamma, 1.0e-8);
    EXPECT_NEAR(out.vega[i], bg.greeks.vega, 1.0e-8);
    EXPECT_NEAR(out.theta[i], bg.greeks.theta, 1.0e-8);
    EXPECT_NEAR(out.rho[i], bg.greeks.rho, 1.0e-8);
    EXPECT_NEAR(out.vanna[i], bg.greeks.vanna, 1.0e-8);
    EXPECT_NEAR(out.volga[i], bg.greeks.volga, 1.0e-8);
    EXPECT_NEAR(out.charm[i], bg.greeks.charm, 1.0e-8);
  }
}

// ── A contract list may span underlyings ─────────────────────────────────

TEST(Bulk, ContractListCanSpanUids) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  const std::array<double, 3> strikes_b{40.0, 50.0, 60.0};
  ExpiryId eid_a = 0;
  ExpiryId eid_b = 0;
  const Uid uid_a = add_underlying(u, "BULK", 100.0, kStrikes, T, eid_a);
  const Uid uid_b = add_underlying(u, "BULK2", 50.0, strikes_b, T, eid_b);
  const double f_a = 100.0;
  const double f_b = 50.0;
  CurveSet cs_a = make_curves(100.0, r, T);
  CurveSet cs_b = make_curves(50.0, r, T);
  VolSurface surf_a = make_surface(uid_a, T, f_a, eid_a, 0.20 * 0.20 * T, 0.45, -0.20);
  VolSurface surf_b = make_surface(uid_b, T, f_b, eid_b, 0.25 * 0.25 * T, 0.35, -0.10);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid_a, UnderlyingMarket{&surf_a, &cs_a, nullptr, nullptr});
  b.set_market(uid_b, UnderlyingMarket{&surf_b, &cs_b, nullptr, nullptr});

  const std::vector<ContractId> ids{
      make_contract_id(uid_a, eid_a, 2, Side::Call),
      make_contract_id(uid_b, eid_b, 1, Side::Put),
  };
  BulkRequest req;
  req.select_kind = BulkSelectKind::ContractList;
  req.uid = kInvalidUid;  // allow multi-uid
  req.contract_ids = ids;
  req.risk_mode = BulkRiskMode::PriceOnly;

  auto res = bulk_price(req, b, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& out = res.value().out;
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out.status[0], LaneStatus::Ok);
  EXPECT_EQ(out.status[1], LaneStatus::Ok);

  const double sig_a = surf_a.iv(std::log(100.0 / f_a), T);
  const double sig_b = surf_b.iv(std::log(50.0 / f_b), T);
  EXPECT_NEAR(out.price[0], black76_price(f_a, 100.0, T, sig_a, 1.0, Side::Call),
              1.0e-9);
  EXPECT_NEAR(out.price[1], black76_price(f_b, 50.0, T, sig_b, 1.0, Side::Put),
              1.0e-9);
}

// ── Aggregation reduces qty-weighted Greeks (contract list) ──────────────

TEST(Bulk, AggregateMatchesQtyWeightedGreekSum) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "BULK", 100.0, kStrikes, T, eid);
  const double F = 100.0;
  CurveSet cs = make_curves(100.0, r, T);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  const std::vector<ContractId> ids{
      make_contract_id(uid, eid, 1, Side::Call),
      make_contract_id(uid, eid, 2, Side::Put),
      make_contract_id(uid, eid, 3, Side::Call),
  };
  const std::vector<std::int64_t> qty{2, -1, 3};
  BulkRequest req;
  req.select_kind = BulkSelectKind::ContractList;
  req.uid = uid;
  req.contract_ids = ids;
  req.qty = qty;
  req.risk_mode = BulkRiskMode::B76Greeks;

  auto res = bulk_price(req, b, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& aggs = res.value().aggregates;
  ASSERT_EQ(aggs.size(), 1u);
  EXPECT_DOUBLE_EQ(aggs[0].net_qty, 4.0);

  const Underlying* under = u.get_underlying(uid).value();
  atx::vol::Greeks ref{};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const std::uint16_t sx = cid_strike_idx(ids[i]);
    const Side side = cid_side(ids[i]);
    const double K = under->chains[eid].strikes[sx];
    const double sigma = surf.iv(std::log(K / F), T);
    const auto g = black76_greeks(F, K, T, sigma, r, 1.0, side).greeks;
    const double q = static_cast<double>(qty[i]);
    ref.delta += q * g.delta;
    ref.gamma += q * g.gamma;
    ref.vega += q * g.vega;
    ref.theta += q * g.theta;
    ref.rho += q * g.rho;
  }
  EXPECT_NEAR(aggs[0].greeks.delta, ref.delta, 1.0e-8);
  EXPECT_NEAR(aggs[0].greeks.gamma, ref.gamma, 1.0e-8);
  EXPECT_NEAR(aggs[0].greeks.vega, ref.vega, 1.0e-8);
  EXPECT_NEAR(aggs[0].greeks.theta, ref.theta, 1.0e-8);
  EXPECT_NEAR(aggs[0].greeks.rho, ref.rho, 1.0e-8);
}

// ── American first-order (no correction) applies the spot scaling ────────

TEST(Bulk, AmericanFirstOrderNoCorrectionScaling) {
  Universe u;
  const double T = 0.5;
  const double r = 0.0;  // => q == 0, m == 1, F == spot
  ExpiryId eid = 0;
  const Uid uid = add_underlying(u, "BULK", 100.0, kStrikes, T, eid);
  const double F = 100.0;
  CurveSet cs = make_curves(100.0, r, T);
  VolSurface surf = make_surface(uid, T, F, eid, 0.20 * 0.20 * T, 0.45, -0.20);

  MarketBinding b;
  b.universe = &u;
  b.set_market(uid, UnderlyingMarket{&surf, &cs, nullptr, nullptr});

  BulkRequest req;
  req.select_kind = BulkSelectKind::ChainStrikeRange;
  req.uid = uid;
  req.strike_lo = 80.0;
  req.strike_hi = 120.0;
  req.risk_mode = BulkRiskMode::AmericanFirstOrder;

  auto res = bulk_price(req, b, AggMode::Total);
  ASSERT_TRUE(res.has_value());
  const auto& out = res.value().out;
  ASSERT_EQ(out.size(), 10u);

  const Underlying* under = u.get_underlying(uid).value();
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint16_t sx = cid_strike_idx(out.contract_id[i]);
    const Side side = cid_side(out.contract_id[i]);
    const double K = under->chains[eid].strikes[sx];
    const double sigma = surf.iv(std::log(K / F), T);
    const auto bg = black76_greeks(F, K, T, sigma, r, 1.0, side).greeks;

    EXPECT_EQ(out.status[i], LaneStatus::Ok);
    EXPECT_EQ(out.route[i], static_cast<std::uint8_t>(PricingRoute::B76AlCold));
    // r == q == 0: m == 1, F == spot, so delta/gamma/vega/theta collapse to B76.
    EXPECT_NEAR(out.delta[i], bg.delta, 1.0e-8);
    EXPECT_NEAR(out.gamma[i], bg.gamma, 1.0e-8);
    EXPECT_NEAR(out.vega[i], bg.vega, 1.0e-8);
    EXPECT_NEAR(out.theta[i], bg.theta, 1.0e-8);
    // rho picks up the American forward-carry term T * F * delta.
    EXPECT_NEAR(out.rho[i], bg.rho + T * F * bg.delta, 1.0e-8);
  }
}
