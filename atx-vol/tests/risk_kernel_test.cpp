// RiskKernel (api/risk/risk_kernel.hpp): the cold analytic kernel against
// PricedSurface::greeks_analytic (bit for bit), the family kernel against the cold
// kernel inside its box, near-edge routing, the rho / carry-set coupling, thread-count
// determinism and a throughput smoke. The allocation bound lives in
// risk_kernel_alloc_test.cpp (its own executable: it replaces global operator new).

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "atx/vol/api/risk/risk_kernel.hpp"
#include "support/risk_synthetic_market.hpp"

using namespace atx::vol;
namespace rt = atx::vol::risk_test;

namespace {

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] RiskFrame evaluate_or_die(const RiskKernel &kernel, const PreparedRiskBook &book,
                                        const MarketState &state, RiskNeeds needs) {
  RiskFrame frame;
  RiskWorkspace ws;
  const Status st = kernel.evaluate(book, state, needs, frame, ws);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? "" : st.error().to_string());
  return frame;
}

template <class T> [[nodiscard]] bool bytes_equal(const std::vector<T> &a, const std::vector<T> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

[[nodiscard]] bool frames_byte_equal(const RiskFrame &a, const RiskFrame &b) {
  const bool cols =
      bytes_equal(a.T, b.T) && bytes_equal(a.sigma, b.sigma) && bytes_equal(a.forward, b.forward) &&
      bytes_equal(a.q_eff, b.q_eff) && bytes_equal(a.rate, b.rate) &&
      bytes_equal(a.price, b.price) && bytes_equal(a.delta, b.delta) &&
      bytes_equal(a.gamma, b.gamma) && bytes_equal(a.vega, b.vega) &&
      bytes_equal(a.theta, b.theta) && bytes_equal(a.rho, b.rho) &&
      bytes_equal(a.vanna, b.vanna) && bytes_equal(a.volga, b.volga) &&
      bytes_equal(a.charm, b.charm) && bytes_equal(a.vendor_price, b.vendor_price) &&
      bytes_equal(a.vendor_delta, b.vendor_delta) && bytes_equal(a.route, b.route) &&
      bytes_equal(a.flags, b.flags) && bytes_equal(a.status, b.status) &&
      bytes_equal(a.uid_spot, b.uid_spot) && bytes_equal(a.uid_flat_rate, b.uid_flat_rate) &&
      bytes_equal(a.uid_status, b.uid_status) && bytes_equal(a.share_mark, b.share_mark) &&
      bytes_equal(a.share_status, b.share_status);
  // Every counter except the two wall clocks.
  const FrameCounters &x = a.counters;
  const FrameCounters &y = b.counters;
  return cols && a.ts_ns == b.ts_ns && a.book_logical_id == b.book_logical_id &&
         x.families_built == y.families_built && x.carry_sets_built == y.carry_sets_built &&
         x.family_rows == y.family_rows && x.cold_rows == y.cold_rows &&
         x.exercised_rows == y.exercised_rows && x.refused_rows == y.refused_rows &&
         x.status_count == y.status_count;
}

// Reference spot derivative of the cold Andersen-Lake price: a Richardson-extrapolated
// central difference (O(h^4)). The cold bundle's own delta is a single central
// difference over the base boundary, whose h^2 * speed / 6 truncation reaches ~1e-4
// relative on short-dated wings; this is what separates that from a family error.
[[nodiscard]] double richardson_delta(double S, double K, double T, double sigma, double r,
                                      double q, Side side) {
  const std::optional<AlOpts> opts{al_fast_opts()};
  const auto P = [&](double s) { return andersen_lake(s, K, T, sigma, r, q, side, opts).value(); };
  const double h = 0.005 * S;
  const double d_h = (P(S + h) - P(S - h)) / (2.0 * h);
  const double d_h2 = (P(S + 0.5 * h) - P(S - 0.5 * h)) / h;
  return (4.0 * d_h2 - d_h) / 3.0;
}

[[nodiscard]] double rel_gap(double a, double b) noexcept {
  const double scale = std::max(std::fabs(b), 1e-300);
  return std::fabs(a - b) / scale;
}

} // namespace

