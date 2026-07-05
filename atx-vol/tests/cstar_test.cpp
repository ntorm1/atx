#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/cstar.hpp"

// Coverage for the CStar (C16M "modal") evaluator, modal basis, block-gradient
// slicing, and surface-level calendar projection. Mirrors the C ats-vol tests
// test_vol_cstar_blocks.c (block extraction) and test_vol_cstar_calendar.c
// (calendar monotonicity + repair), plus direct eval/basis checks.

namespace {

using atx::vol::CStarBlock;
using atx::vol::CStarParams;
using atx::vol::CStarSurface;
using atx::vol::CStarTier;
using atx::vol::cstar_apply_block_step;
using atx::vol::cstar_base;
using atx::vol::cstar_basis;
using atx::vol::cstar_basis_center;
using atx::vol::cstar_block_dim;
using atx::vol::cstar_calendar_min_dw;
using atx::vol::cstar_extract_block_grad;
using atx::vol::cstar_min_roper_g;
using atx::vol::cstar_modal_indices;
using atx::vol::cstar_project_calendar;
using atx::vol::cstar_slice_grad_w;
using atx::vol::cstar_slice_iv;
using atx::vol::cstar_slice_w;
using atx::vol::cstar_tier_mask;
using atx::vol::kCStarNBase;
using atx::vol::kCStarNModes;
using atx::vol::kCStarNParams;

// ── Shared fixtures (mirror test_vol_cstar_blocks.c make_test_slice_) ───────

CStarParams make_test_slice() {
  CStarParams s{};
  s.T = 0.05;
  s.F = 100.0;
  s.theta = 0.04;  // sigma_atm ≈ 0.28
  s.s2 = 0.10;
  s.c2 = 0.50;
  s.C_left = 0.20;
  s.C_right = 0.15;
  s.active_modes = cstar_tier_mask(CStarTier::C16);
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    s.beta[j] = 0.001 * static_cast<double>(j + 1);
  }
  return s;
}

// ── Modal basis / base shape ────────────────────────────────────────────────

TEST(CStarBasis, AtCenter_IsOne) {
  EXPECT_DOUBLE_EQ(cstar_basis(5, 0.0), 1.0);   // center 0
  EXPECT_DOUBLE_EQ(cstar_basis(0, -4.0), 1.0);  // center -4
  EXPECT_DOUBLE_EQ(cstar_basis(10, 4.0), 1.0);  // center +4
}

TEST(CStarBasis, OutsideSupport_IsZero) {
  EXPECT_DOUBLE_EQ(cstar_basis(5, 1.5), 0.0);    // |u| = 1.5 > 1
  EXPECT_DOUBLE_EQ(cstar_basis(3, -10.0), 0.0);  // far outside support
  EXPECT_DOUBLE_EQ(cstar_basis(-1, 0.0), 0.0);   // out-of-range index
  EXPECT_DOUBLE_EQ(cstar_basis(11, 0.0), 0.0);   // out-of-range index
}

TEST(CStarBasis, Centers_MatchGrid) {
  EXPECT_DOUBLE_EQ(cstar_basis_center(0), -4.0);
  EXPECT_DOUBLE_EQ(cstar_basis_center(5), 0.0);
  EXPECT_DOUBLE_EQ(cstar_basis_center(10), 4.0);
}

TEST(CStarBase, Atm_IsOne) {
  // f_base(0) = 1 by construction, for any (s2, c2, C_left, C_right).
  EXPECT_DOUBLE_EQ(cstar_base(0.0, 0.1, 0.5, 0.2, 0.15), 1.0);
  EXPECT_DOUBLE_EQ(cstar_base(0.0, -0.3, 0.0, 1.0, 1.0), 1.0);
}

TEST(CStarSliceW, BaseOnly_AtmEqualsTheta) {
  CStarParams s = make_test_slice();
  s.active_modes = cstar_tier_mask(CStarTier::C5);  // base only
  EXPECT_DOUBLE_EQ(cstar_slice_w(s, 0.0), s.theta);
}

TEST(CStarSliceW, NonPositiveTheta_FloorsToEpsilon) {
  CStarParams s{};
  s.theta = 0.0;
  EXPECT_DOUBLE_EQ(cstar_slice_w(s, 0.1), 1.0e-12);
}

TEST(CStarSliceIv, EqualsSqrtWOverT) {
  CStarParams s = make_test_slice();
  s.active_modes = cstar_tier_mask(CStarTier::C5);
  const double w = cstar_slice_w(s, 0.02);
  EXPECT_DOUBLE_EQ(cstar_slice_iv(s, 0.02), std::sqrt(w / s.T));
}

