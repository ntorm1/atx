#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp" // american_implied_vol (AlSigmaFamily IV pin)
#include "atx/vol/api/pricing/black76.hpp"     // black76_price (AlSigmaFamily premium check)

// L2b al-geometry (2026-10-19). The prepared expiry geometry (AlExpiryGeometry), the
// boundary handle (AlBoundaryHandle) and the price pack (al_price_pack) promise prices
// BIT-IDENTICAL to the fresh entry points (andersen_lake, american_greeks_al). These
// tests pin that promise over a grid spanning puts and calls, every exercise regime of
// both sides (American, European, the double-continuation refusal), degenerate and
// invalid T / sigma / carry, short and long expiries, deep ITM and OTM strikes, and the
// accurate, Fast-profile, fast-preset and generic-kernel (unspecialized) schemes.

namespace {

using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;
using atx::vol::al_fast_opts;
using atx::vol::al_fast_profile;
using atx::vol::al_price_pack;
using atx::vol::AlBoundaryHandle;
using atx::vol::AlContract;
using atx::vol::AlExpiryGeometry;
using atx::vol::AlOpts;
using atx::vol::american_greeks_al;
using atx::vol::AmericanGreeks;
using atx::vol::andersen_lake;
using atx::vol::kAlPackSigmaBump;
using atx::vol::prepare_al_geometry;
using atx::vol::Side;
using atx::vol::detail::classify_regime;
using atx::vol::detail::ExerciseRegime;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kSpot = 100.0;

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// The pack's and the handle's failure channel is NaN where andersen_lake errors.
[[nodiscard]] double value_or_nan(const Result<double> &r) noexcept { return r ? *r : kNaN; }

// NaN-aware bit identity: two NaNs agree (the failure channel); anything else must
// match to the last bit (so -0.0 != +0.0).
[[nodiscard]] bool identical(double want, double got) noexcept {
  return std::isnan(want) ? std::isnan(got) : same_bits(want, got);
}

// Same outcome as the fresh entry point: both Ok with identical bits, or both errors
// with the same code and the same message.
[[nodiscard]] bool same_outcome(const Result<double> &want, const Result<double> &got) {
  if (want.has_value() != got.has_value()) {
    return false;
  }
  if (want) {
    return same_bits(*want, *got);
  }
  return want.error().code() == got.error().code() &&
         want.error().to_string() == got.error().to_string();
}

[[nodiscard]] bool same_greeks(const Result<AmericanGreeks> &want,
                               const Result<AmericanGreeks> &got) {
  if (want.has_value() != got.has_value()) {
    return false;
  }
  if (!want) {
    return want.error().code() == got.error().code() &&
           want.error().to_string() == got.error().to_string();
  }
  const AmericanGreeks &a = *want;
  const AmericanGreeks &b = *got;
  return same_bits(a.delta, b.delta) && same_bits(a.gamma, b.gamma) &&
         same_bits(a.vega, b.vega) && same_bits(a.theta, b.theta) && same_bits(a.rho, b.rho) &&
         same_bits(a.vanna, b.vanna) && same_bits(a.volga, b.volga) &&
         same_bits(a.charm, b.charm) && same_bits(a.price, b.price);
}

// One EXPECT per grid instead of one per point; the first few mismatches are kept.
struct Tally {
  std::size_t checked = 0;
  std::size_t mismatched = 0;
  std::size_t american = 0; // checked points that priced through a boundary solve
  std::string first;

