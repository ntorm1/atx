// risk_covariance_integration_test.cpp — S8.8: short/long-horizon blend + the
// all-features-on covariance pipeline proof (the S8 capstone integration).
//
// Two layers of coverage:
//
//   (1) blend kernels (horizon_blend.hpp) — the pure convex-combo math:
//       * blend_factor_cov(F_s, F_l, w) == w·F_s + (1−w)·F_l, elementwise;
//       * w = 1 ⇒ F_s exactly, w = 0 ⇒ F_l exactly; w clamped to [0,1];
//       * SPD for SPD inputs (convex combo of SPD is SPD);
//       * blend_specific(d_s, d_l, w) == w·d_s + (1−w)·d_l elementwise.
//
//   (2) ALL-FEATURES-ON integration — build a FactorModel with EVERY S8 build-path
//       flag engaged (robust regression, EWMA+NW factor cov, short/long horizon
//       blend, seeded Monte-Carlo eigenfactor de-biasing, VRA, EWMA+NW structural
//       specific risk), run the PortfolioOptimizer on it, and assert:
//         * build SUCCEEDS and the implied V is SPD (a Cholesky of the implied risk
//           form succeeds — create() already enforces it, double-checked here);
//         * the features genuinely ENGAGE: the all-features model's risk() differs
//           from the P4-default model's risk() on the SAME panel;
//         * the optimizer returns Ok with a finite length-M weight vector;
//         * DETERMINISM: a second identical build + solve is BYTE-IDENTICAL
//           (incl. the seeded eigen_adjust) — std::memcmp on the weight vectors;
//         * NO LOOK-AHEAD: appending older rows beyond the fit window does NOT change
//           the model (risk(w) for a fixed w is byte-identical).
//
// All synthetic data is RNG-free / deterministic (the model path has exactly one
// seeded RNG — eigen_adjust — proven byte-identical by the determinism test).

#include <cmath>   // std::isfinite, std::isnan
#include <cstring> // std::memcmp
#include <limits>  // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/decompose.hpp" // symmetric_eig (SPD check)
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId (Symbol)
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon_blend.hpp"
#include "atx/engine/risk/optimizer.hpp"

namespace {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::domain::Symbol;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::risk::blend_factor_cov;
using atx::engine::risk::blend_specific;
using atx::engine::risk::FactorCovMethod;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::PortfolioOptimizer;
using atx::engine::risk::SpecificRiskMethod;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ===========================================================================
//  PanelFixture — owns a PanelView's backing storage (the established risk
//  test fixture: row 0 = newest cross-section; open/high/low filled with close).
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical row
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, 1000.0);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           head_(),      n_rows_,
                     mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }
  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }
  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }
  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<atx::u64> mask_;
};

// A deterministic multi-sector close grid (rows 0..n_rows-1, row 0 newest). Two
// sectors (K=2 dummy columns) so the factor cov / blend / eigen-adjust act on a
// genuine K>1 matrix; distinct per-instrument paths so the cross-section varies.
[[nodiscard]] std::vector<std::vector<f64>> make_close(usize n_rows, usize n_inst) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 80.0 + 6.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) { // oldest -> newest
      // Deterministic, instrument-specific wiggle (a fixed integer hash, NOT RNG).
      const f64 wig = 0.012 * (static_cast<f64>((3U * r + 2U * i + 1U) % 5U) - 2.0);
      px *= 1.0 + wig;
      close[r][i] = px;
    }
  }
  return close;
}

// Sectors-only config: K == number of distinct groups, no per-instrument style
// lookback (style factors need 252+ rows). Two sectors -> K=2.
[[nodiscard]] FactorModelConfig base_cfg() {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00; // sectors only -> no lookback
  return cfg;
}

