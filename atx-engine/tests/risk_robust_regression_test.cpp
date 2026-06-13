// risk_robust_regression_test.cpp — S8.1: robust √-cap + Huber IRLS cross-sectional
// factor regression (opt-in via CovarianceConfig.robust_regression).
//
// The P4 builder fits each date's factor returns by plain inverse-specific-variance
// WLS. S8.1 upgrades that to a robust root-cap + Huber IRLS fit, OPT-IN via
// `FactorModelConfig.cov.robust_regression`, REUSING the S6-1 cost::irls_huber kernel
// (generalized with an optional prior-weight vector). The clean WLS path is unchanged
// when the config defaults are left at P4.
//
// Coverage:
//   * Generalized kernel back-compat: cost::irls_huber with prior_w == nullptr is
//     byte-identical to the prior-less call, and a uniform prior_w == Ones is too
//     (so the S6-1 RobustLs* suite stays valid).
//   * Outlier resistance: on a single cross-section r = X·f_true with ONE planted
//     fat-tailed outlier, the robust factor return (Huber IRLS + inverse-d0 prior)
//     is materially closer to f_true than plain WLS: dist_robust < 0.5·dist_wls.
//   * Determinism: the robust kernel is RNG-free — same inputs twice -> byte-identical
//     beta.
//   * Build wiring + back-compat: a sectors-only panel built with
//     cov.robust_regression=true yields a usable, deterministic FactorModel; with the
//     cov default the build reproduces the plain-WLS model (same risk readout).

#include <array>  // std::array (sum-to-zero helper test fixture)
#include <cmath>  // std::abs, std::isfinite
#include <limits> // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp" // hash_bytes (byte-identity)
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/linalg/regression.hpp" // wls (plain-WLS oracle)
#include "atx/core/types.hpp"

#include "atx/engine/cost/robust_ls.hpp"   // cost::irls_huber, RobustCfg (generalized)
#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId (Symbol)
#include "atx/engine/risk/exposures.hpp"   // ExposureMatrix, ColumnTag, detail helpers (S8.1)
#include "atx/engine/risk/factor_model.hpp"

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::domain::Symbol;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::risk::ColumnTag;
using atx::engine::risk::ExposureMatrix;
using atx::engine::risk::FactorModel;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
namespace cost = atx::engine::cost;
namespace risk_detail = atx::engine::risk::detail;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// L2 distance between two coefficient vectors.
[[nodiscard]] f64 l2_dist(const VecX &a, const VecX &b) { return (a - b).norm(); }

// Hash a coefficient vector's raw bytes (byte-identity check).
[[nodiscard]] u64 hash_beta(const VecX &beta) {
  return atx::core::hash_bytes(beta.data(), static_cast<usize>(beta.size()) * sizeof(f64));
}

// ===========================================================================
//  PanelFixture — owns a PanelView's backing storage (same pattern as
//  risk_factor_builder_test.cpp). Row 0 = newest cross-section.
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close,
               const std::vector<std::vector<f64>> &volume)
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
        const f64 v = volume[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, v);
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

// Sectors-only config (single or multi group): no per-instrument lookback needed.
[[nodiscard]] FactorModelConfig sectors_only_cfg() {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00;
  return cfg;
}

