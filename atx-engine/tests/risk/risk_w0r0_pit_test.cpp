// risk_w0r0_pit_test.cpp — W0-R0 (R-03, R-06): point-in-time exposures in the
// FactorModelBuilder regression passes.
//
// R-03: the return r_s = close(s)/close(s+1) − 1 must be regressed on exposures built
//       at row s+1 (every trailing window ends BEFORE r_s is realized). Pre-W0 the
//       builder used row s, whose vol/beta/momentum/adv windows contain r_s itself.
// R-06: style z-scores use a cap-weighted mean with ±3σ winsorizing, and the cap and
//       group used at each historical date are that date's values (PitSideInputs).
//
// Suite: RiskFactorModelPit.

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

namespace atx_test_w0_r0_pit {

using atx::f64;
using atx::u32;
using atx::u8;
using atx::usize;
using atx::core::linalg::MatX;
using atx::engine::PanelView;
using atx::engine::risk::build_exposures;
using atx::engine::risk::ExposureMatrix;
using atx::engine::risk::ExposureTiming;
using atx::engine::risk::FactorComponents;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::FactorReturnPanel;
using atx::engine::risk::PitSideInputs;
using atx::engine::risk::StyleFactor;
using atx::engine::risk::ZScoreRule;
using atx_test_w0_r0_fixture::closes_from_returns;
using atx_test_w0_r0_fixture::Grid;
using atx_test_w0_r0_fixture::mean_t;
using atx_test_w0_r0_fixture::Rng;
using atx_test_w0_r0_fixture::StorePanel;

[[nodiscard]] u8 bit(StyleFactor f) {
  return static_cast<u8>(1U << static_cast<unsigned>(f));
}

[[nodiscard]] bool same_matrix(const MatX &a, const MatX &b) {
  if (a.rows() != b.rows() || a.cols() != b.cols()) {
    return false;
  }
  for (Eigen::Index c = 0; c < a.cols(); ++c) {
    for (Eigen::Index r = 0; r < a.rows(); ++r) {
      if (a(r, c) != b(r, c)) { // byte-equality intended (NaN never present here)
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool same_exposures(const ExposureMatrix &a, const ExposureMatrix &b) {
  return a.instrument_rows == b.instrument_rows && a.columns.size() == b.columns.size() &&
         same_matrix(a.x, b.x);
}

// A random-walk panel: n_inst names, `rows` rows, daily σ = sigma, volume jitter.
struct Noise {
  Grid close;
  Grid volume;
};
[[nodiscard]] Noise noise_panel(usize rows, usize n_inst, f64 sigma, atx::u64 seed) {
  Rng rng{seed};
  Grid ret(rows - 1U, std::vector<f64>(n_inst));
  for (auto &row : ret) {
    for (f64 &v : row) {
      v = sigma * rng.normal();
    }
  }
  Noise out{closes_from_returns(ret), Grid(rows, std::vector<f64>(n_inst))};
  for (usize r = 0; r < rows; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      out.volume[r][i] = 1.0e5 * std::exp(0.3 * rng.normal());
    }
  }
  return out;
}

// ===========================================================================
//  R-03: the regression exposure row. LaggedV2 (default) builds X for r_s at row s+1;
//  ContemporaneousV1 reproduces the pre-W0 row s. Byte-equal to build_exposures.
// ===========================================================================
TEST(RiskFactorModelPit, LaggedExposureRowIsThePriorClose) {
  const Noise p = noise_panel(/*rows=*/120U, /*n_inst=*/12U, 0.02, 11U);
  const StorePanel sp{p.close, p.volume};
  const PanelView v = sp.view();
  FactorModelConfig cfg;
  cfg.style_mask = static_cast<u8>(bit(StyleFactor::Volatility) | bit(StyleFactor::Liquidity));
  const PitSideInputs side{};
  EXPECT_EQ(cfg.exposure_timing, ExposureTiming::LaggedV2); // corrected default
  EXPECT_EQ(atx::engine::risk::detail::exposure_row(ExposureTiming::LaggedV2, 7U), 8U);
  EXPECT_EQ(atx::engine::risk::detail::exposure_row(ExposureTiming::ContemporaneousV1, 7U), 7U);

  for (const usize s : {0U, 5U, 30U}) {
    const FactorModelBuilder lag{cfg};
    const auto got = lag.regression_exposures(v, s, side);
    const auto want = build_exposures(v, cfg, s + 1U, side);
    ASSERT_TRUE(got.has_value() && want.has_value());
    EXPECT_TRUE(same_exposures(*got, *want)) << "s=" << s;

    FactorModelConfig v1 = cfg;
    v1.exposure_timing = ExposureTiming::ContemporaneousV1;
    const auto got1 = FactorModelBuilder{v1}.regression_exposures(v, s, side);
    const auto want1 = build_exposures(v, v1, s, side);
    ASSERT_TRUE(got1.has_value() && want1.has_value());
    EXPECT_TRUE(same_exposures(*got1, *want1)) << "V1 s=" << s;
  }
}

// ===========================================================================
//  R-03 acceptance, per date: the X that explains r_s is invariant to ANY perturbation
//  of the data at or after r_s's realization (closes/volumes at rows 0..s). Pre-W0
//  (ContemporaneousV1) the same perturbation moves X — the defect this closes.
// ===========================================================================
TEST(RiskFactorModelPit, RegressionExposuresInvariantToFuturePerturbation) {
  const usize rows = 330U;
  const usize n_inst = 10U;
  const Noise base = noise_panel(rows, n_inst, 0.02, 21U);
  FactorModelConfig cfg; // all five panel styles (Size omitted: no cap) + no sectors
  usize v1_moved = 0U;
  usize checked = 0U;
  for (const usize s : {0U, 3U, 40U}) {
    Noise pert = base;
    Rng rng{1000U + s};
    for (usize r = 0; r <= s; ++r) { // rows 0..s = on/after the realization of r_s
      for (usize i = 0; i < n_inst; ++i) {
        pert.close[r][i] *= std::exp(0.25 * rng.normal());
        pert.volume[r][i] *= std::exp(0.5 * rng.normal());
      }
    }
    const StorePanel a{base.close, base.volume};
    const StorePanel b{pert.close, pert.volume};
    const PitSideInputs side{};

    const auto xa = FactorModelBuilder{cfg}.regression_exposures(a.view(), s, side);
    const auto xb = FactorModelBuilder{cfg}.regression_exposures(b.view(), s, side);
    ASSERT_TRUE(xa.has_value() && xb.has_value());
    ASSERT_GT(xa->n_instruments(), 0U);
    ASSERT_EQ(xa->n_factors(), 4U); // Momentum, Volatility, Beta, Liquidity
    EXPECT_TRUE(same_exposures(*xa, *xb)) << "LaggedV2 X moved under a future perturbation, s="
                                          << s;
    ++checked;

    FactorModelConfig v1 = cfg;
    v1.exposure_timing = ExposureTiming::ContemporaneousV1;
    const auto ya = FactorModelBuilder{v1}.regression_exposures(a.view(), s, side);
    const auto yb = FactorModelBuilder{v1}.regression_exposures(b.view(), s, side);
    ASSERT_TRUE(ya.has_value() && yb.has_value());
    v1_moved += same_exposures(*ya, *yb) ? 0U : 1U;
  }
  EXPECT_EQ(checked, 3U);
  EXPECT_EQ(v1_moved, 3U); // the pre-W0 rule leaks r_s into X at every checked date
  std::printf("[W0-R0 evidence] future-perturbation: LaggedV2 X identical at %zu/3 dates; "
              "ContemporaneousV1 X moved at %zu/3 dates\n",
              checked, v1_moved);
}

// ===========================================================================
//  HARNESS-SHAPE SMOKE TEST (not the R-03 discriminator — reviewer minor 1). The
//  causality-harness form of "future-perturbation invariance of the model built at t":
//  one storage buffer holds 12 rows NEWER than t; the model at t is built from the live
//  view at t. Perturbing every future close, volume, cap and group leaves (X, F, D)
//  byte-identical. PanelView cannot address rows newer than its row 0 and the side
//  inputs are sliced here, so this passes on the pre-W0 estimator too; it pins the
//  harness plumbing only. The R-03 proofs are RegressionExposuresInvariantToFuture-
//  Perturbation (per date) and PassBSeriesInvariantToInteriorPerturbation (model level).
// ===========================================================================
TEST(RiskFactorModelPit, ModelBuiltAtTInvariantToFuturePerturbation) {
  const usize future = 12U;
  const usize window = 60U;
  const usize n_inst = 24U;
  const usize rows = future + window + 1U + 60U + 1U; // + Volatility lookback + lag
  const Noise base = noise_panel(rows, n_inst, 0.02, 31U);
  // PIT side inputs over the WHOLE storage (row 0 = newest storage date).
  std::vector<f64> cap(rows * n_inst);
  std::vector<u32> grp(rows * n_inst);
  Rng rc{77U};
  for (usize r = 0; r < rows; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      cap[r * n_inst + i] = 1.0e9 * std::exp(rc.normal());
      grp[r * n_inst + i] = static_cast<u32>(i % 3U);
    }
  }
  Noise pert = base;
  std::vector<f64> cap_p = cap;
  std::vector<u32> grp_p = grp;
  Rng rp{78U};
  for (usize r = 0; r < future; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      pert.close[r][i] *= std::exp(0.3 * rp.normal());
      pert.volume[r][i] *= 3.0;
      cap_p[r * n_inst + i] *= 10.0;
      grp_p[r * n_inst + i] = static_cast<u32>((i + 1U) % 4U);
    }
  }
  FactorModelConfig cfg;
  cfg.style_mask = static_cast<u8>(bit(StyleFactor::Size) | bit(StyleFactor::Volatility) |
                                   bit(StyleFactor::Liquidity));
  const FactorModelBuilder builder{cfg};
  const usize at_rows = rows - future;
  auto side_at_t = [&](const std::vector<f64> &c, const std::vector<u32> &g) {
    return PitSideInputs::per_date(std::span<const f64>{c}.subspan(future * n_inst),
                                   std::span<const u32>{g}.subspan(future * n_inst), at_rows);
  };
  const StorePanel a{base.close, base.volume};
  const StorePanel b{pert.close, pert.volume};
  const auto ma = builder.build_components(a.view_at(future), window, side_at_t(cap, grp));
  const auto mb = builder.build_components(b.view_at(future), window, side_at_t(cap_p, grp_p));
  ASSERT_TRUE(ma.has_value()) << ma.error().to_string();
  ASSERT_TRUE(mb.has_value()) << mb.error().to_string();
  EXPECT_EQ(ma->X.cols(), 6); // 3 sectors + Size, Volatility, Liquidity
  EXPECT_TRUE(same_matrix(ma->X, mb->X));
  EXPECT_TRUE(same_matrix(ma->F, mb->F));
  EXPECT_TRUE(same_matrix(MatX(ma->D), MatX(mb->D)));
  // Sanity: the perturbation is visible to a build AT the newest storage date.
  const auto na = builder.build_components(
      a.view(), window, PitSideInputs::per_date(std::span<const f64>{cap},
                                                std::span<const u32>{grp}, rows));
  const auto nb = builder.build_components(
      b.view(), window, PitSideInputs::per_date(std::span<const f64>{cap_p},
                                                std::span<const u32>{grp_p}, rows));
  ASSERT_TRUE(na.has_value() && nb.has_value());
  EXPECT_FALSE(same_matrix(na->F, nb->F) && same_matrix(na->X, nb->X));
}

// ===========================================================================
//  R-03 measured: spurious factor premium from contemporaneous exposures.
//  DGP: independent random-walk names (no factor structure at all), and dollar volume
//  that moves with the same-day return (volume_s = K·u_s / close_{s+1}, so
//  close_s·volume_s = K·u_s·(1+r_s) — a realistic up-day volume effect). Liquidity =
//  ln adv20 at row s then contains r_s. The regression of r_s on ContemporaneousV1 X
//  finds a large, "significant" Liquidity factor return; LaggedV2 finds none.
// ===========================================================================
TEST(RiskFactorModelPit, ContemporaneousLiquidityPremiumIsRemoved) {
  const usize n_inst = 80U;
  const usize window = 150U;
  const usize rows = window + 1U + 20U + 1U;
  Rng rng{4242U};
  Grid ret(rows - 1U, std::vector<f64>(n_inst));
  for (auto &row : ret) {
    for (f64 &v : row) {
      v = 0.03 * rng.normal();
    }
  }
  const Grid close = closes_from_returns(ret);
  Grid volume(rows, std::vector<f64>(n_inst));
  for (usize r = 0; r < rows; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      const f64 prev = (r + 1U < rows) ? close[r + 1U][i] : close[r][i];
      volume[r][i] = 1.0e6 * std::exp(0.02 * rng.normal()) / prev;
    }
  }
  const StorePanel sp{close, volume};
  FactorModelConfig cfg;
  cfg.style_mask = bit(StyleFactor::Liquidity);
  cfg.sector_factors = false;

  auto liq_premium = [&](ExposureTiming timing) {
    FactorModelConfig c = cfg;
    c.exposure_timing = timing;
    const auto fr = FactorModelBuilder{c}.factor_returns(sp.view(), window, PitSideInputs{});
    EXPECT_TRUE(fr.has_value());
    std::vector<f64> f;
    if (fr.has_value()) {
      EXPECT_EQ(fr->f.cols(), 1);
      for (Eigen::Index u = 0; u < fr->f.rows(); ++u) {
        f.push_back(fr->f(u, 0));
      }
    }
    return f;
  };
  const std::vector<f64> f1 = liq_premium(ExposureTiming::ContemporaneousV1);
  const std::vector<f64> f2 = liq_premium(ExposureTiming::LaggedV2);
  ASSERT_GE(f1.size(), 140U);
  ASSERT_GE(f2.size(), 140U);
  const auto m1 = mean_t(f1);
  const auto m2 = mean_t(f2);
  std::printf("[W0-R0 evidence] pure-noise Liquidity factor return: ContemporaneousV1 mean=%.6f "
              "t=%.2f (n=%zu); LaggedV2 mean=%.6f t=%.2f (n=%zu)\n",
              m1.mean, m1.t, f1.size(), m2.mean, m2.t, f2.size());
  EXPECT_GT(m1.t, 5.0);             // the look-ahead manufactures a premium
  EXPECT_LT(std::fabs(m2.t), 3.0);  // none once X is lagged
}

// ===========================================================================
//  R-06: the cap and group of each HISTORICAL date are that date's values.
//  Size exposures for r_s (row s+1) must use cap row s+1; a name that changed group
//  at an older date carries its OLD dummy there. The static broadcast form is the
//  legacy (look-ahead) behaviour and is shown to differ.
// ===========================================================================
TEST(RiskFactorModelPit, CapAndGroupAreReadPerDate) {
  const usize rows = 12U;
  const usize n_inst = 6U;
  const Noise p = noise_panel(rows, n_inst, 0.02, 51U);
  const StorePanel sp{p.close, p.volume};
  std::vector<f64> cap(rows * n_inst);
  std::vector<u32> grp(rows * n_inst);
  for (usize r = 0; r < rows; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      // Cap ranks REVERSE between old and new dates; name 0 moves group 1 -> 2 at row 4.
      const f64 rank = (r < 5U) ? static_cast<f64>(i + 1U) : static_cast<f64>(n_inst - i);
      cap[r * n_inst + i] = 1.0e8 * rank;
      grp[r * n_inst + i] = (i == 0U && r >= 4U) ? 1U : ((i < 3U) ? 2U : 3U);
    }
  }
  FactorModelConfig cfg;
  cfg.style_mask = bit(StyleFactor::Size);
  const PitSideInputs pit = PitSideInputs::per_date(std::span<const f64>{cap},
                                                    std::span<const u32>{grp}, rows);
  const FactorModelBuilder builder{cfg};
  for (const usize s : {0U, 6U, 9U}) {
    const auto got = builder.regression_exposures(sp.view(), s, pit);
    ASSERT_TRUE(got.has_value());
    const auto want = build_exposures(
        sp.view(), cfg, s + 1U, std::span<const f64>{cap}.subspan((s + 1U) * n_inst, n_inst),
        std::span<const u32>{grp}.subspan((s + 1U) * n_inst, n_inst));
    ASSERT_TRUE(want.has_value());
    EXPECT_TRUE(same_exposures(*got, *want)) << "s=" << s;
  }
  // Old date s=6 (row 7): name 0 is in group 1 there, in group 2 today.
  const auto old = builder.regression_exposures(sp.view(), 6U, pit);
  ASSERT_TRUE(old.has_value());
  ASSERT_EQ(old->columns.front().group_id, 1U);
  EXPECT_EQ(old->x(0, 0), 1.0); // row 0 == instrument 0 (all present)
  // Size at the old date ranks by the OLD caps: instrument 0 (largest then, smallest
  // today) has the highest Size exposure.
  const Eigen::Index size_col = static_cast<Eigen::Index>(old->n_factors() - 1U);
  for (Eigen::Index r = 1; r < old->x.rows(); ++r) {
    EXPECT_GT(old->x(0, size_col), old->x(r, size_col));
  }
  // The static broadcast (today's cap/group at every date) gives a different X.
  const auto stat = builder.regression_exposures(
      sp.view(), 6U,
      PitSideInputs::broadcast(std::span<const f64>{cap}.subspan(0U, n_inst),
                               std::span<const u32>{grp}.subspan(0U, n_inst)));
  ASSERT_TRUE(stat.has_value());
  EXPECT_FALSE(same_exposures(*old, *stat));
}

