#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "atx/vol/curve.hpp"
#include "atx/vol/derivatives.hpp"
#include "atx/vol/surface.hpp"

// Vol-derivatives coverage, ported from the C ats-vol Sprint-22 tests:
//   test_vol_deriv_var_strip.c    -> VarStrip (flat-vol strip recovers sigma^2)
//   test_vol_deriv_aged.c         -> AgedDispatch (variance blend + discrete corr)
//   test_vol_deriv_marquee_pnl.c  -> Marquee (vol-swap PnL identity end to end)
//   test_realized_tracker.c       -> RealizedTracker (EWMA-free close-to-close RV)
//   test_deriv_reserved_validation.c -> ReservedValidation (non-zero reserved
//                                       fields rejected)
//
// The C fixtures built an eSSVI surface via the natural->reparam helper; this
// port sets theta/phi/rho directly on the base eSSVI slice (the atx surface
// evaluator consumes those), which is numerically the same flat surface.

namespace {

using atx::vol::CurveSet;
using atx::vol::deriv_default_config;
using atx::vol::deriv_price;
using atx::vol::DerivConfig;
using atx::vol::DerivContract;
using atx::vol::DerivDiscreteCorrection;
using atx::vol::DerivFlags;
using atx::vol::DerivKind;
using atx::vol::DerivMarkingConvention;
using atx::vol::DerivQuality;
using atx::vol::ErrorCode;
using atx::vol::EssviSlice;
using atx::vol::EssviSurface;
using atx::vol::ForwardPoint;
using atx::vol::has_flag;
using atx::vol::RealizedTracker;
using atx::vol::RealizedVarianceSpec;
using atx::vol::var_swap_fair_strike;
using atx::vol::vol_swap_fair_strike;

// Flat-vol synthetic surface: two eSSVI slices with theta = sigma^2 * T and
// near-zero curvature (phi ~ 0, rho = 0) so w(k) is essentially flat in k and
// iv(0, T) == sigma exactly for any T in [T_lo, T_hi].
EssviSurface make_flat_surface(double sigma, double T_lo, double T_hi) {
  EssviSurface surf(2);
  const EssviSlice s0{sigma * sigma * T_lo, 1.0e-6, 0.0, T_lo};
  const EssviSlice s1{sigma * sigma * T_hi, 1.0e-6, 0.0, T_hi};
  EXPECT_TRUE(surf.set_slice(0, s0).has_value());
  EXPECT_TRUE(surf.set_slice(1, s1).has_value());
  return surf;
}

// Zero-rate curve set with F == spot at both forward pillars (no carry).
CurveSet make_flat_curves(double spot, double T_lo, double T_hi) {
  CurveSet cs;
  cs.spot = spot;
  const double t[] = {T_lo * 0.5, (T_lo + T_hi) * 0.5, T_hi * 2.0};
  const double r[] = {0.0, 0.0, 0.0};
  EXPECT_TRUE(cs.set_yield(t, r).has_value());
  std::vector<ForwardPoint> pts(2);
  pts[0].T = T_lo;
  pts[0].F = spot;
  pts[1].T = T_hi;
  pts[1].F = spot;
  cs.forward.set(pts);
  return cs;
}

// ── Variance strip (test_vol_deriv_var_strip.c) ──────────────────────────

TEST(VarStrip, FlatVol_StandardQuality_RecoversSigmaSquared) {
  const double spot = 100.0;
  const double sigma_atm = 0.20;
  const double T_test = 0.10;  // ~25 trading days
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 1.00);

  // Sanity: surface IV at ATM equals sigma_atm.
  EXPECT_LT(std::fabs(surf.iv(0.0, T_test) - sigma_atm), 1.0e-7);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());

  const double k_var_expected = sigma_atm * sigma_atm;
  EXPECT_LT(std::fabs(q->fair_strike_dec - k_var_expected), 5.0e-5);
  EXPECT_GT(q->fair_strike_dec, 0.0);
}

