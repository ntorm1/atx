// risk_vol_regime_test.cpp — S8.5: Volatility Regime Adjustment (VRA).
//
// vol_regime_multiplier(fseries, f_forecast, vra_halflife) computes a market-wide
// volatility-regime multiplier λ² from realized-vs-forecast factor moves and returns
// it alongside the per-date factor cross-sectional bias-statistic series B_t:
//   * per kept date t (row t, row 0 = NEWEST):
//       B_t = √( (1/Kt) · Σ_j (f_{t,j}/σ_j)² ),  σ_j = √F_jj  (j with F_jj>0 only)
//   * λ² = EWMA(B_t² ; H) = Σ_t w_t·B_t² / Σ_t w_t,  w_t = 2^(−t/H)  (the SAME
//     weighting as cov_ewma.hpp; H ≥ 1 because vra_halflife==0 is the no-VRA sentinel)
// E[B_t²]=1 if the forecast is unbiased ⇒ λ²≈1 on a stationary panel; a newest-rows
// vol spike against a calmer forecast pushes λ²>1.
//
// build() wiring (vra_halflife>0): F ← λ²·F and D ← λ²·D (the same market-wide
// multiplier on both — a documented simplification). vra_halflife==0 ⇒ no VRA, the
// pre-S8.5 F/D byte-identical.
//
// Coverage (plan §5 S8.5 acceptance):
//   * Stationary panel (forecast diag == sample variances) ⇒ λ²≈1 (within ~5%).
//   * Newest-rows vol spike vs a calm forecast ⇒ λ²>1 materially (> 1.2).
//   * bias_stats length == T; the kernel is RNG-free deterministic (byte-identical).
//   * Degenerate guard: a zero forecast diagonal entry excludes that factor (no NaN).
//   * Truncation-invariance: appending older rows beyond the EWMA reach is negligible.
//   * build-level determinism: same panel/config twice ⇒ byte-identical risk readout.

#include <array>   // std::array
#include <cmath>   // std::pow, std::sqrt, std::isfinite, std::isnan
#include <cstdint> // fixed-width
#include <cstring> // std::memcmp
#include <limits>  // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId (Symbol)
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/vol_regime.hpp"

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
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::RegimeAdjust;
using atx::engine::risk::vol_regime_multiplier;

constexpr f64 kTestNaN = std::numeric_limits<f64>::quiet_NaN();

// Population variance of a column of `m` (divisor T), the unbiased-forecast oracle:
// a stationary panel whose forecast diag == its sample variance has E[B_t²]=1.
[[nodiscard]] f64 col_pop_variance(const MatX &m, Eigen::Index c) {
  const Eigen::Index t = m.rows();
  const f64 mean = m.col(c).mean();
  f64 ss = 0.0;
  for (Eigen::Index r = 0; r < t; ++r) {
    const f64 d = m(r, c) - mean;
    ss += d * d;
  }
  return ss / static_cast<f64>(t);
}

// Minimal single-sector PanelView fixture (same backing-storage pattern as
// risk_cov_ewma_test.cpp / risk_factor_builder_test.cpp). Row 0 = newest.
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kTestNaN);
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

// ===========================================================================
//  Stationary panel ⇒ λ²≈1. The forecast diag is set to the per-column sample
//  variances, so E[B_t²]=1 and the EWMA of B_t² averages to ≈1 (within ~5%).
// ===========================================================================
TEST(RiskVolRegime, StationaryPanelGivesUnitMultiplier) {
  // 30×3 deterministic, comparable-scale, mean-near-zero factor returns (row 0 newest).
  const usize T = 30U;
  const usize K = 3U;
  MatX fseries(static_cast<Eigen::Index>(T), static_cast<Eigen::Index>(K));
  std::uint64_t state = 0xD1B54A32D192ED03ULL;
  auto next_u = [&]() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = state >> 11U;
    return static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U); // [0,1)
  };
  for (Eigen::Index r = 0; r < static_cast<Eigen::Index>(T); ++r) {
    for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
      fseries(r, c) = 0.02 * (2.0 * next_u() - 1.0); // (−0.02, 0.02), mean ≈ 0
    }
  }
  // Forecast = diag(sample variances) ⇒ the forecast IS unbiased for this panel.
  MatX f_forecast = MatX::Zero(static_cast<Eigen::Index>(K), static_cast<Eigen::Index>(K));
  for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
    f_forecast(c, c) = col_pop_variance(fseries, c);
  }

  const RegimeAdjust ra = vol_regime_multiplier(fseries, f_forecast, /*vra_halflife=*/12U);
  EXPECT_GT(ra.lambda2, 0.0);
  EXPECT_NEAR(ra.lambda2, 1.0, 0.05); // unbiased forecast ⇒ λ²≈1 within 5%
  EXPECT_EQ(ra.bias_stats.size(), static_cast<Eigen::Index>(T));
}

