// MarketState (api/risk/market_state.hpp): marks-before-surface sigma resolution,
// the carry precedence (term_carry row > flat / curve + escrowed cash dividends,
// or the surface's own carry), Rolled / ModelUnavailable refusals, and the
// surfaces-only convenience builder.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "atx/vol/api/risk/market_state.hpp"
#include "support/risk_synthetic_market.hpp"

using namespace atx::vol;
namespace rt = atx::vol::risk_test;

namespace {

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// A marks-only underlier (no surface) with State carry.
[[nodiscard]] UnderlierState marks_only_underlier(std::uint32_t uid) {
  UnderlierState u;
  u.uid = uid;
  u.spot = 100.0;
  u.flat_rate = 0.05;
  u.flat_q = 0.01;
  u.carry = CarryFrom::State;
  return u;
}

[[nodiscard]] MarketStateStorage::MarksColumns one_mark(std::uint32_t uid, double K,
                                                        std::int64_t expiry, Side side,
                                                        double sigma) {
  MarketStateStorage::MarksColumns m;
  m.uid = uid;
  m.strike = {K};
  m.expiry_ns = {expiry};
  m.side = {side};
  m.sigma = {sigma};
  return m;
}

} // namespace

TEST(MarketState, ResolvesMarksBeforeSurface) {
  const auto owned = rt::make_state(rt::StateMove{});
  MarketStateStorage st = owned->storage;
  // uid 1 has a surface AND a mark for (expiry 1, put, K = 100).
  st.marks.push_back(one_mark(1u, 100.0, rt::expiry_ns(1), Side::Put, 0.3333));
  // uid 9 is marks-only, with vendor parity columns.
  st.underliers.push_back(marks_only_underlier(9u));
  MarketStateStorage::MarksColumns m9;
  m9.uid = 9u;
  // Sorted by (expiry, side, strike): Call (0) before Put (1).
  m9.strike = {105.0, 95.0};
  m9.expiry_ns = {rt::expiry_ns(1), rt::expiry_ns(1)};
  m9.side = {Side::Call, Side::Put};
  m9.sigma = {0.21, 0.25};
  m9.vendor_price = {2.25, 1.5};
  m9.vendor_delta = {0.4, -0.3};
  st.marks.push_back(m9);
  const MarketState state = make_market_state(st).value();

  const ResolvedContract hit = state.resolve(1u, 100.0, rt::expiry_ns(1), Side::Put);
  ASSERT_EQ(hit.status, RiskLaneStatus::Ok);
  EXPECT_EQ(hit.sigma, 0.3333);
  EXPECT_FALSE(has_flag(hit.flags, RiskFlag::MarkMissing));
  EXPECT_FALSE(has_flag(hit.flags, RiskFlag::SurfaceMissing));
  EXPECT_NE(hit.sigma, owned->surfaces[0].iv(100.0, hit.T))
      << "the test needs a mark that differs from the surface";

  const ResolvedContract mo = state.resolve(9u, 105.0, rt::expiry_ns(1), Side::Call);
  ASSERT_EQ(mo.status, RiskLaneStatus::Ok);
  EXPECT_EQ(mo.sigma, 0.21);
  EXPECT_TRUE(has_flag(mo.flags, RiskFlag::SurfaceMissing));
  EXPECT_TRUE(has_flag(mo.flags, RiskFlag::VendorParityAvailable));
  EXPECT_EQ(mo.vendor_price, 2.25);
  EXPECT_EQ(mo.vendor_delta, 0.4);
  EXPECT_EQ(mo.spot, 100.0); // spot is always the underlier's

  // A mark is T-independent in sigma_at and flag-free.
  const MarketState::SigmaAt sa = state.sigma_at(9u, 95.0, rt::expiry_ns(1), Side::Put, 0.7);
  EXPECT_EQ(sa.status, RiskLaneStatus::Ok);
  EXPECT_EQ(sa.sigma, 0.25);
  EXPECT_EQ(sa.flags, 0u);
}