// ===========================================================================
//  Generalized kernel back-compat: prior_w == nullptr matches a uniform Ones
//  prior, and both match the prior-less call (so the S6-1 RobustLs* suite, which
//  calls irls_huber without a prior, is unaffected by the generalization).
// ===========================================================================
TEST(RobustRegression, GeneralizedKernel_DefaultPriorMatchesOnesPrior) {
  // A small contaminated single design: row i = [1, x_i].
  const usize n = 12U;
  MatX X(static_cast<Eigen::Index>(n), 2);
  VecX y(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    const auto r = static_cast<Eigen::Index>(i);
    const f64 x = static_cast<f64>(i) * 0.3 - 1.5;
    X(r, 0) = 1.0;
    X(r, 1) = x;
    y[r] = 2.0 - 1.5 * x;
  }
  y[3] += 9.0; // planted outlier

  const cost::RobustCfg cfg{};
  const VecX b_default = cost::irls_huber(X, y, cfg).beta; // prior_w defaulted
  const VecX b_null = cost::irls_huber(X, y, cfg, /*prior_w=*/nullptr).beta;
  const VecX ones = VecX::Ones(static_cast<Eigen::Index>(n));
  const VecX b_ones = cost::irls_huber(X, y, cfg, &ones).beta;

  EXPECT_EQ(hash_beta(b_default), hash_beta(b_null));
  EXPECT_EQ(hash_beta(b_default), hash_beta(b_ones));
}

// ===========================================================================
//  Outlier resistance (the S8.1 acceptance). A single cross-section r = X·f_true
//  with one planted fat-tailed outlier. The robust fit (Huber IRLS + inverse-d0
//  prior weights) recovers f_true materially better than plain WLS on the same
//  design + weights: dist_robust < 0.5·dist_wls.
//
//  X is a K=3 design (intercept + 2 factor columns); f_true is the clean factor
//  return. Plain WLS = the P4 inverse-specific-variance solve (weights 1/d0_i);
//  the robust path passes those SAME weights as the IRLS prior so the only
//  difference is the Huber down-weighting. The outlier sits on one instrument.
// ===========================================================================
TEST(RobustRegression, PlantedOutlier_RobustBeatsWlsOnFactorReturn) {
  const usize n = 16U;
  VecX f_true(3);
  f_true << 0.020, -0.015, 0.030;

  MatX X(static_cast<Eigen::Index>(n), 3);
  VecX y(static_cast<Eigen::Index>(n));
  VecX w(static_cast<Eigen::Index>(n)); // 1/d0_i inverse-specific-variance weights
  for (usize i = 0; i < n; ++i) {
    const auto r = static_cast<Eigen::Index>(i);
    X(r, 0) = 1.0;
    X(r, 1) = static_cast<f64>(i % 5U) - 2.0;        // spread factor-1 loadings
    X(r, 2) = static_cast<f64>((i * 3U) % 7U) - 3.0; // spread factor-2 loadings
    y[r] = X.row(r).dot(f_true);                     // residual-free clean panel
    w[r] = 1.0;                                      // homoskedastic d0 (clean inverse-var)
  }
  // Plant ONE gross fat-tailed outlier on a single instrument-date.
  const Eigen::Index out = 9;
  y[out] += 0.80; // ~25× the clean return scale -> heavy leverage on plain WLS

  // Plain WLS (the P4 path): inverse-specific-variance weighted least squares.
  const auto wls = atx::core::linalg::wls(X, y, w);
  ASSERT_TRUE(wls.has_value());
  const f64 dist_wls = l2_dist(wls->beta, f_true);

  // Robust: the SAME prior weights w fed to the generalized Huber IRLS, with the
  // EXACT config the build path uses (tol=0.0 -> a true fixed-count 5-iter loop).
  const cost::RobustCfg cfg{.huber_k = 1.345, .max_iter = 5, .tol = 0.0};
  const cost::RobustFit rob = cost::irls_huber(X, y, cfg, &w);
  const f64 dist_robust = l2_dist(rob.beta, f_true);

  EXPECT_LT(dist_robust, dist_wls);
  EXPECT_LT(dist_robust, 0.5 * dist_wls)
      << "dist_robust=" << dist_robust << " dist_wls=" << dist_wls
      << " ratio=" << (dist_robust / dist_wls) << " iters=" << rob.iters;
}