// PitSideInputs shape and coverage contract.
TEST(RiskFactorModelPit, SideInputShapeAndCoverageAreChecked) {
  const usize rows = 30U;
  const usize n_inst = 4U;
  const Noise p = noise_panel(rows, n_inst, 0.02, 61U);
  const StorePanel sp{p.close, p.volume};
  FactorModelConfig cfg;
  cfg.style_mask = 0U; // sectors only
  const FactorModelBuilder builder{cfg};
  std::vector<u32> grp(10U * n_inst, 1U);
  // Malformed length.
  const auto bad = builder.build_components(
      sp.view(), 8U,
      PitSideInputs::per_date({}, std::span<const u32>{grp}.subspan(0U, 9U), 10U));
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), atx::core::ErrorCode::InvalidArgument);
  // Per-date inputs covering rows [0,10) cannot serve a window of 12 (needs row 12).
  const auto shortfall =
      builder.build_components(sp.view(), 12U, PitSideInputs::per_date({}, grp, 10U));
  ASSERT_FALSE(shortfall.has_value());
  EXPECT_EQ(shortfall.error().code(), atx::core::ErrorCode::InvalidArgument);
  // ... and a window of 9 (rows 0..9) is covered.
  const auto ok = builder.build_components(sp.view(), 9U, PitSideInputs::per_date({}, grp, 10U));
  EXPECT_TRUE(ok.has_value()) << (ok ? "" : ok.error().to_string());
  // Single-date overload: a row beyond the per-date inputs is OutOfRange.
  const auto oor = build_exposures(sp.view(), cfg, 11U, PitSideInputs::per_date({}, grp, 10U));
  ASSERT_FALSE(oor.has_value());
  EXPECT_EQ(oor.error().code(), atx::core::ErrorCode::OutOfRange);
}