TEST(RiskKernel, ColdMatchesPricedSurfaceGreeksBitExact) {
  const auto owned = rt::make_state(rt::shifted_move());
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  const RiskNeeds needs{true, true, true}; // every axis, so every column is compared
  const RiskFrame f = evaluate_or_die(RiskKernel::cold(), prep, owned->state, needs);
  std::size_t compared = 0;
  for (std::size_t r = 0; r < prep.n_unique(); ++r) {
    ASSERT_EQ(f.status[r], RiskLaneStatus::Ok) << r;
    EXPECT_EQ(f.route[r], RiskRoute::ColdAnalytic);
    const PricedSurface &surf = owned->surfaces[prep.uid()[r] - 1u];
    const double K = prep.strike()[r];
    const double T = f.T[r];
    EXPECT_TRUE(same_bits(f.sigma[r], surf.iv(K, T)));
    const AmericanGreeks g =
        surf.greeks_analytic(K, T, prep.side()[r], QueryExecution::ColdReference).value();
    EXPECT_TRUE(same_bits(f.price[r], g.price)) << r;
    EXPECT_TRUE(same_bits(f.delta[r], g.delta)) << r;
    EXPECT_TRUE(same_bits(f.gamma[r], g.gamma)) << r;
    EXPECT_TRUE(same_bits(f.vega[r], g.vega)) << r;
    EXPECT_TRUE(same_bits(f.theta[r], g.theta)) << r;
    EXPECT_TRUE(same_bits(f.rho[r], g.rho)) << r;
    EXPECT_TRUE(same_bits(f.vanna[r], g.vanna)) << r;
    EXPECT_TRUE(same_bits(f.volga[r], g.volga)) << r;
    EXPECT_TRUE(same_bits(f.charm[r], g.charm)) << r;
    ++compared;
  }
  EXPECT_EQ(compared, 60u);
}

TEST(RiskKernel, FamilyMatchesColdInsideBox) {
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  double max_price = 0.0;
  double max_delta = 0.0;
  double max_vega = 0.0;
  double max_fam_vs_ref = 0.0;
  double max_cold_vs_ref = 0.0;
  std::size_t adjudicated = 0;
  std::size_t compared = 0;
  for (const bool shifted : {false, true}) {
    for (const CarryFrom carry : {CarryFrom::Surface, CarryFrom::State}) {
      const auto owned = rt::make_state(shifted ? rt::shifted_move() : rt::StateMove{}, carry);
      const RiskFrame fam = evaluate_or_die(RiskKernel::family(), prep, owned->state, RiskNeeds{});
      const RiskFrame cold = evaluate_or_die(RiskKernel::cold(), prep, owned->state, RiskNeeds{});
      EXPECT_GT(fam.counters.families_built, 0u);
      for (std::size_t r = 0; r < prep.n_unique(); ++r) {
        ASSERT_EQ(cold.status[r], RiskLaneStatus::Ok);
        ASSERT_EQ(fam.status[r], RiskLaneStatus::Ok);
        // Inputs are the same resolution on both kernels.
        EXPECT_TRUE(same_bits(fam.sigma[r], cold.sigma[r]));
        if (fam.route[r] != RiskRoute::Family) {
          continue; // cold-routed or exercised lanes are not the family's numbers
        }
        const double gp = std::fabs(fam.price[r] - cold.price[r]) / cold.price[r];
        const double gd = rel_gap(fam.delta[r], cold.delta[r]);
        const double gv = rel_gap(fam.vega[r], cold.vega[r]);
        EXPECT_LE(gp, 1e-6) << "row " << r;
        EXPECT_LE(gv, 1e-4) << "row " << r;
        if (gd > 1e-4) {
          // Over the bound: adjudicate against the reference derivative of the cold price.
          const std::size_t slot = static_cast<std::size_t>(
              std::lower_bound(prep.uids().begin(), prep.uids().end(), prep.uid()[r]) -
              prep.uids().begin());
          const double ref = richardson_delta(fam.uid_spot[slot], prep.strike()[r], fam.T[r],
                                              fam.sigma[r], fam.rate[r], fam.q_eff[r],
                                              prep.side()[r]);
          const double fam_err = std::fabs(fam.delta[r] - ref);
          const double cold_err = std::fabs(cold.delta[r] - ref);
          EXPECT_LE(fam_err, 1e-4 * std::fabs(ref)) << "row " << r;
          EXPECT_LT(fam_err, cold_err) << "row " << r << ": the gap must be the cold stencil's";
          max_fam_vs_ref = std::max(max_fam_vs_ref, fam_err / std::fabs(ref));
          max_cold_vs_ref = std::max(max_cold_vs_ref, cold_err / std::fabs(ref));
          ++adjudicated;
        }
        max_price = std::max(max_price, gp);
        max_delta = std::max(max_delta, gd);
        max_vega = std::max(max_vega, gv);
        ++compared;
      }
    }
  }
  std::cout << "[RiskKernel] family vs cold inside box: lanes=" << compared
            << " max|dprice|/price=" << max_price << " max|ddelta| rel=" << max_delta
            << " max|dvega| rel=" << max_vega
            << "; delta lanes over 1e-4 adjudicated by a Richardson reference=" << adjudicated
            << " (max family-vs-ref rel=" << max_fam_vs_ref
            << ", max cold-vs-ref rel=" << max_cold_vs_ref << ")\n";
  EXPECT_GE(compared, 4u * 50u) << "the family must serve most lanes of the book";
}

