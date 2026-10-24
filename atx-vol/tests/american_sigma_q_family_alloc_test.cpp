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
// A0 (r6, 2026-10-23): the primary Greek bundle — family_greeks with and without a
// rate-bump set (built outside the counted block: it allocates), refused rows and the
// structure-of-arrays chain entry on its Ok path — is counted too; A0 fix round 1
// (review-A0 m5): so is that entry's InvalidArgument refusal (lengths that differ).
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp"
#include "pricing/american_sigma_q_family.hpp" // K1: the fused entries (internal)
#include "pricing/american_greek_fd_audit.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

TEST(AmericanGreekFdAuditAlloc, SampleObservationAddsNoAllocations) {
  for (const auto side : {Side::Put, Side::Call}) {
    detail::AlGreekFdAudit audit;
    bool ok = false;
    const auto allocations = testkit::count_allocations([&] {
      ok = detail::audit_american_greeks_fd(100.,100.,.5,.25,.05,.03,side,
          detail::AlGreekFdSteps{.1,.001,.0001,.001},audit).has_value();
    });
    EXPECT_TRUE(ok);
    EXPECT_EQ(allocations,0u);
    EXPECT_EQ(audit.sample_attempts,17u);
    EXPECT_EQ(audit.boundary_cold_solves,7u);
    EXPECT_EQ(audit.boundary_price_calls,17u);
  }
}

TEST(AmericanGreekAdmissionAlloc, PartialColdBundlesAndVegaRefusalAllocateNothing) {
  for (const auto side : {Side::Put, Side::Call}) {
    const double S = side == Side::Put ? 772.99 : 811.6395;
    const double K = side == Side::Put ? 811.6395 : 772.99;
    const double r = side == Side::Put ? .04496 : .00162193;
    const double q = side == Side::Put ? .00162193 : .04496;
    const double T = .0682, sigma = .12;
    const auto geometry = prepare_al_geometry(T, r, q);
    ASSERT_TRUE(geometry.prepared());
    const auto warm = american_greeks_al(S, K, T, sigma, r, q, side);
    ASSERT_TRUE(warm);
    std::array<AmericanGreeks, 5> output{};
    unsigned completed = 0;
    const auto allocations = testkit::count_allocations([&] {
      const auto fd = american_greeks_fd(S, K, T, sigma, r, q, side);
      const auto al = american_greeks_al(S, K, T, sigma, r, q, side);
      const auto prepared = american_greeks_al(geometry, S, K, sigma, side);
      if (fd) { output[0] = *fd; ++completed; }
      if (al) { output[1] = *al; ++completed; }
      if (prepared) { output[2] = *prepared; ++completed; }
      for (std::size_t i = 0; i < 2; ++i) {
        const auto isa = i == 0 ? simd::SimdIsa::ForceScalar : simd::SimdIsa::ForceAvx2;
        if (side == Side::Put)
          (void)simd::american_put_greeks_batch(&S, &K, &T, &sigma, &r, &q, 1,
                                               std::nullopt, &output[3+i], isa);
        else
          (void)simd::american_call_greeks_batch(&S, &K, &T, &sigma, &r, &q, 1,
                                                std::nullopt, &output[3+i], isa);
      }
      const auto vega = american_vega_al(S, K, T, sigma, r, q, side);
      completed += !vega && vega.error().code() == ErrorCode::Unavailable ? 1u : 0u;
    });
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(completed, 4u);
    for (const auto &value : output) {
      EXPECT_TRUE(has_field(value.supported, GreekFieldMask::Price));
      EXPECT_TRUE(std::isfinite(value.price));
      EXPECT_FALSE(has_field(value.supported, GreekFieldMask::Gamma));
      EXPECT_EQ(value.reasons[1], AmericanGreekReason::StencilCrossing);
    }
  }
}

TEST(AlSigmaFamilyAlloc, GreekValidityDirectDetachedChainAndShockAllocateNothing) {
  constexpr double S = 772.99, T = 0.0682, K = S * 1.05, sigma = 0.12;
  const auto geometry = prepare_al_geometry(T, 0.04496, 0.001589 + 0.37 * (0.001678 - 0.001589));
  AlSigmaFamily family(geometry, Side::Put, S);
  ASSERT_TRUE(family.prepare(0.10, 0.50, 12));
  const auto detached = family_detach(family);
  using Mask = AlSigmaFamilyGreeksMask;
  const auto warm = family_greeks(family, S, K, sigma, Mask::all());
  ASSERT_TRUE(warm.near_edge);
  std::array<double, 1> strikes{K}, sigmas{sigma}, zero{0.0}, price{}, theta{}, edge{};
  std::array<Side, 1> sides{Side::Put};
  std::array<AlSigmaFamilyStatus, 1> status{};
  std::array<Mask, 1> supported{};
  std::array<AlSigmaFamilyGreekReasons, 1> reason{};
  AlSigmaFamilyGreekColumns cols;
  cols.price = price; cols.theta = theta; cols.edge_sd = edge;
  cols.supported = supported; cols.reason = reason;
  unsigned good = 0;
  const auto allocations = testkit::count_allocations([&] {
    const auto direct = family_greeks(family, S, K, sigma, Mask::all());
    const auto copy = detached.greeks(S, K, sigma, Mask::all());
    const auto refused = family_greeks(family, S, K, 0.9, Mask::all());
    good += direct.reason == warm.reason && copy.reason == warm.reason &&
            refused.supported.bits == 0 ? 1u : 0u;
    good += family_chain_greeks(&family, nullptr, S, strikes, sigmas, sides,
                                Mask::all(), cols, status).has_value() ? 1u : 0u;
    good += family_shock_grid(family, S, K, sigma, zero, zero, Mask::all(), cols,
                              status).has_value() ? 1u : 0u;
    std::array<Mask, 2> bad_support{};
    cols.supported = bad_support;
    good += !family_chain_greeks(&family, nullptr, S, strikes, sigmas, sides,
                                 Mask::all(), cols, status).has_value() ? 1u : 0u;
    good += !family_shock_grid(family, S, K, sigma, zero, zero, Mask::all(), cols,
                               status).has_value() ? 1u : 0u;
  });
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(good, 5u);
  EXPECT_EQ(reason[0], warm.reason);
  EXPECT_TRUE(std::isnan(theta[0]));
}

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
          sq::QTile tile;
          if (sq::contract_q(table, 0.02, tile) == sq::FusedStatus::Ok) {
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
      EXPECT_EQ(served, 4u * (4u + 1u + 4u));
      EXPECT_GT(sum, 0.0);
    }
  }
}