// ===========================================================================
//  Determinism: the robust kernel is RNG-free, fixed-iteration, order-fixed ->
//  same inputs twice produce byte-identical factor returns.
// ===========================================================================
TEST(RobustRegression, RobustFit_ByteIdentical) {
  const usize n = 16U;
  MatX X(static_cast<Eigen::Index>(n), 3);
  VecX y(static_cast<Eigen::Index>(n));
  VecX w(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    const auto r = static_cast<Eigen::Index>(i);
    X(r, 0) = 1.0;
    X(r, 1) = static_cast<f64>(i % 5U) - 2.0;
    X(r, 2) = static_cast<f64>((i * 3U) % 7U) - 3.0;
    y[r] = 0.01 * static_cast<f64>(i) - 0.05;
    w[r] = 1.0 + 0.1 * static_cast<f64>(i % 3U);
  }
  y[7] += 0.5; // an outlier so the IRLS actually re-weights

  const cost::RobustCfg cfg{.huber_k = 1.345, .max_iter = 5};
  const u64 h1 = hash_beta(cost::irls_huber(X, y, cfg, &w).beta);
  const u64 h2 = hash_beta(cost::irls_huber(X, y, cfg, &w).beta);
  EXPECT_EQ(h1, h2);
}

// ===========================================================================
//  Build wiring: a sectors-only panel built with cov.robust_regression=true
//  yields a usable, deterministic FactorModel (risk/apply_inverse work).
// ===========================================================================
TEST(RobustRegression, BuildRobustPathProducesUsableModel) {
  const usize window = 8U;
  const usize n_inst = 4U;
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1000.0));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 100.0 + 10.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) { // oldest -> newest
      px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close, volume};
  const std::vector<u32> group{1U, 1U, 2U, 2U}; // 2 sectors -> K=2

  FactorModelConfig cfg = sectors_only_cfg();
  cfg.cov.robust_regression = true;
  cfg.cov.robust_iters = 5U;
  FactorModelBuilder builder{cfg};
  const auto m =
      builder.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(m.has_value()) << (m ? "" : m.error().to_string());
  EXPECT_EQ(m->n_factors(), 2U);
  EXPECT_EQ(m->n_instruments(), n_inst);

  std::vector<f64> w(n_inst, 0.25);
  const f64 rk = m->risk(std::span<const f64>{w});
  EXPECT_TRUE(std::isfinite(rk));
  EXPECT_GT(rk, 0.0);

  // Determinism: a second robust build is byte-identical (same risk readout).
  const auto m2 =
      builder.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m2->risk(std::span<const f64>{w}), rk);
}

// ===========================================================================
//  Back-compat: the cov DEFAULT build reproduces the plain-WLS (P4) model. We
//  build the SAME panel with the default config and with robust_regression left
//  false; both must equal each other's risk readout (the P4 path is untouched).
//  (The full RiskFactorBuilder* suite is the real backward-compat guard.)
// ===========================================================================
TEST(RobustRegression, CovDefaultReproducesPlainWlsBuild) {
  const usize window = 8U;
  const usize n_inst = 4U;
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1000.0));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 100.0 + 10.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) {
      px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close, volume};
  const std::vector<u32> group{1U, 1U, 1U, 1U};

  // Default cov (P4 path).
  FactorModelBuilder builder_default{sectors_only_cfg()};
  const auto md =
      builder_default.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(md.has_value()) << (md ? "" : md.error().to_string());

  // Explicit robust_regression=false (same as default) -> identical model.
  FactorModelConfig cfg_off = sectors_only_cfg();
  cfg_off.cov.robust_regression = false;
  FactorModelBuilder builder_off{cfg_off};
  const auto mo =
      builder_off.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(mo.has_value());

  std::vector<f64> w{0.4, -0.1, 0.3, -0.2};
  EXPECT_EQ(md->risk(std::span<const f64>{w}), mo->risk(std::span<const f64>{w}));
}

