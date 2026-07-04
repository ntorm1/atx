// gp_turnover_test.cpp — S4-5a [OPT-IN, B8]: `garleanu_pedersen.hpp` ships the
// closed-form GP aim (`gp_aim_and_value`) but the book's existing partial-trade
// step (S1/S7, `atx-impl/src/stage_optimize.cpp:159-172`) blends toward the
// freshly-shaped TARGET each period (`w := prev + rate*(target-prev)`), never
// toward the GP AIM this header already computes. For a mean-reverting/
// decaying signal the per-period target can swing sharply even though the aim
// (which already folds in the signal's own decay path) barely moves -- the
// linear blend churns the book chasing noise the aim already discounted.
//
// `gp_turnover_native_step` (new) ships the turnover-native replacement step,
// literally `w := prev + kappa*(aim_pos - prev)` -- same functional FORM as
// today's blend, just fed the aim instead of the target. This is the header's
// pure PRODUCER only; wiring it into the live book is an S1/S5 seam at
// stage_optimize.cpp (S4 must not edit that file -- recorded in the ledger).
//
// Load-bearing checks:
//   (a) GpTurnoverReduces -- a by-construction 2-period fixture (a target path
//       that swings sharply, an aim path that anticipates the reversal and
//       stays damped) where a target-blend rate is ALGEBRAICALLY SOLVED so
//       both methods land at the SAME final tracking error to the (moving)
//       aim -- and the aim-blend's cumulative turnover is strictly lower.
//   (b) GpFullRateByteIdentical -- trade_rate==1.0 => w == aim_pos EXACTLY,
//       a pure algebraic identity (any prev, any aim_pos), AND, chained with
//       the header's ALREADY-documented degenerate collapse (H=1 + identity
//       decay => ⍺̄ == α_t bit-identical => aim_pos IS the single-period
//       Markowitz target), a REAL gp_aim_and_value call at full rate is
//       byte-identical to the plain (1/2λ)V⁻¹α Markowitz solve.
//   (c) PureFunction_DeterministicAcrossRunsAndZeroPrev -- no allocation
//       surprises, no state; a zero-prev call degenerates to trade_rate*aim.

#include <cmath> // std::sqrt, std::fabs

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/garleanu_pedersen.hpp"

namespace atxtest_gp_turnover_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::FactorModel;
using atx::engine::risk::gp_aim_and_value;
using atx::engine::risk::gp_turnover_native_step;