TEST(MarketState, MissingMarkFallsThroughToSurfaceWithFlag) {
  const auto owned = rt::make_state(rt::StateMove{});
  MarketStateStorage st = owned->storage;
  st.marks.push_back(one_mark(1u, 100.0, rt::expiry_ns(1), Side::Put, 0.3333));
  const MarketState state = make_market_state(st).value();
  const PricedSurface &surf = owned->surfaces[0];

  // Same strike, other side: not marked -> surface, flagged MarkMissing.
  const ResolvedContract rc = state.resolve(1u, 100.0, rt::expiry_ns(1), Side::Call);
  ASSERT_EQ(rc.status, RiskLaneStatus::Ok);
  EXPECT_TRUE(same_bits(rc.sigma, surf.iv(100.0, rc.T)));
  EXPECT_TRUE(has_flag(rc.flags, RiskFlag::MarkMissing));
  EXPECT_FALSE(has_flag(rc.flags, RiskFlag::TenorExtrapolated));
  EXPECT_TRUE(std::isnan(rc.vendor_price));

  // An expiry past the last pillar is served, flagged TenorExtrapolated.
  const std::int64_t far = rt::instant_ns(12000);
  const ResolvedContract ext = state.resolve(1u, 100.0, far, Side::Call);
  ASSERT_EQ(ext.status, RiskLaneStatus::Ok);
  EXPECT_TRUE(has_flag(ext.flags, RiskFlag::MarkMissing));
  EXPECT_TRUE(has_flag(ext.flags, RiskFlag::TenorExtrapolated));
  EXPECT_TRUE(same_bits(ext.sigma, surf.iv(100.0, ext.T)));

  // sigma_at answers the surface at an explicit T, with the tenor flag.
  const MarketState::SigmaAt sa = state.sigma_at(1u, 100.0, far, Side::Call, 0.9);
  EXPECT_EQ(sa.status, RiskLaneStatus::Ok);
  EXPECT_TRUE(same_bits(sa.sigma, surf.iv(100.0, 0.9)));
  EXPECT_TRUE(has_flag(sa.flags, RiskFlag::TenorExtrapolated));
}

TEST(MarketState, NoModelIsModelUnavailable) {
  MarketStateStorage st;
  st.ts_ns = rt::kT0;
  st.underliers.push_back(marks_only_underlier(5u)); // neither marks nor a surface
  const MarketState state = make_market_state(st).value();

  const ResolvedContract none = state.resolve(5u, 100.0, rt::expiry_ns(0), Side::Put);
  EXPECT_EQ(none.status, RiskLaneStatus::ModelUnavailable);
  EXPECT_TRUE(has_flag(none.flags, RiskFlag::MarkMissing));
  EXPECT_TRUE(has_flag(none.flags, RiskFlag::SurfaceMissing));
  EXPECT_TRUE(std::isnan(none.sigma));
  EXPECT_TRUE(std::isnan(none.forward));

  // A uid absent from the state entirely.
  const ResolvedContract absent = state.resolve(77u, 100.0, rt::expiry_ns(0), Side::Put);
  EXPECT_EQ(absent.status, RiskLaneStatus::ModelUnavailable);
  EXPECT_TRUE(std::isnan(absent.sigma));
  EXPECT_EQ(state.underlier(77u), nullptr);
  EXPECT_EQ(state.marks(77u), nullptr);
  EXPECT_FALSE(state.surface(77u).valid());

  // A marked uid whose key is absent and that has no surface is refused too.
  MarketStateStorage st2 = st;
  st2.marks.push_back(one_mark(5u, 90.0, rt::expiry_ns(0), Side::Put, 0.2));
  const MarketState s2 = make_market_state(st2).value();
  EXPECT_EQ(s2.resolve(5u, 100.0, rt::expiry_ns(0), Side::Put).status,
            RiskLaneStatus::ModelUnavailable);
  EXPECT_EQ(s2.resolve(5u, 90.0, rt::expiry_ns(0), Side::Put).status, RiskLaneStatus::Ok);
  // A bad strike is the contract's fault, not the model's.
  EXPECT_EQ(s2.resolve(5u, -1.0, rt::expiry_ns(0), Side::Put).status,
            RiskLaneStatus::InvalidContract);
  EXPECT_EQ(s2.resolve(5u, std::numeric_limits<double>::quiet_NaN(), rt::expiry_ns(0),
                       Side::Put)
                .status,
            RiskLaneStatus::InvalidContract);
}

TEST(MarketState, RolledWhenExpiryBeforeState) {
  const auto owned = rt::make_state(rt::StateMove{});
  const MarketState &state = owned->state;
  for (const std::int64_t expiry : {rt::kT0 - rt::kUnitNs, rt::kT0}) {
    const ResolvedContract rc = state.resolve(1u, 100.0, expiry, Side::Put);
    EXPECT_EQ(rc.status, RiskLaneStatus::Rolled);
    EXPECT_TRUE(has_flag(rc.flags, RiskFlag::Rolled));
    EXPECT_TRUE(std::isnan(rc.sigma));
  }
  // A state one step past the first expiry rolls that expiry's contracts.
  rt::StateMove late;
  late.time_units = rt::kExpiryUnits[0] + 1;
  const auto later = rt::make_state(late);
  EXPECT_EQ(later->state.resolve(1u, 100.0, rt::expiry_ns(0), Side::Put).status,
            RiskLaneStatus::Rolled);
  EXPECT_EQ(later->state.resolve(1u, 100.0, rt::expiry_ns(1), Side::Put).status,
            RiskLaneStatus::Ok);
}

