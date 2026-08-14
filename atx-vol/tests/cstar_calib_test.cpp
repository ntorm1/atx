#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/api/pricing/black76.hpp"      // black76_price (synthetic chain mids)
#include "fitting/cstar.hpp"
#include "fitting/cstar_calib.hpp"
#include "atx/vol/api/marketdata/universe.hpp"     // Chain, chain_index
#include "atx/vol/api/fitting/vol_surface.hpp"  // EssviParams, essvi_total_w

// CStar calibration coverage. Mirrors the C ats-vol test
// test_calibrate_cstar_lm.c: synthetic w-targets from a known-truth slice,
// perturb, drive the vol-domain block LM, assert convergence within the C's
// tolerances. Also covers the eSSVI seed and the price-domain per-slice
// calibrator end-to-end.

namespace {

using atx::vol::black76_price;
using atx::vol::CalibOpts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CStarBlock;
using atx::vol::CStarLmStatus;
using atx::vol::CStarParams;
using atx::vol::CStarTier;
using atx::vol::cstar_calibrate_slice;
using atx::vol::cstar_lm_inner_block_w;
using atx::vol::cstar_seed_from_essvi;
using atx::vol::cstar_slice_w;
using atx::vol::cstar_tier_mask;
using atx::vol::EssviParams;
using atx::vol::essvi_total_w;
using atx::vol::kCStarNModes;
using atx::vol::Side;

constexpr int kLmInnerMax = 12;  // ATS_VOL_CSTAR_LM_INNER_MAX

// Empty optional spans for the vol-domain LM (spread_w / w_obs => 1.0).
const std::span<const double> kNoSpan{};

// ── Truth slice + synthetic-target sampler (mirror the C fixtures) ──────────

CStarParams make_truth_slice() {
  CStarParams s{};
  s.T = 0.05;
  s.F = 580.0;
  s.theta = 0.0030;  // sigma_atm² · T ≈ (0.245)² · 0.05
  s.s2 = -0.05;      // mild left skew
  s.c2 = 0.30;       // mild curvature
  s.C_left = 0.30;
  s.C_right = 0.20;
  s.active_modes = cstar_tier_mask(CStarTier::C5);  // base only
  return s;
}

void sample_w_targets(const CStarParams& truth, std::vector<double>& k_log,
                      std::vector<double>& w_target, int n_obs) {
  const double sqrt_theta = std::sqrt(truth.theta);
  k_log.resize(static_cast<std::size_t>(n_obs));
  w_target.resize(static_cast<std::size_t>(n_obs));
  for (int i = 0; i < n_obs; ++i) {
    const double z = -3.0 + 6.0 * static_cast<double>(i) /
                               static_cast<double>(n_obs - 1);
    const auto ui = static_cast<std::size_t>(i);
    k_log[ui] = z * sqrt_theta;
    w_target[ui] = cstar_slice_w(truth, k_log[ui]);
  }
}

double sse_vs_target(const CStarParams& s, const std::vector<double>& k_log,
                     const std::vector<double>& w_target) {
  double sse = 0.0;
  for (std::size_t i = 0; i < k_log.size(); ++i) {
    const double r = cstar_slice_w(s, k_log[i]) - w_target[i];
    sse += r * r;
  }
  return sse;
}

// ── Vol-domain block LM recovery ────────────────────────────────────────────

TEST(CStarLm, BaseBlock_ConvergesFromPerturbedSeed) {
  const CStarParams truth = make_truth_slice();
  constexpr int kN = 21;
  std::vector<double> k_log;
  std::vector<double> w_target;
  sample_w_targets(truth, k_log, w_target, kN);

  CStarParams slice = truth;
  slice.theta *= 1.20;
  slice.s2 += 0.10;
  slice.c2 *= 0.80;
  slice.C_left *= 1.15;
  slice.C_right *= 0.85;

  const double sse_init = sse_vs_target(slice, k_log, w_target);
  ASSERT_GT(sse_init, 1.0e-6);

  double lambda = 1.0e-3;
  for (int outer = 0; outer < 2; ++outer) {
    const auto rc = cstar_lm_inner_block_w(
        slice, CStarBlock::Base, std::span<const double>{k_log},
        std::span<const double>{w_target}, kNoSpan, kNoSpan, kLmInnerMax,
        lambda);
    ASSERT_TRUE(rc.has_value());
  }

  const double sse_final = sse_vs_target(slice, k_log, w_target);
  EXPECT_LT(sse_final, 0.01 * sse_init);
  EXPECT_LT(std::fabs(slice.theta - truth.theta) / truth.theta, 0.05);
}

TEST(CStarLm, ModalBlock_RecoversKnownBetas) {
  CStarParams truth = make_truth_slice();
  truth.active_modes = cstar_tier_mask(CStarTier::C8);
  truth.beta[2] = -0.02;    // left shoulder
  truth.beta[5] = +0.03;    // ATM bump
  truth.beta[8] = -0.015;   // right shoulder

  constexpr int kN = 31;
  std::vector<double> k_log;
  std::vector<double> w_target;
  sample_w_targets(truth, k_log, w_target, kN);

  CStarParams slice = truth;  // start on truth base + zeroed modes
  slice.beta[2] = 0.0;
  slice.beta[5] = 0.0;
  slice.beta[8] = 0.0;

  double lambda = 1.0e-3;
  for (int outer = 0; outer < 3; ++outer) {
    const auto rc = cstar_lm_inner_block_w(
        slice, CStarBlock::Modal, std::span<const double>{k_log},
        std::span<const double>{w_target}, kNoSpan, kNoSpan, kLmInnerMax,
        lambda);
    ASSERT_TRUE(rc.has_value());
  }

  EXPECT_NEAR(slice.beta[2], truth.beta[2], 5.0e-3);
  EXPECT_NEAR(slice.beta[5], truth.beta[5], 5.0e-3);
  EXPECT_NEAR(slice.beta[8], truth.beta[8], 5.0e-3);
}

TEST(CStarLm, FullBlock_ConvergesWithC8Mask) {
  CStarParams truth = make_truth_slice();
  truth.active_modes = cstar_tier_mask(CStarTier::C8);
  truth.beta[2] = -0.015;
  truth.beta[5] = +0.025;
  truth.beta[8] = -0.010;

  constexpr int kN = 31;
  std::vector<double> k_log;
  std::vector<double> w_target;
  sample_w_targets(truth, k_log, w_target, kN);

  CStarParams slice = truth;
  slice.theta *= 1.10;
  slice.s2 += 0.05;
  slice.c2 *= 0.90;
  slice.beta[2] = 0.0;
  slice.beta[5] = 0.0;
  slice.beta[8] = 0.0;

  double lambda = 1.0e-3;
  for (int outer = 0; outer < 4; ++outer) {
    const auto rc = cstar_lm_inner_block_w(
        slice, CStarBlock::Full, std::span<const double>{k_log},
        std::span<const double>{w_target}, kNoSpan, kNoSpan, kLmInnerMax,
        lambda);
    ASSERT_TRUE(rc.has_value());
  }

  EXPECT_LT(sse_vs_target(slice, k_log, w_target), 1.0e-6);
}

TEST(CStarLm, NoStep_WhenDimZero) {
  CStarParams slice = make_truth_slice();
  slice.active_modes = cstar_tier_mask(CStarTier::C5);  // MODAL block => dim 0
  const CStarParams before = slice;

  constexpr int kN = 5;
  std::vector<double> k_log;
  std::vector<double> w_target;
  sample_w_targets(slice, k_log, w_target, kN);

  double lambda = 1.0e-3;
  const auto rc = cstar_lm_inner_block_w(
      slice, CStarBlock::Modal, std::span<const double>{k_log},
      std::span<const double>{w_target}, kNoSpan, kNoSpan, kLmInnerMax, lambda);
  ASSERT_TRUE(rc.has_value());
  EXPECT_EQ(*rc, CStarLmStatus::Accepted);
  EXPECT_DOUBLE_EQ(slice.theta, before.theta);
  EXPECT_DOUBLE_EQ(slice.s2, before.s2);
  EXPECT_DOUBLE_EQ(slice.c2, before.c2);
}

TEST(CStarLm, InvalidArgs_AreRejected) {
  CStarParams slice = make_truth_slice();
  std::vector<double> k_log = {0.0, 0.1};
  std::vector<double> w_target = {0.003};  // size mismatch
  double lambda = 1.0e-3;
  const auto rc = cstar_lm_inner_block_w(
      slice, CStarBlock::Base, std::span<const double>{k_log},
      std::span<const double>{w_target}, kNoSpan, kNoSpan, kLmInnerMax, lambda);
  EXPECT_FALSE(rc.has_value());
}

// ── eSSVI seed ──────────────────────────────────────────────────────────────

TEST(CStarSeed, RecoversEssviAtmAndNearWings) {
  EssviParams src{};
  src.theta = 0.04;
  src.phi = 1.0;
  src.rho = -0.3;
  src.T = 0.05;
  src.F = 100.0;

  const auto seed = cstar_seed_from_essvi(src);
  ASSERT_TRUE(seed.has_value());

  // ATM level matches theta = w_essvi(0).
  EXPECT_NEAR(cstar_slice_w(*seed, 0.0), essvi_total_w(src, 0.0), 1.0e-3);

  // Near-ATM total variance tracks eSSVI within a modest relative tolerance.
  for (const double k : {-0.10, -0.05, 0.05, 0.10}) {
    const double w_c = cstar_slice_w(*seed, k);
    const double w_e = essvi_total_w(src, k);
    EXPECT_NEAR(w_c, w_e, 0.05 * w_e);
  }

  EXPECT_EQ(seed->fit_tier, CStarTier::C16);
  EXPECT_EQ(seed->active_modes, cstar_tier_mask(CStarTier::C16));
}

TEST(CStarSeed, RejectsDegenerateEssvi) {
  EssviParams src{};
  src.theta = 0.0;  // non-positive ATM variance
  EXPECT_FALSE(cstar_seed_from_essvi(src).has_value());
}

// ── Price-domain per-slice calibration (Black-76 European) ──────────────────

Chain make_flat_vol_chain(const std::vector<double>& strikes, double F, double T,
                          double vol, double df, double half_spread) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = T;
  c.strikes = strikes;

