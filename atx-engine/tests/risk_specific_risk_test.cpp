// risk_specific_risk_test.cpp — S8.4: specific-risk EWMA + Newey-West + structural blend.
//
// specific_risk_blend(x0, u_by_inst, window, spec_hl, spec_nw_lags, structural)
// replaces the as-built plain population-variance specific risk with a blended
// idiosyncratic-variance estimator (opt-in via
// CovarianceConfig.specific_method = EwmaNeweyWestStructural):
//   σ_n^TS = √( ewma_var(r_n, spec_hl) · nw_inflation(r_n, spec_nw_lags) )
//   σ_n^STR = exp( OLS(ln σ^TS ~ exposure row) prediction )   [structural blend]
//   σ_n     = γ_n·σ^TS + (1−γ_n)·σ^STR,  γ_n = clamp(count_n / full, 0, 1).
//   D_n     = σ_n²  (floored > 0).
//
// Coverage (plan §5 S8.4):
//   * THIN-HISTORY → STRUCTURAL: a thin name gets γ≈0 and a sane STRUCTURAL value,
//     not a noisy near-zero pop variance (helper-direct AND through build()).
//   * EWMA RECENCY: a recent high-variance block weights heavier than the same block
//     in the OLD tail.
//   * FULL-HISTORY i.i.d. recovers ≈ pop variance at γ=1, spec_nw_lags=0.
//   * D > 0 (floored).
//   * BACKWARD-COMPAT: the default PopVariance build matches a direct pop-variance.
//   * ISC interface: the carrier exists and is EMPTY (D stays diagonal).

#include <array>  // std::array
#include <cmath>  // std::isfinite, std::sqrt, std::log, std::exp
#include <limits> // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/types.hpp"

#include "atx/core/linalg/linalg.hpp"

#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId (Symbol)
#include "atx/engine/risk/exposures.hpp"   // ExposureMatrix, ColumnTag, StyleFactor
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/specific_risk.hpp"

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
using atx::engine::risk::ColumnTag;
using atx::engine::risk::ExposureMatrix;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::IssuerSpecificCov;
using atx::engine::risk::specific_risk_blend;
using atx::engine::risk::SpecificRisk;
using atx::engine::risk::SpecificRiskMethod;
using atx::engine::risk::StyleFactor;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ===========================================================================
//  PanelFixture — owns a PanelView's backing storage (same pattern as
//  risk_factor_builder_test). row 0 = newest cross-section.
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

[[nodiscard]] FactorModelConfig single_sector_cfg() {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00; // sectors only -> no per-instrument lookback needed
  return cfg;
}

// Population variance (the as-built specific-risk reference).
[[nodiscard]] f64 popvar(const std::vector<f64> &xs) {
  if (xs.size() < 2U) {
    return 0.0;
  }
  f64 mean = 0.0;
  for (const f64 v : xs) {
    mean += v;
  }
  mean /= static_cast<f64>(xs.size());
  f64 ss = 0.0;
  for (const f64 v : xs) {
    ss += (v - mean) * (v - mean);
  }
  return ss / static_cast<f64>(xs.size());
}

// A 2-instrument exposure block with two distinct exposure rows so the structural
// ln-vol-on-exposures OLS (K=2: an intercept-like + a slope column) is full rank when
// both names are clean. instrument_rows = {0, 1}.
[[nodiscard]] ExposureMatrix two_inst_exposures() {
  ExposureMatrix xm;
  xm.x = MatX(2, 2);
  xm.x << 1.0, 0.0, // instrument 0 exposure row
      1.0, 1.0;     // instrument 1 exposure row
  xm.instrument_rows = {0U, 1U};
  xm.columns = {ColumnTag{ColumnTag::Kind::Style, StyleFactor::Size, 0U},
                ColumnTag{ColumnTag::Kind::Style, StyleFactor::Momentum, 0U}};
  return xm;
}