TEST(RiskKernel, NearEdgeRoutesCold) {
  const auto owned = rt::make_state(rt::StateMove{});
  // One day to expiry (below the family's tau floor) and three weeks, deep ITM puts
  // swept across the exercise boundary on uid 1 (spot 100).
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  const std::int64_t one_day = rt::instant_ns(rt::kDayUnits);
  legs.push_back(OptionLeg{id++, 1u, 130.0, one_day, Side::Put, 1.0, 100.0});
  const std::int64_t three_weeks = rt::instant_ns(21 * rt::kDayUnits);
  for (double K = 100.0; K <= 140.0; K += 1.0) {
    legs.push_back(OptionLeg{id++, 1u, K, three_weeks, Side::Put, 1.0, 100.0});
  }
  const RiskBook book = RiskBook::create(legs, {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  const RiskFrame f = evaluate_or_die(RiskKernel::family(), prep, owned->state, RiskNeeds{});

  const std::uint32_t row = prep.leg_prepared_row()[0];
  ASSERT_EQ(f.status[row], RiskLaneStatus::Ok);
  EXPECT_NE(f.route[row], RiskRoute::Family);
  const bool cold_edge = has_flag(f.flags[row], RiskFlag::ColdFallback) &&
                         has_flag(f.flags[row], RiskFlag::NearExerciseEdge);
  EXPECT_TRUE(cold_edge || f.route[row] == RiskRoute::Exercised)
      << "route " << to_string(f.route[row]) << " flags " << f.flags[row];

  std::size_t edge_cold = 0;
  std::size_t exercised = 0;
  std::size_t family = 0;
  for (std::size_t i = 1; i < legs.size(); ++i) {
    const std::uint32_t r = prep.leg_prepared_row()[i];
    ASSERT_EQ(f.status[r], RiskLaneStatus::Ok);
    if (has_flag(f.flags[r], RiskFlag::NearExerciseEdge)) {
      EXPECT_NE(f.route[r], RiskRoute::Family) << "K=" << legs[i].K;
      EXPECT_TRUE(has_flag(f.flags[r], RiskFlag::ColdFallback));
      ++edge_cold;
    }
    exercised += f.route[r] == RiskRoute::Exercised ? 1u : 0u;
    family += f.route[r] == RiskRoute::Family ? 1u : 0u;
  }
  std::cout << "[RiskKernel] 3w put sweep K=100..140: family=" << family
            << " exercised=" << exercised << " near-edge cold=" << edge_cold << "\n";
  EXPECT_GT(family, 0u);
  EXPECT_GT(exercised + edge_cold, 0u) << "the sweep must cross the exercise boundary";
}

TEST(RiskKernel, RhoOnlyWithNeeds) {
  const auto owned = rt::make_state(rt::shifted_move());
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();

  RiskNeeds no_rho;
  no_rho.rho = false;
  const RiskFrame off = evaluate_or_die(RiskKernel::family(), prep, owned->state, no_rho);
  EXPECT_EQ(off.counters.carry_sets_built, 0u);
  for (std::size_t r = 0; r < off.size(); ++r) {
    ASSERT_EQ(off.status[r], RiskLaneStatus::Ok);
    EXPECT_EQ(off.rho[r], 0.0) << r;
  }

  RiskNeeds with_rho;
  with_rho.rho = true;
  const RiskFrame on = evaluate_or_die(RiskKernel::family(), prep, owned->state, with_rho);
  EXPECT_GT(on.counters.carry_sets_built, 0u);
  std::size_t nonzero = 0;
  for (std::size_t r = 0; r < on.size(); ++r) {
    ASSERT_EQ(on.status[r], RiskLaneStatus::Ok);
    EXPECT_TRUE(std::isfinite(on.rho[r])) << r;
    nonzero += on.rho[r] != 0.0 ? 1u : 0u;
    // Rho does not move any other column.
    EXPECT_TRUE(same_bits(on.price[r], off.price[r])) << r;
    EXPECT_TRUE(same_bits(on.delta[r], off.delta[r])) << r;
  }
  EXPECT_GT(nonzero, on.size() / 2u);
}

TEST(RiskKernel, ThreadCountBitIdentical) {
  const auto owned = rt::make_state(rt::shifted_move());
  const RiskBook book = RiskBook::create(rt::make_option_legs(), rt::make_share_legs()).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  RiskNeeds needs;
  needs.rho = true;
  for (const bool family : {true, false}) {
    FamilyKernelPolicy fp;
    ColdKernelPolicy cp;
    fp.n_threads = 1;
    cp.n_threads = 1;
    const RiskFrame one = evaluate_or_die(family ? RiskKernel::family(fp) : RiskKernel::cold(cp),
                                          prep, owned->state, needs);
    fp.n_threads = 8;
    cp.n_threads = 8;
    const RiskFrame eight = evaluate_or_die(
        family ? RiskKernel::family(fp) : RiskKernel::cold(cp), prep, owned->state, needs);
    EXPECT_TRUE(frames_byte_equal(one, eight)) << (family ? "family" : "cold");
  }
}

TEST(RiskKernel, FamilyThroughputSmoke) {
  const auto owned = rt::make_state(rt::StateMove{});
  const RiskBook book = RiskBook::create(rt::make_dense_legs(2000), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  ASSERT_GE(prep.n_unique(), 1900u);
  FamilyKernelPolicy fp;
  fp.n_threads = 1;
  ColdKernelPolicy cp;
  cp.n_threads = 1;
  const RiskKernel family = RiskKernel::family(fp);
  RiskFrame f;
  RiskWorkspace ws;
  ASSERT_TRUE(family.evaluate(prep, owned->state, RiskNeeds{}, f, ws).has_value()); // warm
  ASSERT_TRUE(family.evaluate(prep, owned->state, RiskNeeds{}, f, ws).has_value());
  const RiskFrame c = evaluate_or_die(RiskKernel::cold(cp), prep, owned->state, RiskNeeds{});
  const double n = static_cast<double>(prep.n_unique());
  const double fam_us = f.counters.evaluate_seconds / n * 1e6;
  const double cold_us = c.counters.evaluate_seconds / n * 1e6;
  std::cout << "[RiskKernel] throughput (1 thread, " << prep.n_unique()
            << " contracts): family=" << fam_us << " us/contract cold=" << cold_us
            << " us/contract ratio=" << cold_us / fam_us << " family-kernel cold lanes="
            << f.counters.cold_rows << " exercised=" << f.counters.exercised_rows
            << " families=" << f.counters.families_built << "\n";
  EXPECT_LT(fam_us, cold_us / 5.0);
}