// ===========================================================================
//  R-06: CapWeightedWinsorV2 z-score. Hand-computable 2-name case, the cap-weighted
//  mean-zero / unit-std contract, and a planted outlier that V1 lets compress every
//  other name while V2 clips it to +3.
// ===========================================================================
TEST(RiskFactorModelPit, ZScoreIsCapWeightedAndWinsorized) {
  // (a) caps {e, e³}: ln = {1, 3}; μ_cap = 1 + 2e²/(1+e²); σ_eq = 1.
  {
    const StorePanel sp{Grid{{100.0, 100.0}}, Grid{{10.0, 10.0}}};
    FactorModelConfig cfg;
    cfg.style_mask = bit(StyleFactor::Size);
    const std::vector<f64> cap{std::exp(1.0), std::exp(3.0)};
    const auto x = build_exposures(sp.view(), cfg, 0U, std::span<const f64>{cap}, {});
    ASSERT_TRUE(x.has_value());
    const f64 e2 = std::exp(2.0);
    const f64 mu = 1.0 + 2.0 * e2 / (1.0 + e2);
    EXPECT_NEAR(x->x(0, 0), 1.0 - mu, 1e-12);
    EXPECT_NEAR(x->x(1, 0), 3.0 - mu, 1e-12);
  }
  // (b) 40 names, caps spread over 3 decades, one planted 40-log-point data error in the
  //     Liquidity descriptor (ln adv20) of name 7 — e.g. a volume unit bug.
  const usize n = 40U;
  const usize rows = 20U;
  Rng rng{91U};
  Grid close(rows, std::vector<f64>(n));
  Grid volume(rows, std::vector<f64>(n));
  std::vector<f64> cap(n);
  for (usize i = 0; i < n; ++i) {
    const f64 dollar = 1.0e6 * std::exp(1.0 * rng.normal());
    cap[i] = 1.0e9 * std::exp(1.2 * rng.normal());
    for (usize r = 0; r < rows; ++r) {
      close[r][i] = 50.0;
      volume[r][i] = (i == 7U ? std::exp(40.0) : 1.0) * dollar / 50.0;
    }
  }
  const StorePanel sp{close, volume};
  FactorModelConfig v2;
  v2.style_mask = bit(StyleFactor::Liquidity);
  FactorModelConfig v1 = v2;
  v1.zscore_rule = ZScoreRule::EqualWeightV1;
  const auto x2 = build_exposures(sp.view(), v2, 0U, std::span<const f64>{cap}, {});
  const auto x1 = build_exposures(sp.view(), v1, 0U, std::span<const f64>{cap}, {});
  ASSERT_TRUE(x2.has_value() && x1.has_value());
  f64 max_abs2 = 0.0;
  f64 sd_rest1 = 0.0;
  f64 sd_rest2 = 0.0;
  f64 m1 = 0.0;
  f64 m2 = 0.0;
  for (usize i = 0; i < n; ++i) {
    max_abs2 = std::max(max_abs2, std::fabs(x2->x(static_cast<Eigen::Index>(i), 0)));
    if (i != 7U) {
      m1 += x1->x(static_cast<Eigen::Index>(i), 0);
      m2 += x2->x(static_cast<Eigen::Index>(i), 0);
    }
  }
  m1 /= static_cast<f64>(n - 1U);
  m2 /= static_cast<f64>(n - 1U);
  for (usize i = 0; i < n; ++i) {
    if (i != 7U) {
      const f64 d1 = x1->x(static_cast<Eigen::Index>(i), 0) - m1;
      const f64 d2 = x2->x(static_cast<Eigen::Index>(i), 0) - m2;
      sd_rest1 += d1 * d1;
      sd_rest2 += d2 * d2;
    }
  }
  sd_rest1 = std::sqrt(sd_rest1 / static_cast<f64>(n - 1U));
  sd_rest2 = std::sqrt(sd_rest2 / static_cast<f64>(n - 1U));
  std::printf("[W0-R0 evidence] planted ln-adv outlier: V1 outlier z=%.3f, spread of the other "
              "39 names sd=%.3f; V2 outlier z=%.3f, others sd=%.3f, max|z|=%.3f\n",
              x1->x(7, 0), sd_rest1, x2->x(7, 0), sd_rest2, max_abs2);
  // The ±3σ winsorizing bound, in the equal-weight frame it is defined in (fix pass 1:
  // no post-centring clip, so |z| itself may exceed 3 by |μ_eq − μ_w|/σ), and the
  // cap-weighted mean of z is exactly 0 (it held only when the old clip did not bind).
  f64 eq_mean2 = 0.0;
  f64 cap_sum = 0.0;
  f64 cap_z2 = 0.0;
  for (usize i = 0; i < n; ++i) {
    eq_mean2 += x2->x(static_cast<Eigen::Index>(i), 0);
    cap_sum += cap[i];
    cap_z2 += cap[i] * x2->x(static_cast<Eigen::Index>(i), 0);
  }
  eq_mean2 /= static_cast<f64>(n);
  f64 max_dev2 = 0.0;
  for (usize i = 0; i < n; ++i) {
    max_dev2 = std::max(max_dev2, std::fabs(x2->x(static_cast<Eigen::Index>(i), 0) - eq_mean2));
  }
  // Winsorizing bound: the 16-pass iteration converges geometrically, so the planted
  // 40-log-point outlier ends 4.4e-8 σ past the bound (measured); 1e-6 bounds that
  // residual. (Pre-fix, |z| ≤ 3 held exactly only because z itself was clipped.)
  EXPECT_LE(max_dev2, 3.0 + 1e-6);
  std::printf("[W0-R0 evidence] V2 winsor bound: max|z - mean_eq(z)| = %.12f, cap-weighted "
              "mean of z = %.3e\n",
              max_dev2, cap_z2 / cap_sum);
  EXPECT_NEAR(cap_z2 / cap_sum, 0.0, 1e-12);  // cap-weighted mean exactly 0
  EXPECT_GE(x2->x(7, 0), 2.0);      // the outlier stays the top name, at the bound region
  EXPECT_GT(x1->x(7, 0), 5.0);      // V1 keeps the outlier extreme ...
  EXPECT_LT(sd_rest1, 0.35);        // ... and crushes everyone else
  EXPECT_GT(sd_rest2, 0.6);         // V2 keeps the rest informative
  // (c) no outlier: cap-weighted mean exactly 0 and equal-weight population std 1.
  for (usize r = 0; r < rows; ++r) {
    volume[r][7] /= std::exp(40.0);
  }
  const StorePanel sp3{close, volume};
  const auto x3 = build_exposures(sp3.view(), v2, 0U, std::span<const f64>{cap}, {});
  ASSERT_TRUE(x3.has_value());
  f64 wsum = 0.0;
  f64 wz = 0.0;
  f64 ez = 0.0;
  for (usize i = 0; i < n; ++i) {
    wsum += cap[i];
    wz += cap[i] * x3->x(static_cast<Eigen::Index>(i), 0);
    ez += x3->x(static_cast<Eigen::Index>(i), 0);
  }
  ez /= static_cast<f64>(n);
  f64 ss = 0.0;
  for (usize i = 0; i < n; ++i) {
    const f64 d = x3->x(static_cast<Eigen::Index>(i), 0) - ez;
    ss += d * d;
  }
  // Exact contract unconditionally (fix pass 1: no post-centring clip can break it).
  EXPECT_NEAR(wz / wsum, 0.0, 1e-12);
  EXPECT_NEAR(std::sqrt(ss / static_cast<f64>(n)), 1.0, 1e-12);
}

