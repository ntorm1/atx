// risk_w0r0_thin_floor_test.cpp — W0-R0 (R-05): specific-variance floor for names
// with too little residual history.
//
// Pre-W0 a name with fewer than 2 residual observations got pop_variance == 0, i.e.
// D = 1e-12 after FactorModel::create's numerical floor — an essentially riskless
// asset, so a minimum-variance / mean-variance optimizer piles into it. The default
// SpecificFloorRule::StructuralMedianV2 shrinks thin names toward a structural
// ln-D-on-exposures prediction and floors every D at 0.1·median(D).
//
// Suite: RiskThinNameFloor.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/exposures.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "risk_w0r0_fixture.hpp"

namespace atx_test_w0_r0_thin_floor {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::ColumnTag;
using atx::engine::risk::CovarianceConfig;
using atx::engine::risk::ExposureMatrix;
using atx::engine::risk::FactorModel;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::PitSideInputs;
using atx::engine::risk::SpecificFloorRule;
using atx::engine::risk::StyleFactor;
using atx::engine::risk::detail::floor_specific_variances;
using atx::engine::risk::detail::SpecificFloorStats;
using atx_test_w0_r0_fixture::closes_from_returns;
using atx_test_w0_r0_fixture::Grid;
using atx_test_w0_r0_fixture::kNaN;
using atx_test_w0_r0_fixture::Rng;
using atx_test_w0_r0_fixture::StorePanel;

[[nodiscard]] f64 median(std::vector<f64> v) {
  std::sort(v.begin(), v.end());
  const usize n = v.size();
  return (n % 2U == 1U) ? v[n / 2U] : 0.5 * (v[n / 2U - 1U] + v[n / 2U]);
}

// 30 names in 3 sectors (10 each), window 60, r = f_g + e_i with per-name σ_e from 1%
// to 3%. Name 29 (sector 3) lists `thin_rows` rows before the current date.
struct ThinPanel {
  StorePanel panel;
  std::vector<u32> group;
};
[[nodiscard]] ThinPanel thin_panel(usize window, usize thin_rows, atx::u64 seed) {
  const usize n_inst = 30U;
  Rng rng{seed};
  std::vector<std::vector<f64>> f(3U, std::vector<f64>(window));
  for (auto &fg : f) {
    for (f64 &v : fg) {
      v = 0.01 * rng.normal();
    }
  }
  Grid ret(window, std::vector<f64>(n_inst));
  for (usize s = 0; s < window; ++s) {
    for (usize i = 0; i < n_inst; ++i) {
      const f64 sigma = 0.01 + 0.02 * static_cast<f64>(i % 10U) / 9.0;
      ret[s][i] = f[i / 10U][s] + sigma * rng.normal();
    }
  }
  Grid close = closes_from_returns(ret);
  for (usize r = thin_rows + 1U; r <= window; ++r) {
    close[r][29] = kNaN; // name 29 lists at row `thin_rows`
  }
  const Grid volume(window + 1U, std::vector<f64>(n_inst, 1.0e5));
  std::vector<u32> group(n_inst);
  for (usize i = 0; i < n_inst; ++i) {
    group[i] = static_cast<u32>(1U + i / 10U);
  }
  return ThinPanel{StorePanel{close, volume}, std::move(group)};
}

[[nodiscard]] FactorModelConfig sectors_cfg(SpecificFloorRule rule) {
  FactorModelConfig cfg;
  cfg.style_mask = 0U; // sectors only: a thin name still has a full exposure row
  cfg.cov.specific_floor = rule;
  return cfg;
}

// Minimum-variance weights w = V⁻¹1 / 1ᵀV⁻¹1 through the model's Woodbury apply.
[[nodiscard]] std::vector<f64> min_variance(const FactorModel &m) {
  const usize n = m.n_instruments();
  const std::vector<f64> ones(n, 1.0);
  std::vector<f64> w(n, 0.0);
  m.apply_inverse(std::span<const f64>{ones}, std::span<f64>{w});
  f64 sum = 0.0;
  for (const f64 v : w) {
    sum += v;
  }
  for (f64 &v : w) {
    v /= sum;
  }
  return w;
}

// ===========================================================================
//  Acceptance "No D < 0.1·median" + the optimizer consequence, measured.
//  Name 29 listed 1 row ago -> exactly 1 residual observation.
// ===========================================================================
TEST(RiskThinNameFloor, ThinNameNoLongerLooksRiskless) {
  const usize window = 60U;
  const ThinPanel tp = thin_panel(window, /*thin_rows=*/1U, 7U);
  const PitSideInputs side = PitSideInputs::broadcast({}, std::span<const u32>{tp.group});

  const FactorModelBuilder v2{sectors_cfg(SpecificFloorRule::StructuralMedianV2)};
  const FactorModelBuilder v1{sectors_cfg(SpecificFloorRule::NoneV1)};
  const auto c2 = v2.build_components(tp.panel.view(), window, side);
  const auto c1 = v1.build_components(tp.panel.view(), window, side);
  ASSERT_TRUE(c2.has_value()) << c2.error().to_string();
  ASSERT_TRUE(c1.has_value()) << c1.error().to_string();
  ASSERT_EQ(c2->D.size(), 30);
  const Eigen::Index thin = 29; // all names present at row 0 -> row index == instrument

  std::vector<f64> d2(c2->D.data(), c2->D.data() + c2->D.size());
  const f64 med2 = median(d2);
  usize below = 0U;
  for (const f64 d : d2) {
    below += (d < 0.1 * med2) ? 1U : 0U;
  }
  EXPECT_EQ(below, 0U); // acceptance: no D < 0.1·median(D)
  EXPECT_GE(c2->D[thin], 0.1 * med2);

  // Pre-W0 value: 1 residual -> pop variance 0 (create() then floors at 1e-12).
  EXPECT_LT(c1->D[thin], 1e-10);

  // The structural fallback lands inside the thick names' range (sector 3 spans
  // σ_e 1%..3% -> D 1e-4..9e-4).
  f64 lo = 1.0;
  f64 hi = 0.0;
  for (Eigen::Index r = 0; r < 29; ++r) {
    lo = std::min(lo, c2->D[r]);
    hi = std::max(hi, c2->D[r]);
  }
  EXPECT_GE(c2->D[thin], lo);
  EXPECT_LE(c2->D[thin], hi);

  // Optimizer consequence: the thin name's share of the minimum-variance portfolio.
  const auto m2 = v2.build(tp.panel.view(), window, side);
  const auto m1 = v1.build(tp.panel.view(), window, side);
  ASSERT_TRUE(m2.has_value() && m1.has_value());
  const std::vector<f64> w2 = min_variance(*m2);
  const std::vector<f64> w1 = min_variance(*m1);
  f64 max_other2 = 0.0;
  for (usize i = 0; i < 29U; ++i) {
    max_other2 = std::max(max_other2, std::fabs(w2[i]));
  }
  std::printf("[W0-R0 evidence] thin name (1 residual): D V1=%.3e V2=%.3e (median %.3e, thick "
              "range %.3e..%.3e); min-variance weight V1=%.4f V2=%.4f (max other name V2 %.4f)\n",
              c1->D[thin], c2->D[thin], med2, lo, hi, w1[29], w2[29], max_other2);
  // Pre-W0 the optimizer piles into the "riskless" thin name (only its sector-factor
  // risk limits the position): measured 0.378 of the book, 3.8x the largest V2 name.
  EXPECT_GT(w1[29], 0.3);
  EXPECT_GT(w1[29], 3.0 * max_other2);
  EXPECT_LT(w2[29], 0.1);              // V2: an ordinary position ...
  EXPECT_LE(w2[29], max_other2 * 1.5); // ... comparable to the other names
  EXPECT_GT(w1[29], 10.0 * w2[29]);    // the concentration the floor removes
}

// The V2 rule also repairs the pass-B WLS weight of a thin name: with V1 its 1e12
// weight makes it dictate sector 3's factor return on the date it appears.
TEST(RiskThinNameFloor, ThinNameDoesNotDictateItsSectorReturn) {
  const usize window = 60U;
  const ThinPanel tp = thin_panel(window, /*thin_rows=*/1U, 8U);
  const PitSideInputs side = PitSideInputs::broadcast({}, std::span<const u32>{tp.group});
  const auto f2 = FactorModelBuilder{sectors_cfg(SpecificFloorRule::StructuralMedianV2)}
                      .factor_returns(tp.panel.view(), window, side);
  const auto f1 = FactorModelBuilder{sectors_cfg(SpecificFloorRule::NoneV1)}.factor_returns(
      tp.panel.view(), window, side);
  ASSERT_TRUE(f2.has_value() && f1.has_value());
  ASSERT_EQ(f2->dates.front(), 0U);
  // r_0 of the thin name.
  const auto v = tp.panel.view();
  const f64 r_thin = v.close(0U, 29U) / v.close(1U, 29U) - 1.0;
  const f64 g3_v1 = f1->f(0, 2);
  const f64 g3_v2 = f2->f(0, 2);
  std::printf("[W0-R0 evidence] sector-3 return on the thin name's first date: thin r=%.5f, "
              "V1 f=%.5f, V2 f=%.5f\n",
              r_thin, g3_v1, g3_v2);
  EXPECT_NEAR(g3_v1, r_thin, 1e-6);           // V1: the 1e12 weight copies the thin name
  EXPECT_GT(std::fabs(g3_v2 - r_thin), 1e-4); // V2: an ordinary weighted mean
}

// ===========================================================================
//  floor_specific_variances unit contract: exact structural recovery, γ blending,
//  the global floor, and the degenerate cases.
// ===========================================================================
[[nodiscard]] ExposureMatrix two_sector_with_style(const std::vector<f64> &style,
                                                   const std::vector<u32> &grp) {
  ExposureMatrix x;
  const usize m = style.size();
  x.x.resize(static_cast<Eigen::Index>(m), 3);
  for (usize r = 0; r < m; ++r) {
    x.x(static_cast<Eigen::Index>(r), 0) = (grp[r] == 1U) ? 1.0 : 0.0;
    x.x(static_cast<Eigen::Index>(r), 1) = (grp[r] == 2U) ? 1.0 : 0.0;
    x.x(static_cast<Eigen::Index>(r), 2) = style[r];
    x.instrument_rows.push_back(r);
  }
  x.columns = {{ColumnTag::Kind::Sector, StyleFactor{}, 1U},
               {ColumnTag::Kind::Sector, StyleFactor{}, 2U},
               {ColumnTag::Kind::Style, StyleFactor::Volatility, 0U}};
  return x;
}

TEST(RiskThinNameFloor, StructuralFallbackRecoversPlantedModel) {
  // ln D = a_g + b·x exactly on 20 thick names; rows 20 (0 obs) and 21 (15 of 30 obs).
  const usize m = 22U;
  std::vector<f64> style(m);
  std::vector<u32> grp(m);
  for (usize r = 0; r < m; ++r) {
    style[r] = -1.5 + 3.0 * static_cast<f64>(r % 10U) / 9.0;
    grp[r] = (r % 2U == 0U) ? 1U : 2U;
  }
  style[20] = 0.25; // inside the thick range -> no clamp
  style[21] = -0.5;
  const ExposureMatrix x0 = two_sector_with_style(style, grp);
  auto planted = [&](usize r) {
    return std::exp(((grp[r] == 1U) ? -8.0 : -7.2) + 0.4 * style[r]);
  };
  VecX d(static_cast<Eigen::Index>(m));
  std::vector<usize> obs(m, 60U);
  for (usize r = 0; r < m; ++r) {
    d[static_cast<Eigen::Index>(r)] = planted(r);
  }
  d[20] = 0.0;   // 0 observations: pure structural
  obs[20] = 0U;
  d[21] = 4.0 * planted(21); // noisy own estimate from 10 obs (γ = 10/21)
  obs[21] = 10U;
  CovarianceConfig cov;
  VecX out = d;
  const SpecificFloorStats st = floor_specific_variances(x0, obs, cov, out);
  EXPECT_EQ(st.min_obs, 21U); // min(21, max(2, ⌈60/2⌉))
  EXPECT_EQ(st.n_thin, 2U);
  EXPECT_TRUE(st.structural);
  EXPECT_NEAR(out[20], planted(20), 1e-12 * planted(20) + 1e-15);
  const f64 gamma = 10.0 / 21.0;
  const f64 sig = gamma * std::sqrt(4.0 * planted(21)) + (1.0 - gamma) * std::sqrt(planted(21));
  EXPECT_NEAR(out[21], sig * sig, 1e-12);
  for (usize r = 0; r < 20U; ++r) { // thick names untouched (none below the floor)
    EXPECT_EQ(out[static_cast<Eigen::Index>(r)], d[static_cast<Eigen::Index>(r)]);
  }
  // NoneV1 is the caller's switch; the kernel itself is only reached under V2.
}

TEST(RiskThinNameFloor, GlobalFloorAndDegenerateCases) {
  const usize m = 9U;
  std::vector<f64> style(m, 0.0);
  std::vector<u32> grp(m, 1U);
  const ExposureMatrix x0 = two_sector_with_style(style, grp);
  VecX d(9);
  d << 4e-4, 5e-4, 3e-4, 6e-4, 4.5e-4, 5.5e-4, 1e-8, 3.5e-4, 4e-4; // row 6 stale-price-like
  const std::vector<usize> obs(m, 100U);
  CovarianceConfig cov;
  VecX out = d;
  const SpecificFloorStats st = floor_specific_variances(x0, obs, cov, out);
  EXPECT_EQ(st.n_thin, 0U);
  EXPECT_NEAR(st.median, 4e-4, 1e-18);
  EXPECT_NEAR(st.floor, 4e-5, 1e-18);
  EXPECT_EQ(st.n_floored, 1U);
  EXPECT_NEAR(out[6], 4e-5, 1e-18);
  for (Eigen::Index r = 0; r < 9; ++r) {
    EXPECT_GE(out[r], 0.1 * st.median);
  }
  // frac = 0 disables the floor; no thick names -> no fallback, floor only.
  cov.specific_floor_frac = 0.0;
  VecX out0 = d;
  (void)floor_specific_variances(x0, obs, cov, out0);
  EXPECT_EQ(out0[6], 1e-8);
  cov.specific_floor_frac = 0.1;
  const std::vector<usize> none(m, 1U); // everyone thin (min_obs = 2)
  VecX out1 = d;
  const SpecificFloorStats s1 = floor_specific_variances(x0, none, cov, out1);
  EXPECT_EQ(s1.n_thin, 9U);
  EXPECT_FALSE(s1.structural);
  for (Eigen::Index r = 0; r < 9; ++r) {
    EXPECT_GE(out1[r], 0.1 * s1.median);
  }
  // Empty cross-section is a no-op.
  ExposureMatrix empty;
  empty.x.resize(0, 0);
  VecX de(0);
  const SpecificFloorStats se = floor_specific_variances(empty, {}, cov, de);
  EXPECT_EQ(se.n_thin, 0U);
}

} // namespace atx_test_w0_r0_thin_floor
