// RiskBook / PreparedRiskBook (api/risk/risk_book.hpp): exact-bit dedup of option
// uniques, share legs as their own rows, the grouped ascending-K prepared order and
// its permutations, and invalid legs kept (reported at evaluation, never dropped).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "support/risk_synthetic_market.hpp"

using namespace atx::vol;
namespace rt = atx::vol::risk_test;

namespace {

[[nodiscard]] OptionLeg leg(std::uint64_t id, std::uint32_t uid, double K, std::int64_t expiry,
                            Side side, double qty, double mult = 100.0) {
  return OptionLeg{id, uid, K, expiry, side, qty, mult};
}

} // namespace

TEST(RiskBook, DedupsOnExactBits) {
  const std::int64_t e0 = rt::expiry_ns(0);
  const std::int64_t e1 = rt::expiry_ns(1);
  const double K = 100.0;
  const double K_next = std::nextafter(K, 200.0);
  const std::vector<OptionLeg> legs{
      leg(1, 1, K, e0, Side::Put, 1.0),       // unique 0
      leg(2, 1, K, e0, Side::Put, -3.0),      // same bits -> unique 0
      leg(3, 1, K_next, e0, Side::Put, 1.0),  // one ulp away -> unique 1
      leg(4, 1, K, e1, Side::Put, 1.0),       // other expiry -> unique 2
      leg(5, 1, K, e0, Side::Call, 1.0),      // other side -> unique 3
      leg(6, 2, K, e0, Side::Put, 1.0),       // other uid -> unique 4
      leg(7, 1, 0.0, e0, Side::Put, 1.0),     // +0.0 -> unique 5
      leg(8, 1, -0.0, e0, Side::Put, 1.0),    // -0.0 differs in bits -> unique 6
      leg(9, 1, K, e0, Side::Put, 7.0, 50.0), // multiplier is not part of the key -> 0
  };
  const RiskBook book = RiskBook::create(legs, {}).value();
  EXPECT_EQ(book.n_option_legs(), 9u);
  EXPECT_EQ(book.n_unique_options(), 7u);
  const std::vector<std::uint32_t> expect{0, 0, 1, 2, 3, 4, 5, 6, 0};
  const auto slots = book.option_unique_slot();
  ASSERT_EQ(slots.size(), expect.size());
  for (std::size_t i = 0; i < expect.size(); ++i) {
    EXPECT_EQ(slots[i], expect[i]) << "leg " << i;
  }
  // The unique table keeps the first leg that named it.
  EXPECT_EQ(book.unique_options()[0].id, 1u);
  EXPECT_EQ(book.unique_options()[1].K, K_next);
  ASSERT_EQ(book.uids().size(), 2u);
  EXPECT_EQ(book.uids()[0], 1u);
  EXPECT_EQ(book.uids()[1], 2u);
  EXPECT_EQ(book.n_underliers(), 2u);
  // Legs are echoed in input order.
  for (std::size_t i = 0; i < legs.size(); ++i) {
    EXPECT_EQ(book.option_legs()[i], legs[i]);
  }
}

TEST(RiskBook, SharesAreSeparateRows) {
  const std::vector<OptionLeg> legs{leg(1, 1, 100.0, rt::expiry_ns(0), Side::Put, 1.0)};
  const std::vector<ShareLeg> shares{ShareLeg{10, 1, 100.0}, ShareLeg{11, 1, 100.0},
                                     ShareLeg{12, 3, -5.0}};
  const RiskBook book = RiskBook::create(legs, shares).value();
  EXPECT_EQ(book.n_share_legs(), 3u);
  EXPECT_EQ(book.n_unique_options(), 1u); // shares never dedup against options
  ASSERT_EQ(book.share_legs().size(), 3u);
  EXPECT_EQ(book.share_legs()[1], shares[1]); // identical share legs stay separate
  // uids cover options AND shares.
  ASSERT_EQ(book.uids().size(), 2u);
  EXPECT_EQ(book.uids()[1], 3u);

  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  ASSERT_EQ(prep.share_uid_slot().size(), 3u);
  EXPECT_EQ(prep.share_uid_slot()[0], 0u);
  EXPECT_EQ(prep.share_uid_slot()[1], 0u);
  EXPECT_EQ(prep.share_uid_slot()[2], 1u);
  EXPECT_EQ(prep.uids()[prep.share_uid_slot()[2]], 3u);
}

TEST(RiskBook, PreparedGroupsAscendingK) {
  const RiskBook book = RiskBook::create(rt::make_option_legs(), rt::make_share_legs()).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  EXPECT_EQ(prep.n_unique(), book.n_unique_options());
  EXPECT_EQ(prep.n_groups(), 24u); // 3 uids x 4 expiries x 2 sides
  const auto groups = prep.groups();
  std::uint32_t expect_begin = 0;
  for (std::size_t g = 0; g < groups.size(); ++g) {
    const RiskGroup &grp = groups[g];
    EXPECT_EQ(grp.begin, expect_begin);
    ASSERT_LT(grp.begin, grp.end);
    expect_begin = grp.end;
    EXPECT_EQ(prep.uids()[grp.uid_slot], grp.uid);
    if (g > 0) {
      const RiskGroup &prev = groups[g - 1];
      const auto key = [](const RiskGroup &x) {
        return std::tuple{x.uid, x.expiry_ns, static_cast<int>(x.side)};
      };
      EXPECT_LT(key(prev), key(grp)) << "groups ordered by (uid, expiry, side)";
    }
    for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
      EXPECT_EQ(prep.uid()[r], grp.uid);
      EXPECT_EQ(prep.expiry_ns()[r], grp.expiry_ns);
      EXPECT_EQ(prep.side()[r], grp.side);
      if (r > grp.begin) {
        EXPECT_LT(prep.strike()[r - 1], prep.strike()[r]) << "ascending K inside a group";
      }
    }
  }
  EXPECT_EQ(expect_begin, prep.n_unique());
  // The SoA columns are 64-byte aligned.
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(prep.strike().data()) % 64u, 0u);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(prep.expiry_ns().data()) % 64u, 0u);
  EXPECT_EQ(prep.book_logical_id(), book.logical_id());
}

