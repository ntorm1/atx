// risk_horizon_test.cpp — P2-S1-3: the PIT multi-horizon forecast trajectory.
//
// forecast_trajectory is a PURE numeric kernel (amendment D8): it takes the
// PRE-EVALUATED current cross-sections α_t,s (one span per signal source) plus a
// per-source SignalHorizon, and projects each forward to alpha[h][i] =
//   Σ_{s : α_t,s[i] non-NaN} decay_s(h) · α_t,s[i]
// with decay_s(h) = 2^{-h / halflife_s}. No-look-ahead (R2) holds structurally:
// the trajectory is a deterministic function of the CURRENT α_t only — there is
// no future read and no PanelView coupling.
//
// Coverage:
//   1. Identity decay (boundary-pin precondition): halflife=+inf ⇒ alpha[h] == α_t
//      EXACTLY for all h; a NaN cell stays NaN across all h (constant trajectory).
//   2. Finite-halflife decay (R8 differential): single source, hand-computed 2^{-h/2}.
//   3. Multi-source superposition: decay_A(h)·a_A + decay_B(h)·a_B.
//   4. NaN semantics: NaN-in-A-only ⇒ B only; NaN-in-both ⇒ NaN (NOT 0); present-in-
//      both ⇒ full sum.
//   5. decay() edges: halflife=0 ⇒ decay(0)=1, decay(h>0)=0; identity ⇒ decay=1 ∀h.
//   6. Determinism (R1): two builds are bit-identical (NaN bit patterns compared
//      bitwise so NaN == NaN).
//   7. Boundaries/errors: empty sources ⇒ Err; span length != M ⇒ Err; halflife<0 ⇒
//      Err; H=0 ⇒ exactly 1 row == α_t; M=1 single-name universe.

#include <bit>     // std::bit_cast (bitwise NaN-aware determinism compare)
#include <cmath>   // std::isnan, std::exp2
#include <cstdint> // std::uint64_t
#include <limits>  // std::numeric_limits (quiet_NaN, infinity)
#include <span>
#include <utility> // std::pair
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/horizon.hpp"

