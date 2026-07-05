#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "atx/vol/fit_metrics.hpp"

// Coverage for the Vola-style fit-quality metrics: per-quote vol error bars,
// reduced chi-square, minimum-edge verdicts, avE5, and the end-to-end slice
// bundle. Inputs are hand-chosen so every expectation is checkable in closed
// form (flat error bars, integer standardized residuals).

namespace {

using atx::vol::avg_abs_error_e5;
using atx::vol::ChiSquareResult;
using atx::vol::EdgeResult;
using atx::vol::ErrorCode;
using atx::vol::minimum_edge;
using atx::vol::reduced_chi_square;
using atx::vol::slice_fit_metrics;
using atx::vol::SliceFitMetrics;
using atx::vol::vol_error_bar;

// ── reduced_chi_square ───────────────────────────────────────────────────

TEST(FitMetrics, ReducedChiSquare_PerfectFit_ZeroChi2) {
  const std::vector<double> resid{0.0, 0.0, 0.0, 0.0};
  const std::vector<double> err{0.01, 0.02, 0.005, 0.03};
  const auto res = reduced_chi_square(resid, err, /*dof=*/2);
  ASSERT_TRUE(res.has_value());
  EXPECT_DOUBLE_EQ(res->chi2, 0.0);
  EXPECT_DOUBLE_EQ(res->chi2_reduced, 0.0);
  EXPECT_EQ(res->n, std::size_t{4});
  EXPECT_EQ(res->dof, std::size_t{2});
}

TEST(FitMetrics, ReducedChiSquare_UnitStandardizedResiduals_ReducedIsOne) {
  // resid_i == err_bar_i ⇒ each standardized residual is 1 ⇒ chi2 = N.
  const std::vector<double> err{0.01, 0.02, 0.005, 0.03};
  const std::vector<double> resid = err;  // copy: resid_i = err_i
  const auto res = reduced_chi_square(resid, err, /*dof=*/0);
  ASSERT_TRUE(res.has_value());
  EXPECT_NEAR(res->chi2, 4.0, 1e-12);
  EXPECT_NEAR(res->chi2_reduced, 1.0, 1e-12);  // N/(N-0) = 4/4
}

TEST(FitMetrics, ReducedChiSquare_UnitResidualsWithDof_ReducedIsNOverNminusDof) {
  const std::vector<double> err{0.01, 0.02, 0.005, 0.03, 0.04};
  const std::vector<double> resid = err;  // 5 unit standardized residuals
  const auto res = reduced_chi_square(resid, err, /*dof=*/2);
  ASSERT_TRUE(res.has_value());
  EXPECT_NEAR(res->chi2, 5.0, 1e-12);
  EXPECT_NEAR(res->chi2_reduced, 5.0 / 3.0, 1e-12);  // 5 / (5 - 2)
}

TEST(FitMetrics, ReducedChiSquare_NLeqDof_ReturnsErr) {
  const std::vector<double> resid{0.01, 0.02};
  const std::vector<double> err{0.01, 0.02};
  const auto res = reduced_chi_square(resid, err, /*dof=*/2);  // N == dof
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(FitMetrics, ReducedChiSquare_Empty_ReturnsErr) {
  const std::vector<double> empty{};
  const auto res = reduced_chi_square(empty, empty, /*dof=*/0);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(FitMetrics, ReducedChiSquare_LengthMismatch_ReturnsErr) {
  const std::vector<double> resid{0.01, 0.02, 0.03};
  const std::vector<double> err{0.01, 0.02};
  const auto res = reduced_chi_square(resid, err, /*dof=*/0);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(FitMetrics, ReducedChiSquare_NonPositiveErrorBar_ReturnsErr) {
  const std::vector<double> resid{0.01, 0.02, 0.03};
  const std::vector<double> err{0.01, 0.0, 0.02};  // zero bar ⇒ div-by-zero
  const auto res = reduced_chi_square(resid, err, /*dof=*/0);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── vol_error_bar ────────────────────────────────────────────────────────

TEST(FitMetrics, VolErrorBar_MonotonicInSpread_Increases) {
  const double vega = 1.0;  // unclamped region: bar = 0.5*spread
  const double tight = vol_error_bar(0.10, 0.12, vega);   // spread 0.02
  const double mid = vol_error_bar(0.10, 0.16, vega);     // spread 0.06
  const double wide = vol_error_bar(0.10, 0.20, vega);    // spread 0.10
  EXPECT_LT(tight, mid);
  EXPECT_LT(mid, wide);
  EXPECT_NEAR(tight, 0.01, 1e-12);
  EXPECT_NEAR(wide, 0.05, 1e-12);
}

TEST(FitMetrics, VolErrorBar_MonotonicInVega_Decreases) {
  const double bid = 0.10, ask = 0.20;  // fixed half-spread 0.05
  const double lo_vega = vol_error_bar(bid, ask, 0.5);  // 0.05/0.5 = 0.10
  const double mid_vega = vol_error_bar(bid, ask, 1.0); // 0.05/1.0 = 0.05
  const double hi_vega = vol_error_bar(bid, ask, 2.0);  // 0.05/2.0 = 0.025
  EXPECT_GT(lo_vega, mid_vega);
  EXPECT_GT(mid_vega, hi_vega);
  EXPECT_NEAR(mid_vega, 0.05, 1e-12);
}

TEST(FitMetrics, VolErrorBar_TightSpread_FlooredAtMin) {
  const double min_bar = 1e-4;
  // Zero spread and crossed spread both floor to min_bar.
  EXPECT_DOUBLE_EQ(vol_error_bar(0.10, 0.10, 1.0, min_bar, 5.0), min_bar);
  EXPECT_DOUBLE_EQ(vol_error_bar(0.12, 0.10, 1.0, min_bar, 5.0), min_bar);
}

TEST(FitMetrics, VolErrorBar_TinyVega_CappedAtMax) {
  const double max_bar = 5.0;
  EXPECT_DOUBLE_EQ(vol_error_bar(0.10, 0.20, 1e-12, 1e-4, max_bar), max_bar);
  // Zero / non-positive vega maps to the widest bar too.
  EXPECT_DOUBLE_EQ(vol_error_bar(0.10, 0.20, 0.0, 1e-4, max_bar), max_bar);
}

// ── minimum_edge ─────────────────────────────────────────────────────────

TEST(FitMetrics, MinimumEdge_SmallDiff_WithinBand) {
  const EdgeResult e = minimum_edge(0.201, 0.200, /*err=*/0.01, /*k=*/1.0);
  EXPECT_TRUE(e.within_band);  // |0.001| < 0.01
  EXPECT_NEAR(e.edge_vol, 0.001, 1e-12);
  EXPECT_NEAR(e.n_sigma, 0.1, 1e-12);
}

TEST(FitMetrics, MinimumEdge_LargeDiff_OutsideBand) {
  const EdgeResult e = minimum_edge(0.250, 0.200, /*err=*/0.01, /*k=*/1.0);
  EXPECT_FALSE(e.within_band);  // |0.05| > 0.01
  EXPECT_NEAR(e.edge_vol, 0.05, 1e-12);
  EXPECT_NEAR(e.n_sigma, 5.0, 1e-12);
}

TEST(FitMetrics, MinimumEdge_ModelBelowMarket_NegativeSignedEdge) {
  const EdgeResult e = minimum_edge(0.190, 0.200, /*err=*/0.02, /*k=*/1.0);
  EXPECT_TRUE(e.within_band);  // |-0.01| < 0.02
  EXPECT_LT(e.edge_vol, 0.0);
  EXPECT_LT(e.n_sigma, 0.0);
  EXPECT_NEAR(e.edge_vol, -0.01, 1e-12);
  EXPECT_NEAR(e.n_sigma, -0.5, 1e-12);
}

TEST(FitMetrics, MinimumEdge_WiderK_ExpandsBand) {
  // |diff| = 0.015 sits outside 1σ but inside 2σ of a 0.01 bar.
  EXPECT_FALSE(minimum_edge(0.215, 0.200, 0.01, 1.0).within_band);
  EXPECT_TRUE(minimum_edge(0.215, 0.200, 0.01, 2.0).within_band);
}

// ── avg_abs_error_e5 ─────────────────────────────────────────────────────

TEST(FitMetrics, AvgAbsErrorE5_KnownVector_MatchesHand) {
  const std::vector<double> resid{0.001, -0.002, 0.003};
  // mean(|r|) = (0.001 + 0.002 + 0.003)/3 = 0.002 ⇒ ·1e5 = 200.
  EXPECT_NEAR(avg_abs_error_e5(resid), 200.0, 1e-9);
}

TEST(FitMetrics, AvgAbsErrorE5_Empty_IsZero) {
  const std::vector<double> empty{};
  EXPECT_DOUBLE_EQ(avg_abs_error_e5(empty), 0.0);
}

// ── slice_fit_metrics ────────────────────────────────────────────────────

TEST(FitMetrics, SliceFitMetrics_PerfectFit_ZeroErrorAllWithinBand) {
  const std::vector<double> model{0.20, 0.21, 0.19, 0.22};
  const std::vector<double> mkt = model;  // identical ⇒ every residual is 0
  const std::vector<double> bid{0.10, 0.10, 0.10, 0.10};
  const std::vector<double> ask{0.12, 0.12, 0.12, 0.12};
  const std::vector<double> vega{1.0, 1.0, 1.0, 1.0};
  const auto res = slice_fit_metrics(model, mkt, bid, ask, vega, /*dof=*/0);
  ASSERT_TRUE(res.has_value());
  EXPECT_DOUBLE_EQ(res->rmse_vol, 0.0);
  EXPECT_DOUBLE_EQ(res->rmse_vol_weighted, 0.0);
  EXPECT_DOUBLE_EQ(res->chi2_reduced, 0.0);
  EXPECT_DOUBLE_EQ(res->avE5_vol, 0.0);
  EXPECT_EQ(res->n, std::size_t{4});
  EXPECT_EQ(res->n_within_band, std::size_t{4});  // 0 < err for all
}

TEST(FitMetrics, SliceFitMetrics_SyntheticSlice_MatchesHand) {
  // vega = 1 and (ask - bid) = 0.02 for every quote ⇒ err_bar = 0.01.
  //   i0: r = 0.000  → z=0.0 , within (0    < 0.01)
  //   i1: r = 0.015  → z=1.5 , outside (0.015 > 0.01)
  //   i2: r = 0.005  → z=0.5 , within (0.005 < 0.01)
  //   i3: r = 0.020  → z=2.0 , outside (0.020 > 0.01)
  // Σ z² = 0 + 2.25 + 0.25 + 4 = 6.5 ; dof=0 ⇒ chi2_reduced = 6.5/4 = 1.625.
  const std::vector<double> model{0.200, 0.215, 0.205, 0.220};
  const std::vector<double> mkt{0.200, 0.200, 0.200, 0.200};
  const std::vector<double> bid{0.10, 0.10, 0.10, 0.10};
  const std::vector<double> ask{0.12, 0.12, 0.12, 0.12};
  const std::vector<double> vega{1.0, 1.0, 1.0, 1.0};
  const auto res = slice_fit_metrics(model, mkt, bid, ask, vega, /*dof=*/0);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->n, std::size_t{4});
  EXPECT_EQ(res->n_within_band, std::size_t{2});
  EXPECT_NEAR(res->chi2_reduced, 1.625, 1e-9);
  // Unweighted RMSE: sqrt((0 + 0.015² + 0.005² + 0.020²)/4) = sqrt(6.5e-4/... )
  const double expect_rmse =
      std::sqrt((0.0 + 0.015 * 0.015 + 0.005 * 0.005 + 0.020 * 0.020) / 4.0);
  EXPECT_NEAR(res->rmse_vol, expect_rmse, 1e-9);
  // Flat error bars ⇒ weighted RMSE collapses to the unweighted RMSE.
  EXPECT_NEAR(res->rmse_vol_weighted, expect_rmse, 1e-9);
  // avE5 = mean(|r|)·1e5 = ((0+0.015+0.005+0.020)/4)·1e5 = 0.01·1e5 = 1000.
  EXPECT_NEAR(res->avE5_vol, 1000.0, 1e-6);
}

TEST(FitMetrics, SliceFitMetrics_LengthMismatch_ReturnsErr) {
  const std::vector<double> model{0.20, 0.21};
  const std::vector<double> mkt{0.20, 0.21, 0.22};  // longer
  const std::vector<double> bid{0.10, 0.10};
  const std::vector<double> ask{0.12, 0.12};
  const std::vector<double> vega{1.0, 1.0};
  const auto res = slice_fit_metrics(model, mkt, bid, ask, vega, /*dof=*/0);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(FitMetrics, SliceFitMetrics_Empty_ReturnsErr) {
  const std::vector<double> empty{};
  const auto res = slice_fit_metrics(empty, empty, empty, empty, empty, 0);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(FitMetrics, SliceFitMetrics_NLeqDof_ReturnsErr) {
  const std::vector<double> model{0.20, 0.21, 0.19};
  const std::vector<double> mkt{0.20, 0.21, 0.19};
  const std::vector<double> bid{0.10, 0.10, 0.10};
  const std::vector<double> ask{0.12, 0.12, 0.12};
  const std::vector<double> vega{1.0, 1.0, 1.0};
  const auto res = slice_fit_metrics(model, mkt, bid, ask, vega, /*dof=*/3);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