TEST(AlSigmaQFamilyAlloc, GreeksBundlePathAllocatesNothing) {
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  for (const Side side : {Side::Put, Side::Call}) {
    for (const std::uint16_t n_q : {std::uint16_t{1}, std::uint16_t{4}}) {
      AlSigmaQFamily fam(g, side, 100.0);
      const double q_hi = (n_q == 1) ? 0.02 : 0.021;
      ASSERT_TRUE(fam.prepare(0.1, 0.6, 12, 0.02, q_hi, n_q));
      AlSigmaQFamilyRateBumps bumps; // built (allocating) outside the counted block
      ASSERT_TRUE(bumps.prepare(fam));
      constexpr std::array<double, 4> kStrikes = {70.0, 95.0, 100.0, 120.0};
      constexpr std::array<double, 4> kSigmas = {0.1, 0.23, 0.41, 0.59};
      constexpr std::array<double, 4> kQs = {0.02, 0.02, 0.5, 0.02}; // lane 2 refuses
      constexpr std::array<std::uint8_t, 4> kActive = {1, 1, 1, 0};
      std::array<double, 4> price{};
      std::array<double, 4> delta{};
      std::array<double, 4> rho{};
      std::array<AlSigmaQFamilyStatus, 4> status{};
      std::array<AlFamilyGreekMask, 4> supported{};
      std::array<AlFamilyGreekReasons, 4> reason{};
      AlSigmaQGreekColumns cols;
      cols.price = price;
      cols.delta = delta;
      cols.rho = rho;
      cols.status = status;
      cols.supported = supported;
      cols.reason = reason;
      AlSigmaQGreeksRequest req;
      req.mask = AlFamilyGreekMask::all();
      double sum = 0.0;
      unsigned served = 0;
      const std::uint64_t n = testkit::count_allocations([&] {
        for (const double sigma : kSigmas) {
          for (const double K : kStrikes) {
            const AlSigmaQFamilyGreeks a = family_greeks(fam, 100.0, K, sigma, 0.02, req);
            const AlSigmaQFamilyGreeks b = family_greeks(fam, bumps, 100.0, K, sigma, 0.02, req);
            const AlSigmaQFamilyGreeks r = family_greeks(fam, bumps, 100.0, K, sigma, 0.5, req);
            if (a.status == AlSigmaQFamilyStatus::Ok && b.status == AlSigmaQFamilyStatus::Ok &&
                r.status == AlSigmaQFamilyStatus::YieldOutsideBox) {
              sum += a.price + std::abs(a.delta) + std::abs(b.rho); // put Δ, rho are < 0
              ++served;
            }
          }
          if (family_greeks_many(fam, 100.0, kStrikes, kSigmas, kQs, kActive, cols, req, &bumps)) {
            sum += price[0] + std::abs(rho[1]);
            ++served;
          }
        }
      });
      EXPECT_EQ(n, 0u) << (side == Side::Put ? "put" : "call") << " n_q=" << n_q;
      EXPECT_EQ(served, 4u * (4u + 1u));
      EXPECT_GT(sum, 0.0);
      EXPECT_EQ(status[2], AlSigmaQFamilyStatus::YieldOutsideBox);
      // The refusal path: K shorter than sigma / q (nothing written, typed, no allocation).
      constexpr std::array<double, 3> kShortK = {70.0, 95.0, 100.0};
      bool refused = false;
      const std::uint64_t n_err = testkit::count_allocations([&] {
        const Status st =
            family_greeks_many(fam, 100.0, kShortK, kSigmas, kQs, kActive, cols, req, &bumps);
        refused = !st && st.error().code() == atx::core::ErrorCode::InvalidArgument;
      });
      EXPECT_TRUE(refused);
      EXPECT_EQ(n_err, 0u) << (side == Side::Put ? "put" : "call") << " n_q=" << n_q
                           << " (the InvalidArgument refusal)";
    }
  }
}

} // namespace
} // namespace atx::vol