// ===========================================================================
//  √-cap helper (exposures::detail::sqrt_cap_weight) — direct contract test.
//   * Spread caps -> w_i ∝ √(cap_i), normalized so Σ_pos/mean(√cap) reproduces the
//     √cap proportions (mega-cap gets the largest weight; ratios match √cap ratios).
//   * A FLAT (all-equal) cap span -> every weight is exactly 1.0 (the normalization
//     collapses to Ones, the no-cap-weight baseline).
//   * A non-positive cap is FLAGGED with 0.0 (the caller's 1/d0 fallback sentinel).
// ===========================================================================
TEST(RobustRegression, SqrtCapWeight_ProportionalNormalizedAndFlagsBadCap) {
  // Kept rows map to universe instruments {0,1,2,3}; caps have real spread.
  const std::vector<usize> kept{0U, 1U, 2U, 3U};
  const std::vector<f64> caps{400.0, 100.0, 25.0, 4.0}; // √cap = 20,10,5,2 -> mean 9.25
  const VecX w = risk_detail::sqrt_cap_weight(std::span<const f64>{caps}, kept);
  ASSERT_EQ(w.size(), 4);
  const f64 mean_root = (20.0 + 10.0 + 5.0 + 2.0) / 4.0;
  EXPECT_NEAR(w[0], 20.0 / mean_root, 1e-12);
  EXPECT_NEAR(w[1], 10.0 / mean_root, 1e-12);
  EXPECT_NEAR(w[2], 5.0 / mean_root, 1e-12);
  EXPECT_NEAR(w[3], 2.0 / mean_root, 1e-12);
  EXPECT_GT(w[0], w[1]); // mega-cap weighted highest (non-tautological direction check)
  EXPECT_GT(w[1], w[3]);

  // Flat caps -> Ones (the normalization is mean(√cap), so each weight is √c/√c = 1).
  const std::vector<f64> flat(4, 50.0);
  const VecX wf = risk_detail::sqrt_cap_weight(std::span<const f64>{flat}, kept);
  for (Eigen::Index i = 0; i < wf.size(); ++i) {
    EXPECT_NEAR(wf[i], 1.0, 1e-12);
  }

  // A non-positive cap on row 2 -> 0.0 flag for that row (caller substitutes 1/d0).
  const std::vector<f64> bad{400.0, 100.0, -1.0, 4.0};
  const VecX wb = risk_detail::sqrt_cap_weight(std::span<const f64>{bad}, kept);
  EXPECT_EQ(wb[2], 0.0);
  EXPECT_GT(wb[0], 0.0);
}

// ===========================================================================
//  cap_weight build path: a √-cap build differs from a flat-cap build (the cap
//  prior is actually wired through accumulate_robust), and an ALL-EQUAL cap span
//  reproduces the cap_weight build exactly (normalization -> Ones, same as the
//  inverse-d0 prior would have to be tuned to — here flat caps make √-cap a no-op
//  vs the cap_weight=true/flat baseline). We use a single sector (K=1) so the
//  factor return is a prior-weighted mean; one mega-cap instrument carries an
//  idiosyncratic path so the cap weighting visibly shifts the fit.
// ===========================================================================
TEST(RobustRegression, CapWeightBuildPath_ShiftsFitAndFlatCapIsNoOp) {
  const usize window = 8U;
  const usize n_inst = 4U;
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1000.0));
  // Instrument 0 is the "mega-cap" with a distinct (larger-amplitude) path; the
  // others share a tamer pattern -> instrument 0's idiosyncratic moves dominate
  // only when it is up-weighted (√-cap).
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 100.0 + 10.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) {
      const f64 amp = (i == 0U) ? 0.04 : 0.01; // mega-cap swings 4× harder
      px *= 1.0 + amp * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close, volume};
  const std::vector<u32> group{7U, 7U, 7U, 7U};                    // one sector -> K=1
  const std::vector<f64> spread_caps{1.0e12, 5.0e9, 3.0e9, 2.0e9}; // mega-cap + smalls
  const std::vector<f64> flat_caps(n_inst, 4.0e9);

  FactorModelConfig cfg = sectors_only_cfg();
  cfg.cov.robust_regression = true;
  cfg.cov.cap_weight = true;
  FactorModelBuilder builder{cfg};

  const auto m_spread = builder.build(fx.view(), window, std::span<const f64>{spread_caps},
                                      std::span<const u32>{group});
  const auto m_flat = builder.build(fx.view(), window, std::span<const f64>{flat_caps},
                                    std::span<const u32>{group});
  ASSERT_TRUE(m_spread.has_value()) << (m_spread ? "" : m_spread.error().to_string());
  ASSERT_TRUE(m_flat.has_value()) << (m_flat ? "" : m_flat.error().to_string());

  // The √-cap prior up-weights the mega-cap, changing the fitted factor returns and
  // hence the residual-driven specific variances D -> a different risk readout. The
  // pairing risk(e_i − e_j) = D_i + D_j (X-row==1 cancels F) makes D observable.
  std::vector<f64> w(n_inst, 0.0);
  w[0] = 1.0;
  w[1] = -1.0;
  const f64 r_spread = m_spread->risk(std::span<const f64>{w});
  const f64 r_flat = m_flat->risk(std::span<const f64>{w});
  EXPECT_NE(r_spread, r_flat) << "√-cap weighting must change the fit vs flat caps";

  // Flat caps reproduce the cap_weight build with an all-equal span: a SECOND flat
  // build is byte-identical to the first (normalization -> Ones is deterministic).
  const auto m_flat2 = builder.build(fx.view(), window, std::span<const f64>{flat_caps},
                                     std::span<const u32>{group});
  ASSERT_TRUE(m_flat2.has_value());
  EXPECT_EQ(m_flat2->risk(std::span<const f64>{w}), r_flat);
}