TEST(MarketState, StateCarryEscrowsDividends) {
  MarketStateStorage st;
  st.ts_ns = rt::kT0;
  st.underliers.push_back(marks_only_underlier(9u));
  // Paid before expiry 1 (counted), after it (not), and in the past (not).
  st.dividend_schedules.push_back({DividendEvent{rt::instant_ns(500), 1.0},
                                   DividendEvent{rt::instant_ns(3000), 1.5},
                                   DividendEvent{rt::instant_ns(-10), 2.0}});
  st.underliers[0].cash_divs = st.dividend_schedules[0];
  MarketStateStorage::MarksColumns m;
  m.uid = 9u;
  m.strike = {100.0, 100.0};
  m.expiry_ns = {rt::expiry_ns(1), rt::expiry_ns(2)};
  m.side = {Side::Put, Side::Put};
  m.sigma = {0.25, 0.25};
  st.marks.push_back(m);
  const MarketState state = make_market_state(st).value();

  const ResolvedContract rc = state.resolve(9u, 100.0, rt::expiry_ns(1), Side::Put);
  ASSERT_EQ(rc.status, RiskLaneStatus::Ok);
  const double T = rt::years(rt::kExpiryUnits[1]);
  EXPECT_EQ(rc.T, T);
  const double r = 0.05;
  const double pv = 1.0 * std::exp(-r * rt::years(500));
  const double F = (100.0 - pv) * std::exp((r - 0.01) * T);
  EXPECT_DOUBLE_EQ(rc.forward, F);
  EXPECT_EQ(rc.rate, r);
  EXPECT_DOUBLE_EQ(rc.q_eff, r - std::log(F / 100.0) / T);
  EXPECT_FALSE(has_flag(rc.flags, RiskFlag::CarryFromSurface));
  // carry_at at the same T is what resolve used.
  const MarketState::Carry c = state.carry_at(9u, T);
  EXPECT_EQ(c.status, RiskLaneStatus::Ok);
  EXPECT_TRUE(same_bits(c.forward, rc.forward));
  EXPECT_TRUE(same_bits(c.q_eff, rc.q_eff));

  // A term_carry row with the exact expiry wins over the schedule.
  MarketStateStorage st2 = st;
  st2.term_carries.push_back({ExpiryCarry{rt::expiry_ns(1), 2.0, 0.06, 0.02}});
  st2.underliers[0].cash_divs = st2.dividend_schedules[0];
  st2.underliers[0].term_carry = st2.term_carries[0];
  const MarketState s2 = make_market_state(st2).value();
  const ResolvedContract row = s2.resolve(9u, 100.0, rt::expiry_ns(1), Side::Put);
  ASSERT_EQ(row.status, RiskLaneStatus::Ok);
  EXPECT_EQ(row.rate, 0.06);
  EXPECT_DOUBLE_EQ(row.forward, (100.0 - 2.0) * std::exp((0.06 - 0.02) * T));
  EXPECT_TRUE(same_bits(s2.carry_at(9u, T).forward, row.forward));
  // Another expiry has no row: back to the schedule (both dividends now inside T).
  const ResolvedContract other = s2.resolve(9u, 100.0, rt::expiry_ns(2), Side::Put);
  ASSERT_EQ(other.status, RiskLaneStatus::Ok);
  const double T2 = rt::years(rt::kExpiryUnits[2]);
  const double pv2 = 1.0 * std::exp(-r * rt::years(500)) + 1.5 * std::exp(-r * rt::years(3000));
  EXPECT_DOUBLE_EQ(other.forward, (100.0 - pv2) * std::exp((r - 0.01) * T2));
}

TEST(MarketState, SurfaceCarryMatchesPricedSurface) {
  const auto owned = rt::make_state(rt::shifted_move());
  for (std::size_t ui = 0; ui < rt::kUnderliers.size(); ++ui) {
    const PricedSurface &surf = owned->surfaces[ui];
    const std::uint32_t uid = rt::kUnderliers[ui].uid;
    for (std::size_t ei = 0; ei < rt::kExpiryUnits.size(); ++ei) {
      const double K = rt::kUnderliers[ui].spot * 1.03;
      const ResolvedContract rc = owned->state.resolve(uid, K, rt::expiry_ns(ei), Side::Call);
      ASSERT_EQ(rc.status, RiskLaneStatus::Ok);
      EXPECT_TRUE(has_flag(rc.flags, RiskFlag::CarryFromSurface));
      EXPECT_TRUE(has_flag(rc.flags, RiskFlag::MarkMissing));
      EXPECT_TRUE(same_bits(rc.forward, surf.forward_at(rc.T)));
      EXPECT_TRUE(same_bits(rc.q_eff, surf.q_eff_at(rc.T)));
      EXPECT_TRUE(same_bits(rc.rate, surf.rate_at(rc.T)));
      EXPECT_TRUE(same_bits(rc.sigma, surf.iv(K, rc.T)));
      EXPECT_TRUE(same_bits(rc.spot, surf.pricing().S));
    }
  }
}