// ===========================================================================
//  Fix pass 1 (reviewer minor 3): Size = ln cap standardized with a CAP-weighted mean
//  sits ≈ σ_ln above the equal-weight mean for a lognormal cap spread, so a ±3 clip of
//  z after centring would pin the whole small-cap tail at −3. The decision (USE4: trim
//  the raw descriptor, never clip after cap-weighted centring) keeps that tail ordered.
//  400 names, ln-cap sd 2 (roughly a listed-universe spread).
// ===========================================================================
TEST(RiskFactorModelPit, SizeSmallCapTailKeepsItsOrder) {
  const usize n = 400U;
  Rng rng{515U};
  std::vector<f64> cap(n);
  for (f64 &c : cap) {
    c = 1.0e9 * std::exp(2.0 * rng.normal());
  }
  const StorePanel sp{Grid(1U, std::vector<f64>(n, 50.0)), Grid(1U, std::vector<f64>(n, 1.0e4))};
  FactorModelConfig cfg;
  cfg.style_mask = bit(StyleFactor::Size);
  const auto x = build_exposures(sp.view(), cfg, 0U, std::span<const f64>{cap}, {});
  ASSERT_TRUE(x.has_value());
  ASSERT_EQ(x->n_instruments(), n);
  ASSERT_EQ(x->n_factors(), 1U);
  std::vector<f64> z(n);
  for (usize r = 0; r < n; ++r) {
    z[x->instrument_rows[r]] = x->x(static_cast<Eigen::Index>(r), 0);
  }
  f64 wsum = 0.0;
  f64 wz = 0.0;
  f64 ez = 0.0;
  for (usize i = 0; i < n; ++i) {
    wsum += cap[i];
    wz += cap[i] * z[i];
    ez += z[i];
  }
  ez /= static_cast<f64>(n);
  f64 ss = 0.0;
  f64 max_dev = 0.0;
  usize below3 = 0U; // names a post-centring ±3 clip would have pinned at −3
  const f64 zmin = *std::min_element(z.begin(), z.end());
  usize at_min = 0U;
  for (usize i = 0; i < n; ++i) {
    ss += (z[i] - ez) * (z[i] - ez);
    max_dev = std::max(max_dev, std::fabs(z[i] - ez));
    below3 += (z[i] < -3.0) ? 1U : 0U;
    at_min += (z[i] == zmin) ? 1U : 0U;
  }
  EXPECT_NEAR(wz / wsum, 0.0, 1e-12);                          // cap-weighted mean 0
  EXPECT_NEAR(std::sqrt(ss / static_cast<f64>(n)), 1.0, 1e-12); // equal-weight std 1
  EXPECT_LE(max_dev, 3.0 + 1e-12);                             // winsorizing bound
  // z is non-decreasing in cap (standardization is monotone; only raw-winsorized
  // names tie, at the bound).
  std::vector<usize> order(n);
  for (usize i = 0; i < n; ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](usize a, usize b) { return cap[a] < cap[b]; });
  usize ties = 0U;
  for (usize k = 1U; k < n; ++k) {
    EXPECT_LE(z[order[k - 1U]], z[order[k]]) << "k=" << k;
    ties += (z[order[k - 1U]] == z[order[k]]) ? 1U : 0U;
  }
  EXPECT_GT(below3, n / 20U); // the old clip would have flattened > 5% of the universe
  EXPECT_LE(at_min, n / 100U); // now only the raw-winsorized extreme(s) share the minimum
  std::printf("[W0-R0 evidence] Size z (ln-cap sd 2, n=%zu): min z=%.3f, names below -3 "
              "(pinned by a post-centring clip)=%zu, names at the minimum=%zu, adjacent ties=%zu\n",
              n, zmin, below3, at_min, ties);
}