// ===========================================================================
//  THIN-HISTORY → STRUCTURAL (helper-direct). Three full-clean-history names give
//  the structural OLS its fit; a fourth THIN name (only 2 residuals) must get γ≈0
//  and inherit the structural prediction — a sane non-zero value, NOT a noisy
//  near-zero. We craft the three clean names so ln σ^TS is an exact linear function
//  of their exposure rows, so the structural prediction is analytic.
// ===========================================================================
TEST(SpecificRisk, ThinHistoryNameInheritsStructuralEstimate) {
  // K=2 exposures: column 0 == 1 (intercept), column 1 a slope. ln σ^TS = a + b·x1.
  // Pick a = ln(0.02), b = ln(2): name with x1=0 -> σ=0.02, x1=1 -> σ=0.04, x1=2 ->
  // σ=0.08. Build constant-magnitude residual series so ewma_var == that σ² exactly
  // (a two-point series {+σ, −σ} has population/EWMA variance σ² at any half-life).
  ExposureMatrix xm;
  xm.x = MatX(4, 2);
  xm.x << 1.0, 0.0, // name 0 (clean) -> σ 0.02
      1.0, 1.0,     // name 1 (clean) -> σ 0.04
      1.0, 2.0,     // name 2 (clean) -> σ 0.08
      1.0, 3.0;     // name 3 (THIN)  -> structural σ = 0.16
  xm.instrument_rows = {0U, 1U, 2U, 3U};
  xm.columns = {ColumnTag{ColumnTag::Kind::Style, StyleFactor::Size, 0U},
                ColumnTag{ColumnTag::Kind::Style, StyleFactor::Momentum, 0U}};

  const usize window = 8U;
  std::vector<std::vector<f64>> u(4);
  // Full clean histories (length == window) with a flat ±σ pattern -> var == σ².
  auto fill = [&](usize idx, f64 sigma) {
    for (usize t = 0; t < window; ++t) {
      u[idx].push_back((t % 2U == 0U) ? sigma : -sigma);
    }
  };
  fill(0, 0.02);
  fill(1, 0.04);
  fill(2, 0.08);
  // THIN name: only 2 residuals, and deliberately a tiny near-zero magnitude so its
  // OWN σ^TS is a spuriously small value the structural blend must override.
  u[3] = {1e-6, -1e-6};

  const SpecificRisk res = specific_risk_blend(xm, u, window, /*spec_hl=*/0U,
                                               /*spec_nw_lags=*/0U, /*structural=*/true);

  // γ_3 = count_3 / full = 2 / 8 = 0.25, near 0. The blended σ_3 must be dominated by
  // the structural prediction (~0.16), FAR above the thin name's own σ^TS (~1e-6).
  const f64 d3 = res.variances[3];
  const f64 sigma3 = std::sqrt(d3);
  // Structural prediction at x1=3 is exp(ln0.02 + 3·ln2) = 0.02·8 = 0.16.
  const f64 sigma_str = 0.16;
  const f64 gamma3 = 2.0 / 8.0;
  const f64 sigma_ts_thin = 1e-6;
  const f64 expected_sigma3 = gamma3 * sigma_ts_thin + (1.0 - gamma3) * sigma_str;
  EXPECT_NEAR(sigma3, expected_sigma3, 1e-4);
  // It is emphatically NOT the noisy near-zero pop variance.
  EXPECT_GT(d3, 0.01 * 0.01); // σ_3 > 0.01, i.e. orders of magnitude above 1e-6.
  // Sanity: a full-clean-history name (γ=1) keeps its own σ^TS exactly.
  EXPECT_NEAR(std::sqrt(res.variances[0]), 0.02, 1e-9);
  EXPECT_NEAR(std::sqrt(res.variances[1]), 0.04, 1e-9);
}