  const std::size_t n2 = strikes.size() * 2u;
  c.bids.assign(n2, 0.0);
  c.asks.assign(n2, 0.0);
  c.mids.assign(n2, 0.0);
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.bid_sizes.assign(n2, 1);
  c.ask_sizes.assign(n2, 1);
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, 0u);

  for (std::size_t s = 0; s < strikes.size(); ++s) {
    for (int side_i = 0; side_i < 2; ++side_i) {
      const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const std::size_t idx =
          chain_index(static_cast<std::uint16_t>(s), side);
      const double mid = black76_price(F, strikes[s], T, vol, df, side);
      c.mids[idx] = mid;
      c.bids[idx] = mid - half_spread;
      c.asks[idx] = mid + half_spread;
    }
  }
  return c;
}

TEST(CStarCalibrateSlice, FitsFlatVolChainWithoutError) {
  constexpr double F = 100.0;
  constexpr double T = 0.5;
  constexpr double vol = 0.20;
  const double df = std::exp(-0.03 * T);
  const std::vector<double> strikes = {80.0, 90.0, 95.0, 100.0, 105.0, 110.0, 120.0};
  const Chain chain = make_flat_vol_chain(strikes, F, T, vol, df, 0.02);

  EssviParams src{};
  src.theta = vol * vol * T;  // ATM variance for a ~flat 20% smile
  src.phi = 1.0;
  src.rho = 0.0;
  src.T = T;
  src.F = F;

  const CalibOpts opts{};
  const auto fit = cstar_calibrate_slice(src, chain, df, opts);
  ASSERT_TRUE(fit.has_value());
  EXPECT_GT(fit->theta, 0.0);
  EXPECT_TRUE(std::isfinite(fit->rmse_price));
}

}  // namespace