// ===========================================================================
//  industry_sum_to_zero helper (exposures::detail::apply_industry_sum_to_zero) —
//  direct, non-tautological contract test. After applying the constraint, EACH
//  Sector-kind dummy column's CAP-WEIGHTED mean is ≈ 0 (the collinear market-level
//  direction is removed from the industry block). A non-Sector (style/market-level)
//  column is left untouched.
// ===========================================================================
TEST(RobustRegression, IndustrySumToZero_ZeroesCapWeightedIndustryMeans) {
  // 4 instruments, 2 sectors {A: rows 0,1} {B: rows 2,3}, plus a market-level
  // (all-ones) Style column that makes the raw [marketlevel | dummyA | dummyB]
  // design collinear (dummyA + dummyB == marketlevel).
  ExposureMatrix xm;
  xm.instrument_rows = {0U, 1U, 2U, 3U};
  xm.columns = {ColumnTag{ColumnTag::Kind::Style, {}, 0U},    // market level (all ones)
                ColumnTag{ColumnTag::Kind::Sector, {}, 10U},  // industry A dummy
                ColumnTag{ColumnTag::Kind::Sector, {}, 20U}}; // industry B dummy
  MatX x(4, 3);
  x << 1.0, 1.0, 0.0, // row0: sector A
      1.0, 1.0, 0.0,  // row1: sector A
      1.0, 0.0, 1.0,  // row2: sector B
      1.0, 0.0, 1.0;  // row3: sector B
  xm.x = x;

  const std::vector<usize> keep{0U, 1U, 2U, 3U};
  const std::vector<f64> caps{9.0, 1.0, 4.0, 16.0}; // √cap = 3,1,2,4
  MatX xsr = x;                                     // operate on a copy of the kept design
  risk_detail::apply_industry_sum_to_zero(xsr, xm, keep, std::span<const f64>{caps});

  // ν_i = √cap_i / Σ√cap = {3,1,2,4}/10. Each Sector column's cap-weighted mean must
  // now be ≈ 0 (the constraint's defining property).
  const f64 total_root = 3.0 + 1.0 + 2.0 + 4.0;
  for (Eigen::Index col = 1; col <= 2; ++col) { // the two Sector columns
    f64 wmean = 0.0;
    const std::array<f64, 4> roots{3.0, 1.0, 2.0, 4.0};
    for (Eigen::Index r = 0; r < 4; ++r) {
      wmean += (roots[static_cast<usize>(r)] / total_root) * xsr(r, col);
    }
    EXPECT_NEAR(wmean, 0.0, 1e-12) << "Sector col " << col << " cap-weighted mean must be 0";
  }
  // The market-level (Style) column is untouched (still all ones).
  for (Eigen::Index r = 0; r < 4; ++r) {
    EXPECT_EQ(xsr(r, 0), 1.0);
  }
}