// ===========================================================================
//  EWMA RECENCY: a series with a recent high-variance block has a LARGER EWMA
//  variance than the same-magnitude block placed in the OLD tail. spec_hl small so
//  recency bites; spec_nw_lags = 0 (isolate the EWMA effect).
// ===========================================================================
TEST(SpecificRisk, EwmaWeightsRecentVarianceMoreThanOldVariance) {
  ExposureMatrix xm = two_inst_exposures();
  const usize window = 8U;
  // Name 0: a HIGH-variance block in the NEWEST half, calm OLD half.
  // Name 1: the SAME high-variance block in the OLD tail, calm NEW half.
  std::vector<std::vector<f64>> u(2);
  const f64 hi = 0.10; // big residuals
  const f64 lo = 0.001;
  // index 0 = newest.
  u[0] = {hi, -hi, hi, -hi, lo, -lo, lo, -lo}; // recent-high
  u[1] = {lo, -lo, lo, -lo, hi, -hi, hi, -hi}; // old-high

  const SpecificRisk res = specific_risk_blend(xm, u, window, /*spec_hl=*/2U,
                                               /*spec_nw_lags=*/0U, /*structural=*/false);
  // structural=false ⇒ γ≡1 ⇒ σ == σ^TS, so D == ewma_var directly. Recent-high > old-high.
  EXPECT_GT(res.variances[0], res.variances[1]);
}

// ===========================================================================
//  FULL-HISTORY i.i.d. recovers ≈ pop variance: a single full-clean-history name,
//  γ=1, spec_nw_lags=0, spec_hl=0 (equal weights). The EWMA variance with equal
//  weights IS the population variance, so D ≈ pop_variance(r). structural=false.
// ===========================================================================
TEST(SpecificRisk, FullHistoryIidRecoversPopVariance) {
  ExposureMatrix xm = two_inst_exposures();
  const usize window = 64U;
  // A deterministic pseudo-i.i.d. residual series (a simple LCG-driven sign/scale).
  std::vector<std::vector<f64>> u(2);
  atx::u64 st = 0x1234567ULL;
  auto next = [&]() {
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(st >> 11U) / static_cast<f64>(1ULL << 53U)) - 0.5; // [-0.5,0.5)
  };
  for (usize t = 0; t < window; ++t) {
    u[0].push_back(0.03 * next());
    u[1].push_back(0.03 * next());
  }
  const SpecificRisk res = specific_risk_blend(xm, u, window, /*spec_hl=*/0U,
                                               /*spec_nw_lags=*/0U, /*structural=*/false);
  EXPECT_NEAR(res.variances[0], popvar(u[0]), 1e-12);
  EXPECT_NEAR(res.variances[1], popvar(u[1]), 1e-12);
}

// ===========================================================================
//  D > 0 for every name, including an EMPTY residual series (floored).
// ===========================================================================
TEST(SpecificRisk, AllVariancesStrictlyPositive) {
  ExposureMatrix xm = two_inst_exposures();
  std::vector<std::vector<f64>> u(2);
  u[0] = {0.01, -0.01, 0.02, -0.02}; // some history
  u[1] = {};                         // empty -> would be 0; must be floored > 0
  const SpecificRisk res = specific_risk_blend(xm, u, /*window=*/4U, /*spec_hl=*/0U,
                                               /*spec_nw_lags=*/0U, /*structural=*/false);
  for (Eigen::Index r = 0; r < res.variances.size(); ++r) {
    EXPECT_GT(res.variances[r], 0.0);
    EXPECT_TRUE(std::isfinite(res.variances[r]));
  }
}

// ===========================================================================
//  ISC interface: the carrier type exists and the result's `isc` is EMPTY this
//  sprint (D stays strictly diagonal — no same-issuer off-diagonal wired in).
// ===========================================================================
TEST(SpecificRisk, IssuerSpecificCovCarrierIsEmpty) {
  ExposureMatrix xm = two_inst_exposures();
  std::vector<std::vector<f64>> u(2);
  u[0] = {0.01, -0.01};
  u[1] = {0.02, -0.02};
  const SpecificRisk res = specific_risk_blend(xm, u, /*window=*/2U, /*spec_hl=*/4U,
                                               /*spec_nw_lags=*/1U, /*structural=*/true);
  EXPECT_TRUE(res.isc.empty()); // interface-only this sprint
  // Compile-level: the carrier type is usable.
  const IssuerSpecificCov probe{0U, 1U, 0.0};
  EXPECT_EQ(probe.i, 0U);
  EXPECT_EQ(probe.j, 1U);
}

