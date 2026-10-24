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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/pricing_executor.hpp"
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
         x.status_count == y.status_count && x.family_nodes == y.family_nodes &&
         x.uneconomic_rows == y.uneconomic_rows && x.short_tenor_rows == y.short_tenor_rows;
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

// The same for the sigma derivative (vega): the cold bundle's vega is a sigma stencil
// whose truncation reaches ~1e-4 relative on long-dated lanes (T ~ 2 y; lane A2).
[[nodiscard]] double richardson_vega(double S, double K, double T, double sigma, double r,
                                     double q, Side side) {
  const std::optional<AlOpts> opts{al_fast_opts()};
  const auto P = [&](double s) { return andersen_lake(S, K, T, s, r, q, side, opts).value(); };
  const double h = 0.01 * sigma;
  const double d_h = (P(sigma + h) - P(sigma - h)) / (2.0 * h);
  const double d_h2 = (P(sigma + 0.5 * h) - P(sigma - 0.5 * h)) / h;
  return (4.0 * d_h2 - d_h) / 3.0;
}

[[nodiscard]] double rel_gap(double a, double b) noexcept {
  const double scale = std::max(std::fabs(b), 1e-300);
  return std::fabs(a - b) / scale;
}

// Every group keeps its family (economics off): the policy of the tests whose subject
// IS a family (its accuracy, its carry set, its degenerate box), whatever the group size.
// `fixed_nodes` restores the pre-A2 family: n_sigma_min = n_sigma = 12 over the 0.25 pad.
[[nodiscard]] FamilyKernelPolicy families_always(bool fixed_nodes = false) {
  FamilyKernelPolicy p;
  p.family_economics = false;
  if (fixed_nodes) {
    p.n_sigma_min = p.n_sigma;
    p.sigma_box_pad = 0.25;
  }
  return p;
}

// Family-vs-cold gaps over the family-served lanes of one (family, cold) frame pair:
// price <= 1e-6 and vega <= 1e-4 relative (G4); a delta over 1e-4 is adjudicated
// against a Richardson reference of the cold price (the cold delta's own stencil error,
// lane A deviation 1), and so is a vega over 1e-4 (the cold vega's sigma stencil on
// long-dated lanes, lane A2).
struct GateStats {
  double max_price{0.0};
  double max_delta{0.0};
  double max_vega{0.0};
  double max_fam_vs_ref{0.0};
  double max_cold_vs_ref{0.0};
  std::size_t adjudicated{0};
  double max_vega_fam_vs_ref{0.0};
  double max_vega_cold_vs_ref{0.0};
  std::size_t vega_adjudicated{0};
  std::size_t compared{0};
};

void expect_family_gate(const PreparedRiskBook &prep, const RiskFrame &fam, const RiskFrame &cold,
                        GateStats &s) {
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
    const std::size_t slot = static_cast<std::size_t>(
        std::lower_bound(prep.uids().begin(), prep.uids().end(), prep.uid()[r]) -
        prep.uids().begin());
    if (gv > 1e-4) {
      // Over the bound: adjudicate against the reference sigma derivative of the cold price.
      const double ref = richardson_vega(fam.uid_spot[slot], prep.strike()[r], fam.T[r],
                                         fam.sigma[r], fam.rate[r], fam.q_eff[r],
                                         prep.side()[r]);
      const double fam_err = std::fabs(fam.vega[r] - ref);
      const double cold_err = std::fabs(cold.vega[r] - ref);
      EXPECT_LE(fam_err, 1e-4 * std::fabs(ref)) << "row " << r;
      EXPECT_LT(fam_err, cold_err) << "row " << r << ": the vega gap must be the cold stencil's";
      s.max_vega_fam_vs_ref = std::max(s.max_vega_fam_vs_ref, fam_err / std::fabs(ref));
      s.max_vega_cold_vs_ref = std::max(s.max_vega_cold_vs_ref, cold_err / std::fabs(ref));
      ++s.vega_adjudicated;
    }
    if (gd > 1e-4) {
      // Over the bound: adjudicate against the reference derivative of the cold price.
      const double ref = richardson_delta(fam.uid_spot[slot], prep.strike()[r], fam.T[r],
                                          fam.sigma[r], fam.rate[r], fam.q_eff[r],
                                          prep.side()[r]);
      const double fam_err = std::fabs(fam.delta[r] - ref);
      const double cold_err = std::fabs(cold.delta[r] - ref);
      EXPECT_LE(fam_err, 1e-4 * std::fabs(ref)) << "row " << r;
      EXPECT_LT(fam_err, cold_err) << "row " << r << ": the gap must be the cold stencil's";
      s.max_fam_vs_ref = std::max(s.max_fam_vs_ref, fam_err / std::fabs(ref));
      s.max_cold_vs_ref = std::max(s.max_cold_vs_ref, cold_err / std::fabs(ref));
      ++s.adjudicated;
    }
    s.max_price = std::max(s.max_price, gp);
    s.max_delta = std::max(s.max_delta, gd);
    s.max_vega = std::max(s.max_vega, gv);
    ++s.compared;
  }
}

