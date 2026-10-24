// RiskKernel (api/risk/risk_kernel.hpp): the cold analytic kernel against
// PricedSurface::greeks_analytic (bit for bit), the family kernel against the cold
// kernel inside its box, near-edge routing, the rho / carry-set coupling, thread-count
// determinism, a throughput smoke, and the documented failure paths (cold_fallback
// off, structural refusals, the FD cold route, the degenerate sigma box). The
// allocation bound lives in risk_kernel_alloc_test.cpp (its own executable: it
// replaces global operator new).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "atx/vol/api/risk/pnl_explain.hpp"
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

// One deep ITM put one day out (below the family's tau floor) and a three-week put
// sweep across the exercise boundary, on uid 1 (spot 100).
[[nodiscard]] std::vector<OptionLeg> edge_sweep_legs() {
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  const std::int64_t one_day = rt::instant_ns(rt::kDayUnits);
  legs.push_back(OptionLeg{id++, 1u, 130.0, one_day, Side::Put, 1.0, 100.0});
  const std::int64_t three_weeks = rt::instant_ns(21 * rt::kDayUnits);
  for (double K = 100.0; K <= 140.0; K += 1.0) {
    legs.push_back(OptionLeg{id++, 1u, K, three_weeks, Side::Put, 1.0, 100.0});
  }
  return legs;
}

// A state on the VolTime clock at the fixture's instant (Nov 2023, before the
// convention's covered 2024-2032 window): its TimeSpec refuses every book expiry.
[[nodiscard]] std::unique_ptr<rt::OwnedState> refusing_state() {
  std::unique_ptr<rt::OwnedState> s = rt::make_state(rt::StateMove{});
  s->storage.time.convention = TimeConvention::VolTime;
  s->state = make_market_state(s->storage).value();
  return s;
}

// A structural refusal leaves `out` in clear_values(): every row Refused, no number.
void expect_cleared(const RiskFrame &f) {
  ASSERT_GT(f.size(), 0u);
  for (std::size_t r = 0; r < f.size(); ++r) {
    EXPECT_EQ(f.status[r], RiskLaneStatus::Refused) << r;
    EXPECT_EQ(f.route[r], RiskRoute::None) << r;
    EXPECT_TRUE(std::isnan(f.price[r]) && std::isnan(f.sigma[r])) << r;
  }
}

void expect_invalid_argument(const Status &st) {
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument) << st.error().to_string();
}