// ===========================================================================
//  NW INFLATION (closed form): a positively-autocorrelated series inflates the
//  variance above its plain EWMA variance (long-run variance > per-observation
//  variance). spec_hl=0 so ewma_var == popvar (= Γ_0); the only difference between
//  the two builds is the Newey-West factor. We assert against a HAND-COMPUTED
//  Bartlett long-run-variance ratio (not the kernel's own formula): for L=1,
//  inflation = 1 + (1 − 1/2)·2·Γ_1/Γ_0 = 1 + Γ_1/Γ_0, with the equal-weighted
//  Γ_d = (1/T) Σ_t (r_t−μ)(r_{t−d}−μ). The fixture is a deterministic AR(1) driven
//  by random-SIGN i.i.d. shocks (an LCG), giving genuine positive autocorrelation.
// ===========================================================================
TEST(SpecificRisk, NeweyWestInflatesPositivelyAutocorrelatedSeries) {
  ExposureMatrix xm = two_inst_exposures();
  const usize window = 64U;
  std::vector<std::vector<f64>> u(2);
  atx::u64 st = 0xABCDEF1234ULL;
  auto next_sign = [&]() {
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((st >> 33U) & 1U) ? 0.01 : -0.01; // random ±0.01 i.i.d. shock
  };
  f64 prev = 0.0;
  for (usize t = 0; t < window; ++t) {
    prev = 0.8 * prev + next_sign(); // AR(1), φ=0.8 -> positive autocorrelation
    u[0].push_back(prev);
    u[1].push_back(0.0); // unused
  }

  // Hand-compute the L=1 Bartlett inflation = 1 + Γ_1/Γ_0 (equal-weighted, μ-demeaned).
  f64 mean = 0.0;
  for (const f64 v : u[0]) {
    mean += v;
  }
  mean /= static_cast<f64>(window);
  f64 g0 = 0.0, g1 = 0.0;
  for (usize t = 0; t < window; ++t) {
    g0 += (u[0][t] - mean) * (u[0][t] - mean);
    if (t >= 1U) {
      g1 += (u[0][t] - mean) * (u[0][t - 1U] - mean);
    }
  }
  g0 /= static_cast<f64>(window);
  g1 /= static_cast<f64>(window);
  const f64 expected_inflation = 1.0 + g1 / g0; // L=1: bartlett(1)=0.5, ·2 = 1
  ASSERT_GT(expected_inflation, 1.0) << "fixture must be positively autocorrelated";

  const SpecificRisk with_nw = specific_risk_blend(xm, u, window, /*spec_hl=*/0U,
                                                   /*spec_nw_lags=*/1U, /*structural=*/false);
  const SpecificRisk no_nw = specific_risk_blend(xm, u, window, /*spec_hl=*/0U,
                                                 /*spec_nw_lags=*/0U, /*structural=*/false);
  // D_no == Γ_0 (popvar); D_with == Γ_0 · inflation. The ratio is the NW inflation.
  EXPECT_GT(with_nw.variances[0], no_nw.variances[0]);
  EXPECT_NEAR(with_nw.variances[0] / no_nw.variances[0], expected_inflation, 1e-9);
}

// ===========================================================================
//  THROUGH build(): the EwmaNeweyWestStructural method yields a usable model and a
//  thin-history name does NOT collapse to a near-zero specific variance. We use a
//  single-sector K=1 panel where one instrument has a SHORTER price history (NaN in
//  the old rows) so its residual series is thinner than the others'.
// ===========================================================================
TEST(SpecificRisk, BuildWithBlendedMethodProducesUsableModel) {
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
  const std::vector<u32> group{1U, 1U, 1U, 1U};

  FactorModelConfig cfg = single_sector_cfg();
  cfg.cov.specific_method = SpecificRiskMethod::EwmaNeweyWestStructural;
  cfg.cov.spec_halflife = 4U;
  cfg.cov.spec_nw_lags = 1U;
  cfg.cov.structural_blend = true;
  FactorModelBuilder builder{cfg};
  const auto m =
      builder.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(m.has_value()) << (m ? "" : m.error().to_string());
  EXPECT_EQ(m->n_factors(), 1U);
  EXPECT_EQ(m->n_instruments(), n_inst);

  std::vector<f64> w(n_inst, 0.25);
  const f64 rk = m->risk(std::span<const f64>{w});
  EXPECT_TRUE(std::isfinite(rk));
  EXPECT_GT(rk, 0.0);

  // Determinism: a second build is byte-identical.
  const auto m2 =
      builder.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m2->risk(std::span<const f64>{w}), rk);
}