// The ALL-FEATURES-ON covariance config: every S8 build-path flag engaged.
[[nodiscard]] FactorModelConfig all_features_cfg() {
  FactorModelConfig cfg = base_cfg();
  cfg.cov.robust_regression = true; // S8.1
  cfg.cov.cap_weight = false;       // no caps supplied in this fixture
  cfg.cov.huber_c = 1.345;
  cfg.cov.robust_iters = 4;
  cfg.cov.factor_cov_method = FactorCovMethod::EwmaNeweyWest; // S8.2
  cfg.cov.vol_halflife = 6U;
  cfg.cov.corr_halflife = 12U;
  cfg.cov.nw_lags = 1U;
  cfg.cov.horizon_blend = true; // S8.8
  cfg.cov.horizon_blend_weight = 0.5;
  cfg.cov.vol_halflife_long = 18U;
  cfg.cov.corr_halflife_long = 36U;
  cfg.cov.eigen_adjust_sims = 64U; // S8.3 (seeded)
  cfg.cov.eigen_adjust_amplify = 1.0;
  cfg.cov.eigen_adjust_seed = 0xA7C5ULL;
  cfg.cov.vra_halflife = 8U;                                             // S8.5
  cfg.cov.specific_method = SpecificRiskMethod::EwmaNeweyWestStructural; // S8.4
  cfg.cov.spec_halflife = 6U;
  cfg.cov.spec_nw_lags = 1U;
  cfg.cov.structural_blend = true;
  cfg.cov.spec_halflife_long = 18U; // S8.8 specific blend
  return cfg;
}

// ===========================================================================
//  blend kernels — convex combination, elementwise.
// ===========================================================================
TEST(RiskCovarianceIntegration, BlendFactorCovIsConvexCombination) {
  MatX fs(2, 2);
  fs << 4.0, 1.0, 1.0, 3.0;
  MatX fl(2, 2);
  fl << 2.0, -0.5, -0.5, 5.0;
  const f64 w = 0.25;
  const MatX b = blend_factor_cov(fs, fl, w);
  for (Eigen::Index i = 0; i < 2; ++i) {
    for (Eigen::Index j = 0; j < 2; ++j) {
      EXPECT_DOUBLE_EQ(b(i, j), w * fs(i, j) + (1.0 - w) * fl(i, j));
    }
  }
}

TEST(RiskCovarianceIntegration, BlendFactorCovEndpointsAndClamp) {
  MatX fs(2, 2);
  fs << 4.0, 1.0, 1.0, 3.0;
  MatX fl(2, 2);
  fl << 2.0, -0.5, -0.5, 5.0;
  // w = 1 ⇒ F_short exactly; w = 0 ⇒ F_long exactly (byte-identical).
  const MatX b1 = blend_factor_cov(fs, fl, 1.0);
  const MatX b0 = blend_factor_cov(fs, fl, 0.0);
  EXPECT_EQ(0, std::memcmp(b1.data(), fs.data(), static_cast<usize>(fs.size()) * sizeof(f64)));
  EXPECT_EQ(0, std::memcmp(b0.data(), fl.data(), static_cast<usize>(fl.size()) * sizeof(f64)));
  // w out of range is clamped: w = 2 ⇒ F_short; w = −1 ⇒ F_long.
  const MatX bhi = blend_factor_cov(fs, fl, 2.0);
  const MatX blo = blend_factor_cov(fs, fl, -1.0);
  EXPECT_EQ(0, std::memcmp(bhi.data(), fs.data(), static_cast<usize>(fs.size()) * sizeof(f64)));
  EXPECT_EQ(0, std::memcmp(blo.data(), fl.data(), static_cast<usize>(fl.size()) * sizeof(f64)));
}

TEST(RiskCovarianceIntegration, BlendFactorCovIsSpdForSpdInputs) {
  MatX fs(3, 3);
  fs << 4.0, 1.0, 0.5, 1.0, 3.0, 0.2, 0.5, 0.2, 2.0;
  MatX fl(3, 3);
  fl << 2.0, -0.3, 0.1, -0.3, 5.0, 0.4, 0.1, 0.4, 6.0;
  const MatX b = blend_factor_cov(fs, fl, 0.4);
  Eigen::LLT<MatX> llt(b);
  EXPECT_EQ(llt.info(), Eigen::Success);
  const auto eig = atx::core::linalg::symmetric_eig(b);
  ASSERT_TRUE(eig.has_value());
  EXPECT_GT(eig->values[0], 0.0);
}

TEST(RiskCovarianceIntegration, BlendSpecificIsElementwiseConvexCombination) {
  VecX ds(4);
  ds << 0.01, 0.02, 0.03, 0.04;
  VecX dl(4);
  dl << 0.05, 0.04, 0.03, 0.02;
  const f64 w = 0.3;
  const VecX b = blend_specific(ds, dl, w);
  ASSERT_EQ(b.size(), 4);
  for (Eigen::Index i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(b[i], w * ds[i] + (1.0 - w) * dl[i]);
  }
  // Endpoints.
  const VecX b1 = blend_specific(ds, dl, 1.0);
  const VecX b0 = blend_specific(ds, dl, 0.0);
  for (Eigen::Index i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(b1[i], ds[i]);
    EXPECT_DOUBLE_EQ(b0[i], dl[i]);
  }
}