template <class T>
[[nodiscard]] bool bytes_equal(const std::vector<T> &a, const std::vector<T> &b) {
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
  const std::vector<OptionLeg> legs = edge_sweep_legs();
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

TEST(RiskKernel, ColdFallbackOffLeavesRefused) {
  const auto owned = rt::make_state(rt::StateMove{});
  const std::vector<OptionLeg> legs = edge_sweep_legs();
  const RiskBook book = RiskBook::create(legs, {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  FamilyKernelPolicy no_fallback;
  no_fallback.cold_fallback = false;
  const RiskFrame on = evaluate_or_die(RiskKernel::family(), prep, owned->state, RiskNeeds{});
  const RiskFrame off =
      evaluate_or_die(RiskKernel::family(no_fallback), prep, owned->state, RiskNeeds{});
  constexpr RiskFlags kCold = static_cast<RiskFlags>(RiskFlag::ColdFallback);
  constexpr RiskFlags kEdge = static_cast<RiskFlags>(RiskFlag::NearExerciseEdge);
  std::size_t refused = 0;
  std::size_t refused_edge = 0;
  for (std::size_t r = 0; r < prep.n_unique(); ++r) {
    if (!has_flag(on.flags[r], RiskFlag::ColdFallback)) {
      // Never handed to the cold route: the same row with or without the fallback.
      EXPECT_EQ(off.route[r], on.route[r]) << r;
      EXPECT_EQ(off.status[r], on.status[r]) << r;
      EXPECT_EQ(off.flags[r], on.flags[r]) << r;
      EXPECT_TRUE(same_bits(off.price[r], on.price[r])) << r;
      continue;
    }
    EXPECT_EQ(off.status[r], RiskLaneStatus::Refused) << r;
    EXPECT_EQ(off.route[r], RiskRoute::Refused) << r;
    EXPECT_TRUE(std::isnan(off.price[r]) && std::isnan(off.delta[r]) && std::isnan(off.T[r]))
        << r;
    EXPECT_FALSE(has_flag(off.flags[r], RiskFlag::ColdFallback)) << r;
    // The family's reasons survive, minus ColdFallback (the fallback side's cold pass
    // may add its own intrinsic NearExerciseEdge bit, so that bit is compared apart).
    EXPECT_EQ(off.flags[r] & ~kEdge, on.flags[r] & ~(kCold | kEdge)) << r;
    ++refused;
    refused_edge += has_flag(off.flags[r], RiskFlag::NearExerciseEdge) ? 1u : 0u;
  }
  std::cout << "[RiskKernel] cold_fallback=false: refused=" << refused
            << " (NearExerciseEdge kept on " << refused_edge << ")\n";
  EXPECT_GT(refused, 0u);
  EXPECT_EQ(refused_edge, 3u) << "the family's edge-band lanes keep NearExerciseEdge";
  EXPECT_EQ(off.counters.refused_rows, refused);
  EXPECT_EQ(off.counters.cold_rows, 0u);

  // explain excludes those legs: no base mark, so premium 0 and no NaN in any total.
  const ExplainResult e =
      explain(prep, owned->state, off, owned->state, off, ExplainOptions{}).value();
  std::size_t excluded = 0;
  for (std::size_t i = 0; i < legs.size(); ++i) {
    if (off.status[prep.leg_prepared_row()[i]] != RiskLaneStatus::Refused) {
      continue;
    }
    EXPECT_EQ(e.status[i], RiskLaneStatus::Refused) << i;
    EXPECT_TRUE(std::isnan(e.pv_base[i]) && std::isnan(e.pnl_total[i])) << i;
    ++excluded;
  }
  const std::size_t k_refused = static_cast<std::size_t>(RiskLaneStatus::Refused);
  EXPECT_EQ(e.excluded_count[k_refused], excluded);
  EXPECT_EQ(e.excluded_premium[k_refused], 0.0);
  EXPECT_EQ(e.total.n_excluded, excluded);
  EXPECT_EQ(e.total.excluded_premium, 0.0);
  EXPECT_TRUE(std::isfinite(e.total.pv_base) && std::isfinite(e.total.pnl_total));
}

TEST(RiskKernel, RefusesForeignWorkspaceAndFrame) {
  const auto owned = rt::make_state(rt::StateMove{});
  const RiskBook book_a = RiskBook::create(rt::make_option_legs(), {}).value();
  const RiskBook book_b = RiskBook::create(rt::make_option_legs(2), {}).value();
  const PreparedRiskBook a = PreparedRiskBook::create(book_a).value();
  const PreparedRiskBook b = PreparedRiskBook::create(book_b).value();
  const RiskKernel kernel = RiskKernel::family();
  RiskWorkspace ws_a;
  RiskFrame frame_a;
  ASSERT_TRUE(kernel.evaluate(a, owned->state, RiskNeeds{}, frame_a, ws_a).has_value());
  RiskWorkspace ws_b;
  RiskFrame frame_b;
  ASSERT_TRUE(kernel.evaluate(b, owned->state, RiskNeeds{}, frame_b, ws_b).has_value());

  // A frame bound to book a, evaluated for book b.
  RiskWorkspace fresh;
  expect_invalid_argument(kernel.evaluate(b, owned->state, RiskNeeds{}, frame_a, fresh));
  expect_cleared(frame_a);
  // A workspace bound to book a, used for book b.
  expect_invalid_argument(kernel.evaluate(b, owned->state, RiskNeeds{}, frame_b, ws_a));
  expect_cleared(frame_b);
  // Rebinding the workspace makes it legal again.
  ws_a.reserve(b);
  EXPECT_TRUE(kernel.evaluate(b, owned->state, RiskNeeds{}, frame_b, ws_a).has_value());
  EXPECT_EQ(frame_b.status[0], RiskLaneStatus::Ok);
}

TEST(RiskKernel, RefusesInvalidPolicyAndTimeSpec) {
  const auto owned = rt::make_state(rt::StateMove{});
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  RiskFrame frame;
  RiskWorkspace ws;
  ASSERT_TRUE(
      RiskKernel::family().evaluate(prep, owned->state, RiskNeeds{}, frame, ws).has_value());

  FamilyKernelPolicy few_nodes;
  few_nodes.n_sigma = 3;
  FamilyKernelPolicy wide_pad;
  wide_pad.sigma_box_pad = 1.0;
  FamilyKernelPolicy zero_floor;
  zero_floor.sigma_box_floor = 0.0;
  for (const FamilyKernelPolicy &bad : {few_nodes, wide_pad, zero_floor}) {
    expect_invalid_argument(
        RiskKernel::family(bad).evaluate(prep, owned->state, RiskNeeds{}, frame, ws));
    expect_cleared(frame);
  }

  // A TimeSpec that refuses a book expiry is structural, on either kernel.
  const auto refusing = refusing_state();
  ASSERT_FALSE(refusing->state.time_to_expiry(rt::expiry_ns(0)).has_value()) << "the premise";
  const std::array<RiskKernel, 2> kernels{RiskKernel::family(), RiskKernel::cold()};
  for (const RiskKernel &kernel : kernels) {
    ASSERT_TRUE(kernel.evaluate(prep, owned->state, RiskNeeds{}, frame, ws).has_value());
    expect_invalid_argument(kernel.evaluate(prep, refusing->state, RiskNeeds{}, frame, ws));
    expect_cleared(frame);
  }
}

TEST(RiskKernel, ColdFdRouteServesEveryLane) {
  const auto owned = rt::make_state(rt::shifted_move());
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  ColdKernelPolicy fd_policy;
  fd_policy.analytic = false;
  const RiskFrame fd =
      evaluate_or_die(RiskKernel::cold(fd_policy), prep, owned->state, RiskNeeds{});
  const RiskFrame al = evaluate_or_die(RiskKernel::cold(), prep, owned->state, RiskNeeds{});
  double max_price = 0.0;
  double max_delta = 0.0;
  for (std::size_t r = 0; r < prep.n_unique(); ++r) {
    ASSERT_EQ(fd.status[r], RiskLaneStatus::Ok) << r;
    EXPECT_EQ(fd.route[r], RiskRoute::ColdFd) << r;
    EXPECT_TRUE(std::isfinite(fd.delta[r]) && std::isfinite(fd.gamma[r]) &&
                std::isfinite(fd.vega[r]) && std::isfinite(fd.theta[r]))
        << r;
    max_price = std::max(max_price, std::fabs(fd.price[r] - al.price[r]) / al.price[r]);
    max_delta = std::max(max_delta, std::fabs(fd.delta[r] - al.delta[r]));
  }
  std::cout << "[RiskKernel] cold FD vs cold analytic: max |dprice|/price=" << max_price
            << " max |ddelta|=" << max_delta << "\n";
  EXPECT_LE(max_price, 1e-9) << "both routes price on andersen_lake";
  EXPECT_LE(max_delta, 1e-3);

  // The family kernel's cold route follows analytic_cold.
  const auto flat = rt::make_state(rt::StateMove{});
  const RiskBook sweep = RiskBook::create(edge_sweep_legs(), {}).value();
  const PreparedRiskBook sweep_prep = PreparedRiskBook::create(sweep).value();
  FamilyKernelPolicy fam_fd;
  fam_fd.analytic_cold = false;
  const RiskFrame f =
      evaluate_or_die(RiskKernel::family(fam_fd), sweep_prep, flat->state, RiskNeeds{});
  std::size_t cold = 0;
  for (std::size_t r = 0; r < sweep_prep.n_unique(); ++r) {
    ASSERT_EQ(f.status[r], RiskLaneStatus::Ok) << r;
    if (has_flag(f.flags[r], RiskFlag::ColdFallback)) {
      EXPECT_EQ(f.route[r], RiskRoute::ColdFd) << r;
      ++cold;
    }
  }
  EXPECT_GT(cold, 0u);
}

TEST(RiskKernel, DegenerateSigmaBoxStillServesFamily) {
  // One contract per group and no pad: the group's sigma range is a single point, so
  // the box is widened symmetrically around it (see family_group).
  const auto owned = rt::make_state(rt::StateMove{});
  const std::vector<OptionLeg> legs{
      OptionLeg{1u, 1u, 105.0, rt::expiry_ns(2), Side::Call, 1.0, 100.0},
      OptionLeg{2u, 1u, 95.0, rt::expiry_ns(2), Side::Put, 1.0, 100.0},
  };
  const RiskBook book = RiskBook::create(legs, {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  FamilyKernelPolicy no_pad;
  no_pad.sigma_box_pad = 0.0;
  const RiskFrame fam =
      evaluate_or_die(RiskKernel::family(no_pad), prep, owned->state, RiskNeeds{});
  const RiskFrame cold = evaluate_or_die(RiskKernel::cold(), prep, owned->state, RiskNeeds{});
  EXPECT_EQ(fam.counters.families_built, 2u);
  for (std::size_t r = 0; r < prep.n_unique(); ++r) {
    ASSERT_EQ(fam.status[r], RiskLaneStatus::Ok) << r;
    EXPECT_EQ(fam.route[r], RiskRoute::Family) << r;
    EXPECT_LE(std::fabs(fam.price[r] - cold.price[r]) / cold.price[r], 1e-6) << r;
    EXPECT_LE(rel_gap(fam.vega[r], cold.vega[r]), 1e-4) << r;
  }
}