// ===========================================================================
//  Newest-rows vol spike ⇒ λ²>1. The newest rows' returns are scaled ×k while the
//  forecast diag reflects the CALMER full-window vol ⇒ the recent B_t² are >1 and
//  the EWMA (which weights the newest rows most) inflates λ² materially.
// ===========================================================================
TEST(RiskVolRegime, NewestSpikeInflatesMultiplier) {
  const usize T = 24U;
  const usize K = 2U;
  MatX fseries(static_cast<Eigen::Index>(T), static_cast<Eigen::Index>(K));
  std::uint64_t state = 0x123456789ABCDEFULL;
  auto next_u = [&]() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = state >> 11U;
    return static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
  };
  for (Eigen::Index r = 0; r < static_cast<Eigen::Index>(T); ++r) {
    for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
      fseries(r, c) = 0.01 * (2.0 * next_u() - 1.0);
    }
  }
  // Forecast diag = the CALM full-window sample variance BEFORE the spike injection.
  MatX f_forecast = MatX::Zero(static_cast<Eigen::Index>(K), static_cast<Eigen::Index>(K));
  for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
    f_forecast(c, c) = col_pop_variance(fseries, c);
  }
  // Inject a ×5 vol spike into the NEWEST 5 rows (rows 0..4), AFTER fixing the forecast.
  for (Eigen::Index r = 0; r < 5; ++r) {
    for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
      fseries(r, c) *= 5.0;
    }
  }

  const RegimeAdjust ra = vol_regime_multiplier(fseries, f_forecast, /*vra_halflife=*/6U);
  EXPECT_GT(ra.lambda2, 1.2); // recent regime far hotter than the forecast ⇒ λ² inflates
}

// ===========================================================================
//  bias_stats series + RNG-free determinism. Calling the kernel twice on the same
//  inputs gives a byte-identical λ² and bias_stats (no RNG, order-fixed reductions).
// ===========================================================================
TEST(RiskVolRegime, DeterministicByteIdentical) {
  MatX fseries(7, 2);
  fseries << 0.011, -0.006, -0.004, 0.013, 0.018, -0.002, -0.009, 0.007, 0.015, -0.011, -0.003,
      0.005, 0.022, -0.014;
  MatX f_forecast(2, 2);
  f_forecast << 1.5e-4, 0.0, 0.0, 1.1e-4;

  const RegimeAdjust a = vol_regime_multiplier(fseries, f_forecast, 5U);
  const RegimeAdjust b = vol_regime_multiplier(fseries, f_forecast, 5U);
  ASSERT_EQ(a.bias_stats.size(), 7);
  ASSERT_EQ(b.bias_stats.size(), 7);
  EXPECT_EQ(a.lambda2, b.lambda2); // bit-exact
  EXPECT_EQ(0, std::memcmp(a.bias_stats.data(), b.bias_stats.data(),
                           static_cast<usize>(a.bias_stats.size()) * sizeof(f64)));
}