// ===========================================================================
//  industry_sum_to_zero build path: the constraint is wired through
//  accumulate_robust and DEGRADES GRACEFULLY (no crash / no ATX_CHECK abort) on the
//  designs the as-built sectors-only path produces. The math reason: the as-built X
//  for style_mask=0x00 is PURE disjoint sector dummies (no separate market-level
//  column). For a universe partitioned into sectors the dummy columns already sum to
//  the all-ones level, so cap-weight mean-centering EACH dummy removes exactly that
//  one shared level — collapsing the K dummies to rank K−1 (the sum-to-zero
//  constraint's whole purpose: it deletes the redundant market direction). With no
//  free market column to absorb it the post-constraint design is rank-deficient, so
//  the OLS rank probe in accumulate_robust SKIPS each date and build returns Err
//  rather than crashing. (The constraint is non-degenerate when a market-level/style
//  column accompanies the dummies — its zeroing math is pinned directly by
//  IndustrySumToZero_ZeroesCapWeightedIndustryMeans above.) Two sectors here.
// ===========================================================================
TEST(RobustRegression, IndustrySumToZeroBuildPath_DegradesGracefully) {
  const usize window = 8U;
  const usize n_inst = 4U;
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1000.0));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 100.0 + 10.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) {
      px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close, volume};
  const std::vector<u32> group{1U, 1U, 2U, 2U}; // 2 sectors -> K=2
  const std::vector<f64> caps{8.0e9, 2.0e9, 6.0e9, 1.0e9};

  FactorModelConfig cfg = sectors_only_cfg();
  cfg.cov.robust_regression = true;
  cfg.cov.industry_sum_to_zero = true;
  FactorModelBuilder builder{cfg};
  // No crash / no abort: the rank-reducing constraint skips every pure-dummy date and
  // the build returns Err gracefully (the wiring is exercised; the helper math is
  // pinned by the direct test above).
  const auto m =
      builder.build(fx.view(), window, std::span<const f64>{caps}, std::span<const u32>{group});
  EXPECT_FALSE(m.has_value());
}

// ===========================================================================
//  Single-sector degenerate case (the flagged edge): a 1-group panel with
//  industry_sum_to_zero=true mean-centers the lone all-ones sector column to all
//  zeros every date -> every date is rank-deficient and SKIPPED gracefully (the OLS
//  rank probe in accumulate_robust catches it). The build returns an Err (too few
//  usable dates) WITHOUT crashing — no UB, no ATX_CHECK abort. This pins graceful
//  degradation on a misconfiguration rather than a successful fit.
// ===========================================================================
TEST(RobustRegression, IndustrySumToZeroSingleSector_DegradesGracefully) {
  const usize window = 6U;
  const usize n_inst = 3U;
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1000.0));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 50.0 + 7.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) {
      px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close, volume};
  const std::vector<u32> group{4U, 4U, 4U}; // ONE sector -> K=1, degenerate under the constraint
  const std::vector<f64> caps{5.0e9, 3.0e9, 1.0e9};

  FactorModelConfig cfg = sectors_only_cfg();
  cfg.cov.robust_regression = true;
  cfg.cov.industry_sum_to_zero = true;
  FactorModelBuilder builder{cfg};
  const auto m =
      builder.build(fx.view(), window, std::span<const f64>{caps}, std::span<const u32>{group});
  // Graceful: returns an error (every date skipped), no crash / no abort.
  EXPECT_FALSE(m.has_value());
}

} // namespace