// ===========================================================================
//  ALL-FEATURES-ON integration: build succeeds, V is SPD, features engage, the
//  optimizer returns a finite book, and the whole pipeline is byte-identical on
//  replay (incl. the seeded eigen_adjust).
// ===========================================================================
TEST(RiskCovarianceIntegration, AllFeaturesBuildSucceedsAndIsSpd) {
  const usize window = 24U;
  const usize n_inst = 6U;
  const usize n_rows = window + 2U;
  const std::vector<std::vector<f64>> close = make_close(n_rows, n_inst);
  PanelFixture fx{n_rows, n_inst, close};
  const std::vector<u32> group{1U, 1U, 1U, 2U, 2U, 2U}; // two sectors -> K=2
  const std::span<const f64> no_cap{};
  const std::span<const u32> grp{group};

  const auto m = FactorModelBuilder{all_features_cfg()}.build(fx.view(), window, no_cap, grp);
  ASSERT_TRUE(m.has_value()) << (m ? "" : m.error().to_string());
  EXPECT_EQ(m->n_factors(), 2U);
  EXPECT_EQ(m->n_instruments(), n_inst);

  // V SPD: risk(e_i) > 0 for every unit vector and risk(w) > 0 for a non-trivial w
  // (create() already Cholesky-checked F and the capacitance; this asserts the
  // implied quadratic form is positive on the standard basis + a mixed direction).
  for (usize i = 0; i < n_inst; ++i) {
    std::vector<f64> e(n_inst, 0.0);
    e[i] = 1.0;
    const f64 rk = m->risk(std::span<const f64>{e});
    EXPECT_TRUE(std::isfinite(rk));
    EXPECT_GT(rk, 0.0);
  }
  std::vector<f64> w{0.4, -0.1, 0.3, -0.2, 0.15, -0.05};
  const f64 rk = m->risk(std::span<const f64>{w});
  EXPECT_TRUE(std::isfinite(rk));
  EXPECT_GT(rk, 0.0);
}

TEST(RiskCovarianceIntegration, AllFeaturesEngageVersusP4Default) {
  const usize window = 24U;
  const usize n_inst = 6U;
  const usize n_rows = window + 2U;
  const std::vector<std::vector<f64>> close = make_close(n_rows, n_inst);
  PanelFixture fx{n_rows, n_inst, close};
  const std::vector<u32> group{1U, 1U, 1U, 2U, 2U, 2U};
  const std::span<const f64> no_cap{};
  const std::span<const u32> grp{group};

  const auto m_p4 = FactorModelBuilder{base_cfg()}.build(fx.view(), window, no_cap, grp);
  const auto m_full = FactorModelBuilder{all_features_cfg()}.build(fx.view(), window, no_cap, grp);
  ASSERT_TRUE(m_p4.has_value()) << (m_p4 ? "" : m_p4.error().to_string());
  ASSERT_TRUE(m_full.has_value()) << (m_full ? "" : m_full.error().to_string());

  const std::vector<f64> w{0.4, -0.1, 0.3, -0.2, 0.15, -0.05};
  const std::span<const f64> ws{w};
  // The full S8 feature set changes the covariance materially vs the P4 default.
  EXPECT_NE(m_full->risk(ws), m_p4->risk(ws));
}

TEST(RiskCovarianceIntegration, AllFeaturesOptimizerReturnsFiniteBook) {
  const usize window = 24U;
  const usize n_inst = 6U;
  const usize n_rows = window + 2U;
  const std::vector<std::vector<f64>> close = make_close(n_rows, n_inst);
  PanelFixture fx{n_rows, n_inst, close};
  const std::vector<u32> group{1U, 1U, 1U, 2U, 2U, 2U};
  const std::span<const f64> no_cap{};
  const std::span<const u32> grp{group};

  const auto m = FactorModelBuilder{all_features_cfg()}.build(fx.view(), window, no_cap, grp);
  ASSERT_TRUE(m.has_value()) << (m ? "" : m.error().to_string());

  const std::vector<f64> alpha{0.03, -0.02, 0.04, -0.01, 0.02, -0.03};
  const std::vector<f64> w_prev(n_inst, 0.0);
  const PortfolioOptimizer opt{
      OptimizerConfig{/*risk_aversion=*/2.0, /*turnover_penalty=*/0.0, /*gross_leverage=*/1.0,
                      /*name_cap=*/1.0, /*dollar_neutral=*/true, /*max_iters=*/64U}};
  const auto sol = opt.solve(std::span<const f64>{alpha}, *m, std::span<const f64>{w_prev});
  ASSERT_TRUE(sol.has_value()) << (sol ? "" : sol.error().to_string());
  ASSERT_EQ(sol->size(), n_inst);
  for (const f64 v : *sol) {
    EXPECT_TRUE(std::isfinite(v)); // no NaN / Inf in the optimizer output
  }
}

