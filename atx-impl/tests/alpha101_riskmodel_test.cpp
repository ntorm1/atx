// atx-impl — Alpha101 Phase 2 risk-model builder unit tests.
//
// Tests:
//   PSD + shapes     — build on a complete-case deterministic R (N=200, T=60, K=15);
//                      verify n_factors()==K, n_instruments()==N, risk(w) >= -1e-9.
//   PIT (causal)     — use the Panel layer on the synthetic ORATS panel;
//                      poison every returns cell at date > d with 1e30;
//                      re-build via the span overload (poisoned copy);
//                      assert model is elementwise IDENTICAL to the un-poisoned build.
//   Determinism      — build twice, assert exposures()/factor_cov()/specific_var()
//                      are elementwise == (no RNG in the APCA pipeline).
//   Error cases      — N <= T returns Err, d < window-1 returns Err, T <= K returns Err.

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/core/linalg/linalg.hpp"

#include "alpha101_support.hpp"      // make_synth_orats_panel, augment_for_alpha101
#include "alpha101_riskmodel.hpp"    // fit_stat_factor_model, build_stat_risk_model, ...

namespace atxtest_alpha101_riskmodel {

using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx_impl_test::StatModelCfg;
using atx_impl_test::fit_stat_factor_model;
using atx_impl_test::build_stat_risk_model;
using atx_impl_test::build_stat_risk_model_from_returns;
using atx_impl_test::RiskModelResult;

// ===========================================================================
//  Helper: build a deterministic complete-case N×T return matrix using a
//  pure formula (no RNG, no rand()).  We use:
//    R(n, t) = 0.01 * sin(0.1*n + 0.07*t) + 0.005 * cos(0.13*n - 0.09*t)
//  This produces a non-degenerate cross-correlated return matrix that satisfies
//  the APCA preconditions when N > T.
// ===========================================================================
[[nodiscard]] static MatX make_deterministic_R(atx::usize N, atx::usize T) {
  MatX R(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (atx::usize n = 0; n < N; ++n) {
    for (atx::usize t = 0; t < T; ++t) {
      const double fn = static_cast<double>(n);
      const double ft = static_cast<double>(t);
      R(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(t)) =
          0.01 * std::sin(0.1 * fn + 0.07 * ft) +
          0.005 * std::cos(0.13 * fn - 0.09 * ft);
    }
  }
  return R;
}

// ===========================================================================
//  TEST 1: PSD + shapes
//
//  N=200, T=60, K=15. Verifies:
//    - fit_stat_factor_model succeeds.
//    - n_factors() == K, n_instruments() == N.
//    - PSD: risk(w) >= -1e-9 for several deterministic test vectors w.
// ===========================================================================
TEST(Alpha101RiskModel, PsdAndShapes) {
  constexpr atx::usize N = 200;
  constexpr atx::usize T = 60;
  constexpr atx::usize K = 15;
  constexpr atx::usize fit_begin = 0;
  constexpr atx::usize fit_end   = T;

  const MatX R = make_deterministic_R(N, T);
  const StatModelCfg cfg{K, /*gls_reweight=*/true};

  auto result = fit_stat_factor_model(R, cfg, fit_begin, fit_end);
  ASSERT_TRUE(result.has_value())
      << "fit_stat_factor_model failed: " << result.error().message();

  const auto &model = result.value();
  EXPECT_EQ(model.n_factors(),      K) << "n_factors() mismatch";
  EXPECT_EQ(model.n_instruments(),  N) << "n_instruments() mismatch";

  // fit window metadata.
  EXPECT_EQ(model.fit_begin(), fit_begin);
  EXPECT_EQ(model.fit_end(),   fit_end);

  // PSD: wᵀ V w >= -1e-9 for several deterministic test vectors.
  // V = X F Xᵀ + D is PSD by construction (F SPD, D > 0), so the quadratic
  // form should be non-negative (tiny floating-point negatives are acceptable
  // within the -1e-9 guard).
  const double kPsdTol = -1e-9;

  // Test vector 1: unit vector e_0.
  {
    std::vector<atx::f64> w(N, 0.0);
    w[0] = 1.0;
    const atx::f64 rsk = model.risk(w);
    EXPECT_GE(rsk, kPsdTol) << "PSD violation: risk(e_0) = " << rsk;
  }
  // Test vector 2: all-ones / sqrt(N) (normalized long-only).
  {
    std::vector<atx::f64> w(N, 1.0 / std::sqrt(static_cast<double>(N)));
    const atx::f64 rsk = model.risk(w);
    EXPECT_GE(rsk, kPsdTol) << "PSD violation: risk(uniform) = " << rsk;
  }
  // Test vector 3: long-short alternating sign.
  {
    std::vector<atx::f64> w(N, 0.0);
    for (atx::usize i = 0; i < N; ++i) {
      w[i] = ((i % 2 == 0) ? 1.0 : -1.0) / static_cast<double>(N);
    }
    const atx::f64 rsk = model.risk(w);
    EXPECT_GE(rsk, kPsdTol) << "PSD violation: risk(long-short) = " << rsk;
  }
  // Test vector 4: single late instrument (e_{N-1}).
  {
    std::vector<atx::f64> w(N, 0.0);
    w[N - 1] = 1.0;
    const atx::f64 rsk = model.risk(w);
    EXPECT_GE(rsk, kPsdTol) << "PSD violation: risk(e_{N-1}) = " << rsk;
  }
  // Test vector 5: zero weight (must be exactly 0).
  {
    std::vector<atx::f64> w(N, 0.0);
    const atx::f64 rsk = model.risk(w);
    EXPECT_GE(rsk, kPsdTol) << "PSD violation: risk(0) = " << rsk;
  }
}

// ===========================================================================
//  TEST 2: Determinism
//
//  Build the same R twice (same cfg, same window bounds) and assert the
//  estimated components are elementwise == (no RNG, sign-pinned eigenvectors,
//  order-fixed reductions — byte-identical on replay).
// ===========================================================================
TEST(Alpha101RiskModel, Determinism) {
  constexpr atx::usize N = 200;
  constexpr atx::usize T = 60;
  constexpr atx::usize K = 15;
  const MatX R = make_deterministic_R(N, T);
  const StatModelCfg cfg{K, /*gls_reweight=*/true};

  auto r1 = fit_stat_factor_model(R, cfg, 0, T);
  auto r2 = fit_stat_factor_model(R, cfg, 0, T);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  const auto &m1 = r1.value();
  const auto &m2 = r2.value();

  // Shapes.
  ASSERT_EQ(m1.n_factors(),     m2.n_factors());
  ASSERT_EQ(m1.n_instruments(), m2.n_instruments());

  // exposures() — M×K matrix.
  const MatX &X1 = m1.exposures();
  const MatX &X2 = m2.exposures();
  ASSERT_EQ(X1.rows(), X2.rows());
  ASSERT_EQ(X1.cols(), X2.cols());
  for (Eigen::Index r = 0; r < X1.rows(); ++r) {
    for (Eigen::Index c = 0; c < X1.cols(); ++c) {
      EXPECT_EQ(X1(r, c), X2(r, c))
          << "exposures() differ at (" << r << "," << c << ")";
    }
  }

  // factor_cov() — K×K matrix.
  const MatX &F1 = m1.factor_cov();
  const MatX &F2 = m2.factor_cov();
  ASSERT_EQ(F1.rows(), F2.rows());
  ASSERT_EQ(F1.cols(), F2.cols());
  for (Eigen::Index r = 0; r < F1.rows(); ++r) {
    for (Eigen::Index c = 0; c < F1.cols(); ++c) {
      EXPECT_EQ(F1(r, c), F2(r, c))
          << "factor_cov() differ at (" << r << "," << c << ")";
    }
  }

  // specific_var() — length-N vector.
  const VecX &D1 = m1.specific_var();
  const VecX &D2 = m2.specific_var();
  ASSERT_EQ(D1.size(), D2.size());
  for (Eigen::Index i = 0; i < D1.size(); ++i) {
    EXPECT_EQ(D1[i], D2[i]) << "specific_var() differ at [" << i << "]";
  }
}

// ===========================================================================
//  TEST 3: Error cases (clean Err, no crash)
// ===========================================================================
TEST(Alpha101RiskModel, ErrorCases) {
  const StatModelCfg cfg{/*K=*/15, /*gls_reweight=*/true};

  // (a) N <= T — violates APCA precondition.
  {
    const MatX R = make_deterministic_R(/*N=*/50, /*T=*/60); // N < T
    auto result = fit_stat_factor_model(R, cfg, 0, 60);
    EXPECT_FALSE(result.has_value()) << "expected Err for N <= T";
  }
  // (b) T <= K — not enough dates to extract K factors.
  {
    const MatX R = make_deterministic_R(/*N=*/200, /*T=*/14); // T=14 < K=15
    StatModelCfg cfg2{/*K=*/15};
    auto result = fit_stat_factor_model(R, cfg2, 0, 14);
    EXPECT_FALSE(result.has_value()) << "expected Err for T <= K";
  }
  // (c) Panel layer: d < window-1.
  {
    // Build a minimal synthetic panel with the returns field.
    atx_impl_test::Panel synth = atx_impl_test::make_synth_orats_panel(/*dates=*/300, /*instruments=*/50);
    auto aug = atx_impl_test::augment_for_alpha101(synth, {});
    ASSERT_TRUE(aug.has_value());
    const auto &panel = aug.value();

    // d=5, window=100 -> d < window-1 (5 < 99).
    auto result = build_stat_risk_model(panel, /*d=*/5, /*window=*/100, cfg);
    EXPECT_FALSE(result.has_value()) << "expected Err for d < window-1";
  }
}

// ===========================================================================
//  TEST 4: PIT (Point-In-Time, causal gather)
//
//  This is the most important test. Uses the Panel layer on the synthetic
//  ORATS panel (augmented with "returns"). Picks a mid date d. Builds the
//  risk model. Then:
//    1. Copies the returns span.
//    2. Overwrites every cell at date index > d with poison value 1e30.
//    3. Re-builds the model using build_stat_risk_model_from_returns with the
//       poisoned copy (same panel, same d, same window).
//    4. Asserts the two FactorModels are elementwise == on X, F, D.
//
//  If anything differs, the builder silently read dates > d — FAIL LOUD.
// ===========================================================================
TEST(Alpha101RiskModel, PitCausalGather) {
  // Build synthetic panel with returns.
  atx_impl_test::Panel synth =
      atx_impl_test::make_synth_orats_panel(/*dates=*/300, /*instruments=*/200);
  auto aug = atx_impl_test::augment_for_alpha101(synth, {});
  ASSERT_TRUE(aug.has_value()) << "augment_for_alpha101 failed: " << aug.error().message();
  const auto &panel = aug.value();

  const atx::usize D = panel.dates();
  const atx::usize I = panel.instruments();
  const atx::usize window = 60;
  // Pick d in the middle so there is plenty of history AND future to poison.
  const atx::usize d = D / 2;
  ASSERT_GE(d, window - 1) << "synthetic panel too short for chosen d/window";

  const StatModelCfg cfg{/*K=*/15, /*gls_reweight=*/true};

  // Resolve the returns field.
  auto returns_id = panel.field_id("returns");
  ASSERT_TRUE(returns_id.has_value()) << "panel missing 'returns' field";
  const std::span<const atx::f64> returns_clean = panel.field_all(*returns_id);

  // Build reference model from the clean returns.
  auto ref_result = build_stat_risk_model_from_returns(panel, d, window, cfg, returns_clean);
  ASSERT_TRUE(ref_result.has_value())
      << "reference build failed: " << ref_result.error().message();
  const auto &ref_model = ref_result.value().model;

  // Build the poisoned returns span: copy + overwrite cells at t > d.
  constexpr atx::f64 kPoison = 1e30;
  std::vector<atx::f64> poisoned(returns_clean.begin(), returns_clean.end());
  for (atx::usize t = d + 1; t < D; ++t) {
    for (atx::usize i = 0; i < I; ++i) {
      poisoned[t * I + i] = kPoison;
    }
  }
  const std::span<const atx::f64> returns_poisoned(poisoned);

  // Build the poisoned model — MUST be identical to the reference.
  auto pit_result = build_stat_risk_model_from_returns(panel, d, window, cfg, returns_poisoned);
  ASSERT_TRUE(pit_result.has_value())
      << "PIT build (poisoned) failed: " << pit_result.error().message();
  const auto &pit_model = pit_result.value().model;

  // Compare active_inst lists first (same set of instruments should be selected).
  const auto &ref_active = ref_result.value().active_inst;
  const auto &pit_active = pit_result.value().active_inst;
  ASSERT_EQ(ref_active.size(), pit_active.size())
      << "PIT: active_inst sizes differ — future dates affected universe selection";
  for (atx::usize k = 0; k < ref_active.size(); ++k) {
    EXPECT_EQ(ref_active[k], pit_active[k])
        << "PIT: active_inst[" << k << "] differs";
  }

  // Compare shapes.
  ASSERT_EQ(ref_model.n_factors(),     pit_model.n_factors());
  ASSERT_EQ(ref_model.n_instruments(), pit_model.n_instruments());

  // Compare exposures() elementwise.
  const MatX &Xref = ref_model.exposures();
  const MatX &Xpit = pit_model.exposures();
  ASSERT_EQ(Xref.rows(), Xpit.rows());
  ASSERT_EQ(Xref.cols(), Xpit.cols());
  for (Eigen::Index r = 0; r < Xref.rows(); ++r) {
    for (Eigen::Index c = 0; c < Xref.cols(); ++c) {
      EXPECT_EQ(Xref(r, c), Xpit(r, c))
          << "PIT FAIL: exposures() differ at (" << r << "," << c
          << ") — builder read dates > d!";
      if (Xref(r, c) != Xpit(r, c)) {
        // Print a single detailed message then give up to avoid flooding.
        FAIL() << "PIT failure detected in exposures(); aborting cell-by-cell check.";
      }
    }
  }

  // Compare factor_cov() elementwise.
  const MatX &Fref = ref_model.factor_cov();
  const MatX &Fpit = pit_model.factor_cov();
  ASSERT_EQ(Fref.rows(), Fpit.rows());
  ASSERT_EQ(Fref.cols(), Fpit.cols());
  for (Eigen::Index r = 0; r < Fref.rows(); ++r) {
    for (Eigen::Index c = 0; c < Fref.cols(); ++c) {
      EXPECT_EQ(Fref(r, c), Fpit(r, c))
          << "PIT FAIL: factor_cov() differ at (" << r << "," << c
          << ") — builder read dates > d!";
      if (Fref(r, c) != Fpit(r, c)) {
        FAIL() << "PIT failure detected in factor_cov(); aborting.";
      }
    }
  }

  // Compare specific_var() elementwise.
  const VecX &Dref = ref_model.specific_var();
  const VecX &Dpit = pit_model.specific_var();
  ASSERT_EQ(Dref.size(), Dpit.size());
  for (Eigen::Index i = 0; i < Dref.size(); ++i) {
    EXPECT_EQ(Dref[i], Dpit[i])
        << "PIT FAIL: specific_var() differ at [" << i
        << "] — builder read dates > d!";
    if (Dref[i] != Dpit[i]) {
      FAIL() << "PIT failure detected in specific_var(); aborting.";
    }
  }

  // Also verify fit window metadata.
  const atx::usize t0 = d - window + 1;
  EXPECT_EQ(ref_model.fit_begin(), t0);
  EXPECT_EQ(ref_model.fit_end(),   d + 1);
  EXPECT_EQ(pit_model.fit_begin(), t0);
  EXPECT_EQ(pit_model.fit_end(),   d + 1);
}

// ===========================================================================
//  TEST 5: No-GLS variant also produces a valid PSD model.
// ===========================================================================
TEST(Alpha101RiskModel, Pass1OnlyIsValid) {
  constexpr atx::usize N = 200;
  constexpr atx::usize T = 60;
  constexpr atx::usize K = 10;
  const MatX R = make_deterministic_R(N, T);
  const StatModelCfg cfg{K, /*gls_reweight=*/false};

  auto result = fit_stat_factor_model(R, cfg, 10, 10 + T);
  ASSERT_TRUE(result.has_value())
      << "Pass-1-only fit failed: " << result.error().message();

  const auto &model = result.value();
  EXPECT_EQ(model.n_factors(),     K);
  EXPECT_EQ(model.n_instruments(), N);
  EXPECT_EQ(model.fit_begin(), 10U);
  EXPECT_EQ(model.fit_end(),   10U + T);

  // PSD check.
  std::vector<atx::f64> w(N, 0.0);
  w[0] = 1.0;
  EXPECT_GE(model.risk(w), -1e-9);
  std::fill(w.begin(), w.end(), 1.0 / std::sqrt(static_cast<double>(N)));
  EXPECT_GE(model.risk(w), -1e-9);
}

} // namespace atxtest_alpha101_riskmodel