// ===========================================================================
//  Degenerate guard: a zero forecast diagonal (a degenerate factor) is EXCLUDED
//  from B_t's K-average — λ² and the bias stats stay finite (no divide-by-σ_j=0 NaN).
// ===========================================================================
TEST(RiskVolRegime, ZeroForecastDiagonalExcludesFactor) {
  MatX fseries(6, 3);
  fseries << 0.01, 0.0, -0.02, -0.015, 0.0, 0.011, 0.02, 0.0, -0.008, -0.007, 0.0, 0.01, 0.013, 0.0,
      -0.012, -0.009, 0.0, 0.006;
  // Factor 1 has a ZERO forecast variance (degenerate column) — must be excluded.
  MatX f_forecast(3, 3);
  f_forecast << 2.0e-4, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.5e-4;

  const RegimeAdjust ra = vol_regime_multiplier(fseries, f_forecast, 4U);
  EXPECT_TRUE(std::isfinite(ra.lambda2));
  EXPECT_GT(ra.lambda2, 0.0);
  for (Eigen::Index t = 0; t < ra.bias_stats.size(); ++t) {
    EXPECT_TRUE(std::isfinite(ra.bias_stats[t]));
  }
}

// ===========================================================================
//  All-degenerate row guard: if EVERY factor has F_jj<=0 then Kt==0 for every row
//  ⇒ B_t=0 ⇒ λ²==0-weighted-mean-of-zeros. λ² stays finite and non-negative (no NaN).
// ===========================================================================
TEST(RiskVolRegime, AllZeroForecastGivesFiniteMultiplier) {
  MatX fseries(5, 2);
  fseries << 0.01, -0.02, -0.015, 0.011, 0.02, -0.008, -0.007, 0.01, 0.013, -0.012;
  const MatX f_forecast = MatX::Zero(2, 2); // every diagonal zero ⇒ no usable factor
  const RegimeAdjust ra = vol_regime_multiplier(fseries, f_forecast, 3U);
  EXPECT_TRUE(std::isfinite(ra.lambda2));
  EXPECT_GE(ra.lambda2, 0.0);
  for (Eigen::Index t = 0; t < ra.bias_stats.size(); ++t) {
    EXPECT_DOUBLE_EQ(ra.bias_stats[t], 0.0); // Kt==0 ⇒ B_t==0
  }
}

// ===========================================================================
//  Truncation-invariance (no-look-ahead): appending OLD, COMPARABLE-SCALE rows far
//  beyond the EWMA reach changes λ² only negligibly — the weight 2^(−t/H) on a row
//  t≫H is vanishingly small, so a trailing-window estimate cannot be materially
//  moved by the distant past. (A pathologically huge tail CAN move it — the EWMA
//  down-weights but does not suppress arbitrary magnitude — so the invariant is
//  about same-regime old data, which is exactly the no-look-ahead claim.)
// ===========================================================================
TEST(RiskVolRegime, TruncationInvariantUnderOldRows) {
  const usize K = 2U;
  const usize T0 = 8U;
  MatX base(static_cast<Eigen::Index>(T0), static_cast<Eigen::Index>(K));
  base << 0.02, -0.01, -0.012, 0.014, 0.015, -0.008, -0.007, 0.01, 0.022, -0.012, -0.016, 0.017,
      0.011, -0.006, -0.004, 0.013;
  MatX f_forecast(2, 2);
  f_forecast << 2.0e-4, 0.0, 0.0, 1.5e-4;

  const usize H = 1U; // tiny half-life ⇒ old rows are weighted ~0
  const RegimeAdjust ra_short = vol_regime_multiplier(base, f_forecast, H);

  // Extend with 12 OLD (high-row-index) rows of the SAME magnitude as `base` (a
  // deterministic walk in the ±0.02 band). With H=1 the join row 8 carries weight
  // 2^(−8) ≈ 0.0039 (decaying), so same-scale old data shifts λ² well under 0.5%.
  const usize extra = 12U;
  MatX extended(static_cast<Eigen::Index>(T0 + extra), static_cast<Eigen::Index>(K));
  extended.topRows(static_cast<Eigen::Index>(T0)) = base;
  for (usize r = 0; r < extra; ++r) {
    for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
      const f64 sign = ((r + static_cast<usize>(c)) % 2U == 0U) ? 1.0 : -1.0;
      extended(static_cast<Eigen::Index>(T0 + r), c) =
          sign * (0.01 + 0.005 * static_cast<f64>(r % 3U)); // ±0.01..±0.02, same regime
    }
  }
  const RegimeAdjust ra_long = vol_regime_multiplier(extended, f_forecast, H);
  EXPECT_NEAR(ra_long.lambda2, ra_short.lambda2, 0.005 * ra_short.lambda2);
}