TEST(CStarMinRoperG, FlatBaseSlice_IsArbFree) {
  // A flat base (all curvature params zero) has w(k) == theta, so the Roper
  // density g(k) = (1 - k*w'/2w)^2 - (w'/2)^2*(1/4 + 1/w) + w''/2 collapses to
  // exactly 1 for every k: provably butterfly-arb-free. Exercises the full
  // 240-point grid + FD derivative path returning a finite positive minimum.
  CStarParams s{};
  s.T = 0.05;
  s.F = 100.0;
  s.theta = 0.04;
  s.active_modes = cstar_tier_mask(CStarTier::C5);
  EXPECT_GT(cstar_min_roper_g(s), -1.0e-9);
}

TEST(CStarMinRoperG, HighCurvatureBaseSlice_IsFlaggedArbViolating) {
  // The block-test fixture carries deliberately large base curvature
  // (c2=0.50, wing curvatures) that violates the Roper density at the wings.
  // C5 (base-only) so the modal damper cannot repair it — the detector must
  // report a negative minimum. Verifies the g functional catches real arb.
  CStarParams s = make_test_slice();
  s.active_modes = cstar_tier_mask(CStarTier::C5);
  EXPECT_LT(cstar_min_roper_g(s), 0.0);
}

// ── Block extraction (mirror test_vol_cstar_blocks.c) ───────────────────────

TEST(CStarBlocks, BaseBlock_ExtractsFirst5Partials) {
  CStarParams s = make_test_slice();
  const auto grad = cstar_slice_grad_w(s, 0.05);
  ASSERT_TRUE(grad.has_value());

  std::array<double, kCStarNParams> out{};
  const int n = cstar_extract_block_grad(*grad, CStarBlock::Base,
                                         s.active_modes, std::span<double>{out});
  EXPECT_EQ(n, static_cast<int>(kCStarNBase));
  for (std::size_t j = 0; j < kCStarNBase; ++j) {
    EXPECT_NEAR(out[j], (*grad)[j], 1.0e-15);
  }
}

TEST(CStarBlocks, ModalBlock_C8Mask_Yields3Partials) {
  CStarParams s = make_test_slice();
  s.active_modes = cstar_tier_mask(CStarTier::C8);
  const auto grad = cstar_slice_grad_w(s, 0.0);
  ASSERT_TRUE(grad.has_value());

  std::array<double, kCStarNParams> out{};
  const int n = cstar_extract_block_grad(*grad, CStarBlock::Modal,
                                         s.active_modes, std::span<double>{out});
  EXPECT_EQ(n, 3);  // C8 mask: bits 2, 5, 8
  EXPECT_NEAR(out[0], (*grad)[kCStarNBase + 2], 1.0e-15);
  EXPECT_NEAR(out[1], (*grad)[kCStarNBase + 5], 1.0e-15);
  EXPECT_NEAR(out[2], (*grad)[kCStarNBase + 8], 1.0e-15);
}

TEST(CStarBlocks, FullBlock_C16Mask_Yields16Partials) {
  CStarParams s = make_test_slice();
  const auto grad = cstar_slice_grad_w(s, 0.10);
  ASSERT_TRUE(grad.has_value());

  std::array<double, kCStarNParams> out{};
  const int n = cstar_extract_block_grad(*grad, CStarBlock::Full,
                                         s.active_modes, std::span<double>{out});
  EXPECT_EQ(n, static_cast<int>(kCStarNParams));
  for (std::size_t j = 0; j < kCStarNParams; ++j) {
    EXPECT_NEAR(out[j], (*grad)[j], 1.0e-15);
  }
}

TEST(CStarBlocks, ModalBlock_C5Mask_YieldsZeroModes) {
  CStarParams s = make_test_slice();
  s.active_modes = cstar_tier_mask(CStarTier::C5);
  const auto grad = cstar_slice_grad_w(s, 0.0);
  ASSERT_TRUE(grad.has_value());

  std::array<double, kCStarNParams> out{};
  const int n = cstar_extract_block_grad(*grad, CStarBlock::Modal,
                                         s.active_modes, std::span<double>{out});
  EXPECT_EQ(n, 0);
}