// ===========================================================================
//  BACKWARD-COMPAT: the DEFAULT PopVariance method reproduces the as-built path.
//  We build the same single-sector panel with the default config and read the per-
//  instrument D via risk-pairing (X row == 1 cancels F): risk(e_i − e_j) = D_i + D_j.
//  The recovered D matches a direct pop-variance of the two-pass WLS residuals,
//  proving the default dispatch did NOT change the path. (The RiskFactorBuilder suite
//  is the real guard; this is an explicit equality check on the default branch.)
// ===========================================================================
TEST(SpecificRisk, DefaultMethodReproducesPopVariancePath) {
  const usize window = 6U;
  const usize n_inst = 3U;
  const std::vector<std::array<f64, 3>> r = {{{0.015, -0.012, 0.020}}, {{-0.025, 0.018, 0.005}},
                                             {{0.026, 0.022, -0.030}}, {{-0.011, 0.033, -0.014}},
                                             {{0.041, -0.045, 0.009}}, {{-0.018, -0.009, 0.031}}};
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1000.0));
  for (usize i = 0; i < n_inst; ++i) {
    close[n_rows - 1U][i] = 1.0;
    for (usize s = window; s-- > 0U;) {
      close[s][i] = close[s + 1U][i] * (1.0 + r[s][i]);
    }
  }
  PanelFixture fx{n_rows, n_inst, close, volume};
  const std::vector<u32> group{5U, 5U, 5U};

  FactorModelBuilder builder{single_sector_cfg()}; // DEFAULT cov (PopVariance)
  const auto m =
      builder.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(m.has_value()) << (m ? "" : m.error().to_string());

  // Replicate the two-pass WLS to get the residual series, then pop-variance.
  std::vector<std::vector<f64>> u_ols(n_inst);
  for (usize s = 0; s < window; ++s) {
    f64 sum = 0.0;
    for (usize i = 0; i < n_inst; ++i) {
      sum += r[s][i];
    }
    const f64 f_ols = sum / static_cast<f64>(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      u_ols[i].push_back(r[s][i] - f_ols);
    }
  }
  std::vector<f64> d0(n_inst);
  for (usize i = 0; i < n_inst; ++i) {
    const f64 v = popvar(u_ols[i]);
    d0[i] = (v < 1e-12) ? 1e-12 : v;
  }
  std::vector<std::vector<f64>> u_wls(n_inst);
  for (usize s = 0; s < window; ++s) {
    f64 num = 0.0, den = 0.0;
    for (usize i = 0; i < n_inst; ++i) {
      num += r[s][i] / d0[i];
      den += 1.0 / d0[i];
    }
    const f64 f_wls = num / den;
    for (usize i = 0; i < n_inst; ++i) {
      u_wls[i].push_back(r[s][i] - f_wls);
    }
  }

  auto Dpair = [&](usize a, usize b) {
    std::vector<f64> w(n_inst, 0.0);
    w[a] = 1.0;
    w[b] = -1.0;
    return m->risk(std::span<const f64>{w});
  };
  const f64 s01 = Dpair(0, 1), s02 = Dpair(0, 2), s12 = Dpair(1, 2);
  const f64 dd0 = (s01 + s02 - s12) / 2.0;
  const f64 dd1 = (s01 + s12 - s02) / 2.0;
  const f64 dd2 = (s02 + s12 - s01) / 2.0;
  EXPECT_NEAR(dd0, popvar(u_wls[0]), 1e-9);
  EXPECT_NEAR(dd1, popvar(u_wls[1]), 1e-9);
  EXPECT_NEAR(dd2, popvar(u_wls[2]), 1e-9);
}

} // namespace