TEST(VarStrip, FlatVol_HighQuality_RecoversSigmaSquaredTighter) {
  const double sigma_atm = 0.20;
  const double T_test = 0.10;
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::High;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_LT(std::fabs(q->fair_strike_dec - sigma_atm * sigma_atm), 1.0e-5);
}

// ── E2 / AN-P1-2: adaptive var-strip wings ───────────────────────────────
//
// The quality tier fixed the log-strike span (Standard ±1.5) with no reference
// to σ√T. A σ = 60%, T = 1y name needs ±3.6 to reach 6σ√T, so the Standard
// strip integrated only the middle ~2.5σ of the distribution and reported
// K_var biased LOW — and, because a parametric eSSVI surface returns a finite
// IV at every k, the StripTruncated* flags never fired. Silently wrong.
//
// Truth for a flat-vol lognormal surface is K_var == σ² exactly.
TEST(VarStrip, HighVolLongTenor_AdaptiveWingsRecoverSigmaSquared) {
  const double spot = 100.0;
  const double sigma_atm = 0.60;
  const double T_test = 1.00; // 6σ√T = 3.6, far outside the Standard ±1.5
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 2.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 2.00);

  ASSERT_LT(std::fabs(surf.iv(0.0, T_test) - sigma_atm), 1.0e-7);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());

  const double truth = sigma_atm * sigma_atm; // 0.36
  // Gate: within 0.5 VARIANCE POINT of the closed form (1 var pt = 1e-4 in
  // decimal variance).
  EXPECT_NEAR(q->fair_strike_dec, truth, 0.5e-4)
      << "K_var=" << q->fair_strike_dec << " truth=" << truth
      << " bias=" << 1.0e4 * (q->fair_strike_dec - truth) << " var pts";

  // With the adaptive span the strip is complete, so neither wing is truncated.
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::StripTruncatedLeft));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::StripTruncatedRight));
}

// E2 / AN-P1-2 second half: truncation must be reported from SPAN COVERAGE, not
// from IV finiteness. A caller that pins a deliberately narrow span on the same
// high-vol tenor gets a biased K_var — that is its right — but it must be told.
TEST(VarStrip, PinnedNarrowSpanOnHighVolTenorFlagsBothWings) {
  const double sigma_atm = 0.60;
  const double T_test = 1.00;
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 2.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 2.00);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;
  cfg.k_min_log = -0.60; // explicit span, far inside 6σ√T = 3.6
  cfg.k_max_log = 0.60;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());

  // The IV is finite at both boundaries — the pre-E2 condition — so this can
  // only pass if truncation is decided by coverage.
  EXPECT_TRUE(std::isfinite(surf.iv(-0.60, T_test)));
  EXPECT_TRUE(std::isfinite(surf.iv(0.60, T_test)));
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::StripTruncatedLeft));
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::StripTruncatedRight));

  // And the pinned span really is biased low, which is what the flag warns of.
  EXPECT_LT(q->fair_strike_dec, sigma_atm * sigma_atm);
}

// ── Aged dispatch (test_vol_deriv_aged.c) ────────────────────────────────

// Mid-life variance swap with rv_done == K_var_future: the linear blend must
// collapse to that single number and price the fair-struck contract to PV = 0.
TEST(AgedDispatch, VarSwap_MidLifeRvEqualsFuture_BlendsToFutureStrike) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();

  const auto strip_q = var_swap_fair_strike(surf, cs, 0.10, cfg);
  ASSERT_TRUE(strip_q.has_value());
  const double k_var_future = strip_q->fair_strike_dec;
  EXPECT_GT(k_var_future, 0.0);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.10;
  c.notional = 1.0e6;
  c.strike_dec = k_var_future;  // fair -> expect PV = 0
  c.marking = DerivMarkingConvention::Otc;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 24u;
  c.rv_spec.n_obs_done = 12u;
  c.rv_spec.rv_done_dec = k_var_future;

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_LT(std::fabs(q->fair_strike_dec - k_var_future), 1.0e-12);
  EXPECT_LT(std::fabs(q->pv), 1.0e-9 * c.notional * k_var_future);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::Aged));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::FullyAged));
  // Each leg contributes K_var_future * 0.5.
  EXPECT_LT(std::fabs(q->accrued_component_dec - 0.5 * k_var_future), 1.0e-12);
  EXPECT_LT(std::fabs(q->future_component_dec - 0.5 * k_var_future), 1.0e-12);
}