TEST(CStarBlocks, ModalIndices_C12_MatchesExtractOrdering) {
  CStarParams s = make_test_slice();
  s.active_modes = cstar_tier_mask(CStarTier::C12);

  std::array<int, kCStarNModes> idx{};
  const int n_idx = cstar_modal_indices(s.active_modes, std::span<int>{idx});
  ASSERT_EQ(n_idx, 5);  // C12: bits 1, 3, 5, 7, 9
  EXPECT_EQ(idx[0], 1);
  EXPECT_EQ(idx[1], 3);
  EXPECT_EQ(idx[2], 5);
  EXPECT_EQ(idx[3], 7);
  EXPECT_EQ(idx[4], 9);

  const auto grad = cstar_slice_grad_w(s, 0.05);
  ASSERT_TRUE(grad.has_value());
  std::array<double, kCStarNParams> out{};
  const int n = cstar_extract_block_grad(*grad, CStarBlock::Modal,
                                         s.active_modes, std::span<double>{out});
  ASSERT_EQ(n, n_idx);
  for (int i = 0; i < n_idx; ++i) {
    EXPECT_NEAR(out[static_cast<std::size_t>(i)],
                (*grad)[kCStarNBase + static_cast<std::size_t>(idx[static_cast<std::size_t>(i)])],
                1.0e-15);
  }
}

TEST(CStarBlocks, BlockDim_MatchesTier) {
  EXPECT_EQ(cstar_block_dim(CStarBlock::Base, cstar_tier_mask(CStarTier::C16)),
            static_cast<int>(kCStarNBase));
  EXPECT_EQ(cstar_block_dim(CStarBlock::Modal, cstar_tier_mask(CStarTier::C8)), 3);
  EXPECT_EQ(cstar_block_dim(CStarBlock::Modal, cstar_tier_mask(CStarTier::C12)), 5);
  EXPECT_EQ(cstar_block_dim(CStarBlock::Full, cstar_tier_mask(CStarTier::C16)),
            static_cast<int>(kCStarNParams));
  EXPECT_EQ(cstar_block_dim(CStarBlock::Modal, cstar_tier_mask(CStarTier::C5)), 0);
}

TEST(CStarBlocks, ApplyBaseStep_UpdatesAndClamps) {
  CStarParams s = make_test_slice();
  s.active_modes = cstar_tier_mask(CStarTier::C5);
  const std::array<double, 5> dx = {0.01, 0.02, -0.1, -1.0, 0.05};
  cstar_apply_block_step(s, CStarBlock::Base, std::span<const double>{dx});
  EXPECT_DOUBLE_EQ(s.theta, 0.05);   // 0.04 + 0.01
  EXPECT_DOUBLE_EQ(s.s2, 0.12);      // 0.10 + 0.02
  EXPECT_DOUBLE_EQ(s.c2, 0.40);      // 0.50 - 0.10
  EXPECT_DOUBLE_EQ(s.C_left, 1.0e-6);  // 0.20 - 1.0 clamped to floor
  EXPECT_DOUBLE_EQ(s.C_right, 0.20);   // 0.15 + 0.05
}

// ── Calendar (mirror test_vol_cstar_calendar.c) ─────────────────────────────

CStarParams make_calendar_slice(double T, double theta) {
  CStarParams s{};
  s.T = T;
  s.F = 100.0;
  s.theta = theta;
  s.s2 = 0.0;
  s.c2 = 0.20;
  s.C_left = 0.20;
  s.C_right = 0.15;
  s.active_modes = cstar_tier_mask(CStarTier::C16);
  return s;
}

CStarSurface make_two_slice_surface(double theta_0, double theta_1) {
  auto surf = CStarSurface::create(/*uid=*/1u, /*cap_slices=*/2);
  EXPECT_TRUE(surf.has_value());
  EXPECT_TRUE(surf->set_slice(0, make_calendar_slice(0.05, theta_0)).has_value());
  EXPECT_TRUE(surf->set_slice(1, make_calendar_slice(0.20, theta_1)).has_value());
  return std::move(*surf);
}

TEST(CStarCalendar, DetectsViolation_WhenThetaDecreases) {
  // Later slice has SMALLER theta — guaranteed calendar arb at ATM.
  CStarSurface surf = make_two_slice_surface(/*t0=*/0.04, /*t1=*/0.02);
  const double dw_min = cstar_calendar_min_dw(surf.slices(), /*n_grid=*/41u);
  EXPECT_LT(dw_min, 0.0);
}

TEST(CStarCalendar, NoViolation_WhenThetaMonotone) {
  CStarSurface surf = make_two_slice_surface(/*t0=*/0.02, /*t1=*/0.08);
  const double dw_min = cstar_calendar_min_dw(surf.slices(), /*n_grid=*/41u);
  EXPECT_GT(dw_min, -1.0e-9);
}