TEST(RiskBook, ReversePermutationIsBijective) {
  const RiskBook book = RiskBook::create(rt::make_option_legs(), rt::make_share_legs()).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  const auto p2u = prep.prepared_to_unique();
  const auto u2p = prep.unique_to_prepared();
  ASSERT_EQ(p2u.size(), prep.n_unique());
  ASSERT_EQ(u2p.size(), prep.n_unique());
  std::set<std::uint32_t> seen;
  for (std::uint32_t p = 0; p < p2u.size(); ++p) {
    EXPECT_EQ(u2p[p2u[p]], p);
    seen.insert(p2u[p]);
    const OptionLeg &u = book.unique_options()[p2u[p]];
    EXPECT_EQ(prep.strike()[p], u.K);
    EXPECT_EQ(prep.expiry_ns()[p], u.expiry_ns);
    EXPECT_EQ(prep.side()[p], u.side);
    EXPECT_EQ(prep.uid()[p], u.uid);
  }
  EXPECT_EQ(seen.size(), p2u.size());
  // Every leg lands on the prepared row of its own contract, weighted qty * mult.
  const auto legs = book.option_legs();
  ASSERT_EQ(prep.leg_prepared_row().size(), legs.size());
  for (std::size_t i = 0; i < legs.size(); ++i) {
    const std::uint32_t row = prep.leg_prepared_row()[i];
    EXPECT_EQ(prep.strike()[row], legs[i].K);
    EXPECT_EQ(prep.side()[row], legs[i].side);
    EXPECT_EQ(prep.leg_weight()[i], legs[i].qty * legs[i].multiplier);
  }
  // A non-positive / non-finite multiplier weighs as 100.
  const std::vector<OptionLeg> odd{leg(1, 1, 100.0, rt::expiry_ns(0), Side::Put, 2.0, 0.0),
                                   leg(2, 1, 100.0, rt::expiry_ns(0), Side::Put, 2.0,
                                       std::numeric_limits<double>::infinity())};
  const RiskBook b2 = RiskBook::create(odd, {}).value();
  const PreparedRiskBook p2 = PreparedRiskBook::create(b2).value();
  EXPECT_EQ(p2.leg_weight()[0], 200.0);
  EXPECT_EQ(p2.leg_weight()[1], 200.0);
}

TEST(RiskBook, InvalidLegsKeptNotDropped) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::int64_t e1 = rt::expiry_ns(1);
  const std::vector<OptionLeg> legs{
      leg(1, 1, 100.0, e1, Side::Put, 1.0), leg(2, 1, nan, e1, Side::Put, 1.0),
      leg(3, 1, -5.0, e1, Side::Call, 1.0), leg(4, 1, 0.0, e1, Side::Call, 1.0),
      leg(5, 1, 105.0, e1, Side::Call, nan)};
  const RiskBook book = RiskBook::create(legs, {}).value();
  EXPECT_EQ(book.n_option_legs(), 5u);
  EXPECT_EQ(book.n_unique_options(), 5u);
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  EXPECT_EQ(prep.n_unique(), 5u);
  EXPECT_EQ(prep.leg_prepared_row().size(), 5u);

  // Every evaluation reports the bad strikes InvalidContract; the good rows price.
  const auto owned = rt::make_state(rt::StateMove{});
  for (const bool family : {false, true}) {
    RiskKernel kernel = family ? RiskKernel::family() : RiskKernel::cold();
    RiskFrame frame;
    RiskWorkspace ws;
    ASSERT_TRUE(kernel.evaluate(prep, owned->state, RiskNeeds{}, frame, ws).has_value());
    ASSERT_EQ(frame.size(), 5u);
    for (std::size_t i = 0; i < legs.size(); ++i) {
      const std::uint32_t row = prep.leg_prepared_row()[i];
      const bool bad_strike = !(std::isfinite(legs[i].K) && legs[i].K > 0.0);
      if (bad_strike) {
        EXPECT_EQ(frame.status[row], RiskLaneStatus::InvalidContract) << "leg " << i;
        EXPECT_EQ(frame.route[row], RiskRoute::Refused);
        EXPECT_TRUE(std::isnan(frame.price[row]));
      } else {
        EXPECT_EQ(frame.status[row], RiskLaneStatus::Ok) << "leg " << i;
        EXPECT_TRUE(std::isfinite(frame.price[row]));
      }
    }
    EXPECT_EQ(frame.counters.status_count[static_cast<std::size_t>(
                  RiskLaneStatus::InvalidContract)],
              3u);
  }
}