// Mid-life with rv_done > K_var_future: the blend is strictly between the leg
// values and PV vs the fair-future strike is positive.
TEST(AgedDispatch, VarSwap_MidLifeHighRealized_PvPositive) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();

  const auto strip_q = var_swap_fair_strike(surf, cs, 0.10, cfg);
  ASSERT_TRUE(strip_q.has_value());
  const double k = strip_q->fair_strike_dec;

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.10;
  c.notional = 1.0e6;
  c.strike_dec = k;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 24u;
  c.rv_spec.n_obs_done = 18u;
  c.rv_spec.rv_done_dec = k * 4.0;  // heavy realized

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());

  // Closed-form blend: (18/24)*4K + (6/24)*K = 3.25 K.
  const double expected = (18.0 / 24.0) * 4.0 * k + (6.0 / 24.0) * k;
  EXPECT_LT(std::fabs(q->fair_strike_dec - expected), 1.0e-12);
  EXPECT_GT(q->pv, 0.0);
}

// DIFFUSION_1_OVER_N scales the future leg by (1 + 1/n_total) and stamps the
// diagnostic flag; FULL_MC is reserved.
TEST(AgedDispatch, DiscreteCorrection_OneOverN_ScalesFutureLeg) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.10;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 5u;  // short enough for the correction to bite
  c.rv_spec.n_obs_done = 0u;
  c.rv_spec.rv_done_dec = 0.0;

  DerivConfig cfg_off = deriv_default_config();
  cfg_off.discrete_correction_mode = DerivDiscreteCorrection::None;
  const auto q_off = deriv_price(surf, cs, c, cfg_off);
  ASSERT_TRUE(q_off.has_value());
  EXPECT_FALSE(has_flag(q_off->flags, DerivFlags::DiscreteCorrApplied));

  DerivConfig cfg_on = deriv_default_config();
  cfg_on.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;
  const auto q_on = deriv_price(surf, cs, c, cfg_on);
  ASSERT_TRUE(q_on.has_value());
  EXPECT_TRUE(has_flag(q_on->flags, DerivFlags::DiscreteCorrApplied));

  // Corrected = uncorrected * (1 + 1/5) = uncorrected * 1.2.
  const double scale = 1.0 + 1.0 / 5.0;
  EXPECT_LT(std::fabs(q_on->fair_strike_dec / q_off->fair_strike_dec - scale),
            1.0e-12);

  DerivConfig cfg_mc = deriv_default_config();
  cfg_mc.discrete_correction_mode = DerivDiscreteCorrection::FullMc;
  const auto q_mc = deriv_price(surf, cs, c, cfg_mc);
  ASSERT_FALSE(q_mc.has_value());
  EXPECT_EQ(q_mc.error().code(), ErrorCode::NotImplemented);
}

// ── Marquee PnL identity (test_vol_deriv_marquee_pnl.c) ──────────────────