TEST(MarketState, FromSurfacesMatchesPricingContext) {
  const PricedSurface a = rt::make_surface(0, rt::StateMove{});
  const PricedSurface b = rt::make_surface(1, rt::StateMove{});
  const std::vector<SurfaceRef> refs{SurfaceRef{&a}, SurfaceRef{&b}};
  const MarketStateStorage st = market_state_from_surfaces(rt::kT0, TimeSpec{}, refs).value();
  ASSERT_EQ(st.underliers.size(), 2u);
  for (std::size_t i = 0; i < 2; ++i) {
    const PricedSurface &s = i == 0 ? a : b;
    const UnderlierState &u = st.underliers[i];
    EXPECT_EQ(u.uid, s.uid());
    EXPECT_TRUE(same_bits(u.spot, s.pricing().S));
    EXPECT_TRUE(same_bits(u.flat_rate, s.pricing().r));
    EXPECT_EQ(u.carry, CarryFrom::Surface);
  }
  EXPECT_EQ(st.ts_ns, rt::kT0);
  const MarketState state = make_market_state(st).value();
  EXPECT_EQ(state.ts_ns(), rt::kT0);
  EXPECT_EQ(state.n_underliers(), 2u);
  EXPECT_TRUE(state.surface(2u).valid());

  // A timestamp that disagrees with the surfaces' is refused unless allowed.
  const std::int64_t other = rt::instant_ns(rt::kDayUnits);
  EXPECT_FALSE(market_state_from_surfaces(other, TimeSpec{}, refs).has_value());
  const auto allowed = market_state_from_surfaces(other, TimeSpec{}, refs, true);
  ASSERT_TRUE(allowed.has_value());
  EXPECT_EQ(allowed->ts_ns, other);

  // Duplicate uids and null surfaces are refused.
  const std::vector<SurfaceRef> dup{SurfaceRef{&a}, SurfaceRef{&a}};
  EXPECT_FALSE(market_state_from_surfaces(rt::kT0, TimeSpec{}, dup).has_value());
  const std::vector<SurfaceRef> null_ref{SurfaceRef{}};
  EXPECT_FALSE(market_state_from_surfaces(rt::kT0, TimeSpec{}, null_ref).has_value());
}

TEST(MarketState, CreateValidatesMarksAndUids) {
  MarketStateStorage st;
  st.ts_ns = rt::kT0;
  st.underliers.push_back(marks_only_underlier(9u));
  // Unsorted marks.
  MarketStateStorage::MarksColumns m;
  m.uid = 9u;
  m.strike = {105.0, 95.0};
  m.expiry_ns = {rt::expiry_ns(1), rt::expiry_ns(1)};
  m.side = {Side::Put, Side::Put};
  m.sigma = {0.2, 0.3};
  st.marks.push_back(m);
  EXPECT_FALSE(make_market_state(st).has_value());
  // normalize_marks sorts them (and drops exact duplicate keys, keeping the first).
  st.marks[0].strike.push_back(95.0);
  st.marks[0].expiry_ns.push_back(rt::expiry_ns(1));
  st.marks[0].side.push_back(Side::Put);
  st.marks[0].sigma.push_back(0.9);
  st.normalize_marks();
  ASSERT_EQ(st.marks[0].strike.size(), 2u);
  EXPECT_EQ(st.marks[0].strike[0], 95.0);
  EXPECT_EQ(st.marks[0].sigma[0], 0.3);
  ASSERT_TRUE(make_market_state(st).has_value());
  // Ragged spans, a marks uid without an underlier, duplicate underliers: refused.
  MarketStateStorage ragged = st;
  ragged.marks[0].sigma.pop_back();
  EXPECT_FALSE(make_market_state(ragged).has_value());
  MarketStateStorage orphan = st;
  orphan.marks[0].uid = 10u;
  EXPECT_FALSE(make_market_state(orphan).has_value());
  MarketStateStorage dup = st;
  dup.underliers.push_back(marks_only_underlier(9u));
  EXPECT_FALSE(make_market_state(dup).has_value());
  MarketStateStorage bad_spot = st;
  bad_spot.underliers[0].spot = 0.0;
  EXPECT_FALSE(make_market_state(bad_spot).has_value());
}