// =============================================================================
//  (a) GpTurnoverReduces -- by-construction fixture, 1 instrument, 2 periods.
//
//  Raw per-period targets (the freshly-shaped, undamped book each period would
//  chase): target_1 = +1.0, target_2 = -1.0 (a sharp reversal).
//  GP aim (decay-anticipated, damped toward the reversion): aim_1 = +0.4,
//  aim_2 = -0.4 (SAME shape, 2.5x smaller magnitude -- exactly what a
//  fast-decaying/mean-reverting source's horizon-average produces, per this
//  header's own documented decay-weighting story).
//
//  Aim-method at trade_rate=1.0 (full rate): w_1 = aim_1 = 0.4 (Δ=0.4);
//  w_2 = aim_2 = -0.4 (Δ=0.8). Turnover_aim = 1.2. Final tracking error to
//  aim_2 is EXACTLY 0 (w_2 == aim_2 by the full-rate identity).
//
//  Target-method rate r solved so ITS w_2 ALSO lands exactly on aim_2 (equal
//  tracking error, both 0), for a fair turnover comparison:
//    w_1(r) = r*target_1 = r
//    w_2(r) = w_1 + r*(target_2 - w_1) = r + r*(-1-r) = -r^2
//    set -r^2 = aim_2 = -0.4  =>  r = sqrt(0.4)
//  Turnover_target(r) = |w_1| + |w_2-w_1| = r + |-r^2 - r| = r + r + r^2
//                      = 2r + r^2 (all terms same-signed here: r>0, r^2>0,
//                      and w_2-w_1 = -r^2-r is negative, magnitude r+r^2).
// =============================================================================
TEST(GpTurnover, GpTurnoverReduces) {
  constexpr f64 kTarget1 = 1.0, kTarget2 = -1.0;
  constexpr f64 kAim1 = 0.4, kAim2 = -0.4;
  constexpr f64 kPrev0 = 0.0;

  // ---- Aim-method: full trade-rate (kappa=1.0) each period. ----
  const std::vector<f64> prev0{kPrev0};
  const std::vector<f64> aim1{kAim1};
  const auto w1_aim = gp_turnover_native_step(std::span<const f64>{prev0},
                                              std::span<const f64>{aim1}, 1.0);
  ASSERT_EQ(w1_aim.size(), 1U);
  EXPECT_DOUBLE_EQ(w1_aim[0], kAim1);
  const std::vector<f64> aim2{kAim2};
  const auto w2_aim =
      gp_turnover_native_step(std::span<const f64>{w1_aim}, std::span<const f64>{aim2}, 1.0);
  ASSERT_EQ(w2_aim.size(), 1U);
  EXPECT_DOUBLE_EQ(w2_aim[0], kAim2);

  const f64 turnover_aim =
      std::fabs(w1_aim[0] - kPrev0) + std::fabs(w2_aim[0] - w1_aim[0]); // 0.4 + 0.8 = 1.2
  EXPECT_NEAR(turnover_aim, 1.2, 1e-12);
  const f64 tracking_error_aim = std::fabs(w2_aim[0] - kAim2); // exactly 0 (full rate)
  EXPECT_NEAR(tracking_error_aim, 0.0, 1e-12);

  // ---- Target-method: the SAME functional step, fed the TARGET path, at the
  // algebraically-solved rate r=sqrt(0.4) that forces an EQUAL (zero) final
  // tracking error to aim_2 -- the fair basis for a turnover comparison.
  const f64 r = std::sqrt(0.4);
  const std::vector<f64> tgt1{kTarget1};
  const auto w1_tgt = gp_turnover_native_step(std::span<const f64>{prev0},
                                              std::span<const f64>{tgt1}, r);
  const std::vector<f64> tgt2{kTarget2};
  const auto w2_tgt =
      gp_turnover_native_step(std::span<const f64>{w1_tgt}, std::span<const f64>{tgt2}, r);
  ASSERT_EQ(w2_tgt.size(), 1U);
  EXPECT_NEAR(w2_tgt[0], kAim2, 1e-9)
      << "the solved rate must land the target-method exactly on aim_2 (equal tracking error)";

  const f64 turnover_target = std::fabs(w1_tgt[0] - kPrev0) + std::fabs(w2_tgt[0] - w1_tgt[0]);
  const f64 tracking_error_target = std::fabs(w2_tgt[0] - kAim2); // ~0 by construction

  EXPECT_NEAR(tracking_error_target, tracking_error_aim, 1e-9)
      << "both methods must be compared AT EQUAL tracking error to the (moving) aim";
  EXPECT_LT(turnover_aim, turnover_target)
      << "at equal tracking error to the aim, the turnover-native GP step must realize "
         "STRICTLY LOWER cumulative turnover than blending toward the raw, undamped target "
         "(turnover_aim=" << turnover_aim << " turnover_target=" << turnover_target << ")";
}

// =============================================================================
//  (b) GpFullRateByteIdentical -- trade_rate==1.0 is a pure algebraic identity
//  (any prev, any aim_pos), AND a REAL gp_aim_and_value call at H=1/identity
//  decay collapses to the plain single-period Markowitz target (1/2λ)V⁻¹α.
// =============================================================================
TEST(GpTurnover, GpFullRateByteIdentical_PureAlgebra) {
  const std::vector<f64> prev{0.37, -1.84, 5.0, 0.0};
  const std::vector<f64> aim{-2.1, 0.0, 5.0, 9.99};
  const auto w = gp_turnover_native_step(std::span<const f64>{prev}, std::span<const f64>{aim}, 1.0);
  ASSERT_EQ(w.size(), aim.size());
  for (usize i = 0; i < aim.size(); ++i) {
    EXPECT_EQ(w[i], aim[i]) << "trade_rate==1.0 must return aim_pos EXACTLY at index " << i
                            << " regardless of prev";
  }
}