// "A vol swap struck at the t0 fair vol k1 with vega v1, realizing r1 to
// expiry, pays (r1 - k1) * v1." Verifies internal consistency of the dispatch:
// t0 strike <-> expiry payoff.
TEST(Marquee, VolSwap_PnlIdentity_HoldsAtExpiry) {
  const double spot = 100.0;
  const double sigma_atm = 0.20;
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 1.00);

  const std::uint32_t n_obs_total = 25u;
  const double T_total = static_cast<double>(n_obs_total) / 252.0;
  const double v1 = 100000.0;  // vol notional ("vega")

  const DerivConfig cfg = deriv_default_config();

  DerivContract contract_t0{};
  contract_t0.kind = DerivKind::VolSwap;
  contract_t0.maturity_t = T_total;
  contract_t0.notional = v1;
  contract_t0.marking = DerivMarkingConvention::Otc;
  contract_t0.rv_spec.annualization = 252.0;
  contract_t0.rv_spec.n_obs_total = n_obs_total;
  contract_t0.rv_spec.n_obs_done = 0u;
  contract_t0.strike_dec = 0.0;  // first pass: extract K_vol from the quote

  const auto q_seed = deriv_price(surf, cs, contract_t0, cfg);
  ASSERT_TRUE(q_seed.has_value());
  const double k_vol = q_seed->fair_strike_dec;
  EXPECT_GT(k_vol, 0.05);
  EXPECT_LT(k_vol, 0.50);
  // Carr-Lee on a flat-vol surface should land near sigma_atm.
  EXPECT_LT(std::fabs(k_vol - sigma_atm), 5.0e-3);

  // Re-price with strike = K_vol -> PV ~ 0 by construction.
  contract_t0.strike_dec = k_vol;
  const auto q_t0 = deriv_price(surf, cs, contract_t0, cfg);
  ASSERT_TRUE(q_t0.has_value());
  EXPECT_LT(std::fabs(q_t0->pv), 1.0e-9 * v1 * k_vol);

  // Drive a deterministic (transcendental) spot path through the tracker.
  auto built = RealizedTracker::create(252.0, n_obs_total);
  ASSERT_TRUE(built.has_value());
  RealizedTracker tracker = std::move(*built);
  ASSERT_TRUE(tracker.observe(spot).has_value());
  for (std::uint32_t i = 0; i < n_obs_total; ++i) {
    const double r = 0.015 * std::sin(0.71 * static_cast<double>(i + 1)) +
                     0.008 * std::cos(1.13 * static_cast<double>(i + 2));
    const double s_prev = (i == 0u) ? spot : tracker.prev_spot();
    const double s_next = s_prev * std::exp(r);
    ASSERT_TRUE(tracker.observe(s_next).has_value());
  }
  const RealizedVarianceSpec rv_at_expiry = tracker.snapshot();
  EXPECT_EQ(rv_at_expiry.n_obs_done, n_obs_total);
  const double r1 = std::sqrt(rv_at_expiry.rv_done_dec);
  EXPECT_GT(r1, 0.0);

  // Price at expiry (fully aged).
  DerivContract contract_exp = contract_t0;
  contract_exp.maturity_t = 0.0;
  contract_exp.rv_spec = rv_at_expiry;
  contract_exp.strike_dec = k_vol;  // strike fixed at trade date

  const auto q_exp = deriv_price(surf, cs, contract_exp, cfg);
  ASSERT_TRUE(q_exp.has_value());

  // The marquee identity, to sub-ULP tolerance.
  const double pv_expected = v1 * (r1 - k_vol);
  const double tol = 1.0e-9 * std::fabs(v1 * std::fmax(k_vol, r1));
  EXPECT_LT(std::fabs(q_exp->pv - pv_expected), tol);

  EXPECT_TRUE(has_flag(q_exp->flags, DerivFlags::FullyAged));
  EXPECT_TRUE(has_flag(q_exp->flags, DerivFlags::Aged));
  EXPECT_LT(std::fabs(q_exp->fair_strike_dec - r1), 1.0e-12);
}

// ── Realized tracker (test_realized_tracker.c) ───────────────────────────

TEST(RealizedTracker, Create_RejectsBadInputs) {
  EXPECT_FALSE(RealizedTracker::create(0.0, 22u).has_value());
  EXPECT_FALSE(RealizedTracker::create(-1.0, 22u).has_value());
  EXPECT_FALSE(RealizedTracker::create(252.0, 0u).has_value());
  EXPECT_EQ(RealizedTracker::create(0.0, 22u).error().code(),
            ErrorCode::InvalidArgument);
}

TEST(RealizedTracker, Create_ClearsState) {
  const auto built = RealizedTracker::create(252.0, 22u);
  ASSERT_TRUE(built.has_value());
  const RealizedTracker& t = *built;
  EXPECT_FALSE(t.have_prev());
  const RealizedVarianceSpec spec = t.snapshot();
  EXPECT_EQ(spec.n_obs_done, 0u);
  EXPECT_EQ(spec.n_obs_total, 22u);
  EXPECT_LT(std::fabs(spec.sum_sq_log_returns_done), 1.0e-15);
  EXPECT_LT(std::fabs(spec.rv_done_dec), 1.0e-15);
  EXPECT_LT(std::fabs(spec.annualization - 252.0), 1.0e-15);
}