namespace atxtest_risk_horizon_test {

using atx::f64;
using atx::usize;
using atx::core::ErrorCode;
using atx::engine::risk::forecast_trajectory;
using atx::engine::risk::HorizonForecast;
using atx::engine::risk::SignalHorizon;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// A (span, horizon) source pair built over an owned buffer the caller keeps alive.
using Source = std::pair<std::span<const f64>, SignalHorizon>;

[[nodiscard]] Source src(const std::vector<f64> &a, f64 halflife) {
  return Source{std::span<const f64>(a), SignalHorizon{halflife}};
}

// Bitwise equality so NaN == NaN (load-bearing for the determinism + NaN pins).
[[nodiscard]] bool bit_eq(f64 a, f64 b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// ===========================================================================
//  1. Identity decay — the boundary-pin precondition (constant trajectory).
// ===========================================================================
TEST(RiskHorizon, IdentityDecayIsConstantTrajectoryAndPreservesNaN) {
  const std::vector<f64> a = {1.0, -2.0, kNaN, 0.5}; // index 2 is "no opinion"
  const std::vector<Source> sources = {
      Source{std::span<const f64>(a), SignalHorizon::identity()}};
  const usize m = 4U;
  const usize h_max = 5U;

  auto r = forecast_trajectory(std::span<const Source>(sources), m, h_max);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const HorizonForecast &fc = *r;
  ASSERT_EQ(fc.H, h_max);
  ASSERT_EQ(fc.alpha.size(), h_max + 1U);

  for (usize h = 0; h <= h_max; ++h) {
    ASSERT_EQ(fc.alpha[h].size(), m) << "h=" << h;
    for (usize i = 0; i < m; ++i) {
      // EXACT equality: an identity horizon must reproduce α_t bit-for-bit at
      // every forward step, and the NaN "no opinion" cell must stay NaN.
      EXPECT_TRUE(bit_eq(fc.alpha[h][i], a[i])) << "h=" << h << " i=" << i;
    }
  }
}

// ===========================================================================
//  2. Finite-halflife decay (R8 differential) — single source, hand-computed.
// ===========================================================================
TEST(RiskHorizon, FiniteHalflifeDecaysGeometrically) {
  const std::vector<f64> a = {2.0, -4.0, 8.0};
  const std::vector<Source> sources = {src(a, 2.0)}; // halflife = 2 periods
  const usize m = 3U;
  const usize h_max = 6U;

  auto r = forecast_trajectory(std::span<const Source>(sources), m, h_max);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const HorizonForecast &fc = *r;

  for (usize h = 0; h <= h_max; ++h) {
    const f64 decay = std::exp2(-static_cast<f64>(h) / 2.0);
    for (usize i = 0; i < m; ++i) {
      EXPECT_NEAR(fc.alpha[h][i], a[i] * decay, 1e-12) << "h=" << h << " i=" << i;
    }
  }
  // h=0 anchor: decay(0)=1 ⇒ alpha[0] == α_t exactly (no-look-ahead anchor).
  for (usize i = 0; i < m; ++i) {
    EXPECT_TRUE(bit_eq(fc.alpha[0][i], a[i])) << "anchor i=" << i;
  }
}

// ===========================================================================
//  3. Multi-source superposition — two horizons sum at each name.
// ===========================================================================
TEST(RiskHorizon, MultiSourceSuperposition) {
  const std::vector<f64> a = {1.0, 2.0, -3.0};
  const std::vector<f64> b = {0.5, -1.0, 4.0};
  const std::vector<Source> sources = {src(a, 1.0), src(b, 4.0)};
  const usize m = 3U;
  const usize h_max = 4U;

  auto r = forecast_trajectory(std::span<const Source>(sources), m, h_max);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const HorizonForecast &fc = *r;

  for (usize h = 0; h <= h_max; ++h) {
    const f64 da = std::exp2(-static_cast<f64>(h) / 1.0);
    const f64 db = std::exp2(-static_cast<f64>(h) / 4.0);
    for (usize i = 0; i < m; ++i) {
      const f64 want = da * a[i] + db * b[i];
      EXPECT_NEAR(fc.alpha[h][i], want, 1e-12) << "h=" << h << " i=" << i;
    }
  }
}

// ===========================================================================
//  4. NaN semantics — three name-classes in one fixture.
//     name 0: present in BOTH ⇒ full sum.
//     name 1: NaN in A, present in B ⇒ B's contribution only.
//     name 2: NaN in BOTH ⇒ NaN for all h (NOT 0 — preserves "no opinion").
// ===========================================================================
TEST(RiskHorizon, NaNSemanticsAcrossNameClasses) {
  const std::vector<f64> a = {1.0, kNaN, kNaN};
  const std::vector<f64> b = {2.0, 3.0, kNaN};
  const std::vector<Source> sources = {src(a, 2.0), src(b, 3.0)};
  const usize m = 3U;
  const usize h_max = 4U;

  auto r = forecast_trajectory(std::span<const Source>(sources), m, h_max);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const HorizonForecast &fc = *r;

  for (usize h = 0; h <= h_max; ++h) {
    const f64 da = std::exp2(-static_cast<f64>(h) / 2.0);
    const f64 db = std::exp2(-static_cast<f64>(h) / 3.0);
    // name 0: both sources contribute.
    EXPECT_NEAR(fc.alpha[h][0], da * a[0] + db * b[0], 1e-12) << "both h=" << h;
    // name 1: A is NaN ⇒ only B contributes (A simply not added).
    EXPECT_NEAR(fc.alpha[h][1], db * b[1], 1e-12) << "B-only h=" << h;
    // name 2: NaN in both ⇒ NaN preserved (NOT 0) for the downstream optimizer.
    EXPECT_TRUE(std::isnan(fc.alpha[h][2])) << "no-opinion h=" << h;
  }
}

// ===========================================================================
//  5. decay() edges — instant decay (halflife=0) and identity.
// ===========================================================================
TEST(RiskHorizon, DecayEdgeCases) {
  const SignalHorizon instant{0.0};
  EXPECT_EQ(instant.decay(0U), 1.0); // decay(0) == 1 even for instant decay
  EXPECT_EQ(instant.decay(1U), 0.0);
  EXPECT_EQ(instant.decay(5U), 0.0);

  const SignalHorizon id = SignalHorizon::identity();
  EXPECT_EQ(id.decay(0U), 1.0);
  EXPECT_EQ(id.decay(1U), 1.0);
  EXPECT_EQ(id.decay(100U), 1.0);

  // identity() is exactly the +inf halflife.
  EXPECT_TRUE(std::isinf(id.halflife_periods) && id.halflife_periods > 0.0);
}

// halflife=0 inside forecast_trajectory: alpha[0]==α_t, alpha[h>0]==0 (finite, not NaN
// where there is an opinion).
TEST(RiskHorizon, InstantDecayInTrajectory) {
  const std::vector<f64> a = {3.0, -5.0};
  const std::vector<Source> sources = {src(a, 0.0)};
  const usize m = 2U;
  const usize h_max = 3U;

  auto r = forecast_trajectory(std::span<const Source>(sources), m, h_max);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const HorizonForecast &fc = *r;

  for (usize i = 0; i < m; ++i) {
    EXPECT_TRUE(bit_eq(fc.alpha[0][i], a[i])) << "h=0 i=" << i; // anchor exact
  }
  for (usize h = 1; h <= h_max; ++h) {
    for (usize i = 0; i < m; ++i) {
      EXPECT_EQ(fc.alpha[h][i], 0.0) << "h=" << h << " i=" << i; // finite 0, not NaN
    }
  }
}

// ===========================================================================
//  6. Determinism (R1) — two builds are byte-identical (NaN bits included).
// ===========================================================================
TEST(RiskHorizon, DeterministicAcrossBuilds) {
  const std::vector<f64> a = {1.0, kNaN, 0.5, -2.5};
  const std::vector<f64> b = {kNaN, 4.0, -1.0, 3.0};
  const std::vector<Source> sources = {src(a, 2.5), src(b, 7.0)};
  const usize m = 4U;
  const usize h_max = 5U;

  auto r1 = forecast_trajectory(std::span<const Source>(sources), m, h_max);
  auto r2 = forecast_trajectory(std::span<const Source>(sources), m, h_max);
  ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().to_string());
  ASSERT_TRUE(r2.has_value()) << (r2 ? "" : r2.error().to_string());
  ASSERT_EQ(r1->alpha.size(), r2->alpha.size());
  for (usize h = 0; h <= h_max; ++h) {
    ASSERT_EQ((*r1).alpha[h].size(), (*r2).alpha[h].size());
    for (usize i = 0; i < m; ++i) {
      // bit_cast so NaN bit patterns compare equal (NaN == NaN bitwise).
      EXPECT_TRUE(bit_eq((*r1).alpha[h][i], (*r2).alpha[h][i]))
          << "h=" << h << " i=" << i;
    }
  }
}

// ===========================================================================
//  7. Boundaries / errors.
// ===========================================================================
TEST(RiskHorizon, ErrsOnEmptySources) {
  const std::vector<Source> sources; // empty
  auto r = forecast_trajectory(std::span<const Source>(sources), 3U, 2U);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskHorizon, ErrsOnSpanLengthMismatch) {
  const std::vector<f64> a = {1.0, 2.0, 3.0};       // length 3
  const std::vector<f64> b = {1.0, 2.0};            // length 2 — wrong
  const std::vector<Source> sources = {src(a, 2.0), src(b, 2.0)};
  auto r = forecast_trajectory(std::span<const Source>(sources), 3U, 2U);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskHorizon, ErrsOnNegativeHalflife) {
  const std::vector<f64> a = {1.0, 2.0};
  const std::vector<Source> sources = {src(a, -1.0)}; // invalid halflife
  auto r = forecast_trajectory(std::span<const Source>(sources), 2U, 2U);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskHorizon, HorizonZeroGivesSingleRowEqualToAlphaT) {
  const std::vector<f64> a = {1.0, -2.0, kNaN};
  const std::vector<Source> sources = {src(a, 3.0)};
  const usize m = 3U;

  auto r = forecast_trajectory(std::span<const Source>(sources), m, 0U);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const HorizonForecast &fc = *r;
  ASSERT_EQ(fc.H, 0U);
  ASSERT_EQ(fc.alpha.size(), 1U); // exactly one row
  for (usize i = 0; i < m; ++i) {
    EXPECT_TRUE(bit_eq(fc.alpha[0][i], a[i])) << "i=" << i; // == α_t (NaN preserved)
  }
}

TEST(RiskHorizon, SingleNameUniverse) {
  const std::vector<f64> a = {2.0};
  const std::vector<Source> sources = {src(a, 1.0)};
  const usize m = 1U;
  const usize h_max = 3U;

  auto r = forecast_trajectory(std::span<const Source>(sources), m, h_max);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const HorizonForecast &fc = *r;
  for (usize h = 0; h <= h_max; ++h) {
    ASSERT_EQ(fc.alpha[h].size(), 1U);
    EXPECT_NEAR(fc.alpha[h][0], a[0] * std::exp2(-static_cast<f64>(h) / 1.0), 1e-12)
        << "h=" << h;
  }
}


}  // namespace atxtest_risk_horizon_test
