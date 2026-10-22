// Allocation budget of the (σ, q) family's price path: ZERO.
//
// L2h-b sigma-q family (2026-10-21). After prepare(), AlSigmaQFamily::price() and
// price_and_chord() run on the caller's stack (an AlBoundary copy + the barycentric
// weights) and read only const state, so a fan-out over a chain allocates nothing.
// Counted with tests/support/alloc_counter.* (thread-local armed tally over a replaced
// global operator new), linked ONLY into atx-vol-sigma-q-family-alloc-tests, never into
// the monolithic atx-vol-tests or a slice (which must keep the default operator new).
// L2h-c (2026-10-21): price_strikes and self_check() join the counted path (the q box
// narrowed to [0.02, 0.021] so the 4-node family passes the q-axis accuracy guard).
// K1 (r5, 2026-10-22): the fused price + vega entries (the internal K1 block) — the
// one-shot, the strike vector, the q tile and the table path — are counted too; the
// premium table itself is built (and allocates) outside the counted block.
#include <array>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_sigma_q_family.hpp" // K1: the fused entries (internal)
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

TEST(AlSigmaQFamilyAlloc, PricePathAllocatesNothing) {
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  for (const Side side : {Side::Put, Side::Call}) {
    for (const std::uint16_t n_q : {std::uint16_t{1}, std::uint16_t{4}}) {
      AlSigmaQFamily fam(g, side, 100.0);
      const double q_hi = (n_q == 1) ? 0.02 : 0.021;
      ASSERT_TRUE(fam.prepare(0.1, 0.6, 12, 0.02, q_hi, n_q, AlSigmaFamilyAxis::LogSigma,
                              AlSigmaQFamilyCheck::OffNode));
      constexpr std::array<double, 4> kStrikes = {70.0, 95.0, 100.0, 120.0};
      std::array<double, 4> batch{};
      double sum = 0.0;
      unsigned priced = 0;
      unsigned batches = 0;
      const std::uint64_t n = testkit::count_allocations([&] {
        for (const double K : kStrikes) {
          for (const double sigma : {0.1, 0.23, 0.41, 0.59}) {
            const AlSigmaQFamilyPrice p = fam.price(100.0, K, sigma, 0.02);
            const AlSigmaQFamilyChord c = fam.price_and_chord(100.0, K, sigma, 0.02);
            const AlSigmaQFamilyPrice refused = fam.price(100.0, K, sigma, 0.5);
            if (p.status == AlSigmaQFamilyStatus::Ok && c.status == AlSigmaQFamilyStatus::Ok &&
                refused.status == AlSigmaQFamilyStatus::YieldOutsideBox) {
              sum += p.price + c.price_sigma_bumped;
              ++priced;
            }
          }
        }
        for (const double sigma : {0.1, 0.23, 0.41, 0.59}) {
          if (fam.price_strikes(100.0, kStrikes, sigma, 0.02, batch) ==
              AlSigmaQFamilyStatus::Ok) {
            sum += batch[0] + batch[3];
            ++batches;
          }
        }
        if (fam.self_check().status == AlSigmaQFamilyStatus::Ok) {
          sum += fam.self_check().max_abs_per_strike;
        }
      });
      EXPECT_EQ(n, 0u) << (side == Side::Put ? "put" : "call") << " n_q=" << n_q;
      EXPECT_EQ(priced, 16u);
      EXPECT_EQ(batches, 4u);
      EXPECT_GT(sum, 0.0);
    }
  }
}

TEST(AlSigmaQFamilyAlloc, FusedPriceVegaPathAllocatesNothing) {
  namespace sq = sigma_q_family_detail;
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  for (const Side side : {Side::Put, Side::Call}) {
    for (const std::uint16_t n_q : {std::uint16_t{1}, std::uint16_t{4}}) {
      AlSigmaQFamily fam(g, side, 100.0);
      const double q_hi = (n_q == 1) ? 0.02 : 0.021;
      ASSERT_TRUE(fam.prepare(0.1, 0.6, 12, 0.02, q_hi, n_q));
      sq::PremiumTable table; // built (allocating) outside the counted block
      ASSERT_EQ(sq::build_premium_table(fam, table), sq::FusedStatus::Ok);
      constexpr std::array<double, 4> kStrikes = {70.0, 95.0, 100.0, 120.0};
      std::array<double, 4> bp{};
      std::array<double, 4> bv{};
      double sum = 0.0;
      unsigned served = 0;
      const std::uint64_t n = testkit::count_allocations([&] {
        for (const double sigma : {0.1, 0.23, 0.41, 0.59}) {
          for (const double K : kStrikes) {
            const sq::FusedPriceVega a = sq::price_and_vega(fam, 100.0, K, sigma, 0.02);
            const sq::FusedPriceVega r = sq::price_and_vega(fam, 100.0, K, sigma, 0.5);
            if (a.status == sq::FusedStatus::Ok && r.status == sq::FusedStatus::OutsideBox) {
              sum += a.price + a.vega;
              ++served;
            }
          }
          if (sq::price_vega_strikes(fam, 100.0, kStrikes, sigma, 0.02, bp, bv) ==
              sq::FusedStatus::Ok) {
            sum += bp[0] + bv[3];
            ++served;
          }
          for (const sq::TileMode mode : {sq::TileMode::Precontracted, sq::TileMode::Contracted}) {
            sq::QTile tile;
            if (sq::contract_q(table, 0.02, tile, mode) != sq::FusedStatus::Ok) {
              continue;
            }
            for (const double K : kStrikes) {
              const sq::FusedPriceVega t = sq::price_vega_on(table, tile, 100.0, K, sigma);
              if (t.status == sq::FusedStatus::Ok) {
                sum += t.price + t.vega;
                ++served;
              }
            }
          }
        }
      });
      EXPECT_EQ(n, 0u) << (side == Side::Put ? "put" : "call") << " n_q=" << n_q;
      EXPECT_EQ(served, 4u * (4u + 1u + 2u * 4u));
      EXPECT_GT(sum, 0.0);
    }
  }
}

} // namespace
} // namespace atx::vol
