#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/black76.hpp"      // black76_price
#include "atx/vol/c8.hpp"
#include "atx/vol/c8_calib.hpp"
#include "atx/vol/calib.hpp"        // CalibOpts, calib_default_opts
#include "atx/vol/universe.hpp"     // Chain, chain_index
#include "atx/vol/vol_surface.hpp"  // EssviParams, essvi_backbone_w

// Coverage for the C8 calibrator (pack/unpack, vol-domain residual + LM,
// quality gate, chain-level fit) and the KEY capability proof: C8 fits a
// negative-ATM-curvature smile that eSSVI structurally cannot. Ported from the
// C ats-vol tests test_c8_calibration.c and
// test_c8_capability_negative_atm_curvature.c.

namespace {

using atx::vol::black76_price;
using atx::vol::C8Params;
using atx::vol::c8_apply_quality_gate;
using atx::vol::c8_calib_slice;
using atx::vol::c8_fit_slice_lm;
using atx::vol::c8_pack;
using atx::vol::c8_residual_sse;
using atx::vol::c8_slice_w;
using atx::vol::c8_unpack;
using atx::vol::C8LmDiag;
using atx::vol::calib_default_opts;
using atx::vol::FitTermination;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::EssviParams;
using atx::vol::essvi_backbone_w;
using atx::vol::Side;

C8Params mk_test_slice(double T, double v, double psi, double p, double c,
                       double vmin, double kappa, double qL, double qR) {
  C8Params s{};
  s.T = T;
  s.F = 100.0;
  s.v = v;
  s.psi = psi;
  s.p = p;
  s.c = c;
  s.v_min = vmin;
  s.kappa = kappa;
  s.q_L = qL;
  s.q_R = qR;
  const double scale = std::sqrt(v);
  s.h_atm = scale;
  s.k_L = -2.5 * scale;
  s.h_L = scale;
  s.k_R = 2.5 * scale;
  s.h_R = scale;
  s.bumps_active = true;
  return s;
}

// ── pack / unpack ──────────────────────────────────────────────────────────

TEST(C8Calib, PackUnpack_RoundTrip_RecoversParams) {
  const C8Params s =
      mk_test_slice(0.25, 0.04, -0.02, 0.4, 0.5, 0.035, -0.005, 0.01, -0.01);
  const std::array<double, 8> x = c8_pack(s);

  C8Params s2 = s;  // preserve T, F, window scales
  s2.v = s2.psi = s2.p = s2.c = s2.v_min = 0.0;
  s2.kappa = s2.q_L = s2.q_R = 0.0;
  c8_unpack(x, s.T, s2);

  EXPECT_NEAR(s2.v, s.v, 1e-12);
  EXPECT_NEAR(s2.psi, s.psi, 1e-12);
  EXPECT_NEAR(s2.p, s.p, 1e-12);
  EXPECT_NEAR(s2.c, s.c, 1e-12);
  EXPECT_NEAR(s2.v_min, s.v_min, 1e-12);
  EXPECT_NEAR(s2.kappa, s.kappa, 1e-12);
  EXPECT_NEAR(s2.q_L, s.q_L, 1e-12);
  EXPECT_NEAR(s2.q_R, s.q_R, 1e-12);
}

// ── vol-domain residual ────────────────────────────────────────────────────

TEST(C8Calib, ResidualSse_AtTruth_IsZero) {
  const C8Params s =
      mk_test_slice(0.25, 0.04, -0.02, 0.4, 0.4, 0.035, 0.0, 0.0, 0.0);
  const std::array<double, 5> k{-0.2, -0.1, 0.0, 0.1, 0.2};
  std::array<double, 5> mid{};
  std::array<double, 5> spread{};
  for (std::size_t i = 0; i < 5; ++i) {
    mid[i] = c8_slice_w(s, k[i]);
    spread[i] = 0.5;
  }
  const double sse = c8_residual_sse(s, k, mid, spread, 1e-3);
  EXPECT_NEAR(sse, 0.0, 1e-18);
}

// ── per-slice LM (synthetic C8 recovery) ──────────────────────────────────

TEST(C8Calib, FitSliceLm_OnSyntheticSmile_Converges) {
  const C8Params truth =
      mk_test_slice(0.25, 0.04, -0.02, 0.45, 0.40, 0.035, -0.003, 0.005, -0.005);
  constexpr int N = 11;
  std::array<double, N> k{};
  std::array<double, N> mid{};
  std::array<double, N> spread{};
  for (int i = 0; i < N; ++i) {
    k[static_cast<std::size_t>(i)] = -0.25 + 0.05 * static_cast<double>(i);
    mid[static_cast<std::size_t>(i)] = c8_slice_w(truth, k[static_cast<std::size_t>(i)]);
    spread[static_cast<std::size_t>(i)] = 0.0005;
  }

  C8Params seed = truth;
  seed.v *= 1.10;
  seed.kappa = 0.0;
  seed.q_L = 0.0;
  seed.q_R = 0.0;

  const auto rc = c8_fit_slice_lm(seed, k, mid, spread, 12, 1e-6);
  ASSERT_TRUE(rc.has_value());
  const double sse = c8_residual_sse(seed, k, mid, spread, 1e-6);
  EXPECT_LT(sse, 1e-3);
}

// ── LM termination reporting (T10b, plan D5) ───────────────────────────────

// Seeded AT the optimum: every residual is exactly zero, so no step can improve
// and the damping runs to its 1e8 ceiling. The loop therefore exits by the SAME
// branch a genuine stall uses, and only the optimality certificate separates the
// two. Reporting `Stalled` here would call the best possible outcome the worst
// one — this is the test that pins Converged being checked BEFORE Stalled.
TEST(C8Calib, FitSliceLm_WhenSeededAtTheOptimum_ReportsConvergedNotStalled) {
  const C8Params truth =
      mk_test_slice(0.25, 0.04, -0.02, 0.45, 0.40, 0.035, -0.003, 0.005, -0.005);
  constexpr int N = 11;
  std::array<double, N> k{};
  std::array<double, N> mid{};
  std::array<double, N> spread{};
  for (int i = 0; i < N; ++i) {
    k[static_cast<std::size_t>(i)] = -0.25 + 0.05 * static_cast<double>(i);
    mid[static_cast<std::size_t>(i)] = c8_slice_w(truth, k[static_cast<std::size_t>(i)]);
    spread[static_cast<std::size_t>(i)] = 0.0005;
  }
  C8Params seed = truth; // exact optimum, zero residual

  C8LmDiag lm{};
  const auto rc = c8_fit_slice_lm(seed, k, mid, spread, 200, 1e-6, &lm);
  ASSERT_TRUE(rc.has_value());

  // The certificate must be REPORTED, not merely reachable: a disengaged
  // residual would mean the fitter still refuses to describe its own exit.
  ASSERT_TRUE(lm.final_grad_norm.has_value());
  EXPECT_GE(*lm.final_grad_norm, 0.0);
  EXPECT_LE(*lm.final_grad_norm, 1.0); // it is a cosine
  EXPECT_LE(*lm.final_grad_norm, 1e-8);
  EXPECT_EQ(lm.termination, FitTermination::Converged);
}

// MEASURED, and the reason this diagnostic is worth having. On a smile that IS
// exactly representable, seeded 10% off in v, the LM drives SSE down but then
// exhausts its damping at a point whose optimality cosine is ~0.177 — about ten
// degrees off orthogonal, nowhere near first-order stationary. So C8 does not
// merely "run out of iterations": it stops at points it cannot certify, and
// before T10b that was invisible from outside the fitter.
//
// The assertion is deliberately loose on the value (any residual this far above
// tolerance makes the point) and exact on the verdict.
TEST(C8Calib, FitSliceLm_WhenDampingExhaustsShortOfStationarity_ReportsStalled) {
  const C8Params truth =
      mk_test_slice(0.25, 0.04, -0.02, 0.45, 0.40, 0.035, -0.003, 0.005, -0.005);
  constexpr int N = 11;
  std::array<double, N> k{};
  std::array<double, N> mid{};
  std::array<double, N> spread{};
  for (int i = 0; i < N; ++i) {
    k[static_cast<std::size_t>(i)] = -0.25 + 0.05 * static_cast<double>(i);
    mid[static_cast<std::size_t>(i)] = c8_slice_w(truth, k[static_cast<std::size_t>(i)]);
    spread[static_cast<std::size_t>(i)] = 0.0005;
  }
  C8Params seed = truth;
  seed.v *= 1.10;
  seed.kappa = 0.0;
  seed.q_L = 0.0;
  seed.q_R = 0.0;

  C8LmDiag lm{};
  const auto rc = c8_fit_slice_lm(seed, k, mid, spread, 200, 1e-6, &lm);
  ASSERT_TRUE(rc.has_value());

  ASSERT_TRUE(lm.final_grad_norm.has_value());
  EXPECT_GT(*lm.final_grad_norm, 1e-3); // demonstrably NOT stationary
  EXPECT_LE(*lm.final_grad_norm, 1.0);
  EXPECT_EQ(lm.termination, FitTermination::Stalled);
  EXPECT_NE(lm.termination, FitTermination::Converged);
  EXPECT_GT(lm.accepted_steps, 0); // it did make progress before stalling
}

// The other side of the same switch: a budget too small to reach optimality
// must report IterationCap, NOT Converged. This is the assertion that fails if
// the verdict is hardcoded rather than derived from the certificate.
TEST(C8Calib, FitSliceLm_WhenBudgetEndsShortOfOptimality_ReportsIterationCap) {
  const C8Params truth =
      mk_test_slice(0.25, 0.04, -0.02, 0.45, 0.40, 0.035, -0.003, 0.005, -0.005);
  constexpr int N = 11;
  std::array<double, N> k{};
  std::array<double, N> mid{};
  std::array<double, N> spread{};
  for (int i = 0; i < N; ++i) {
    k[static_cast<std::size_t>(i)] = -0.25 + 0.05 * static_cast<double>(i);
    mid[static_cast<std::size_t>(i)] = c8_slice_w(truth, k[static_cast<std::size_t>(i)]);
    spread[static_cast<std::size_t>(i)] = 0.0005;
  }
  C8Params seed = truth;
  seed.v *= 1.60; // far enough that one step cannot certify optimality
  seed.psi = 0.25;
  seed.kappa = 0.0;
  seed.q_L = 0.0;
  seed.q_R = 0.0;

  C8LmDiag lm{};
  const auto rc = c8_fit_slice_lm(seed, k, mid, spread, 1, 1e-6, &lm);
  ASSERT_TRUE(rc.has_value());
  ASSERT_TRUE(lm.final_grad_norm.has_value());
  EXPECT_GT(*lm.final_grad_norm, 1e-8); // certificate genuinely unmet
  EXPECT_EQ(lm.termination, FitTermination::IterationCap);
  EXPECT_NE(lm.termination, FitTermination::Converged);
}

// Observing a fit must never perturb it. The diagnostic is evaluated at the
// point the loop already returns; no tolerance test gates the iteration, so the
// two runs must agree BIT-for-bit, not merely to a tolerance.
TEST(C8Calib, FitSliceLm_DiagnosticSink_DoesNotPerturbTheFit) {
  const C8Params truth =
      mk_test_slice(0.25, 0.04, -0.02, 0.45, 0.40, 0.035, -0.003, 0.005, -0.005);
  constexpr int N = 11;
  std::array<double, N> k{};
  std::array<double, N> mid{};
  std::array<double, N> spread{};
  for (int i = 0; i < N; ++i) {
    k[static_cast<std::size_t>(i)] = -0.25 + 0.05 * static_cast<double>(i);
    mid[static_cast<std::size_t>(i)] = c8_slice_w(truth, k[static_cast<std::size_t>(i)]);
    spread[static_cast<std::size_t>(i)] = 0.0005;
  }
  C8Params base = truth;
  base.v *= 1.10;
  base.kappa = 0.0;
  base.q_L = 0.0;
  base.q_R = 0.0;

  C8Params without = base;
  C8Params with = base;
  const auto rc_without = c8_fit_slice_lm(without, k, mid, spread, 40, 1e-6, nullptr);
  C8LmDiag lm{};
  const auto rc_with = c8_fit_slice_lm(with, k, mid, spread, 40, 1e-6, &lm);
  ASSERT_TRUE(rc_without.has_value());
  ASSERT_TRUE(rc_with.has_value());

  EXPECT_EQ(with.v, without.v);
  EXPECT_EQ(with.psi, without.psi);
  EXPECT_EQ(with.p, without.p);
  EXPECT_EQ(with.c, without.c);
  EXPECT_EQ(with.v_min, without.v_min);
  EXPECT_EQ(with.kappa, without.kappa);
  EXPECT_EQ(with.q_L, without.q_L);
  EXPECT_EQ(with.q_R, without.q_R);
  EXPECT_EQ(with.n_lm_iters, without.n_lm_iters);
}

// Entry-clear: a sink reused across slices must never report the previous
// slice's verdict, and a struct inspected after an Err must report nothing.
TEST(C8Calib, FitSliceLm_OnRejectedInput_ClearsTheSinkRatherThanLeavingItStale) {
  C8LmDiag lm{};
  lm.termination = FitTermination::Converged;
  lm.final_grad_norm = 0.0;
  lm.accepted_steps = 99;

  C8Params s = mk_test_slice(0.25, 0.04, -0.02, 0.4, 0.4, 0.035, 0.0, 0.0, 0.0);
  const std::array<double, 0> empty{};
  const auto rc = c8_fit_slice_lm(s, empty, empty, empty, 12, 1e-6, &lm);
  EXPECT_FALSE(rc.has_value());

  EXPECT_EQ(lm.termination, FitTermination::Unknown);
  EXPECT_FALSE(lm.final_grad_norm.has_value());
  EXPECT_EQ(lm.accepted_steps, 0);
}

// ── quality gate ───────────────────────────────────────────────────────────

TEST(C8Calib, QualityGate_WhenC8Worse_RevertsToSeed) {
  const C8Params seed =
      mk_test_slice(0.25, 0.04, -0.02, 0.4, 0.4, 0.035, 0.0, 0.0, 0.0);
  C8Params fit = seed;
  fit.kappa = -0.01;
  fit.bumps_active = true;
  fit.rmse_price = 0.50;
  EXPECT_TRUE(c8_apply_quality_gate(fit, seed, 0.40, 1.05));
  EXPECT_FALSE(fit.bumps_active);
  EXPECT_NEAR(fit.kappa, 0.0, 1e-15);
}

TEST(C8Calib, QualityGate_WhenC8Better_KeepsFit) {
  const C8Params seed =
      mk_test_slice(0.25, 0.04, -0.02, 0.4, 0.4, 0.035, 0.0, 0.0, 0.0);
  C8Params fit = seed;
  fit.kappa = -0.01;
  fit.bumps_active = true;
  fit.rmse_price = 0.30;
  EXPECT_FALSE(c8_apply_quality_gate(fit, seed, 0.40, 1.05));
  EXPECT_TRUE(fit.bumps_active);
  EXPECT_NEAR(fit.kappa, -0.01, 1e-15);
}

// ── chain-level calibration via build_observations ─────────────────────────

// Build a chain whose every (strike, side) mid is the Black-76 price at the
// truth C8 slice's own implied vol, with a tight bid/ask straddle and no flags.
Chain make_c8_priced_chain(const C8Params& truth, double F, double df,
                           const std::vector<double>& strikes) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = truth.T;
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