// ===========================================================================
//  R-03 at the MODEL level (fix pass 1, reviewer minor 1): perturb the data at an
//  INTERIOR row s0 of the window and compare the pass-B factor-return series.
//  DGP: r_s = f_s · z_{s+1} EXACTLY, where z_{s+1} is the Liquidity z-score at row
//  s+1. Dollar volume is set directly (volume = dollar / close), so ln adv20 does not
//  depend on the closes and z can be computed before the closes exist. Every date is
//  then an exact fit under LaggedV2, so its WLS solution does not depend on the
//  cross-date pass-A weights d0, which the perturbation DOES move.
//  Perturb every close at rows 0..s0−1 and every volume at rows 0..s0. Returns r_s for
//  s ≥ s0 are unchanged, and so is every LaggedV2 regressor row s+1 ≥ s0+1, so the
//  factor return at EVERY s ≥ s0 must be unchanged (to rounding) and equal the planted
//  f_s. ContemporaneousV1 (pre-W0) regresses r_s0 on row s0, which holds the perturbed
//  volume, so its f_s0 moves — this test fails on the pre-W0 estimator.
// ===========================================================================
TEST(RiskFactorModelPit, PassBSeriesInvariantToInteriorPerturbation) {
  const usize n_inst = 16U;
  const usize window = 40U;
  const usize rows = window + 1U + 20U + 1U; // adv20 lookback at row `window` + slack
  const usize s0 = 12U;
  FactorModelConfig cfg;
  cfg.style_mask = bit(StyleFactor::Liquidity);
  cfg.sector_factors = false;
  const PitSideInputs side{};
  Rng rng{9001U};
  Grid dollar(rows, std::vector<f64>(n_inst));
  for (auto &row : dollar) {
    for (f64 &v : row) {
      v = 1.0e6 * std::exp(0.8 * rng.normal());
    }
  }
  // z[q] = the Liquidity z-scores at row q from the dollar volumes alone (close ≡ 100).
  Grid unit_vol(rows, std::vector<f64>(n_inst));
  for (usize r = 0; r < rows; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      unit_vol[r][i] = dollar[r][i] / 100.0;
    }
  }
  const StorePanel zp{Grid(rows, std::vector<f64>(n_inst, 100.0)), unit_vol};
  Grid z(window + 1U, std::vector<f64>(n_inst, 0.0));
  for (usize q = 1U; q <= window; ++q) {
    const auto xq = build_exposures(zp.view(), cfg, q, side);
    ASSERT_TRUE(xq.has_value());
    ASSERT_EQ(xq->n_instruments(), n_inst);
    ASSERT_EQ(xq->n_factors(), 1U);
    for (usize r = 0; r < n_inst; ++r) {
      z[q][xq->instrument_rows[r]] = xq->x(static_cast<Eigen::Index>(r), 0);
    }
  }
  std::vector<f64> planted(window);
  Grid ret(rows - 1U, std::vector<f64>(n_inst));
  for (usize s = 0; s < rows - 1U; ++s) {
    const f64 fs = 0.01 * rng.normal();
    for (usize i = 0; i < n_inst; ++i) {
      ret[s][i] = (s < window) ? fs * z[s + 1U][i] : 0.01 * rng.normal();
    }
    if (s < window) {
      planted[s] = fs;
    }
  }
  const Grid close = closes_from_returns(ret);
  Grid volume(rows, std::vector<f64>(n_inst));
  for (usize r = 0; r < rows; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      volume[r][i] = dollar[r][i] / close[r][i];
    }
  }
  Grid close_p = close;
  Grid volume_p = volume;
  Rng rp{9002U};
  for (usize r = 0; r <= s0; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      if (r < s0) {
        close_p[r][i] *= std::exp(0.2 * rp.normal());
      }
      volume_p[r][i] *= std::exp(0.6 * rp.normal());
    }
  }
  // Pass-B factor return by estimation date s (NaN = the date was not usable).
  const auto series = [&](const Grid &c, const Grid &v, ExposureTiming timing) {
    FactorModelConfig k = cfg;
    k.exposure_timing = timing;
    const StorePanel sp{c, v};
    const auto fr = FactorModelBuilder{k}.factor_returns(sp.view(), window, side);
    std::vector<f64> by_date(window, atx_test_w0_r0_fixture::kNaN);
    EXPECT_TRUE(fr.has_value());
    if (fr.has_value()) {
      for (usize u = 0; u < fr->dates.size(); ++u) {
        by_date[fr->dates[u]] = fr->f(static_cast<Eigen::Index>(u), 0);
      }
    }
    return by_date;
  };
  const std::vector<f64> a2 = series(close, volume, ExposureTiming::LaggedV2);
  const std::vector<f64> b2 = series(close_p, volume_p, ExposureTiming::LaggedV2);
  const std::vector<f64> a1 = series(close, volume, ExposureTiming::ContemporaneousV1);
  const std::vector<f64> b1 = series(close_p, volume_p, ExposureTiming::ContemporaneousV1);

  f64 max_recover = 0.0; // LaggedV2 recovers the planted series on the base panel
  f64 max_move_old = 0.0; // LaggedV2 change at dates s >= s0 (must be ~0)
  for (usize s = 0; s < window; ++s) {
    ASSERT_FALSE(std::isnan(a2[s]) || std::isnan(b2[s])) << "s=" << s;
    max_recover = std::max(max_recover, std::fabs(a2[s] - planted[s]));
    if (s >= s0) {
      max_move_old = std::max(max_move_old, std::fabs(b2[s] - a2[s]));
    }
  }
  const f64 lag_move_new = std::fabs(b2[s0 - 1U] - a2[s0 - 1U]); // r_{s0-1} itself changed
  ASSERT_FALSE(std::isnan(a1[s0]) || std::isnan(b1[s0]));
  const f64 v1_move = std::fabs(b1[s0] - a1[s0]);
  EXPECT_LT(max_recover, 1e-10);
  EXPECT_LT(max_move_old, 1e-12); // R-03: nothing at/before s0's realization moves
  EXPECT_GT(lag_move_new, 1e-6);  // sanity: the perturbation is live in the window
  EXPECT_GT(v1_move, 1e-6);       // pre-W0 timing leaks the perturbed volume into f_s0
  std::printf("[W0-R0 evidence] interior perturbation (s0=%zu): LaggedV2 max|df| over s>=s0 "
              "= %.3e, |f - planted| max = %.3e, |df(s0-1)| = %.3e; ContemporaneousV1 "
              "|df(s0)| = %.3e\n",
              s0, max_move_old, max_recover, lag_move_new, v1_move);
}

} // namespace atx_test_w0_r0_pit