  void record(bool ok, const std::string &where) {
    ++checked;
    if (!ok) {
      if (mismatched < 5u) {
        first += where;
        first += '\n';
      }
      ++mismatched;
    }
  }
};

[[nodiscard]] std::string describe(double S, double K, double T, double sigma, double r,
                                   double q, Side side, std::size_t scheme) {
  std::ostringstream os;
  os << std::setprecision(17) << (side == Side::Put ? "put" : "call") << " S=" << S
     << " K=" << K << " T=" << T << " sigma=" << sigma << " r=" << r << " q=" << q
     << " scheme#" << scheme;
  return os.str();
}

[[nodiscard]] bool american_regime(Side side, double r, double q) noexcept {
  const double rate = (side == Side::Put) ? r : q;
  const double yield = (side == Side::Put) ? q : r;
  return classify_regime(rate, yield) == ExerciseRegime::American;
}

struct Carry {
  double r;
  double q;
};

// Every regime of both sides: the put orientation is (rate r, yield q), the call's
// McDonald-Schroder internal put is (rate q, yield r).
constexpr std::array<Carry, 8> kCarries{{
    {0.05, 0.0},    // put American, call European
    {0.05, 0.02},   // both American
    {0.03, 0.07},   // both American; put xmax = K r / q
    {0.0, 0.03},    // put European, call American
    {0.0, -0.02},   // put American (rate 0, yield < 0), call European
    {0.04, -0.01},  // put American, call European
    {-0.01, -0.03}, // put double continuation (NotImplemented), call European
    {-0.03, -0.01}, // put European, call double continuation
}};

// Accurate preset, the fast preset, the accurate scheme on the Fast kernel, and a
// (9, 16) scheme that no specialized kernel covers (the generic, unbound geometry).
[[nodiscard]] std::vector<std::optional<AlOpts>> scheme_grid() {
  return {std::nullopt, al_fast_opts(), al_fast_profile(std::nullopt),
          AlOpts{.n_collocation = 9, .n_quadrature = 16}};
}

constexpr std::array<double, 9> kStrikes{35.0, 60.0, 80.0, 95.0, 100.0, 105.0, 120.0, 150.0,
                                         210.0};

// ── AlExpiryGeometry: andersen_lake overload ─────────────────────────────

TEST(AlExpiryGeometry, PricesMatchFreshAndersenLakeBitForBit) {
  constexpr std::array<double, 6> kExpiries{1.0 / 365.0, 7.0 / 365.0, 0.1, 0.5, 2.0, 5.0};
  // 0 and 5e-9: the degenerate sigma closed form; 2e-8: just above it (the frozen-
  // sweep NotImplemented corner for heavy carry); then ordinary to extreme vols.
  constexpr std::array<double, 7> kSigmas{0.0, 5.0e-9, 2.0e-8, 0.04, 0.25, 0.8, 2.5};
  const auto schemes = scheme_grid();
  AlExpiryGeometry g; // ONE object, re-prepared in place for every (T, r, q, opts)
  Tally tally;
  for (std::size_t s = 0; s < schemes.size(); ++s) {
    for (const double T : kExpiries) {
      for (const Carry &c : kCarries) {
        g.prepare(T, c.r, c.q, schemes[s]);
        ASSERT_TRUE(g.prepared());
        ASSERT_TRUE(same_bits(g.T(), T) && same_bits(g.r(), c.r) && same_bits(g.q(), c.q));
        for (const double K : kStrikes) {
          for (const double sigma : kSigmas) {
            for (const Side side : {Side::Call, Side::Put}) {
              const Result<double> want =
                  andersen_lake(kSpot, K, T, sigma, c.r, c.q, side, schemes[s]);
              const Result<double> got = andersen_lake(g, kSpot, K, sigma, side);
              tally.record(same_outcome(want, got),
                           describe(kSpot, K, T, sigma, c.r, c.q, side, s));
              if (want && sigma > 1.0e-8 && american_regime(side, c.r, c.q)) {
                ++tally.american;
              }
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(tally.mismatched, 0u) << tally.mismatched << " of " << tally.checked << "\n"
                                  << tally.first;
  // Not vacuous: thousands of the points ran a prepared boundary solve.
  EXPECT_GT(tally.american, 5000u);
}

TEST(AlExpiryGeometry, DegenerateAndInvalidInputsMatchTheFreshOutcomes) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr std::array<double, 6> kExpiries{0.0, 1.0e-13, -0.25, kNaN, kInf, 0.3};
  constexpr std::array<Carry, 4> kOddCarries{{{0.05, 0.02}, {kNaN, 0.02}, {0.05, kInf},
                                              {0.03, 0.07}}};
  constexpr std::array<std::pair<double, double>, 5> kSpotStrike{
      {{100.0, 90.0}, {100.0, 110.0}, {0.0, 100.0}, {100.0, -1.0}, {kNaN, 100.0}}};
  constexpr std::array<double, 4> kSigmas{0.3, 0.0, -0.1, kNaN};
  AlExpiryGeometry g;
  Tally tally;
  for (const double T : kExpiries) {
    for (const Carry &c : kOddCarries) {
      g.prepare(T, c.r, c.q, std::nullopt);
      for (const auto &[S, K] : kSpotStrike) {
        for (const double sigma : kSigmas) {
          for (const Side side : {Side::Call, Side::Put}) {
            const Result<double> want = andersen_lake(S, K, T, sigma, c.r, c.q, side);
            const Result<double> got = andersen_lake(g, S, K, sigma, side);
            tally.record(same_outcome(want, got), describe(S, K, T, sigma, c.r, c.q, side, 0));
          }
        }
      }
    }
  }
  EXPECT_EQ(tally.mismatched, 0u) << tally.mismatched << " of " << tally.checked << "\n"
                                  << tally.first;
}

TEST(AlExpiryGeometry, UnpreparedAndMovedFromGeometriesRefuse) {
  const AlExpiryGeometry fresh;
  EXPECT_FALSE(fresh.prepared());
  EXPECT_TRUE(std::isnan(fresh.T()));
  const Result<double> unprepared = andersen_lake(fresh, kSpot, 100.0, 0.3, Side::Put);
  ASSERT_FALSE(unprepared);
  EXPECT_EQ(unprepared.error().code(), ErrorCode::InvalidArgument);

  AlExpiryGeometry a = prepare_al_geometry(0.5, 0.05, 0.02);
  AlExpiryGeometry b = std::move(a);
  // The moved-from geometry owns nothing and refuses; the target prices as before.
  EXPECT_FALSE(a.prepared()); // NOLINT(bugprone-use-after-move): the moved-from contract
  const Result<double> moved = andersen_lake(a, kSpot, 100.0, 0.3, Side::Put);
  ASSERT_FALSE(moved);
  EXPECT_EQ(moved.error().code(), ErrorCode::InvalidArgument);
  const Result<double> want = andersen_lake(kSpot, 100.0, 0.5, 0.3, 0.05, 0.02, Side::Put);
  EXPECT_TRUE(same_outcome(want, andersen_lake(b, kSpot, 100.0, 0.3, Side::Put)));
  // prepare() re-allocates a moved-from geometry.
  a.prepare(0.5, 0.05, 0.02);
  EXPECT_TRUE(same_outcome(want, andersen_lake(a, kSpot, 100.0, 0.3, Side::Put)));
}

// ── AlExpiryGeometry: american_greeks_al overload ────────────────────────

TEST(AlExpiryGeometry, GreeksMatchFreshAmericanGreeksAlBitForBit) {
  constexpr std::array<double, 5> kGreekStrikes{60.0, 95.0, 100.0, 105.0, 150.0};
  constexpr std::array<double, 3> kExpiries{7.0 / 365.0, 0.5, 3.0};
  constexpr std::array<double, 3> kSigmas{0.0, 0.15, 0.6}; // 0: InvalidArgument in both
  struct Need {
    bool vega;
    bool rho;
    bool charm;
  };
  constexpr std::array<Need, 3> kNeeds{{{true, true, true}, {false, false, false},
                                        {true, false, true}}};
  const std::vector<std::optional<AlOpts>> schemes{std::nullopt, al_fast_opts(),
                                                   AlOpts{.n_collocation = 9,
                                                          .n_quadrature = 16}};
  AlExpiryGeometry g;
  Tally tally;
  for (std::size_t s = 0; s < schemes.size(); ++s) {
    for (const double T : kExpiries) {
      for (const Carry &c : kCarries) {
        g.prepare(T, c.r, c.q, schemes[s]);
        for (const double K : kGreekStrikes) {
          for (const double sigma : kSigmas) {
            for (const Side side : {Side::Call, Side::Put}) {
              for (const Need &n : kNeeds) {
                const auto want = american_greeks_al(kSpot, K, T, sigma, c.r, c.q, side,
                                                     schemes[s], n.vega, n.rho, n.charm);
                const auto got = american_greeks_al(g, kSpot, K, sigma, side, n.vega, n.rho,
                                                    n.charm);
                tally.record(same_greeks(want, got),
                             describe(kSpot, K, T, sigma, c.r, c.q, side, s));
                if (want && sigma > 0.0 && american_regime(side, c.r, c.q)) {
                  ++tally.american;
                }
              }
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(tally.mismatched, 0u) << tally.mismatched << " of " << tally.checked << "\n"
                                  << tally.first;
  EXPECT_GT(tally.american, 1000u);
}

TEST(AlExpiryGeometry, GreeksOverloadRefusesAnUnpreparedGeometry) {
  const AlExpiryGeometry fresh;
  const auto r = american_greeks_al(fresh, kSpot, 100.0, 0.3, Side::Call);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

// ── AlBoundaryHandle ─────────────────────────────────────────────────────

TEST(AlBoundaryHandle, OneCallBoundaryPricesEveryStrikeAtItsSpotBitForBit) {
  constexpr std::array<Carry, 3> kCallAmerican{{{0.05, 0.02}, {0.03, 0.07}, {0.0, 0.03}}};
  const std::vector<std::optional<AlOpts>> schemes{std::nullopt, al_fast_opts()};
  Tally tally;
  for (std::size_t s = 0; s < schemes.size(); ++s) {
    for (const Carry &c : kCallAmerican) {
      for (const double T : {0.1, 1.5}) {
        const AlExpiryGeometry g = prepare_al_geometry(T, c.r, c.q, schemes[s]);
        AlBoundaryHandle h(g, Side::Call, kSpot);
        EXPECT_TRUE(std::isnan(h.price_from_boundary(kSpot, 100.0))) << "priced before solve";
        for (const double sigma : {0.2, 0.55}) { // re-solved in place
          ASSERT_TRUE(h.solve(sigma));
          for (const double K : kStrikes) {
            const double want =
                value_or_nan(andersen_lake(kSpot, K, T, sigma, c.r, c.q, Side::Call, schemes[s]));
            tally.record(!std::isnan(want) && same_bits(want, h.price_from_boundary(kSpot, K)),
                         describe(kSpot, K, T, sigma, c.r, c.q, Side::Call, s));
          }
          // Another spot is another call boundary: refused, never mispriced.
          EXPECT_TRUE(std::isnan(h.price_from_boundary(kSpot + 1.0, 100.0)));
        }
      }
    }
  }
  EXPECT_EQ(tally.mismatched, 0u) << tally.mismatched << " of " << tally.checked << "\n"
                                  << tally.first;
  EXPECT_EQ(tally.checked, 2u * 3u * 2u * 2u * kStrikes.size());
}

TEST(AlBoundaryHandle, OnePutBoundaryPricesItsStrikeAtEverySpotBitForBit) {
  constexpr std::array<Carry, 4> kPutAmerican{{{0.05, 0.0}, {0.05, 0.02}, {0.03, 0.07},
                                               {0.0, -0.02}}};
  constexpr std::array<double, 7> kSpots{40.0, 80.0, 99.0, 100.0, 101.0, 130.0, 260.0};
  constexpr double kStrike = 100.0;
  const std::vector<std::optional<AlOpts>> schemes{std::nullopt, al_fast_opts()};
  Tally tally;
  for (std::size_t s = 0; s < schemes.size(); ++s) {
    for (const Carry &c : kPutAmerican) {
      for (const double T : {0.1, 1.5}) {
        const AlExpiryGeometry g = prepare_al_geometry(T, c.r, c.q, schemes[s]);
        AlBoundaryHandle h(g, Side::Put, kStrike);
        for (const double sigma : {0.2, 0.55}) {
          ASSERT_TRUE(h.solve(sigma));
          for (const double S : kSpots) {
            const double want =
                value_or_nan(andersen_lake(S, kStrike, T, sigma, c.r, c.q, Side::Put, schemes[s]));
            tally.record(!std::isnan(want) && same_bits(want, h.price_from_boundary(S, kStrike)),
                         describe(S, kStrike, T, sigma, c.r, c.q, Side::Put, s));
          }
          // Another strike is another put boundary.
          EXPECT_TRUE(std::isnan(h.price_from_boundary(kSpot, kStrike + 5.0)));
        }
      }
    }
  }
  EXPECT_EQ(tally.mismatched, 0u) << tally.mismatched << " of " << tally.checked << "\n"
                                  << tally.first;
  EXPECT_EQ(tally.checked, 2u * 4u * 2u * 2u * kSpots.size());
}

TEST(AlBoundaryHandle, ClosedFormRegimesAndRefusalsMatchAndersenLake) {
  // European regime (a call with q = 0): no boundary, the closed form of andersen_lake.
  {
    const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.0);
    AlBoundaryHandle h(g, Side::Call, kSpot);
    ASSERT_TRUE(h.solve(0.3));
    for (const double K : kStrikes) {
      const double want = value_or_nan(andersen_lake(kSpot, K, 0.5, 0.3, 0.05, 0.0, Side::Call));
      EXPECT_TRUE(identical(want, h.price_from_boundary(kSpot, K))) << K;
    }
  }
  // Degenerate sigma: the sigma -> 0 limit, in the American regime too.
  {
    const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
    AlBoundaryHandle h(g, Side::Put, 100.0);
    ASSERT_TRUE(h.solve(0.0));
    for (const double S : {80.0, 100.0, 120.0}) {
      const double want = value_or_nan(andersen_lake(S, 100.0, 0.5, 0.0, 0.05, 0.02, Side::Put));
      EXPECT_TRUE(identical(want, h.price_from_boundary(S, 100.0))) << S;
    }
  }
  // Double continuation (put q < r <= 0): andersen_lake's own refusal.
  {
    const AlExpiryGeometry g = prepare_al_geometry(0.5, -0.01, -0.03);
    AlBoundaryHandle h(g, Side::Put, 100.0);
    const Status st = h.solve(0.3);
    const Result<double> want = andersen_lake(100.0, 100.0, 0.5, 0.3, -0.01, -0.03, Side::Put);
    ASSERT_FALSE(st);
    ASSERT_FALSE(want);
    EXPECT_EQ(st.error().code(), ErrorCode::NotImplemented);
    EXPECT_EQ(st.error().to_string(), want.error().to_string());
    EXPECT_TRUE(std::isnan(h.price_from_boundary(100.0, 100.0)));
  }
  // An unprepared geometry and an invalid anchor refuse like andersen_lake.
  {
    const AlExpiryGeometry unprepared;
    AlBoundaryHandle h(unprepared, Side::Call, kSpot);
    const Status st = h.solve(0.3);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
    AlBoundaryHandle zero(g, Side::Call, 0.0);
    const Status bad = zero.solve(0.3);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
  }
  // A failed re-solve drops the earlier boundary: nothing is priced off it.
  {
    const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
    AlBoundaryHandle h(g, Side::Put, 100.0);
    ASSERT_TRUE(h.solve(0.3));
    EXPECT_FALSE(std::isnan(h.price_from_boundary(100.0, 100.0)));
    ASSERT_FALSE(h.solve(-0.1));
    EXPECT_TRUE(std::isnan(h.price_from_boundary(100.0, 100.0)));
  }
}

// L2f (inherited L2b review m2): a handle observes its geometry, so a temporary
// geometry must not bind. The rvalue constructor is deleted; an lvalue still binds.
static_assert(!std::is_constructible_v<AlBoundaryHandle, AlExpiryGeometry &&, Side, double>,
              "AlBoundaryHandle must not bind a temporary AlExpiryGeometry");
static_assert(!std::is_constructible_v<AlBoundaryHandle, const AlExpiryGeometry &&, Side, double>,
              "AlBoundaryHandle must not bind a const temporary AlExpiryGeometry");
static_assert(std::is_constructible_v<AlBoundaryHandle, const AlExpiryGeometry &, Side, double>);

// L2f (inherited L2b review m2): re-preparing the geometry after solve() moves (T, r, q)
// under the solved boundary. The handle refuses to price (NaN) until it is solved again,
// and the re-solve prices the new expiry bit-identically to andersen_lake.
TEST(AlBoundaryHandle, RePreparedGeometryRefusesUntilSolvedAgain) {
  for (const Side side : {Side::Put, Side::Call}) {
    const double anchor = 100.0;
    const double S = side == Side::Put ? 95.0 : anchor;
    const double K = side == Side::Put ? anchor : 105.0;
    AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.03);
    AlBoundaryHandle h(g, side, anchor);
    ASSERT_TRUE(h.solve(0.3));
    const double before = h.price_from_boundary(S, K);
    ASSERT_TRUE(same_bits(before, *andersen_lake(S, K, 0.5, 0.3, 0.05, 0.03, side)));
    g.prepare(2.0, 0.05, 0.03); // another expiry: the solved boundary is stale
    EXPECT_TRUE(std::isnan(h.price_from_boundary(S, K)))
        << "stale boundary priced: " << h.price_from_boundary(S, K);
    ASSERT_TRUE(h.solve(0.3));
    const double after = h.price_from_boundary(S, K);
    EXPECT_TRUE(same_bits(after, *andersen_lake(S, K, 2.0, 0.3, 0.05, 0.03, side)))
        << after << " vs " << *andersen_lake(S, K, 2.0, 0.3, 0.05, 0.03, side);
    // A re-prepare with IDENTICAL inputs still refuses: the generation, not the
    // values, decides (the handle cannot see what changed).
    g.prepare(2.0, 0.05, 0.03);
    EXPECT_TRUE(std::isnan(h.price_from_boundary(S, K)));
  }
}

// ── al_price_pack ────────────────────────────────────────────────────────

// A chain shaped to exercise every reuse decision the pack makes: calls at one spot
// and one sigma (one shared boundary), calls at one spot with a per-strike smile (no
// sharing across sigma), puts across strikes, puts at one strike across spots (shared),
// call/put alternation, a repeated contract, and contracts andersen_lake refuses or
// prices in closed form (S = 0, NaN sigma, sigma 0, sigma below the degenerate floor).
struct PackInput {
  std::vector<AlContract> contracts;
  std::vector<double> sigma;
};

[[nodiscard]] PackInput pack_chain() {
  PackInput in;
  const auto add = [&in](double S, double K, Side side, double sigma) {
    in.contracts.push_back(AlContract{.S = S, .K = K, .side = side});
    in.sigma.push_back(sigma);
  };
  for (const double K : {70.0, 85.0, 100.0, 115.0, 140.0}) {
    add(kSpot, K, Side::Call, 0.3);
  }
  for (const double K : {70.0, 85.0, 100.0, 115.0, 140.0}) {
    add(kSpot, K, Side::Call, 0.25 + 0.4 * std::abs(std::log(K / kSpot)));
  }
  for (const double K : {60.0, 90.0, 100.0, 110.0, 160.0}) {
    add(kSpot, K, Side::Put, 0.35);
  }
  for (const double S : {70.0, 95.0, 100.0, 105.0, 150.0}) {
    add(S, 100.0, Side::Put, 0.35);
  }
  for (const double K : {95.0, 105.0}) {
    add(kSpot, K, Side::Call, 0.28);
    add(kSpot, K, Side::Put, 0.28);
    add(kSpot, K, Side::Call, 0.28);
  }
  add(kSpot, 100.0, Side::Put, 0.31);
  add(kSpot, 100.0, Side::Put, 0.31); // repeated: priced from the retained boundary
  add(0.0, 100.0, Side::Call, 0.3);   // InvalidArgument -> NaN
  add(kSpot, 100.0, Side::Put, kNaN); // InvalidArgument -> NaN
  add(kSpot, 90.0, Side::Put, 0.0);   // degenerate sigma; the bumped price is not
  add(kSpot, 110.0, Side::Call, 5.0e-9);
  return in;
}

TEST(AlPricePack, PricesAndBumpedPricesMatchFreshAndersenLakeBitForBit) {
  struct Expiry {
    double T;
    double r;
    double q;
  };
  constexpr std::array<Expiry, 4> kExpiries{
      {{0.5, 0.05, 0.02}, {0.08, 0.03, 0.07}, {2.0, 0.0, 0.03}, {1.0, 0.05, 0.0}}};
  const std::vector<std::optional<AlOpts>> schemes{std::nullopt, al_fast_opts()};
  const PackInput in = pack_chain();
  const std::size_t n = in.contracts.size();
  Tally tally;
  for (std::size_t s = 0; s < schemes.size(); ++s) {
    for (const Expiry &e : kExpiries) {
      const AlExpiryGeometry g = prepare_al_geometry(e.T, e.r, e.q, schemes[s]);
      std::vector<double> price(n, -1.0);
      std::vector<double> bumped(n, -1.0);
      ASSERT_TRUE(al_price_pack(g, in.contracts, in.sigma, price, bumped));
      for (std::size_t i = 0; i < n; ++i) {
        const AlContract &c = in.contracts[i];
        const double want =
            value_or_nan(andersen_lake(c.S, c.K, e.T, in.sigma[i], e.r, e.q, c.side, schemes[s]));
        const double want_bumped = value_or_nan(andersen_lake(
            c.S, c.K, e.T, in.sigma[i] + kAlPackSigmaBump, e.r, e.q, c.side, schemes[s]));
        const std::string where = describe(c.S, c.K, e.T, in.sigma[i], e.r, e.q, c.side, s);
        tally.record(identical(want, price[i]), where + " price");
        tally.record(identical(want_bumped, bumped[i]), where + " bumped");
      }
    }
  }
  EXPECT_EQ(tally.mismatched, 0u) << tally.mismatched << " of " << tally.checked << "\n"
                                  << tally.first;
}

TEST(AlPricePack, ChordSlopeMatchesTheCentralDifferenceWithinTheVolgaBound) {
  // The pack's (bumped - price) / h is the forward chord; the central difference
  // differs from it by (P(s+h) - 2 P(s) + P(s-h)) / (2h) ~ (h / 2) volga. The bound the
  // header documents is h * |volga| + 1e-8 (volga from american_greeks_al, a 2x margin
  // plus price-noise headroom).
  constexpr double kT = 0.5;
  constexpr double kR = 0.05;
  constexpr double kQ = 0.02;
  constexpr double kSigma = 0.3;
  const AlExpiryGeometry g = prepare_al_geometry(kT, kR, kQ);
  std::vector<AlContract> contracts;
  for (const double K : {70.0, 90.0, 100.0, 110.0, 130.0}) {
    contracts.push_back(AlContract{.S = kSpot, .K = K, .side = Side::Call});
    contracts.push_back(AlContract{.S = kSpot, .K = K, .side = Side::Put});
  }
  const std::size_t n = contracts.size();
  const std::vector<double> sigma(n, kSigma);
  std::vector<double> price(n);
  std::vector<double> bumped(n);
  ASSERT_TRUE(al_price_pack(g, contracts, sigma, price, bumped));
  const double h = kAlPackSigmaBump;
  std::size_t with_vega = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const AlContract &c = contracts[i];
    const Result<double> down = andersen_lake(c.S, c.K, kT, kSigma - h, kR, kQ, c.side);
    const auto greeks = american_greeks_al(c.S, c.K, kT, kSigma, kR, kQ, c.side);
    ASSERT_TRUE(down);
    ASSERT_TRUE(greeks);
    const double chord = (bumped[i] - price[i]) / h;
    const double central = (bumped[i] - *down) / (2.0 * h);
    const double bound = h * std::abs(greeks->volga) + 1.0e-8;
    EXPECT_LE(std::abs(chord - central), bound)
        << describe(c.S, c.K, kT, kSigma, kR, kQ, c.side, 0) << " chord " << chord
        << " central " << central << " volga " << greeks->volga;
    if (greeks->vega > 1.0e-6) {
      ++with_vega;
      EXPECT_GT(chord, 0.0);
    }
  }
  EXPECT_GE(with_vega, 8u);
}

TEST(AlPricePack, StatusPathsRefuseBeforeWritingAnything) {
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  const std::vector<AlContract> contracts{{.S = kSpot, .K = 95.0, .side = Side::Call},
                                          {.S = kSpot, .K = 105.0, .side = Side::Put}};
  const std::vector<double> sigma{0.3, 0.3};
  constexpr double kSentinel = 7.0;
  const auto untouched = [](const std::vector<double> &v) {
    for (const double x : v) {
      if (!same_bits(x, kSentinel)) {
        return false;
      }
    }
    return true;
  };
  {
    const AlExpiryGeometry unprepared;
    std::vector<double> price(2, kSentinel);
    std::vector<double> bumped(2, kSentinel);
    const Status st = al_price_pack(unprepared, contracts, sigma, price, bumped);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(untouched(price) && untouched(bumped));
  }
  {
    std::vector<double> price(1, kSentinel); // too short
    const Status st = al_price_pack(g, contracts, sigma, price, {});
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(untouched(price));
  }
  {
    std::vector<double> price(2, kSentinel);
    const std::vector<double> one_sigma{0.3};
    const Status st = al_price_pack(g, contracts, one_sigma, price, {});
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(untouched(price));
  }
  {
    std::vector<double> price(2, kSentinel);
    std::vector<double> bumped(3, kSentinel); // non-empty and the wrong length
    const Status st = al_price_pack(g, contracts, sigma, price, bumped);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(untouched(price) && untouched(bumped));
  }
  {
    // No bumped span: prices only.
    std::vector<double> price(2, kSentinel);
    ASSERT_TRUE(al_price_pack(g, contracts, sigma, price, {}));
    for (std::size_t i = 0; i < 2u; ++i) {
      const AlContract &c = contracts[i];
      const double want = value_or_nan(andersen_lake(c.S, c.K, 0.5, 0.3, 0.05, 0.02, c.side));
      EXPECT_TRUE(identical(want, price[i])) << i;
    }
  }
  {
    // An empty pack is a valid no-op.
    std::vector<double> none;
    EXPECT_TRUE(al_price_pack(g, std::span<const AlContract>{}, none, none, none));
  }
}

// ── AlSigmaFamily (L2h sigma-family, 2026-10-19) ─────────────────────────
//
// The family is NOT bit-identical to the cold solve by design (σ-interpolation
// between nodes, the few-ULP homogeneity rescale off the anchor): its contract is a
// MEASURED distance to the accurate cold Reference (PM ruling (a)), pinned here with
// 2-figure round-ups of the worst case measured at 12 nodes on the Sigma axis, the
// way AlFastProfile / AlScreenProfile pin theirs. The numbers and the wider error
// study are in sprints/2026-10-19-L2h-sigma-family-sprint.md.

using atx::vol::AlSigmaFamily;
using atx::vol::AlSigmaFamilyAxis;
using atx::vol::AlSigmaFamilyChord;
using atx::vol::AlSigmaFamilyPrice;
using atx::vol::AlSigmaFamilyStatus;
using atx::vol::kAlSigmaFamilyMaxNodes;
using atx::vol::kAlSigmaFamilyMinNodes;
using atx::vol::kAlSigmaFamilyMinSigma;
using atx::vol::kAlSigmaFamilyMinTau;

constexpr std::uint16_t kFamilyNodes = 12;

// The family's own σ-nodes (Chebyshev-Lobatto on [lo, hi], the TU's z = -cos form).
[[nodiscard]] std::vector<double> family_node_sigmas(double lo, double hi, unsigned n) {
  std::vector<double> v;
  for (unsigned s = 0; s < n; ++s) {
    double z = -std::cos(3.14159265358979323846 * static_cast<double>(s) /
                         static_cast<double>(n - 1));
    if (s == 0) {
      z = -1.0;
    }
    if (s == n - 1) {
      z = 1.0;
    }
    v.push_back(lo + 0.5 * (hi - lo) * (z + 1.0));
  }
  return v;
}

// Midpoints between adjacent σ-nodes: the P2.5 off-node pattern (where the
// interpolation error is largest).
[[nodiscard]] std::vector<double> family_off_node_sigmas(double lo, double hi, unsigned n) {
  const std::vector<double> nodes = family_node_sigmas(lo, hi, n);
  std::vector<double> v;
  for (std::size_t i = 1; i < nodes.size(); ++i) {
    v.push_back(0.5 * (nodes[i - 1] + nodes[i]));
  }
  return v;
}

// One (expiry, side) family and the contracts to compare against the cold solve.
struct FamilyCase {
  double S;
  double anchor; // internal-put strike of the node solves
  double T;
  double r;
  double q;
  Side side;
  double sigma_lo;
  double sigma_hi;
  std::vector<double> strikes;
  std::vector<double> sigmas; // queried σ (nodes and off-node)
};

// Worst |P_family - P_cold| / max(P_cold, 1) over a case; -1 when nothing priced.
struct FamilyWorst {
  double rel = -1.0;
  std::size_t priced = 0;
  std::size_t refused = 0;
  std::string at;
};

[[nodiscard]] FamilyWorst family_worst(const FamilyCase &c, std::uint16_t n_sigma,
                                       AlSigmaFamilyAxis axis) {
  FamilyWorst w;
  const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, c.q);
  AlSigmaFamily fam(g, c.side, c.anchor);
  const Status st = fam.prepare(c.sigma_lo, c.sigma_hi, n_sigma, axis);
  if (!st) {
    w.at = "prepare refused: " + st.error().to_string();
    return w;
  }
  for (const double K : c.strikes) {
    for (const double sigma : c.sigmas) {
      const Result<double> cold = andersen_lake(g, c.S, K, sigma, c.side);
      const AlSigmaFamilyPrice p = fam.price(c.S, K, sigma);
      if (!cold || p.status != AlSigmaFamilyStatus::Ok) {
        ++w.refused;
        continue;
      }
      ++w.priced;
      const double rel = std::abs(p.price - *cold) / std::max(*cold, 1.0);
      if (rel > w.rel) {
        w.rel = rel;
        std::ostringstream os;
        os << std::setprecision(17) << describe(c.S, K, c.T, sigma, c.r, c.q, c.side, 0)
           << " cold=" << *cold << " family=" << p.price;
        w.at = os.str();
      }
    }
  }
  return w;
}

// The AlFastProfile 1,350-contract grid regrouped by (T, r, q, side): one family per
// group over the grid's whole σ range [0.08, 0.7] — a wide box, the hard shape.
[[nodiscard]] std::vector<FamilyCase> family_profile_grid(bool off_node) {
  std::vector<FamilyCase> v;
  const std::vector<double> nodes = {0.08, 0.25, 0.7};
  const std::vector<double> off = family_off_node_sigmas(0.08, 0.7, kFamilyNodes);
  for (const Side side : {Side::Put, Side::Call}) {
    for (const double T : {0.02, 0.1, 0.5, 1.5, 4.0}) {
      for (const double r : {0.005, 0.045, 0.1}) {
        for (const double q : {0.0, 0.02, 0.09}) {
          v.push_back(FamilyCase{100.0, 100.0, T, r, q, side, 0.08, 0.7,
                                 {70.0, 90.0, 100.0, 110.0, 140.0}, off_node ? off : nodes});
        }
      }
    }
  }
  return v;
}

// A short-dated index-like slice: S = 773, T = 23 d, the SPY 2026-11-06 carry.
[[nodiscard]] std::vector<FamilyCase> family_spy_slice(bool off_node) {
  std::vector<FamilyCase> v;
  const std::vector<double> nodes = {0.12, 0.16, 0.2, 0.27, 0.35};
  const std::vector<double> off = family_off_node_sigmas(0.12, 0.35, kFamilyNodes);
  std::vector<double> strikes;
  for (const double m : {0.80, 0.90, 0.95, 0.98, 1.0, 1.02, 1.05, 1.10, 1.20}) {
    strikes.push_back(773.0 * m);
  }
  for (const Side side : {Side::Put, Side::Call}) {
    v.push_back(FamilyCase{773.0, 773.0, 0.068, 0.045, 0.0016, side, 0.12, 0.35, strikes,
                           off_node ? off : nodes});
  }
  return v;
}

// Screen's worst corner: long-dated, low-vol, deep-carry puts at the money.
[[nodiscard]] std::vector<FamilyCase> family_deep_carry(bool off_node) {
  std::vector<FamilyCase> v;
  const std::vector<double> nodes = {0.05, 0.065, 0.08, 0.1, 0.12};
  const std::vector<double> off = family_off_node_sigmas(0.05, 0.12, kFamilyNodes);
  for (const double T : {2.0, 3.0, 4.0, 5.0}) {
    for (const double r : {0.05, 0.075, 0.1}) {
      for (const double q : {0.0, 0.015, 0.03}) {
        v.push_back(FamilyCase{100.0, 100.0, T, r, q, Side::Put, 0.05, 0.12, {100.0},
                               off_node ? off : nodes});
      }
    }
  }
  return v;
}

[[nodiscard]] FamilyWorst family_worst_over(const std::vector<FamilyCase> &cases,
                                            std::uint16_t n_sigma, AlSigmaFamilyAxis axis) {
  FamilyWorst w;
  for (const FamilyCase &c : cases) {
    const FamilyWorst cw = family_worst(c, n_sigma, axis);
    w.priced += cw.priced;
    w.refused += cw.refused;
    if (cw.rel > w.rel) {
      w.rel = cw.rel;
      w.at = cw.at;
    }
  }
  return w;
}

TEST(AlSigmaFamily, PrepareRefusesBadInputsAndLeavesTheFamilyUnprepared) {
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  {
    const AlExpiryGeometry unprepared;
    AlSigmaFamily f(unprepared, Side::Put, kSpot);
    const Status st = f.prepare(0.1, 0.5, kFamilyNodes);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(f.prepared());
    EXPECT_EQ(f.price(kSpot, kSpot, 0.3).status, AlSigmaFamilyStatus::NotPrepared);
  }
  {
    AlSigmaFamily f(g, Side::Put, kSpot);
    // Degenerate boxes: inverted, zero width, below the small-σ guard, non-finite.
    EXPECT_EQ(f.prepare(0.5, 0.1, kFamilyNodes).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(f.prepare(0.3, 0.3, kFamilyNodes).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(f.prepare(0.5 * kAlSigmaFamilyMinSigma, 0.5, kFamilyNodes).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(f.prepare(0.1, kNaN, kFamilyNodes).error().code(), ErrorCode::InvalidArgument);
    // Node counts outside [4, 16].
    EXPECT_EQ(f.prepare(0.1, 0.5, static_cast<std::uint16_t>(kAlSigmaFamilyMinNodes - 1))
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(f.prepare(0.1, 0.5, static_cast<std::uint16_t>(kAlSigmaFamilyMaxNodes + 1))
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
    EXPECT_FALSE(f.prepared());
  }
  {
    AlSigmaFamily f(g, Side::Call, 0.0); // anchor must be > 0
    EXPECT_EQ(f.prepare(0.1, 0.5, kFamilyNodes).error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(f.prepared());
  }
  {
    // Below the near-expiry guard: the whole expiry prices cold.
    const AlExpiryGeometry near = prepare_al_geometry(0.5 * kAlSigmaFamilyMinTau, 0.05, 0.02);
    AlSigmaFamily f(near, Side::Put, kSpot);
    EXPECT_EQ(f.prepare(0.1, 0.5, kFamilyNodes).error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(f.prepared());
  }
  {
    // Non-American regimes have no boundary: the European put (r = 0, q > 0) and the
    // call whose internal put is the double-continuation corner.
    const AlExpiryGeometry euro_put = prepare_al_geometry(0.5, 0.0, 0.03);
    AlSigmaFamily fp(euro_put, Side::Put, kSpot);
    EXPECT_EQ(fp.prepare(0.1, 0.5, kFamilyNodes).error().code(), ErrorCode::NotImplemented);
    EXPECT_FALSE(fp.prepared());
    const AlExpiryGeometry dc_call = prepare_al_geometry(0.5, -0.03, -0.01);
    AlSigmaFamily fc(dc_call, Side::Call, kSpot);
    EXPECT_EQ(fc.prepare(0.1, 0.5, kFamilyNodes).error().code(), ErrorCode::NotImplemented);
    EXPECT_FALSE(fc.prepared());
  }
  {
    // A refusal after a success discards the earlier preparation (never half-built).
    AlSigmaFamily f(g, Side::Put, kSpot);
    ASSERT_TRUE(f.prepare(0.1, 0.5, kFamilyNodes));
    ASSERT_TRUE(f.prepared());
    EXPECT_EQ(f.price(kSpot, kSpot, 0.3).status, AlSigmaFamilyStatus::Ok);
    EXPECT_FALSE(f.prepare(0.5, 0.1, kFamilyNodes));
    EXPECT_FALSE(f.prepared());
    EXPECT_EQ(f.price(kSpot, kSpot, 0.3).status, AlSigmaFamilyStatus::NotPrepared);
    EXPECT_TRUE(std::isnan(f.price(kSpot, kSpot, 0.3).price));
  }
  {
    // Accessors on a prepared family.
    AlSigmaFamily f(g, Side::Call, kSpot);
    ASSERT_TRUE(f.prepare(0.1, 0.5, 8));
    EXPECT_EQ(f.n_sigma(), 8);
    EXPECT_EQ(f.sigma_lo(), 0.1);
    EXPECT_EQ(f.sigma_hi(), 0.5);
    EXPECT_EQ(f.anchor(), kSpot);
    EXPECT_EQ(f.side(), Side::Call);
    EXPECT_GT(AlSigmaFamily::state_bytes(), 4096u);
    EXPECT_LT(AlSigmaFamily::state_bytes(), 16384u);
  }
}

TEST(AlSigmaFamily, NodePricesMatchTheColdSolveAtTheAnchor) {
  // At a σ-node the barycentric evaluator returns the node's own y[] (no
  // interpolation), and at the anchor strike no rescale happens: the only distance
  // from the cold solve is the σ_node -> log -> z -> σ round trip of the default
  // LogSigma axis (measured 8.4357e-10 relative; on the Sigma axis the round trip
  // hits the node bit-exactly and the distance is 0). Bound: the 2-figure round-up.
  constexpr double kBound = 8.5e-10;
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  const std::vector<double> nodes = family_node_sigmas(0.1, 0.6, kFamilyNodes);
  double worst = 0.0;
  std::size_t checked = 0;
  for (const Side side : {Side::Put, Side::Call}) {
    AlSigmaFamily f(g, side, kSpot);
    ASSERT_TRUE(f.prepare(0.1, 0.6, kFamilyNodes));
    for (const double sigma : nodes) {
      for (const double other : {60.0, 90.0, 100.0, 115.0, 150.0}) {
        // Put family: anchor = strike, the spot varies; call family: anchor = spot,
        // the strike varies.
        const double S = (side == Side::Put) ? other : kSpot;
        const double K = (side == Side::Put) ? kSpot : other;
        const Result<double> cold = andersen_lake(g, S, K, sigma, side);
        const AlSigmaFamilyPrice p = f.price(S, K, sigma);
        ASSERT_TRUE(cold);
        ASSERT_EQ(p.status, AlSigmaFamilyStatus::Ok);
        const double rel = std::abs(p.price - *cold) / std::max(*cold, 1.0);
        worst = std::max(worst, rel);
        ++checked;
        EXPECT_LE(rel, kBound) << describe(S, K, 0.5, sigma, 0.05, 0.02, side, 0)
                               << " cold=" << *cold << " family=" << p.price;
      }
    }
  }
  EXPECT_EQ(checked, 2u * kFamilyNodes * 5u);
  std::cout << "[AlSigmaFamily] node/anchor worst rel=" << worst << "\n";
}

// The default axis (what `prepare` without an axis argument selects): pinned below.
constexpr AlSigmaFamilyAxis kFamilyAxis = AlSigmaFamilyAxis::LogSigma;

TEST(AlSigmaFamily, DefaultAxisIsLogSigma) {
  // The measured choice (sprint note §4: tighter on every set at 12 nodes). A family
  // prepared without an axis prices exactly as one prepared with LogSigma.
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaFamily by_default(g, Side::Put, kSpot);
  AlSigmaFamily explicit_log(g, Side::Put, kSpot);
  AlSigmaFamily explicit_sigma(g, Side::Put, kSpot);
  ASSERT_TRUE(by_default.prepare(0.1, 0.6, kFamilyNodes));
  ASSERT_TRUE(explicit_log.prepare(0.1, 0.6, kFamilyNodes, AlSigmaFamilyAxis::LogSigma));
  ASSERT_TRUE(explicit_sigma.prepare(0.1, 0.6, kFamilyNodes, AlSigmaFamilyAxis::Sigma));
  std::size_t differs_from_sigma_axis = 0;
  for (const double sigma : family_off_node_sigmas(0.1, 0.6, kFamilyNodes)) {
    const AlSigmaFamilyPrice a = by_default.price(kSpot, 95.0, sigma);
    const AlSigmaFamilyPrice b = explicit_log.price(kSpot, 95.0, sigma);
    const AlSigmaFamilyPrice c = explicit_sigma.price(kSpot, 95.0, sigma);
    ASSERT_EQ(a.status, AlSigmaFamilyStatus::Ok);
    EXPECT_TRUE(same_bits(a.price, b.price)) << sigma;
    differs_from_sigma_axis += same_bits(a.price, c.price) ? 0u : 1u;
  }
  EXPECT_GT(differs_from_sigma_axis, 0u) << "the two axes produced the same bits";
}

TEST(AlSigmaFamily, PriceWithinMeasuredBoundOfColdReference) {
  // |P_family - P_cold| <= bound * max(P_cold, 1) at 12 nodes on the default axis, per
  // set, queried AT the sets' σ values (nodes and interior points alike). Bounds are
  // 2-figure round-ups of the measured worst (sprint note §4).
  constexpr double kProfileGrid = 2.1e-7; // measured 2.0080e-7 (put K 140, T 4, q 9 %)
  constexpr double kSpySlice = 3.7e-12;   // measured 3.6065e-12
  constexpr double kDeepCarry = 4.8e-9;   // measured 4.7261e-9 (T 5, sigma 0.08, r 10 %)
  const FamilyWorst grid = family_worst_over(family_profile_grid(false), kFamilyNodes,
                                             kFamilyAxis);
  const FamilyWorst spy = family_worst_over(family_spy_slice(false), kFamilyNodes, kFamilyAxis);
  const FamilyWorst deep = family_worst_over(family_deep_carry(false), kFamilyNodes,
                                             kFamilyAxis);
  EXPECT_GT(grid.priced, 1000u);
  EXPECT_EQ(grid.refused, 0u);
  EXPECT_LE(grid.rel, kProfileGrid) << grid.at;
  EXPECT_EQ(spy.priced, 90u);
  EXPECT_LE(spy.rel, kSpySlice) << spy.at;
  EXPECT_EQ(deep.priced, 180u);
  EXPECT_LE(deep.rel, kDeepCarry) << deep.at;
  std::cout << "[AlSigmaFamily] 12-node worst |dP|/max(P,1): grid=" << grid.rel << " at "
            << grid.at << "\n[AlSigmaFamily] spy23-like=" << spy.rel << " at " << spy.at
            << "\n[AlSigmaFamily] deep-carry=" << deep.rel << " at " << deep.at << "\n";
}

TEST(AlSigmaFamily, OffNodeSigmaWithinMeasuredBound) {
  // The same sets queried at the MIDPOINTS between adjacent σ-nodes (the P2.5
  // off-node pattern), where the interpolation error peaks.
  constexpr double kProfileGrid = 4.1e-6; // measured 4.0552e-6 (put S = K, T 4, sigma 0.086, r 10 %)
  constexpr double kSpySlice = 4.4e-12;   // measured 4.3331e-12
  constexpr double kDeepCarry = 1.3e-8;   // measured 1.2071e-8 (T 5, sigma 0.066, r 10 %)
  const FamilyWorst grid = family_worst_over(family_profile_grid(true), kFamilyNodes,
                                             kFamilyAxis);
  const FamilyWorst spy = family_worst_over(family_spy_slice(true), kFamilyNodes, kFamilyAxis);
  const FamilyWorst deep = family_worst_over(family_deep_carry(true), kFamilyNodes,
                                             kFamilyAxis);
  EXPECT_GT(grid.priced, 1000u);
  EXPECT_LE(grid.rel, kProfileGrid) << grid.at;
  EXPECT_EQ(spy.priced, 2u * 9u * (kFamilyNodes - 1u));
  EXPECT_LE(spy.rel, kSpySlice) << spy.at;
  EXPECT_EQ(deep.priced, 36u * (kFamilyNodes - 1u));
  EXPECT_LE(deep.rel, kDeepCarry) << deep.at;
  std::cout << "[AlSigmaFamily] 12-node OFF-NODE worst |dP|/max(P,1): grid=" << grid.rel
            << " at " << grid.at << "\n[AlSigmaFamily] spy23-like=" << spy.rel << " at "
            << spy.at << "\n[AlSigmaFamily] deep-carry=" << deep.rel << " at " << deep.at
            << "\n";
}

TEST(AlSigmaFamily, SigmaAxisPricesWithinItsOwnMeasuredBounds) {
  // The alternative axis is a documented, selectable option with its own measured
  // off-node bounds (12 nodes): grid 2.3136e-5, SPY-like 1.9955e-11, deep-carry
  // 3.3989e-8 — the numbers the LogSigma default beat (sprint note §4).
  const FamilyWorst grid = family_worst_over(family_profile_grid(true), kFamilyNodes,
                                             AlSigmaFamilyAxis::Sigma);
  const FamilyWorst spy = family_worst_over(family_spy_slice(true), kFamilyNodes,
                                            AlSigmaFamilyAxis::Sigma);
  const FamilyWorst deep = family_worst_over(family_deep_carry(true), kFamilyNodes,
                                             AlSigmaFamilyAxis::Sigma);
  EXPECT_GT(grid.priced, 1000u);
  EXPECT_LE(grid.rel, 2.4e-5) << grid.at;
  EXPECT_LE(spy.rel, 2.0e-11) << spy.at;
  EXPECT_LE(deep.rel, 3.4e-8) << deep.at;
  std::cout << "[AlSigmaFamily] 12-node SIGMA-axis off-node worst: grid=" << grid.rel << " at "
            << grid.at << "\n[AlSigmaFamily] spy23-like=" << spy.rel << " at " << spy.at
            << "\n[AlSigmaFamily] deep-carry=" << deep.rel << " at " << deep.at << "\n";
}

TEST(AlSigmaFamily, CallFamilyServesAMovedSpotWithinBound) {
  // A call family's anchor is the spot it was built at; after a 1 % spot move the
  // internal-put strike changes and the homogeneity rescale serves it (a few ULP
  // from a fresh solve at the new spot, plus the σ-interpolation). A dividend-paying
  // carry (q > r) so the calls carry a live early-exercise premium. Measured worst
  // 1.7978e-9 (call S 101, K 95, sigma 0.12); the bound is its 2-figure round-up.
  constexpr double kBound = 1.8e-9;
  constexpr double kT = 1.0;
  constexpr double kR = 0.03;
  constexpr double kQ = 0.07;
  const AlExpiryGeometry g = prepare_al_geometry(kT, kR, kQ);
  AlSigmaFamily f(g, Side::Call, kSpot);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kFamilyNodes));
  double worst = 0.0;
  std::string at;
  std::size_t with_premium = 0;
  for (const double S : {99.0, 101.0}) {
    for (const double K : {80.0, 95.0, 100.0, 105.0, 120.0}) {
      for (const double sigma : family_off_node_sigmas(0.1, 0.5, kFamilyNodes)) {
        const Result<double> cold = andersen_lake(g, S, K, sigma, Side::Call);
        const AlSigmaFamilyPrice p = f.price(S, K, sigma);
        ASSERT_TRUE(cold);
        ASSERT_EQ(p.status, AlSigmaFamilyStatus::Ok);
        const double euro = atx::vol::black76_price(S * std::exp((kR - kQ) * kT), K, kT, sigma,
                                                    std::exp(-kR * kT), Side::Call);
        if (*cold - euro > 1.0e-4) {
          ++with_premium;
        }
        const double rel = std::abs(p.price - *cold) / std::max(*cold, 1.0);
        if (rel > worst) {
          worst = rel;
          at = describe(S, K, kT, sigma, kR, kQ, Side::Call, 0);
        }
      }
    }
  }
  EXPECT_GE(with_premium, 50u); // the case is not premium-free
  EXPECT_LE(worst, kBound) << at;
  std::cout << "[AlSigmaFamily] moved-spot call worst rel=" << worst << " at " << at
            << " (with_premium=" << with_premium << ")\n";
}

TEST(AlSigmaFamily, ChordSlopeMatchesTheCentralDifferenceWithinTheVolgaBound) {
  // Same shape as AlPricePack.ChordSlopeMatchesTheCentralDifferenceWithinTheVolgaBound,
  // on the family's OWN map (the pack's test mixes nothing either: every price there
  // is the served map's): the forward chord vs the central difference of the family's
  // σ ± h prices, within h * |volga| + 1e-8. Then the family chord vs the cold pack's
  // chord (the slope an inversion on the family would use vs the one on the cold
  // map): measured worst relative gap (see note), bound its 2-figure round-up.
  constexpr double kT = 0.5;
  constexpr double kR = 0.05;
  constexpr double kQ = 0.02;
  constexpr double kSigma = 0.3;
  constexpr double kChordVsColdRel = 8.5e-9; // measured 8.4852e-9 (put K 130)
  const AlExpiryGeometry g = prepare_al_geometry(kT, kR, kQ);
  const double h = kAlPackSigmaBump;
  std::size_t with_vega = 0;
  double worst_vs_cold = 0.0;
  std::string worst_at;
  for (const Side side : {Side::Call, Side::Put}) {
    AlSigmaFamily f(g, side, kSpot);
    ASSERT_TRUE(f.prepare(0.1, 0.6, kFamilyNodes));
    for (const double K : {70.0, 90.0, 100.0, 110.0, 130.0}) {
      const AlSigmaFamilyChord c = f.price_and_chord(kSpot, K, kSigma);
      ASSERT_EQ(c.status, AlSigmaFamilyStatus::Ok);
      const AlSigmaFamilyPrice down = f.price(kSpot, K, kSigma - h);
      ASSERT_EQ(down.status, AlSigmaFamilyStatus::Ok);
      const auto greeks = american_greeks_al(kSpot, K, kT, kSigma, kR, kQ, side);
      ASSERT_TRUE(greeks);
      const double chord = (c.price_sigma_bumped - c.price) / h;
      const double central = (c.price_sigma_bumped - down.price) / (2.0 * h);
      const double bound = h * std::abs(greeks->volga) + 1.0e-8;
      EXPECT_LE(std::abs(chord - central), bound)
          << describe(kSpot, K, kT, kSigma, kR, kQ, side, 0) << " chord " << chord
          << " central " << central << " volga " << greeks->volga;
      // The chord's two prices are price()'s own, bit for bit.
      EXPECT_TRUE(same_bits(c.price, f.price(kSpot, K, kSigma).price));
      EXPECT_TRUE(same_bits(c.price_sigma_bumped, f.price(kSpot, K, kSigma + h).price));
      // Against the cold pack's chord on the same contract.
      const std::array<AlContract, 1> one{{AlContract{.S = kSpot, .K = K, .side = side}}};
      const std::array<double, 1> sig{{kSigma}};
      std::array<double, 1> cold_price{};
      std::array<double, 1> cold_bumped{};
      ASSERT_TRUE(al_price_pack(g, one, sig, cold_price, cold_bumped));
      const double cold_chord = (cold_bumped[0] - cold_price[0]) / h;
      if (greeks->vega > 1.0e-6) {
        ++with_vega;
        EXPECT_GT(chord, 0.0);
        const double rel = std::abs(chord - cold_chord) / std::abs(cold_chord);
        if (rel > worst_vs_cold) {
          worst_vs_cold = rel;
          worst_at = describe(kSpot, K, kT, kSigma, kR, kQ, side, 0);
        }
        EXPECT_LE(rel, kChordVsColdRel) << worst_at << " family " << chord << " cold "
                                        << cold_chord;
      }
    }
  }
  EXPECT_GE(with_vega, 8u);
  std::cout << "[AlSigmaFamily] chord vs cold chord worst rel=" << worst_vs_cold << " at "
            << worst_at << "\n";
}

TEST(AlSigmaFamily, RefusalsAreTypedAndCarryNaN) {
  AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaFamily f(g, Side::Put, kSpot);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kFamilyNodes));
  const auto refused = [&](double S, double K, double sigma, AlSigmaFamilyStatus want) {
    const AlSigmaFamilyPrice p = f.price(S, K, sigma);
    EXPECT_EQ(p.status, want) << "S=" << S << " K=" << K << " sigma=" << sigma;
    EXPECT_TRUE(std::isnan(p.price));
  };
  // Outside the box, either side; never extrapolated.
  refused(kSpot, kSpot, 0.5 + 1.0e-9, AlSigmaFamilyStatus::SigmaOutsideBox);
  refused(kSpot, kSpot, 0.1 - 1.0e-9, AlSigmaFamilyStatus::SigmaOutsideBox);
  // Below the small-σ guard (which wins over the box test).
  refused(kSpot, kSpot, 0.5 * kAlSigmaFamilyMinSigma, AlSigmaFamilyStatus::SigmaBelowGuard);
  refused(kSpot, kSpot, 0.0, AlSigmaFamilyStatus::SigmaBelowGuard);
  // Invalid contracts.
  refused(0.0, kSpot, 0.3, AlSigmaFamilyStatus::InvalidArgument);
  refused(kSpot, -1.0, 0.3, AlSigmaFamilyStatus::InvalidArgument);
  refused(kNaN, kSpot, 0.3, AlSigmaFamilyStatus::InvalidArgument);
  refused(kSpot, kSpot, kNaN, AlSigmaFamilyStatus::InvalidArgument);
  // The box edges themselves price.
  EXPECT_EQ(f.price(kSpot, kSpot, 0.1).status, AlSigmaFamilyStatus::Ok);
  EXPECT_EQ(f.price(kSpot, kSpot, 0.5).status, AlSigmaFamilyStatus::Ok);
  // The chord needs kAlPackSigmaBump of headroom: at the top edge it refuses as a
  // whole (both prices NaN) rather than serving a one-sided answer.
  {
    const AlSigmaFamilyChord c = f.price_and_chord(kSpot, kSpot, 0.5);
    EXPECT_EQ(c.status, AlSigmaFamilyStatus::SigmaOutsideBox);
    EXPECT_TRUE(std::isnan(c.price) && std::isnan(c.price_sigma_bumped));
    EXPECT_EQ(f.price_and_chord(kSpot, kSpot, 0.5 - 2.0 * kAlPackSigmaBump).status,
              AlSigmaFamilyStatus::Ok);
  }
  // A geometry re-prepared after prepare() moved (T, r, q) under the family: refuse
  // until the family is prepared again (the AlBoundaryHandle rule).
  g.prepare(0.75, 0.05, 0.02);
  refused(kSpot, kSpot, 0.3, AlSigmaFamilyStatus::GeometryRePrepared);
  EXPECT_EQ(f.price_and_chord(kSpot, kSpot, 0.3).status,
            AlSigmaFamilyStatus::GeometryRePrepared);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kFamilyNodes));
  const AlSigmaFamilyPrice again = f.price(kSpot, kSpot, 0.3);
  ASSERT_EQ(again.status, AlSigmaFamilyStatus::Ok);
  const Result<double> cold = andersen_lake(g, kSpot, kSpot, 0.3, Side::Put);
  ASSERT_TRUE(cold);
  EXPECT_NEAR(again.price, *cold, 1.0e-6 * std::max(*cold, 1.0));
  // Moved-from family.
  AlSigmaFamily moved(std::move(f));
  EXPECT_EQ(moved.price(kSpot, kSpot, 0.3).status, AlSigmaFamilyStatus::Ok);
  EXPECT_EQ(f.price(kSpot, kSpot, 0.3).status, AlSigmaFamilyStatus::NotPrepared); // NOLINT
  EXPECT_FALSE(f.prepare(0.1, 0.5, kFamilyNodes));                                 // NOLINT
}

TEST(AlSigmaFamily, ConcurrentPricesAreBitIdenticalToTheSerialLoop) {
  // N threads pricing through ONE const family (no per-thread scratch, no locks)
  // reproduce the single-thread loop bit for bit — the certification fan-out shape.
  constexpr unsigned kThreads = 8;
  const AlExpiryGeometry g = prepare_al_geometry(0.25, 0.045, 0.01);
  AlSigmaFamily fam(g, Side::Put, kSpot);
  ASSERT_TRUE(fam.prepare(0.1, 0.5, kFamilyNodes));
  const AlSigmaFamily &shared = fam;
  std::vector<double> strikes;
  std::vector<double> sigmas;
  for (const double K : {70.0, 85.0, 95.0, 100.0, 105.0, 115.0, 130.0}) {
    for (const double s : family_off_node_sigmas(0.1, 0.5, kFamilyNodes)) {
      strikes.push_back(K);
      sigmas.push_back(s);
    }
  }
  const std::size_t n = strikes.size();
  std::vector<double> serial(n);
  std::vector<double> serial_bumped(n);
  for (std::size_t i = 0; i < n; ++i) {
    const AlSigmaFamilyChord c = shared.price_and_chord(kSpot, strikes[i], sigmas[i]);
    ASSERT_EQ(c.status, AlSigmaFamilyStatus::Ok);
    serial[i] = c.price;
    serial_bumped[i] = c.price_sigma_bumped;
  }
  std::vector<std::vector<double>> got(kThreads, std::vector<double>(n, kNaN));
  std::vector<std::vector<double>> got_bumped(kThreads, std::vector<double>(n, kNaN));
  {
    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t]() {
        // Each thread walks the contracts in a different order and repeats, so the
        // threads overlap on the same family at the same time.
        for (unsigned rep = 0; rep < 4; ++rep) {
          for (std::size_t j = 0; j < n; ++j) {
            const std::size_t i = (j + t * 7u) % n;
            const AlSigmaFamilyChord c = shared.price_and_chord(kSpot, strikes[i], sigmas[i]);
            got[t][i] = c.status == AlSigmaFamilyStatus::Ok ? c.price : kNaN;
            got_bumped[t][i] =
                c.status == AlSigmaFamilyStatus::Ok ? c.price_sigma_bumped : kNaN;
          }
        }
      });
    }
  }
  std::size_t mismatched = 0;
  for (unsigned t = 0; t < kThreads; ++t) {
    for (std::size_t i = 0; i < n; ++i) {
      if (!same_bits(got[t][i], serial[i]) || !same_bits(got_bumped[t][i], serial_bumped[i])) {
        ++mismatched;
      }
    }
  }
  EXPECT_EQ(mismatched, 0u) << "of " << kThreads * n;
  EXPECT_GE(n, 70u);
}

// A safeguarded Newton on the family's own chord: what a family-based inversion
// iterates. Returns NaN when it does not converge inside the box.
[[nodiscard]] double family_invert(const AlSigmaFamily &f, double S, double K, double target,
                                   double lo, double hi) {
  double a = lo;
  double b = hi - 2.0 * kAlPackSigmaBump; // chord headroom
  double x = 0.5 * (a + b);
  for (int it = 0; it < 60; ++it) {
    const AlSigmaFamilyChord c = f.price_and_chord(S, K, x);
    if (c.status != AlSigmaFamilyStatus::Ok) {
      return kNaN;
    }
    const double fx = c.price - target;
    if (std::abs(fx) <= 1.0e-13 * std::max(target, 1.0)) {
      return x;
    }
    if (fx > 0.0) {
      b = x;
    } else {
      a = x;
    }
    const double slope = (c.price_sigma_bumped - c.price) / kAlPackSigmaBump;
    double nx = (slope > 0.0) ? x - fx / slope : 0.5 * (a + b);
    if (!(nx > a && nx < b)) {
      nx = 0.5 * (a + b);
    }
    if (std::abs(nx - x) <= 1.0e-12) {
      return nx;
    }
    x = nx;
  }
  return kNaN;
}

TEST(AlSigmaFamily, ImpliedVolOnTheFamilyWithinMeasuredBound) {
  // The Reference price of each vega-bearing SPY-like contract, inverted on the
  // family (Newton on price_and_chord) vs the Reference inversion, in bp. Measured
  // worst 0.017734 bp (put K 811.65, sigma 0.1223: a low-vega deep put at the box's
  // low end); the bound is its 2-figure round-up.
  constexpr double kBoundBp = 0.018;
  const AlExpiryGeometry g = prepare_al_geometry(0.068, 0.045, 0.0016);
  int solved = 0;
  double worst_bp = 0.0;
  std::string at;
  for (const FamilyCase &c : family_spy_slice(true)) {
    AlSigmaFamily f(g, c.side, c.anchor);
    ASSERT_TRUE(f.prepare(c.sigma_lo, c.sigma_hi, kFamilyNodes));
    for (const double K : c.strikes) {
      for (const double sigma : c.sigmas) {
        const Result<double> px = andersen_lake(g, c.S, K, sigma, c.side);
        const Result<double> up = andersen_lake(g, c.S, K, sigma + 1.0e-4, c.side);
        const Result<double> dn = andersen_lake(g, c.S, K, sigma - 1.0e-4, c.side);
        ASSERT_TRUE(px && up && dn);
        if (!((*up - *dn) / 2.0e-4 >= 1.0e-2 * std::max(*px, 1.0))) {
          continue; // not vega-bearing (L1c: pinned prices invert to a band)
        }
        const auto ref = atx::vol::american_implied_vol(
            *px, c.S, K, c.T, c.r, c.q, c.side, atx::vol::AmericanMethod::AndersenLake, 1.0e-10,
            100, std::nullopt);
        ASSERT_TRUE(ref);
        const double iv = family_invert(f, c.S, K, *px, c.sigma_lo, c.sigma_hi);
        ASSERT_FALSE(std::isnan(iv)) << describe(c.S, K, c.T, sigma, c.r, c.q, c.side, 0);
        ++solved;
        const double bp = std::abs(iv - *ref) * 1.0e4;
        if (bp > worst_bp) {
          worst_bp = bp;
          at = describe(c.S, K, c.T, sigma, c.r, c.q, c.side, 0) + " ref_iv=" +
               std::to_string(*ref) + " family_iv=" + std::to_string(iv);
        }
        EXPECT_LE(bp, kBoundBp) << at;
      }
    }
  }
  EXPECT_GT(solved, 100);
  std::cout << "[AlSigmaFamily] family IV solved=" << solved << " worst bp=" << worst_bp
            << " at " << at << "\n";
}

} // namespace