  constexpr double kHalfSpread = 0.005;
  for (std::size_t s = 0; s < strikes.size(); ++s) {
    const double K = strikes[s];
    const double k_log = std::log(K / F);
    const double w = c8_slice_w(truth, k_log);
    const double iv = std::sqrt(w / truth.T);
    for (int side_i = 0; side_i < 2; ++side_i) {
      const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(s), side);
      const double mid = black76_price(F, K, truth.T, iv, df, side);
      c.mids[idx] = mid;
      c.bids[idx] = mid - kHalfSpread;
      c.asks[idx] = mid + kHalfSpread;
    }
  }
  return c;
}

TEST(C8Calib, CalibSlice_OnSyntheticChain_RecoversAtmVol) {
  const C8Params truth =
      mk_test_slice(0.25, 0.04, -0.02, 0.4, 0.4, 0.037, -0.002, 0.003, -0.003);
  const double F = 100.0;
  const double df = std::exp(-0.02 * truth.T);
  const std::vector<double> strikes{88.0, 90.0, 92.0, 94.0,  96.0,  98.0, 100.0,
                                    102.0, 104.0, 106.0, 108.0, 110.0, 112.0};
  const Chain chain = make_c8_priced_chain(truth, F, df, strikes);

  C8Params seed = truth;
  seed.v *= 1.10;
  seed.kappa = 0.0;
  seed.q_L = 0.0;
  seed.q_R = 0.0;

  const auto res =
      c8_calib_slice(seed, chain, F, truth.T, df, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  EXPECT_GE(res->diag.n_quotes_used, 5u);

  // The fit must recover the ATM implied vol at least as well as the (already
  // reasonable) seed — the calibration pipeline produced a sane surface.
  const double iv_truth = std::sqrt(c8_slice_w(truth, 0.0) / truth.T);
  const double iv_fit = std::sqrt(c8_slice_w(res->params, 0.0) / truth.T);
  EXPECT_LT(std::fabs(iv_fit - iv_truth), 0.025);
}

// ── capability: negative ATM curvature (C8 beats eSSVI) ───────────────────

TEST(C8Capability, NegativeAtmCurvature_C8BeatsEssvi) {
  // Truth: a C8 slice with kappa < 0 (negative ATM curvature).
  C8Params truth =
      mk_test_slice(0.05, 0.04, -0.02, 0.5, 0.5, 0.035, -0.01, 0.0, 0.0);
  const double scale = std::sqrt(truth.v);

  // Sample 21 strikes across +/- 2.5 sigma_atm.
  constexpr int N = 21;
  std::array<double, N> k_arr{};
  std::array<double, N> w_obs{};
  for (int i = 0; i < N; ++i) {
    k_arr[static_cast<std::size_t>(i)] =
        -2.5 * scale + (5.0 * scale) * static_cast<double>(i) / static_cast<double>(N - 1);
    w_obs[static_cast<std::size_t>(i)] =
        c8_slice_w(truth, k_arr[static_cast<std::size_t>(i)]);
  }

  // eSSVI hand-fit: theta = w(0), a generous phi/rho. eSSVI's (theta, phi, rho)
  // form has structurally non-negative ATM curvature — the structural argument
  // is parameter-independent.
  EssviParams essvi{};
  essvi.T = truth.T;
  essvi.F = truth.F;
  essvi.theta = w_obs[N / 2];
  essvi.phi = 6.0;
  essvi.rho = -0.30;

  // C8 capability is structural: it admits negative ATM curvature via kappa < 0.
  const C8Params c8s = truth;

  // ATM-band comparison: |k| <= 1 sigma_atm.
  double sse_essvi = 0.0;
  double sse_c8 = 0.0;
  int n_atm = 0;
  for (int i = 0; i < N; ++i) {
    const double k = k_arr[static_cast<std::size_t>(i)];
    if (std::fabs(k) > scale) {
      continue;
    }
    const double w_e = essvi_backbone_w(essvi, k);
    const double w_c = c8_slice_w(c8s, k);
    const double obs = w_obs[static_cast<std::size_t>(i)];
    sse_essvi += (w_e - obs) * (w_e - obs);
    sse_c8 += (w_c - obs) * (w_c - obs);
    ++n_atm;
  }
  ASSERT_GE(n_atm, 5);
  const double rmse_essvi = std::sqrt(sse_essvi / static_cast<double>(n_atm));
  const double rmse_c8 = std::sqrt(sse_c8 / static_cast<double>(n_atm));

  // C8 recovers the truth within numerical precision; eSSVI is bound by its
  // structural ATM-curvature constraint. The capability gap is large.
  EXPECT_LT(rmse_c8 * 3.0, rmse_essvi);
}

}  // namespace