TEST(CStarCalendar, ThetaBump_RepairsSimpleViolation) {
  CStarSurface surf = make_two_slice_surface(/*t0=*/0.04, /*t1=*/0.02);
  const double theta1_pre = surf.slices()[1].theta;

  const auto rc = cstar_project_calendar(surf.mutable_slices(), /*n_grid=*/41u,
                                         /*max_theta_bump=*/2.0);
  ASSERT_TRUE(rc.has_value());

  const double dw_min = cstar_calendar_min_dw(surf.slices(), 41u);
  EXPECT_GT(dw_min, -1.0e-9);
  // theta_1 bumped up (no modes were set, so theta is the only repair lever).
  EXPECT_GT(surf.slices()[1].theta, theta1_pre);
}

TEST(CStarCalendar, Projection_IsIdempotent) {
  CStarSurface surf = make_two_slice_surface(0.04, 0.02);
  ASSERT_TRUE(cstar_project_calendar(surf.mutable_slices(), 41u, 2.0).has_value());

  const double theta0_post = surf.slices()[0].theta;
  const double theta1_post = surf.slices()[1].theta;

  ASSERT_TRUE(cstar_project_calendar(surf.mutable_slices(), 41u, 2.0).has_value());
  EXPECT_NEAR(surf.slices()[0].theta, theta0_post, 1.0e-12);
  EXPECT_NEAR(surf.slices()[1].theta, theta1_post, 1.0e-12);
}

TEST(CStarCalendar, ModalDamping_RepairsModalViolation) {
  // Same theta; prev has +ATM-mode amplitude, curr has −ATM. Repair damps
  // curr's beta_5 toward 0 (then bumps theta to fully close the deficit).
  auto surf = CStarSurface::create(1u, 2);
  ASSERT_TRUE(surf.has_value());
  CStarParams s0 = make_calendar_slice(0.05, 0.04);
  CStarParams s1 = make_calendar_slice(0.20, 0.04);
  s0.beta[5] = +0.05;
  s1.beta[5] = -0.05;
  ASSERT_TRUE(surf->set_slice(0, s0).has_value());
  ASSERT_TRUE(surf->set_slice(1, s1).has_value());

  const double dw_pre = cstar_calendar_min_dw(surf->slices(), 41u);
  EXPECT_LT(dw_pre, 0.0);

  ASSERT_TRUE(cstar_project_calendar(surf->mutable_slices(), 41u, 2.0).has_value());

  EXPECT_GT(surf->slices()[1].beta[5], -0.05);  // moved up from -0.05 toward 0
  const double dw_post = cstar_calendar_min_dw(surf->slices(), 41u);
  EXPECT_GT(dw_post, -1.0e-9);
}

TEST(CStarCalendar, NoButterflyRegression_AfterProjection) {
  CStarSurface surf = make_two_slice_surface(0.04, 0.02);
  ASSERT_TRUE(cstar_project_calendar(surf.mutable_slices(), 41u, 2.0).has_value());
  for (const CStarParams& s : surf.slices()) {
    EXPECT_GT(cstar_min_roper_g(s), -1.0e-6);
  }
}

// ── Surface time interpolation (mirrors vol_surface w/iv semantics) ─────────

TEST(CStarSurface, WInterpolatesLinearInTotalVariance) {
  auto surf = CStarSurface::create(1u, 2);
  ASSERT_TRUE(surf.has_value());
  ASSERT_TRUE(surf->set_slice(0, make_calendar_slice(0.05, 0.02)).has_value());
  ASSERT_TRUE(surf->set_slice(1, make_calendar_slice(0.20, 0.08)).has_value());

  const double w0 = cstar_slice_w(surf->slices()[0], 0.0);
  const double w1 = cstar_slice_w(surf->slices()[1], 0.0);
  const double Tmid = 0.5 * (0.05 + 0.20);
  const double alpha = (Tmid - 0.05) / (0.20 - 0.05);
  EXPECT_NEAR(surf->w(0.0, Tmid), w0 + alpha * (w1 - w0), 1.0e-12);
}

TEST(CStarSurface, WPastLastSlice_IsNaN) {
  auto surf = CStarSurface::create(1u, 1);
  ASSERT_TRUE(surf.has_value());
  ASSERT_TRUE(surf->set_slice(0, make_calendar_slice(0.05, 0.02)).has_value());
  EXPECT_TRUE(std::isnan(surf->w(0.0, 1.0)));
}

}  // namespace
