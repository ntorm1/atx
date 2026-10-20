#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"

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

} // namespace