TEST(RealizedTracker, ObserveBatch_HandComputedThreeReturns) {
  // Spots [100, 101, 99, 102] -> three returns; check sum of squares and the
  // annualized decimal variance against the hand computation.
  const double spots[] = {100.0, 101.0, 99.0, 102.0};
  auto built = RealizedTracker::create(252.0, 100u);
  ASSERT_TRUE(built.has_value());
  RealizedTracker t = std::move(*built);
  ASSERT_TRUE(t.observe_batch(spots).has_value());

  const double r1 = std::log(101.0 / 100.0);
  const double r2 = std::log(99.0 / 101.0);
  const double r3 = std::log(102.0 / 99.0);
  const double sum_sq_expected = r1 * r1 + r2 * r2 + r3 * r3;
  const double rv_dec_expected = 252.0 * sum_sq_expected / 3.0;

  const RealizedVarianceSpec spec = t.snapshot();
  EXPECT_EQ(spec.n_obs_done, 3u);
  EXPECT_LT(std::fabs(spec.sum_sq_log_returns_done - sum_sq_expected), 1.0e-15);
  EXPECT_LT(std::fabs(spec.rv_done_dec - rv_dec_expected), 1.0e-13);
}

TEST(RealizedTracker, Observe_RefusesObservationsPastTotal) {
  auto built = RealizedTracker::create(252.0, 2u);
  ASSERT_TRUE(built.has_value());
  RealizedTracker t = std::move(*built);
  // Three spots -> two returns fill the contract.
  EXPECT_TRUE(t.observe(100.0).has_value());
  EXPECT_TRUE(t.observe(101.0).has_value());
  EXPECT_TRUE(t.observe(102.0).has_value());
  // Fourth spot would write a third return -> refuse.
  EXPECT_FALSE(t.observe(103.0).has_value());
  EXPECT_EQ(t.snapshot().n_obs_done, 2u);
}

TEST(RealizedTracker, Observe_RejectsNonPositiveSpot) {
  auto built = RealizedTracker::create(252.0, 10u);
  ASSERT_TRUE(built.has_value());
  RealizedTracker t = std::move(*built);
  EXPECT_FALSE(t.observe(0.0).has_value());
  EXPECT_FALSE(t.observe(-1.0).has_value());
}

// ── Reserved-field validation (test_deriv_reserved_validation.c) ─────────

TEST(ReservedValidation, VarStrip_ZeroReserved_Succeeds) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  const DerivConfig cfg = deriv_default_config();
  const auto q = var_swap_fair_strike(surf, cs, 0.30, cfg);
  EXPECT_TRUE(q.has_value());
}

TEST(ReservedValidation, VarStrip_NonzeroAbsPriceTol_ReturnsNotImplemented) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  DerivConfig cfg = deriv_default_config();
  cfg.abs_price_tol = 1.0e-6;
  const auto q = var_swap_fair_strike(surf, cs, 0.30, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}

TEST(ReservedValidation, VolStrip_NonzeroRelPriceTol_ReturnsNotImplemented) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  DerivConfig cfg = deriv_default_config();
  cfg.rel_price_tol = 1.0e-3;
  const auto q = vol_swap_fair_strike(surf, cs, 0.30, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}

TEST(ReservedValidation, DerivPrice_NonzeroFlagsRequest_ReturnsNotImplemented) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  DerivConfig cfg = deriv_default_config();
  cfg.flags_request = 1u;

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.30;
  c.strike_dec = 0.04;
  c.notional = 1.0;

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}

TEST(ReservedValidation, DerivPrice_NonzeroCapDecOnVarSwap_ReturnsNotImplemented) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  const DerivConfig cfg = deriv_default_config();

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.30;
  c.strike_dec = 0.04;
  c.notional = 1.0;
  c.cap_dec = 0.10;  // reserved

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}

}  // namespace