void print_gate(const char *what, const GateStats &s) {
  std::cout << "[RiskKernel] " << what << ": lanes=" << s.compared
            << " max|dprice|/price=" << s.max_price << " max|ddelta| rel=" << s.max_delta
            << " max|dvega| rel=" << s.max_vega
            << "; delta lanes over 1e-4 adjudicated by a Richardson reference=" << s.adjudicated
            << " (max family-vs-ref rel=" << s.max_fam_vs_ref
            << ", max cold-vs-ref rel=" << s.max_cold_vs_ref << ")"
            << "; vega lanes over 1e-4 adjudicated=" << s.vega_adjudicated
            << " (max family-vs-ref rel=" << s.max_vega_fam_vs_ref
            << ", max cold-vs-ref rel=" << s.max_vega_cold_vs_ref << ")\n";
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

// Re-pinned by lane A2: the gate runs on BOTH node policies with economics off (every
// group keeps its family, which is the subject here): the pre-A2 fixed 12 nodes and the
// adaptive rule, whose narrow boxes take the rule's smallest count.
TEST(RiskKernel, FamilyMatchesColdInsideBox) {
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  for (const bool fixed : {true, false}) {
    GateStats s;
    std::uint64_t families = 0;
    std::uint64_t nodes = 0;
    for (const bool shifted : {false, true}) {
      for (const CarryFrom carry : {CarryFrom::Surface, CarryFrom::State}) {
        const auto owned = rt::make_state(shifted ? rt::shifted_move() : rt::StateMove{}, carry);
        const RiskFrame fam = evaluate_or_die(RiskKernel::family(families_always(fixed)), prep,
                                              owned->state, RiskNeeds{});
        const RiskFrame cold = evaluate_or_die(RiskKernel::cold(), prep, owned->state, RiskNeeds{});
        EXPECT_GT(fam.counters.families_built, 0u);
        families += fam.counters.families_built;
        nodes += fam.counters.family_nodes;
        expect_family_gate(prep, fam, cold, s);
      }
    }
    print_gate(fixed ? "family (12 nodes) vs cold inside box" : "family (adaptive) vs cold inside box",
               s);
    std::cout << "[RiskKernel]   nodes per family: " << static_cast<double>(nodes) /
                                                            static_cast<double>(families)
              << "\n";
    EXPECT_GE(s.compared, 4u * 50u) << "the family must serve most lanes of the book";
    if (fixed) {
      EXPECT_EQ(nodes, 12u * families);
    } else {
      EXPECT_LT(nodes, 12u * families) << "the adaptive rule must save nodes on this book";
    }
  }
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

// Re-pinned by lane A2: economics off. The subject is the family's carry set; under
// rho the economics (x3 family cost) would price these 2-3-lane groups cold instead.
TEST(RiskKernel, RhoOnlyWithNeeds) {
  const auto owned = rt::make_state(rt::shifted_move());
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  const RiskKernel kernel = RiskKernel::family(families_always());

  RiskNeeds no_rho;
  no_rho.rho = false;
  const RiskFrame off = evaluate_or_die(kernel, prep, owned->state, no_rho);
  EXPECT_EQ(off.counters.carry_sets_built, 0u);
  for (std::size_t r = 0; r < off.size(); ++r) {
    ASSERT_EQ(off.status[r], RiskLaneStatus::Ok);
    EXPECT_EQ(off.rho[r], 0.0) << r;
  }

  RiskNeeds with_rho;
  with_rho.rho = true;
  const RiskFrame on = evaluate_or_die(kernel, prep, owned->state, with_rho);
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
  // Lane A2: the shipped family policy (economics + adaptive nodes), the
  // every-group-keeps-its-family one and the shipped one on the other block schedule, with
  // and without rho, beside the cold kernel. The schedule never moves a bit either.
  for (const bool rho : {true, false}) {
    RiskNeeds needs;
    needs.rho = rho;
    std::optional<RiskFrame> shipped;
    for (const int kind : {0, 1, 2, 3}) {
      FamilyKernelPolicy fp = kind == 1 ? families_always() : FamilyKernelPolicy{};
      if (kind == 3) {
        fp.dynamic_schedule = !fp.dynamic_schedule;
      }
      ColdKernelPolicy cp;
      const auto make = [&] {
        return kind == 2 ? RiskKernel::cold(cp) : RiskKernel::family(fp);
      };
      fp.n_threads = 1;
      cp.n_threads = 1;
      const RiskFrame one = evaluate_or_die(make(), prep, owned->state, needs);
      fp.n_threads = 8;
      cp.n_threads = 8;
      const RiskFrame eight = evaluate_or_die(make(), prep, owned->state, needs);
      EXPECT_TRUE(frames_byte_equal(one, eight)) << "kernel " << kind << " rho " << rho;
      if (kind == 0) {
        shipped = eight;
      } else if (kind == 3) {
        EXPECT_TRUE(frames_byte_equal(*shipped, eight)) << "schedule, rho " << rho;
      }
    }
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
  // Lane A2: the economics constants are validated even while economics is off.
  FamilyKernelPolicy free_nodes;
  free_nodes.family_node_cost_us = 0.0;
  FamilyKernelPolicy nan_cold;
  nan_cold.cold_lane_cost_us = std::numeric_limits<double>::quiet_NaN();
  nan_cold.family_economics = false;
  FamilyKernelPolicy low_min;
  low_min.n_sigma_min = 3;
  FamilyKernelPolicy no_spacing;
  no_spacing.sigma_node_spacing = 0.0;
  for (const FamilyKernelPolicy &bad : {few_nodes, wide_pad, zero_floor, free_nodes, nan_cold,
                                        low_min, no_spacing}) {
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
  // Re-pinned by lane A2: economics off (a one-lane group is priced cold by default,
  // and the degenerate box is only reachable on a family).
  FamilyKernelPolicy no_pad = families_always();
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

namespace {

// One sample of the cost-setup receipt at one (T, side): medians (us) over repetitions.
struct CostSample {
  double geometry_us{0.0};
  std::array<double, 4> prepare_us{}; // AlSigmaFamily::prepare at n = 4 / 8 / 12 / 16
  double cold_us{0.0};                // american_greeks_al lane (vega + charm, no rho)
  double cold_geo_us{0.0};            // the same lane on a prepared geometry
  double family_us{0.0};              // one family_greeks lane on a 12-node family
  [[nodiscard]] double node_slope() const noexcept {
    return (prepare_us[3] - prepare_us[0]) / 12.0;
  }
};

[[nodiscard]] double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2u];
}

// The receipt's loops at one (T, side) on the calling thread, al_fast_opts (the preset a
// marks-only state resolves). Below the family's tau floor (`with_family` false) only the
// cold quantities exist.
[[nodiscard]] CostSample cost_sample(double T, Side side, bool with_family) {
  using Clock = std::chrono::steady_clock;
  const std::optional<AlOpts> opts{al_fast_opts()};
  constexpr double kS = 100.0;
  constexpr double kR = 0.043;
  constexpr double kQ = 0.015;
  constexpr int kReps = 7;
  constexpr int kLanes = 40;
  const auto us_since = [](Clock::time_point t0, int n) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count() /
           static_cast<double>(n);
  };
  const double k_lo = with_family ? 90.0 : 95.0;
  const double k_step = with_family ? 0.5 : 0.25;
  const AlExpiryGeometry geo = prepare_al_geometry(T, kR, kQ, opts);
  CostSample out;
  std::vector<double> geo_us;
  std::vector<double> cold_us;
  std::vector<double> cold_geo_us;
  std::vector<double> fam_us;
  double sink = 0.0;
  if (with_family) {
    AlExpiryGeometry scratch = prepare_al_geometry(T, kR, kQ, opts);
    for (int rep = 0; rep < kReps; ++rep) {
      const auto t0 = Clock::now();
      for (int i = 0; i < 20; ++i) {
        scratch.prepare(T + 1e-9 * i, kR, kQ, opts);
      }
      geo_us.push_back(us_since(t0, 20));
    }
    out.geometry_us = median_of(geo_us);
    const std::array<std::uint16_t, 4> nodes{4, 8, 12, 16};
    for (std::size_t k = 0; k < nodes.size(); ++k) {
      AlSigmaFamily fam(geo, side, kS);
      std::vector<double> v;
      for (int rep = 0; rep < kReps; ++rep) {
        const auto t0 = Clock::now();
        for (int i = 0; i < 10; ++i) {
          EXPECT_TRUE(fam.prepare(0.20 + 1e-6 * i, 0.40, nodes[k]).has_value());
        }
        v.push_back(us_since(t0, 10));
      }
      out.prepare_us[k] = median_of(v);
    }
  }
  AlSigmaFamily fam(geo, side, kS);
  const bool fam_ok = with_family && fam.prepare(0.20, 0.40, 12).has_value();
  const AlSigmaFamilyGreeksMask mask{static_cast<std::uint16_t>(
      AlSigmaFamilyGreeksMask::hedge().bits | AlSigmaFamilyGreeksMask::kVanna |
      AlSigmaFamilyGreeksMask::kVolga | AlSigmaFamilyGreeksMask::kCharm)};
  for (int rep = 0; rep < kReps; ++rep) {
    auto t0 = Clock::now();
    for (int i = 0; i < kLanes; ++i) {
      const double K = k_lo + k_step * i;
      sink += american_greeks_al(kS, K, T, 0.3, kR, kQ, side, opts, true, false, true)
                  .value()
                  .price;
    }
    cold_us.push_back(us_since(t0, kLanes));
    t0 = Clock::now();
    for (int i = 0; i < kLanes; ++i) {
      const double K = k_lo + k_step * i;
      sink += american_greeks_al(geo, kS, K, 0.3, side, true, false, true).value().price;
    }
    cold_geo_us.push_back(us_since(t0, kLanes));
    if (fam_ok) {
      t0 = Clock::now();
      for (int i = 0; i < kLanes; ++i) {
        sink += family_greeks(fam, kS, k_lo + k_step * i, 0.3, mask).price;
      }
      fam_us.push_back(us_since(t0, kLanes));
    }
  }
  out.cold_us = median_of(cold_us);
  out.cold_geo_us = median_of(cold_geo_us);
  out.family_us = fam_ok ? median_of(fam_us) : 0.0;
  EXPECT_TRUE(std::isfinite(sink));
  return out;
}

// The sample at `workers` concurrent executor workers, each running the same loops (the
// in-situ load of a fanned-out kernel: shared caches, turbo, P- and E-cores); the median
// over workers of each quantity.
[[nodiscard]] CostSample cost_in_situ(double T, Side side, bool with_family, unsigned workers) {
  std::vector<CostSample> per(workers);
  pricing_executor().run_blocks(workers, workers, [&](std::size_t w) {
    per[w] = cost_sample(T, side, with_family);
  });
  const auto med = [&per](auto field) {
    std::vector<double> v;
    for (const CostSample &s : per) {
      v.push_back(field(s));
    }
    return median_of(v);
  };
  CostSample m;
  m.geometry_us = med([](const CostSample &s) { return s.geometry_us; });
  for (std::size_t k = 0; k < m.prepare_us.size(); ++k) {
    m.prepare_us[k] = med([k](const CostSample &s) { return s.prepare_us[k]; });
  }
  m.cold_us = med([](const CostSample &s) { return s.cold_us; });
  m.cold_geo_us = med([](const CostSample &s) { return s.cold_geo_us; });
  m.family_us = med([](const CostSample &s) { return s.family_us; });
  return m;
}

} // namespace

// On-demand timing receipt (DISABLED_: wall clocks do not gate on a shared host) for
// the family-economics cost constants (FamilyKernelPolicy::family_node_cost_us /
// cold_lane_cost_us), on ONE thread and IN SITU at 8 concurrent workers (the bench's
// fan-out degree): a short- and a mid-dated expiry, both sides. Prints the per-node slope
// of AlSigmaFamily::prepare over n_sigma (the fixed part separated), the cold-analytic
// lane, the same lane on a prepared geometry, one family_greeks lane and one geometry
// prepare; then the cold lane below the family's tau floor; then the mean slope / cold lane
// per degree, the constants the defaults are taken from.
TEST(RiskKernel, DISABLED_FamilyEconomicsCostSetup) {
  for (const unsigned workers : {1u, 8u}) {
    double slope_sum = 0.0;
    double cold_sum = 0.0;
    int n = 0;
    for (const double T : {0.02, 0.15}) {
      for (const Side side : {Side::Put, Side::Call}) {
        const CostSample s = cost_in_situ(T, side, true, workers);
        std::cout << "[RiskKernel] cost setup workers=" << workers << " T=" << T
                  << " side=" << (side == Side::Put ? "put" : "call")
                  << ": geometry prepare=" << s.geometry_us << " us; family prepare n=4/8/12/16="
                  << s.prepare_us[0] << "/" << s.prepare_us[1] << "/" << s.prepare_us[2] << "/"
                  << s.prepare_us[3] << " us (slope " << s.node_slope() << " us/node, fixed "
                  << s.prepare_us[0] - 4.0 * s.node_slope() << " us); cold lane=" << s.cold_us
                  << " us, on geometry=" << s.cold_geo_us << " us; family lane=" << s.family_us
                  << " us\n";
        slope_sum += s.node_slope();
        cold_sum += s.cold_us;
        ++n;
      }
    }
    for (const double T : {0.0007, 0.005}) {
      for (const Side side : {Side::Put, Side::Call}) {
        const CostSample s = cost_in_situ(T, side, false, workers);
        std::cout << "[RiskKernel] cost setup workers=" << workers << " short T=" << T
                  << " side=" << (side == Side::Put ? "put" : "call") << ": cold lane="
                  << s.cold_us << " us, on geometry=" << s.cold_geo_us << " us\n";
      }
    }
    std::cout << "[RiskKernel] cost setup workers=" << workers
              << ": mean node slope=" << slope_sum / n << " us, mean cold lane=" << cold_sum / n
              << " us, ratio cold/node=" << cold_sum / slope_sum << "\n";
  }
}

namespace {

// Groups of 1, 2, 3 and 12 near-the-money lanes: uid 1 at the 0.30 y expiry (12 calls
// K = 95..117, one put K = 98), uid 2 at the 0.15 y expiry (puts K = 240, 245; calls
// K = 250, 255, 260). No lane sits near the exercise edge.
[[nodiscard]] std::vector<OptionLeg> economics_legs() {
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  for (double K = 95.0; K <= 117.0; K += 2.0) {
    legs.push_back(OptionLeg{id++, 1u, K, rt::expiry_ns(2), Side::Call, 1.0, 100.0});
  }
  legs.push_back(OptionLeg{id++, 1u, 98.0, rt::expiry_ns(2), Side::Put, -2.0, 100.0});
  for (const double K : {240.0, 245.0}) {
    legs.push_back(OptionLeg{id++, 2u, K, rt::expiry_ns(1), Side::Put, 3.0, 100.0});
  }
  for (const double K : {250.0, 255.0, 260.0}) {
    legs.push_back(OptionLeg{id++, 2u, K, rt::expiry_ns(1), Side::Call, -1.0, 100.0});
  }
  return legs;
}

[[nodiscard]] std::size_t group_size_of(const PreparedRiskBook &prep, std::size_t row) {
  for (const RiskGroup &g : prep.groups()) {
    if (row >= g.begin && row < g.end) {
      return g.end - g.begin;
    }
  }
  return 0;
}

[[nodiscard]] bool row_bits_equal(const RiskFrame &a, const RiskFrame &b, std::size_t r) {
  return same_bits(a.price[r], b.price[r]) && same_bits(a.delta[r], b.delta[r]) &&
         same_bits(a.gamma[r], b.gamma[r]) && same_bits(a.vega[r], b.vega[r]) &&
         same_bits(a.theta[r], b.theta[r]) && same_bits(a.rho[r], b.rho[r]) &&
         same_bits(a.vanna[r], b.vanna[r]) && same_bits(a.volga[r], b.volga[r]) &&
         same_bits(a.charm[r], b.charm[r]);
}

} // namespace

// Lane A2 lever 1. At a FIXED 12 nodes and the cost constants 33 us / node, 140 us / cold
// lane (set explicitly: the test pins the rule, not the shipped defaults), a family costs
// 396 us: the 1- and 2-lane groups are priced cold-analytic directly (FamilyUneconomic,
// never ColdFallback, no family built, numbers bit-equal to the cold kernel's), the
// 3- and 12-lane groups keep their family. Under rho the family also builds its carry set
// (x3) and a cold lane its two rate-bumped solves (x7/5): the 3-lane group goes cold too.
// Economics off, no cold route, or the FD cold route: every group is tried as before.
TEST(RiskKernel, FamilyEconomicsRoutesSmallGroupsCold) {
  const auto owned = rt::make_state(rt::shifted_move());
  const RiskBook book = RiskBook::create(economics_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  ASSERT_EQ(prep.n_groups(), 4u);
  FamilyKernelPolicy p;
  p.n_sigma = 12;
  p.n_sigma_min = 12;
  p.family_economics = true;
  p.family_node_cost_us = 33.0;
  p.cold_lane_cost_us = 140.0;

  for (const bool rho : {false, true}) {
    RiskNeeds needs;
    needs.rho = rho;
    const std::size_t min_paying = rho ? 4u : 3u; // the 3-lane group pays only without rho
    const RiskFrame f = evaluate_or_die(RiskKernel::family(p), prep, owned->state, needs);
    const RiskFrame cold = evaluate_or_die(RiskKernel::cold(), prep, owned->state, needs);
    std::size_t uneconomic = 0;
    for (std::size_t r = 0; r < prep.n_unique(); ++r) {
      ASSERT_EQ(f.status[r], RiskLaneStatus::Ok) << r;
      if (group_size_of(prep, r) < min_paying) {
        EXPECT_EQ(f.route[r], RiskRoute::ColdAnalytic) << r;
        EXPECT_TRUE(has_flag(f.flags[r], RiskFlag::FamilyUneconomic)) << r;
        EXPECT_FALSE(has_flag(f.flags[r], RiskFlag::ColdFallback)) << r;
        EXPECT_TRUE(row_bits_equal(f, cold, r)) << "row " << r << " must be the cold kernel's";
        ++uneconomic;
      } else {
        EXPECT_EQ(f.route[r], RiskRoute::Family) << r;
        EXPECT_FALSE(has_flag(f.flags[r], RiskFlag::FamilyUneconomic)) << r;
      }
    }
    EXPECT_EQ(uneconomic, rho ? 6u : 3u);
    EXPECT_EQ(f.counters.uneconomic_rows, uneconomic);
    EXPECT_EQ(f.counters.cold_rows, uneconomic);
    EXPECT_EQ(f.counters.families_built, rho ? 1u : 2u);
    EXPECT_EQ(f.counters.family_nodes, 12u * f.counters.families_built);
    EXPECT_EQ(f.counters.carry_sets_built, rho ? 1u : 0u);

    // Thread count never moves the decision.
    FamilyKernelPolicy p8 = p;
    p8.n_threads = 8;
    FamilyKernelPolicy p1 = p;
    p1.n_threads = 1;
    EXPECT_TRUE(frames_byte_equal(
        evaluate_or_die(RiskKernel::family(p1), prep, owned->state, needs),
        evaluate_or_die(RiskKernel::family(p8), prep, owned->state, needs)));
  }

  // The pre-A2 behaviour is reachable: every group keeps its family.
  FamilyKernelPolicy off = p;
  off.family_economics = false;
  FamilyKernelPolicy no_cold = p;
  no_cold.cold_fallback = false;
  FamilyKernelPolicy fd = p;
  fd.analytic_cold = false;
  for (const FamilyKernelPolicy &q : {off, no_cold, fd}) {
    const RiskFrame f = evaluate_or_die(RiskKernel::family(q), prep, owned->state, RiskNeeds{});
    EXPECT_EQ(f.counters.families_built, 4u);
    EXPECT_EQ(f.counters.uneconomic_rows, 0u);
    EXPECT_EQ(f.counters.family_rows, prep.n_unique());
    for (std::size_t r = 0; r < prep.n_unique(); ++r) {
      EXPECT_FALSE(has_flag(f.flags[r], RiskFlag::FamilyUneconomic)) << r;
    }
  }
}

// Lane A2 lever 3. Below the family's tau floor (kAlSigmaFamilyMinTau = 3 / 365) no family
// exists: those lanes are priced cold and flagged ShortTenor | ColdFallback (counted apart
// from FamilyUneconomic), and every cold-routed lane of the family kernel -- short tenor,
// near edge -- is the cold kernel's row bit for bit, though it is priced on the block's
// prepared geometry. With no cold route the short-tenor lanes stay Refused and keep
// ShortTenor.
TEST(RiskKernel, ShortTenorFlaggedAndCounted) {
  const auto owned = rt::make_state(rt::StateMove{}); // the shifted state rolls 1-day legs
  std::vector<OptionLeg> legs = edge_sweep_legs(); // one 1-day put + a 3-week put sweep
  std::uint64_t id = 1000;
  const std::int64_t one_day = rt::instant_ns(rt::kDayUnits);
  for (const double K : {96.0, 98.0, 100.0, 102.0, 104.0}) {
    legs.push_back(OptionLeg{id++, 1u, K, one_day, Side::Call, 1.0, 100.0});
    legs.push_back(OptionLeg{id++, 1u, K, one_day, Side::Put, -1.0, 100.0});
  }
  const RiskBook book = RiskBook::create(legs, {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  for (const bool rho : {false, true}) {
    RiskNeeds needs;
    needs.rho = rho;
    const RiskFrame f = evaluate_or_die(RiskKernel::family(), prep, owned->state, needs);
    const RiskFrame cold = evaluate_or_die(RiskKernel::cold(), prep, owned->state, needs);
    std::size_t short_rows = 0;
    std::size_t cold_routed = 0;
    for (std::size_t r = 0; r < prep.n_unique(); ++r) {
      ASSERT_EQ(f.status[r], RiskLaneStatus::Ok) << r;
      const bool is_short = f.T[r] < kAlSigmaFamilyMinTau;
      EXPECT_EQ(has_flag(f.flags[r], RiskFlag::ShortTenor), is_short) << r;
      if (is_short) {
        EXPECT_EQ(f.route[r], RiskRoute::ColdAnalytic) << r;
        EXPECT_TRUE(has_flag(f.flags[r], RiskFlag::ColdFallback)) << r;
        EXPECT_FALSE(has_flag(f.flags[r], RiskFlag::FamilyUneconomic)) << r;
        ++short_rows;
      }
      if (f.route[r] == RiskRoute::ColdAnalytic) {
        EXPECT_TRUE(row_bits_equal(f, cold, r)) << "row " << r << " must be the cold kernel's";
        ++cold_routed;
      }
    }
    std::cout << "[RiskKernel] short tenor (rho " << rho << "): short rows=" << short_rows
              << " cold-routed rows=" << cold_routed << "\n";
    EXPECT_EQ(short_rows, 11u);
    EXPECT_GT(cold_routed, short_rows) << "the 3-week sweep's near-edge lanes go cold too";
    EXPECT_EQ(f.counters.short_tenor_rows, short_rows);
    EXPECT_EQ(f.counters.uneconomic_rows, 0u);
  }

  FamilyKernelPolicy no_fallback;
  no_fallback.cold_fallback = false;
  const RiskFrame off =
      evaluate_or_die(RiskKernel::family(no_fallback), prep, owned->state, RiskNeeds{});
  std::size_t refused_short = 0;
  for (std::size_t r = 0; r < prep.n_unique(); ++r) {
    if (!has_flag(off.flags[r], RiskFlag::ShortTenor)) {
      continue;
    }
    EXPECT_EQ(off.status[r], RiskLaneStatus::Refused) << r;
    EXPECT_FALSE(has_flag(off.flags[r], RiskFlag::ColdFallback)) << r;
    ++refused_short;
  }
  EXPECT_EQ(refused_short, 11u);
  EXPECT_EQ(off.counters.short_tenor_rows, 11u);
}

namespace {

// The licensed-shaped book: per (uid, expiry, side) THREE strikes at 0.98 / 1.00 / 1.02
// of spot, so every box is narrow (sigma spread <= 0.05), like the SpiderRock book's
// 3.4 lanes per group.
[[nodiscard]] std::vector<OptionLeg> narrow_legs() {
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  for (const rt::UnderlierSpec &u : rt::kUnderliers) {
    for (std::size_t ei = 0; ei < rt::kExpiryUnits.size(); ++ei) {
      for (const Side side : {Side::Call, Side::Put}) {
        for (const double rel : {0.98, 1.0, 1.02}) {
          legs.push_back(OptionLeg{id++, u.uid, std::round(u.spot * rel * 2.0) / 2.0,
                                   rt::expiry_ns(ei), side, 1.0, 100.0});
        }
      }
    }
  }
  return legs;
}

// The node count of every family the frame's groups built, by the documented rule
// (risk_kernel.hpp "ADAPTIVE NODE COUNT") over each group's box as family_group builds it.
// Valid for a frame whose every lane is family-served (the box spans all its lanes).
[[nodiscard]] std::uint64_t rule_nodes(const PreparedRiskBook &prep, const RiskFrame &fam,
                                       const FamilyKernelPolicy &p, std::size_t &n_at_floor) {
  std::uint64_t total = 0;
  for (const RiskGroup &g : prep.groups()) {
    const auto [lo_it, hi_it] =
        std::minmax_element(fam.sigma.begin() + g.begin, fam.sigma.begin() + g.end);
    const double lo = std::max(*lo_it * (1.0 - p.sigma_box_pad), p.sigma_box_floor);
    const double hi = *hi_it * (1.0 + p.sigma_box_pad);
    const double want = std::ceil(std::log(hi / lo) / p.sigma_node_spacing) + 1.0;
    const double floor = static_cast<double>(std::min(p.n_sigma_min, p.n_sigma));
    const double n = std::clamp(want, floor, static_cast<double>(p.n_sigma));
    n_at_floor += n == floor ? 1u : 0u;
    total += static_cast<std::uint64_t>(n);
  }
  return total;
}

// The long-dated narrow book: per underlier and side THREE strikes in the money (puts at
// 1.05 / 1.07 / 1.09 of spot, calls at 0.91 / 0.93 / 0.95) at T ~ 1.1 y and ~ 2.3 y,
// beyond the fixture surfaces' last pillar (TenorExtrapolated; both kernels see the same
// sigma). The licensed store's long-dated put groups (T 0.5 - 2.3 y, three lanes, log box
// width ~ 0.2) are where a 4-node family missed the price gate (lane A2 report).
[[nodiscard]] std::vector<OptionLeg> long_narrow_legs() {
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  for (const rt::UnderlierSpec &u : rt::kUnderliers) {
    for (const std::int64_t units : {std::int64_t{18000}, std::int64_t{37700}}) {
      for (const Side side : {Side::Put, Side::Call}) {
        for (const double rel : {0.05, 0.07, 0.09}) {
          const double m = side == Side::Put ? 1.0 + rel : 1.0 - rel;
          legs.push_back(OptionLeg{id++, u.uid, std::round(u.spot * m * 2.0) / 2.0,
                                   rt::instant_ns(units), side, 1.0, 100.0});
        }
      }
    }
  }
  return legs;
}

} // namespace

// The node ceiling caps the floor: n_sigma = 4 under the default n_sigma_min (5) is a valid
// policy whose every family takes 4 nodes, and n_sigma_min above n_sigma (or equal) gives
// the fixed count n_sigma. Only n_sigma_min below 4 is refused.
TEST(RiskKernel, NodeFloorCappedByCeiling) {
  const auto owned = rt::make_state(rt::StateMove{});
  const RiskBook book = RiskBook::create(narrow_legs(), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  for (const auto &[n_sigma, n_min] : {std::pair<std::uint16_t, std::uint16_t>{4, 5},
                                       {8, 9},
                                       {6, 16},
                                       {12, 12}}) {
    FamilyKernelPolicy p = families_always();
    p.n_sigma = n_sigma;
    p.n_sigma_min = n_min;
    const RiskFrame f = evaluate_or_die(RiskKernel::family(p), prep, owned->state, RiskNeeds{});
    ASSERT_GT(f.counters.families_built, 0u);
    EXPECT_EQ(f.counters.family_nodes, n_sigma * f.counters.families_built)
        << "n_sigma " << n_sigma << " n_sigma_min " << n_min;
    EXPECT_EQ(f.counters.family_rows + f.counters.exercised_rows + f.counters.cold_rows,
              prep.n_unique());
  }
}

// Lane A2 lever 2. The adaptive node count at the shipped spacing keeps the
// family-vs-cold gate (price 1e-6, vega 1e-4, delta 1e-4 or adjudicated) on the narrow
// licensed-shaped boxes, which the rule solves at its SMALLEST count (n_sigma_min) but for
// the few whose sigmas spread wider, on the standard fixture book's wider boxes and on the
// long-dated narrow boxes (the regime that binds the calibration), over four states each.
// Every family takes exactly the documented rule's count. Economics off: every group keeps
// its family, so every lane is a family number under test.
TEST(RiskKernel, AdaptiveNodeCountKeepsPriceGate) {
  const FamilyKernelPolicy p = families_always();
  for (const int kind : {0, 1, 2}) {
    const bool narrow = kind == 0;
    const RiskBook book =
        RiskBook::create(kind == 0   ? narrow_legs()
                         : kind == 1 ? rt::make_option_legs()
                                     : long_narrow_legs(),
                         {})
            .value();
    const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
    GateStats s;
    std::uint64_t families = 0;
    std::uint64_t nodes = 0;
    std::uint64_t expected = 0;
    std::size_t at_floor = 0;
    std::size_t edge_cold = 0;
    double max_spread = 0.0;
    for (const bool shifted : {false, true}) {
      for (const CarryFrom carry : {CarryFrom::Surface, CarryFrom::State}) {
        const auto owned = rt::make_state(shifted ? rt::shifted_move() : rt::StateMove{}, carry);
        const RiskFrame fam =
            evaluate_or_die(RiskKernel::family(p), prep, owned->state, RiskNeeds{});
        const RiskFrame cold =
            evaluate_or_die(RiskKernel::cold(), prep, owned->state, RiskNeeds{});
        families += fam.counters.families_built;
        nodes += fam.counters.family_nodes;
        expected += rule_nodes(prep, fam, p, at_floor);
        for (std::size_t r = 0; r < prep.n_unique(); ++r) {
          const bool edge = fam.route[r] == RiskRoute::ColdAnalytic &&
                            has_flag(fam.flags[r], RiskFlag::NearExerciseEdge);
          edge_cold += edge ? 1u : 0u;
        }
        for (const RiskGroup &g : prep.groups()) {
          const auto [lo, hi] =
              std::minmax_element(fam.sigma.begin() + g.begin, fam.sigma.begin() + g.end);
          max_spread = std::max(max_spread, *hi - *lo);
        }
        expect_family_gate(prep, fam, cold, s);
      }
    }
    print_gate(kind == 0   ? "adaptive nodes, narrow boxes"
               : kind == 1 ? "adaptive nodes, fixture book"
                           : "adaptive nodes, long-dated narrow boxes",
               s);
    std::cout << "[RiskKernel]   families=" << families << " nodes=" << nodes
              << " (per family " << static_cast<double>(nodes) / static_cast<double>(families)
              << ", at n_sigma_min " << at_floor << "), max sigma spread per group "
              << max_spread << "\n";
    ASSERT_GT(families, 0u);
    // Every lane is family-served but the long-dated book's exercise-edge lanes (its
    // deepest in-the-money put at T ~ 2.3 y: priced cold and flagged, as it must be).
    EXPECT_EQ(s.compared + edge_cold, 4u * prep.n_unique());
    if (kind != 2) {
      EXPECT_EQ(edge_cold, 0u);
    }
    EXPECT_LE(edge_cold, 4u);
    EXPECT_EQ(nodes, expected) << "every family takes the documented rule's count";
    if (narrow) {
      EXPECT_LE(max_spread, 0.05) << "the premise: licensed-shaped narrow boxes";
      EXPECT_GE(10u * at_floor, 9u * families) << "most narrow boxes take n_sigma_min";
      EXPECT_LE(nodes, static_cast<std::uint64_t>(p.n_sigma_min + 1u) * families);
    } else {
      EXPECT_LT(nodes, static_cast<std::uint64_t>(p.n_sigma) * families);
    }
  }
}

// On-demand calibration receipt (DISABLED_: a sweep, not a gate) for
// FamilyKernelPolicy::sigma_node_spacing / n_sigma_min / sigma_box_pad. Economics off, so
// every lane is a family number. At a FIXED node count n (n_sigma_min = n_sigma = n) and a
// pad, per book (narrow licensed-shaped, fixture, long-dated narrow) at four states each,
// every group's worst family-vs-cold relative price gap is binned by the group's effective
// node spacing h_eff = log(hi / lo) / (n - 1) (its box as family_group builds it). The rule
// n = ceil(log(hi / lo) / h) + 1 gives every group h_eff <= h, and more nodes at the same
// h_eff are more accurate, so the smallest count binds; the gap also grows with T, which
// is why the long-dated book (and the bench's licensed store) set n_sigma_min, not the
// short-dated fixtures.
TEST(RiskKernel, DISABLED_AdaptiveNodeSpacingSweep) {
  const RiskBook narrow_book = RiskBook::create(narrow_legs(), {}).value();
  const RiskBook fixture_book = RiskBook::create(rt::make_option_legs(), {}).value();
  const RiskBook long_book = RiskBook::create(long_narrow_legs(), {}).value();
  const std::array<PreparedRiskBook, 3> books{PreparedRiskBook::create(narrow_book).value(),
                                              PreparedRiskBook::create(fixture_book).value(),
                                              PreparedRiskBook::create(long_book).value()};
  std::vector<std::unique_ptr<rt::OwnedState>> states;
  for (const bool shifted : {false, true}) {
    for (const CarryFrom carry : {CarryFrom::Surface, CarryFrom::State}) {
      states.push_back(rt::make_state(shifted ? rt::shifted_move() : rt::StateMove{}, carry));
    }
  }
  constexpr std::array<double, 9> kEdges{0.05, 0.075, 0.1, 0.125, 0.15, 0.175, 0.2, 0.3, 1e9};
  constexpr std::array<const char *, 3> kBookNames{"narrow", "fixture", "long-dated"};
  for (const double pad : {0.25, 0.15, 0.10, 0.05}) {
    for (const std::uint16_t n : {4, 5, 6, 7, 8}) {
      FamilyKernelPolicy p = families_always();
      p.sigma_box_pad = pad;
      p.n_sigma = n;
      p.n_sigma_min = n;
      for (std::size_t bi = 0; bi < books.size(); ++bi) {
        const PreparedRiskBook &prep = books[bi];
        std::array<double, kEdges.size()> worst{};
        std::array<std::size_t, kEdges.size()> groups{};
        double w_min = 1e9;
        double w_max = 0.0;
        for (const auto &owned : states) {
          const RiskFrame fam = evaluate_or_die(RiskKernel::family(p), prep, owned->state, {});
          const RiskFrame cold = evaluate_or_die(RiskKernel::cold(), prep, owned->state, {});
          for (const RiskGroup &g : prep.groups()) {
            double smin = 1e9;
            double smax = 0.0;
            double err = 0.0;
            for (std::uint32_t r = g.begin; r < g.end; ++r) {
              if (fam.route[r] != RiskRoute::Family) {
                continue;
              }
              smin = std::min(smin, fam.sigma[r]);
              smax = std::max(smax, fam.sigma[r]);
              err = std::max(err, rel_gap(fam.price[r], cold.price[r]));
            }
            if (smax <= 0.0 || smax <= smin) {
              continue; // no family lane, or a one-lane (degenerate-box) group
            }
            const double w = std::log(smax * (1.0 + pad) / std::max(smin * (1.0 - pad), 0.02));
            w_min = std::min(w_min, w);
            w_max = std::max(w_max, w);
            const double h_eff = w / static_cast<double>(n - 1);
            std::size_t b = 0;
            while (b + 1 < kEdges.size() && h_eff > kEdges[b]) {
              ++b;
            }
            worst[b] = std::max(worst[b], err);
            ++groups[b];
          }
        }
        std::cout << "[RiskKernel] sweep pad=" << pad << " n=" << n << " " << kBookNames[bi]
                  << " w=[" << w_min << ", " << w_max << "] worst rel price by h_eff <=";
        for (std::size_t b = 0; b < kEdges.size(); ++b) {
          if (groups[b] != 0u) {
            std::cout << " " << kEdges[b] << ":" << worst[b] << "(" << groups[b] << ")";
          }
        }
        std::cout << "\n";
      }
    }
  }
}