// ===========================================================================
//  build-level wiring: vra_halflife>0 RESCALES F and D vs the no-VRA build (the
//  dispatch is genuinely wired), and a repeat build is byte-identical (determinism).
//  Single-sector (K=1) panel so no 252-row lookback is needed; F is observed via
//  risk() readouts (F is not otherwise accessible).
// ===========================================================================
TEST(RiskVolRegime, BuildAppliesAndIsDeterministic) {
  const usize window = 10U;
  const usize n_inst = 4U;
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 100.0 + 10.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) { // oldest -> newest
      px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close};
  const std::vector<u32> group{1U, 1U, 1U, 1U}; // one sector -> K=1
  const std::span<const f64> no_cap{};
  const std::span<const u32> grp{group};

  FactorModelConfig base_cfg;
  base_cfg.sector_factors = true;
  base_cfg.style_mask = 0x00; // sectors only -> K=1, no lookback

  FactorModelConfig vra_cfg = base_cfg;
  vra_cfg.cov.vra_halflife = 6U; // turn VRA on

  const auto m_base = FactorModelBuilder{base_cfg}.build(fx.view(), window, no_cap, grp);
  const auto m_vra = FactorModelBuilder{vra_cfg}.build(fx.view(), window, no_cap, grp);
  ASSERT_TRUE(m_base.has_value()) << (m_base ? "" : m_base.error().to_string());
  ASSERT_TRUE(m_vra.has_value()) << (m_vra ? "" : m_vra.error().to_string());

  const std::vector<f64> w{0.4, -0.1, 0.3, -0.2};
  const std::span<const f64> ws{w};
  // VRA rescales the covariance ⇒ the risk readout differs from the no-VRA build.
  EXPECT_NE(m_vra->risk(ws), m_base->risk(ws));

  // Determinism: a second VRA build is byte-identical (RNG-free).
  const auto m_vra2 = FactorModelBuilder{vra_cfg}.build(fx.view(), window, no_cap, grp);
  ASSERT_TRUE(m_vra2.has_value());
  EXPECT_EQ(m_vra->risk(ws), m_vra2->risk(ws)); // byte-identical
}

// ===========================================================================
//  build-level no-op: vra_halflife==0 (the default) reproduces the no-VRA build
//  byte-for-byte (the carried-forward backward-compat invariant).
// ===========================================================================
TEST(RiskVolRegime, ZeroHalfLifeIsByteIdenticalNoOp) {
  const usize window = 8U;
  const usize n_inst = 4U;
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 50.0 + 7.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) {
      px *= 1.0 + 0.008 * (static_cast<f64>((r + 2U * i) % 4U) - 1.5);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close};
  const std::vector<u32> group{3U, 3U, 3U, 3U};
  const std::span<const f64> no_cap{};
  const std::span<const u32> grp{group};

  FactorModelConfig def_cfg;
  def_cfg.sector_factors = true;
  def_cfg.style_mask = 0x00;
  FactorModelConfig zero_cfg = def_cfg;
  zero_cfg.cov.vra_halflife = 0U; // explicit no-VRA == default

  const auto m_def = FactorModelBuilder{def_cfg}.build(fx.view(), window, no_cap, grp);
  const auto m_zero = FactorModelBuilder{zero_cfg}.build(fx.view(), window, no_cap, grp);
  ASSERT_TRUE(m_def.has_value()) << (m_def ? "" : m_def.error().to_string());
  ASSERT_TRUE(m_zero.has_value());
  const std::vector<f64> w{0.3, -0.2, 0.4, -0.1};
  const std::span<const f64> ws{w};
  EXPECT_EQ(m_def->risk(ws), m_zero->risk(ws)); // vra_halflife==0 ⇒ byte-identical
}

} // namespace