TEST(GpTurnover, GpFullRateByteIdentical_RealMarkowitzTarget) {
  // A small M=2 FactorModel with a hand-invertible V (mirrors the existing
  // ClosedFormMatchesHandDerivedGroundTruth fixture's V):
  //   X = [0.5; -0.5], F = [2.0], D = [0.3, 0.4]
  //   V = [[0.8, -0.5], [-0.5, 0.9]]; det = 0.47
  MatX x(2, 1);
  x(0, 0) = 0.5;
  x(1, 0) = -0.5;
  MatX f = MatX::Constant(1, 1, 2.0);
  VecX d(2);
  d[0] = 0.3;
  d[1] = 0.4;
  auto mr = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  ASSERT_TRUE(mr.has_value()) << (mr ? "" : mr.error().to_string());
  const FactorModel v = std::move(*mr);

  const f64 v00 = 0.8, v01 = -0.5, v11 = 0.9;
  const f64 det = v00 * v11 - v01 * v01; // 0.47
  const f64 iv00 = v11 / det, iv01 = -v01 / det, iv11 = v00 / det;

  const std::vector<f64> alpha{1.2, -0.6}; // a plain (no-decay) alpha_bar
  const f64 lambda = 0.9;
  const f64 sc = 1.0 / (2.0 * lambda);
  // The plain single-period Markowitz target, hand-derived WITHOUT gp_aim_and_value.
  const f64 markowitz0 = sc * (iv00 * alpha[0] + iv01 * alpha[1]);
  const f64 markowitz1 = sc * (iv01 * alpha[0] + iv11 * alpha[1]);

  auto gp = gp_aim_and_value(std::span<const f64>{alpha}, v, lambda);
  ASSERT_TRUE(gp.has_value()) << (gp ? "" : gp.error().to_string());
  ASSERT_EQ(gp->aim_pos.size(), 2U);
  EXPECT_NEAR(gp->aim_pos[0], markowitz0, 1e-9);
  EXPECT_NEAR(gp->aim_pos[1], markowitz1, 1e-9);

  // At full trade-rate the step is byte-identical to this Markowitz target --
  // the boundary pin, chained end-to-end through a REAL gp_aim_and_value call.
  const std::vector<f64> prev{10.0, -10.0}; // an arbitrary, far-away prior book
  const auto w =
      gp_turnover_native_step(std::span<const f64>{prev}, std::span<const f64>{gp->aim_pos}, 1.0);
  ASSERT_EQ(w.size(), 2U);
  EXPECT_EQ(w[0], gp->aim_pos[0]);
  EXPECT_EQ(w[1], gp->aim_pos[1]);
  EXPECT_NEAR(w[0], markowitz0, 1e-9);
  EXPECT_NEAR(w[1], markowitz1, 1e-9);
}

// =============================================================================
//  (c) Determinism + the zero-prev degenerate case (w = trade_rate*aim_pos).
// =============================================================================
TEST(GpTurnover, PureFunction_DeterministicAcrossRunsAndZeroPrev) {
  const std::vector<f64> prev{0.0, 0.0, 0.0};
  const std::vector<f64> aim{3.0, -6.0, 0.75};
  const f64 rate = 0.3;
  const auto a = gp_turnover_native_step(std::span<const f64>{prev}, std::span<const f64>{aim}, rate);
  const auto b = gp_turnover_native_step(std::span<const f64>{prev}, std::span<const f64>{aim}, rate);
  ASSERT_EQ(a.size(), 3U);
  ASSERT_EQ(b.size(), 3U);
  for (usize i = 0; i < 3U; ++i) {
    EXPECT_EQ(a[i], b[i]) << "must be a pure, deterministic function of its inputs";
    EXPECT_DOUBLE_EQ(a[i], rate * aim[i]) << "zero prev => w == trade_rate*aim_pos exactly";
  }
}

} // namespace atxtest_gp_turnover_test