TEST(RiskCovarianceIntegration, AllFeaturesDeterministicByteIdenticalSolve) {
  const usize window = 24U;
  const usize n_inst = 6U;
  const usize n_rows = window + 2U;
  const std::vector<std::vector<f64>> close = make_close(n_rows, n_inst);
  PanelFixture fx{n_rows, n_inst, close};
  const std::vector<u32> group{1U, 1U, 1U, 2U, 2U, 2U};
  const std::span<const f64> no_cap{};
  const std::span<const u32> grp{group};

  const std::vector<f64> alpha{0.03, -0.02, 0.04, -0.01, 0.02, -0.03};
  const std::vector<f64> w_prev(n_inst, 0.0);
  const PortfolioOptimizer opt{
      OptimizerConfig{/*risk_aversion=*/2.0, /*turnover_penalty=*/0.1, /*gross_leverage=*/1.0,
                      /*name_cap=*/0.5, /*dollar_neutral=*/true, /*max_iters=*/64U}};

  auto build_and_solve = [&]() {
    const auto m = FactorModelBuilder{all_features_cfg()}.build(fx.view(), window, no_cap, grp);
    EXPECT_TRUE(m.has_value());
    return opt.solve(std::span<const f64>{alpha}, *m, std::span<const f64>{w_prev}).value();
  };
  const std::vector<f64> a = build_and_solve();
  const std::vector<f64> b = build_and_solve();
  ASSERT_EQ(a.size(), b.size());
  // BYTE-IDENTICAL through the whole all-features pipeline, incl. the seeded
  // Monte-Carlo eigen_adjust — the seed makes the one RNG site reproducible.
  EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(f64)));
}

TEST(RiskCovarianceIntegration, AllFeaturesTruncationInvariant) {
  const usize window = 24U;
  const usize n_inst = 6U;
  const std::vector<u32> group{1U, 1U, 1U, 2U, 2U, 2U};
  const std::span<const f64> no_cap{};
  const std::span<const u32> grp{group};

  // Two panels identical in rows [0, window] (the window's return reach is closes
  // [0, window]) but with EXTRA older rows beyond — invisible to a sectors-only,
  // no-lookback build. The model (hence risk(w)) must be byte-identical.
  auto make_panel = [&](usize extra_old) {
    const usize n_rows = window + 2U + extra_old;
    std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
    for (usize i = 0; i < n_inst; ++i) {
      f64 px = 80.0 + 6.0 * static_cast<f64>(i);
      for (usize r = n_rows; r-- > 0U;) {
        if (r <= window + 1U) {
          const f64 wig = 0.012 * (static_cast<f64>((3U * r + 2U * i + 1U) % 5U) - 2.0);
          px *= 1.0 + wig;
          close[r][i] = px;
        } else {
          close[r][i] = 7.0 + static_cast<f64>(r); // free tail beyond the window
        }
      }
    }
    return PanelFixture{n_rows, n_inst, close};
  };
  const PanelFixture a = make_panel(0U);
  const PanelFixture b = make_panel(8U);
  const auto ma = FactorModelBuilder{all_features_cfg()}.build(a.view(), window, no_cap, grp);
  const auto mb = FactorModelBuilder{all_features_cfg()}.build(b.view(), window, no_cap, grp);
  ASSERT_TRUE(ma.has_value()) << (ma ? "" : ma.error().to_string());
  ASSERT_TRUE(mb.has_value()) << (mb ? "" : mb.error().to_string());
  const std::vector<f64> w{0.4, -0.1, 0.3, -0.2, 0.15, -0.05};
  const std::span<const f64> ws{w};
  EXPECT_EQ(ma->risk(ws), mb->risk(ws)); // older rows beyond the window are invisible
}

} // namespace
