#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/core/math.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/projection.hpp"
#include "atx/vol/vol_surface.hpp"

// Surface projection spine, ported from the C ats-vol test_vol_projection.c.
// The oracle IV/prices come from the already-ported VolSurface::w/iv and the
// Black-76 kernels the projection layer sits on top of.

namespace {

using atx::vol::CoordConvertRequest;
using atx::vol::CoordKind;
using atx::vol::curve_forward_T;
using atx::vol::CurveSet;
using atx::vol::DeltaConvention;
using atx::vol::EssviParams;
using atx::vol::EvalRequest;
using atx::vol::ForwardPoint;
using atx::vol::InsertedSliceHandle;
using atx::vol::InterpMode;
using atx::vol::Parametrization;
using atx::vol::ProjExtrapPolicy;
using atx::vol::RoutePolicy;
using atx::vol::Side;
using atx::vol::SviParams;
using atx::vol::TimeMode;
using atx::vol::VolSurface;

constexpr std::array<double, 4> kTs{0.10, 0.25, 0.50, 1.00};

[[nodiscard]] CurveSet make_cs() {
  CurveSet cs;
  cs.spot = 100.0;
  const std::array<double, 11> t{1.0 / 365.25, 7.0 / 365.25, 14.0 / 365.25,
                                 1.0 / 12.0,    2.0 / 12.0,   3.0 / 12.0,
                                 6.0 / 12.0,    9.0 / 12.0,   1.0,
                                 1.5,           2.0};
  const std::array<double, 11> r{0.0405, 0.0410, 0.0415, 0.0420, 0.0425, 0.0430,
                                 0.0440, 0.0450, 0.0455, 0.0460, 0.0465};
  (void)cs.set_yield(t, r);
  std::array<ForwardPoint, 4> pts{};
  for (std::size_t i = 0; i < kTs.size(); ++i) {
    pts[i].T = kTs[i];
    pts[i].F = 100.0 * std::exp(0.04 * kTs[i]);
    pts[i].q_eff = 0.0;
  }
  cs.forward.set(pts);
  return cs;
}

[[nodiscard]] VolSurface make_surface() {
  VolSurface surf = VolSurface::create(1u, Parametrization::Essvi, 4).value();
  for (std::uint16_t i = 0; i < 4; ++i) {
    EssviParams sl{};
    sl.theta = 0.04 + 0.02 * static_cast<double>(i);
    sl.phi = 1.0;
    sl.rho = -0.2;
    sl.T = kTs[i];
    sl.F = 100.0 * std::exp(0.04 * kTs[i]);
    sl.expiry_id = i;
    (void)surf.set_slice_essvi(i, sl);
  }
  return surf;
}

// A raw-SVI, 2-slice surface for ShapeBlend tests (SVI's affine-in-sqrt(w(k))
// shape makes hand-derivable multiplier curves easy to construct).
[[nodiscard]] VolSurface make_svi_surface(const SviParams& sl_lo,
                                          const SviParams& sl_hi) {
  VolSurface surf = VolSurface::create(2u, Parametrization::Svi, 2).value();
  (void)surf.set_slice_svi(0, sl_lo);
  (void)surf.set_slice_svi(1, sl_hi);
  return surf;
}

// Forward-delta, B76 convention (mirrors projection.cpp's internal
// `forward_delta`, which is not exported): calls positive, puts negative.
[[nodiscard]] double test_forward_delta(double F, double K, double tau,
                                        double sigma, Side side) {
  const double v = sigma * std::sqrt(tau);
  const double d1 = (std::log(F / K) + 0.5 * v * v) / v;
  const double n_d1 = atx::core::norm_cdf(d1);
  return (side == Side::Call) ? n_d1 : (n_d1 - 1.0);
}

// Bisection for k such that the forward-delta off an INSERTED SLICE (any
// interp mode) equals `target_delta`. Test-only: production delta inversion
// (`surface_solve_k_for_delta`) works off the raw multi-slice `VolSurface`,
// which cannot see a ShapeBlend-mode inserted slice.
[[nodiscard]] double solve_k_for_delta_on_handle(const VolSurface& surf,
                                                 const InsertedSliceHandle& h,
                                                 double F, double target_delta,
                                                 Side side) {
  auto delta_at = [&](double k) -> double {
    const double iv = atx::vol::iv_on_inserted_slice(surf, h, k);
    if (!(iv > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return test_forward_delta(F, F * std::exp(k), h.tau_vol, iv, side);
  };
  double k_lo = -2.0;
  double k_hi = 2.0;
  double d_lo = delta_at(k_lo);
  double k_mid = 0.5 * (k_lo + k_hi);
  for (int it = 0; it < 100; ++it) {
    k_mid = 0.5 * (k_lo + k_hi);
    const double d_mid = delta_at(k_mid);
    if (std::fabs(d_mid - target_delta) < 1e-10 || (k_hi - k_lo) < 1e-13) {
      break;
    }
    if ((d_mid - target_delta) * (d_lo - target_delta) < 0.0) {
      k_hi = k_mid;
    } else {
      k_lo = k_mid;
      d_lo = d_mid;
    }
  }
  return k_mid;
}

}  // namespace

// ── Defaults ─────────────────────────────────────────────────────────────

TEST(VolProjection, EvalRequestDefault_Fields_MatchShippedPolicy) {
  const EvalRequest req = atx::vol::eval_request_default();
  EXPECT_EQ(req.coord_kind, CoordKind::LogMoneyness);
  EXPECT_EQ(req.interp_mode, InterpMode::PiecewiseTotalVariance);
  EXPECT_EQ(req.extrap_policy, ProjExtrapPolicy::Forbid);
  EXPECT_EQ(req.delta_convention, DeltaConvention::Forward);
  EXPECT_EQ(req.time_mode, TimeMode::Clock);
  EXPECT_EQ(req.pricing_route_policy, RoutePolicy::B76Only);
}

TEST(VolProjection, TauVolFromClock_ClockMode_PassesThrough) {
  const auto tm = atx::vol::time_model_clock();
  auto r = atx::vol::tau_vol_from_clock(tm, 0.42);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->tau_vol, 0.42, 1e-15);
  EXPECT_EQ(r->flags, 0u);
}

// ── forward_T ────────────────────────────────────────────────────────────

TEST(CurveProjection, ForwardT_ExactPillar_NoInterpFlag) {
  CurveSet cs = make_cs();
  auto fl = curve_forward_T(cs, 0.25, ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(fl.has_value());
  EXPECT_NEAR(fl->F, 100.0 * std::exp(0.04 * 0.25), 1e-12);
  EXPECT_EQ(fl->flags & atx::vol::kFlagForwardInterp, 0u);
}

TEST(CurveProjection, ForwardT_MidPillar_InterpolatesLogForward) {
  CurveSet cs = make_cs();
  auto fl = curve_forward_T(cs, 0.375, ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(fl.has_value());
  const double f_expected = std::sqrt(100.0 * std::exp(0.04 * 0.25) *
                                      100.0 * std::exp(0.04 * 0.50));
  EXPECT_NEAR(fl->F, f_expected, 1e-10);
  EXPECT_NE(fl->flags & atx::vol::kFlagForwardInterp, 0u);
}

TEST(CurveProjection, ForwardT_ForbidBeyondLastPillar_ReturnsOutOfRange) {
  CurveSet cs = make_cs();
  auto fl = curve_forward_T(cs, 2.0, ProjExtrapPolicy::Forbid);
  ASSERT_FALSE(fl.has_value());
  EXPECT_EQ(fl.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(CurveProjection, ForwardT_ClampBeyondLastPillar_ClampsToLast) {
  CurveSet cs = make_cs();
  auto fl = curve_forward_T(cs, 2.0, ProjExtrapPolicy::ClampForReporting);
  ASSERT_TRUE(fl.has_value());
  EXPECT_NEAR(fl->F, 100.0 * std::exp(0.04 * 1.00), 1e-12);
  EXPECT_NE(fl->flags & atx::vol::kFlagExtrapolatedT, 0u);
}

// ── convert_coord / eval_ex ──────────────────────────────────────────────

TEST(VolProjection, ConvertCoord_StrikeToKAndBack_RoundTrips) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();

  CoordConvertRequest cr = atx::vol::coord_convert_request_default();
  cr.T_clock = 0.50;
  cr.x = 110.0;
  cr.from_kind = CoordKind::Strike;
  cr.to_kind = CoordKind::LogMoneyness;
  auto out = atx::vol::convert_coord(sf, cs, tm, cr);
  ASSERT_TRUE(out.has_value());

  cr.x = out->k_log;
  cr.from_kind = CoordKind::LogMoneyness;
  cr.to_kind = CoordKind::Strike;
  auto back = atx::vol::convert_coord(sf, cs, tm, cr);
  ASSERT_TRUE(back.has_value());
  EXPECT_NEAR(back->K, 110.0, 1e-10);
}

TEST(VolProjection, EvalEx_LogMoneyness_MatchesHotPathIv) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();

  EvalRequest req = atx::vol::eval_request_default();
  req.T_clock = 0.50;
  req.coord_kind = CoordKind::LogMoneyness;
  req.x = -0.10;
  req.side = Side::Put;
  auto res = atx::vol::surface_eval_ex(sf, cs, nullptr, tm, req);
  ASSERT_TRUE(res.has_value());

  EXPECT_NEAR(res->iv, sf.iv(-0.10, 0.50), 1e-15);
  EXPECT_NEAR(res->total_variance, sf.w(-0.10, 0.50), 1e-15);
  EXPECT_EQ(res->pricing_route, RoutePolicy::B76Only);
  EXPECT_NE(res->flags & atx::vol::kFlagRouteB76Only, 0u);
}

TEST(VolProjection, ConvertCoord_StandardMoneyness_UsesSpotThenForward) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();

  CoordConvertRequest cr = atx::vol::coord_convert_request_default();
  cr.T_clock = 0.50;
  cr.from_kind = CoordKind::StandardMoneyness;
  cr.to_kind = CoordKind::LogMoneyness;
  cr.x = 1.10;  // K = 1.10 * spot
  auto out = atx::vol::convert_coord(sf, cs, tm, cr);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->K, 110.0, 1e-12);
  EXPECT_NEAR(out->k_log, std::log(110.0 / out->F), 1e-12);
}

TEST(VolProjection, EvalGrid_MatchesScalarEvalEx) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();

  constexpr int kN = 5;
  std::array<EvalRequest, kN> reqs{};
  for (int i = 0; i < kN; ++i) {
    reqs[static_cast<std::size_t>(i)] = atx::vol::eval_request_default();
    reqs[static_cast<std::size_t>(i)].T_clock = 0.50;
    reqs[static_cast<std::size_t>(i)].x = -0.20 + 0.10 * static_cast<double>(i);
  }
  std::array<atx::vol::EvalResult, kN> outs{};
  auto st = atx::vol::surface_eval_grid(sf, cs, nullptr, tm, reqs, outs);
  ASSERT_TRUE(st.has_value());
  for (int i = 0; i < kN; ++i) {
    auto scalar =
        atx::vol::surface_eval_ex(sf, cs, nullptr, tm,
                                  reqs[static_cast<std::size_t>(i)]);
    ASSERT_TRUE(scalar.has_value());
    EXPECT_NEAR(outs[static_cast<std::size_t>(i)].iv, scalar->iv, 1e-15);
    EXPECT_NEAR(outs[static_cast<std::size_t>(i)].price, scalar->price, 1e-12);
  }
}

// ── Delta inversion ──────────────────────────────────────────────────────

TEST(VolProjection, SolveKForDelta_Call_RoundTripsQuoteDelta) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();
  auto solved = atx::vol::surface_solve_k_for_delta(
      sf, cs, tm, 0.50, 0.25, Side::Call, DeltaConvention::Forward,
      ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(solved.has_value());
  EXPECT_NEAR(solved->quote_delta, 0.25, 1e-7);
}

TEST(VolProjection, SolveKForDelta_Put_RoundTripsQuoteDelta) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();
  auto solved = atx::vol::surface_solve_k_for_delta(
      sf, cs, tm, 0.50, -0.25, Side::Put, DeltaConvention::Forward,
      ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(solved.has_value());
  EXPECT_NEAR(solved->quote_delta, -0.25, 1e-7);
}

TEST(VolProjection, SolveKForDelta_ReservedConvention_ReturnsNotImplemented) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();
  auto solved = atx::vol::surface_solve_k_for_delta(
      sf, cs, tm, 0.50, -0.25, Side::Put, static_cast<DeltaConvention>(1),
      ProjExtrapPolicy::Forbid);
  ASSERT_FALSE(solved.has_value());
  EXPECT_EQ(solved.error().code(), atx::vol::ErrorCode::NotImplemented);
}

TEST(VolProjection, SolveKForDelta_OutOfRange_IsNotFound) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();
  // +0.5 for a put is impossible.
  auto solved = atx::vol::surface_solve_k_for_delta(
      sf, cs, tm, 0.50, 0.5, Side::Put, DeltaConvention::Forward,
      ProjExtrapPolicy::Forbid);
  ASSERT_FALSE(solved.has_value());
  EXPECT_EQ(solved.error().code(), atx::vol::ErrorCode::NotFound);
}

// ── Inserted slice ───────────────────────────────────────────────────────

TEST(VolProjection, InsertVolSlice_BetweenPillars_RecordsParents) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();
  // T = 0.375 sits between fitted slices 1 (0.25) and 2 (0.50).
  auto h = atx::vol::surface_insert_vol_slice(
      sf, &cs, tm, 0.375, InterpMode::PiecewiseTotalVariance,
      ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->parent_lo_idx, 1u);
  EXPECT_EQ(h->parent_hi_idx, 2u);
  EXPECT_EQ(h->exact_slice_idx, -1);
  EXPECT_NE(h->flags & atx::vol::kFlagInterpolatedT, 0u);
  EXPECT_NE(h->flags & atx::vol::kFlagInsertedSlice, 0u);
  EXPECT_NEAR(h->alpha_T, 0.5, 1e-12);
}

TEST(VolProjection, InsertVolSlice_AtPillar_UsesFastPath) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();
  auto h = atx::vol::surface_insert_vol_slice(
      sf, &cs, tm, 0.50, InterpMode::PiecewiseTotalVariance,
      ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->exact_slice_idx, 2);
  EXPECT_NE(h->flags & atx::vol::kResolverNativeFastPath, 0u);
  EXPECT_EQ(h->flags & atx::vol::kFlagInterpolatedT, 0u);
}

TEST(VolProjection, InsertedSliceIvBatch_MatchesScalarHotPath) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();
  auto h = atx::vol::surface_insert_vol_slice(
      sf, &cs, tm, 0.30, InterpMode::PiecewiseTotalVariance,
      ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h.has_value());

  const std::array<double, 8> k_log{-0.30, -0.20, -0.10, -0.05,
                                    0.00,  0.05,  0.10,  0.30};
  std::array<double, 8> iv_batch{};
  auto st = atx::vol::iv_on_inserted_slice_batch(sf, *h, k_log, iv_batch);
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < k_log.size(); ++i) {
    EXPECT_NEAR(iv_batch[i], sf.iv(k_log[i], 0.30), 1e-13);
  }
}

TEST(VolProjection, InsertVolSlice_ForbidBeyondLastSlice_ReturnsOutOfRange) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();
  auto h = atx::vol::surface_insert_vol_slice(
      sf, &cs, tm, 1.50, InterpMode::PiecewiseTotalVariance,
      ProjExtrapPolicy::Forbid);
  ASSERT_FALSE(h.has_value());
  EXPECT_EQ(h.error().code(), atx::vol::ErrorCode::OutOfRange);
}

// ── ShapeBlend (FLEX-style vol-multiple) interpolation ──────────────────

TEST(VolProjection, ShapeBlendMatchesLinearWForIdenticalShapes) {
  // Two SVI slices built as a self-similar family in standardized moneyness
  // z = k / (atm*sqrt(T)): shared shape constants (A, B, rho, sigma-scale),
  // each scaled by s_x = atm*sqrt(T_x). This makes the vol-multiple curve
  // m_x(z) = sigma_x(k_x)/atm_x LITERALLY IDENTICAL between the two slices
  // (same shape, "scaled in T").
  constexpr double kAtm = 0.20;
  constexpr double kB = 0.3;
  constexpr double kS = 0.3;
  constexpr double kRho = -0.3;
  constexpr double kA = 1.0 - kB * kS;  // g(0) == 1 => w_x(0) == (atm*sqrt(Tx))^2

  constexpr double T_lo = 0.25;
  constexpr double T_hi = 0.50;
  constexpr double T_q = 0.375;  // midpoint

  auto build = [&](double T) {
    const double s = kAtm * std::sqrt(T);
    SviParams sl{};
    sl.a = kA * s * s;
    sl.b = kB * s;
    sl.rho = kRho;
    sl.m = 0.0;
    sl.sigma = kS * s;
    sl.T = T;
    return sl;
  };
  const SviParams sl_lo = build(T_lo);
  const SviParams sl_hi = build(T_hi);
  VolSurface sf = make_svi_surface(sl_lo, sl_hi);
  const auto tm = atx::vol::time_model_clock();

  auto h_shape = atx::vol::surface_insert_vol_slice(
      sf, nullptr, tm, T_q, InterpMode::ShapeBlend, ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h_shape.has_value());
  auto h_lin = atx::vol::surface_insert_vol_slice(
      sf, nullptr, tm, T_q, InterpMode::PiecewiseTotalVariance,
      ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h_lin.has_value());

  // ATM: exact match by construction (both reduce to the same expression at
  // k == 0 -- see InterpMode::ShapeBlend).
  const double iv_shape_atm = atx::vol::iv_on_inserted_slice(sf, *h_shape, 0.0);
  const double iv_lin_atm = atx::vol::iv_on_inserted_slice(sf, *h_lin, 0.0);
  EXPECT_NEAR(iv_shape_atm, iv_lin_atm, 1e-10);

  // Away from ATM: identical shapes after standardization, so ShapeBlend and
  // PiecewiseTotalVariance should be close (not bit-identical -- they
  // interpolate on different axes) across |z| <= 2.
  const double atm_q = iv_shape_atm;
  double max_abs_diff = 0.0;
  for (int i = -8; i <= 8; ++i) {
    const double z = 0.25 * static_cast<double>(i);
    const double k = z * atm_q * std::sqrt(T_q);
    const double iv_shape = atx::vol::iv_on_inserted_slice(sf, *h_shape, k);
    const double iv_lin = atx::vol::iv_on_inserted_slice(sf, *h_lin, k);
    const double diff = std::fabs(iv_shape - iv_lin);
    if (diff > max_abs_diff) max_abs_diff = diff;
  }
  EXPECT_LT(max_abs_diff, 3e-3);
}

TEST(VolProjection, ShapeBlendPreservesSkewBetweenSlices) {
  // slice lo: strong put skew. slice hi: flat (rho == 0).
  SviParams sl_lo{};
  sl_lo.a = 0.008;
  sl_lo.b = 0.04;
  sl_lo.rho = -0.7;
  sl_lo.m = 0.0;
  sl_lo.sigma = 0.05;
  sl_lo.T = 0.25;

  SviParams sl_hi{};
  sl_hi.a = 0.035;
  sl_hi.b = 0.05;
  sl_hi.rho = 0.0;
  sl_hi.m = 0.0;
  sl_hi.sigma = 0.1;
  sl_hi.T = 1.00;

  VolSurface sf = make_svi_surface(sl_lo, sl_hi);
  const auto tm = atx::vol::time_model_clock();
  constexpr double T_q = 0.50;
  constexpr double F = 100.0;

  auto h_blend = atx::vol::surface_insert_vol_slice(
      sf, nullptr, tm, T_q, InterpMode::ShapeBlend, ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h_blend.has_value());
  auto h_lo = atx::vol::surface_insert_vol_slice(
      sf, nullptr, tm, sl_lo.T, InterpMode::ShapeBlend, ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h_lo.has_value());
  ASSERT_GE(h_lo->exact_slice_idx, 0);  // single bracketing slice, weight 1.0
  auto h_hi = atx::vol::surface_insert_vol_slice(
      sf, nullptr, tm, sl_hi.T, InterpMode::ShapeBlend, ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h_hi.has_value());
  ASSERT_GE(h_hi->exact_slice_idx, 0);

  const double k_put_blend =
      solve_k_for_delta_on_handle(sf, *h_blend, F, -0.25, Side::Put);
  const double k_call_blend =
      solve_k_for_delta_on_handle(sf, *h_blend, F, 0.25, Side::Call);
  const double spread_blend =
      atx::vol::iv_on_inserted_slice(sf, *h_blend, k_put_blend) -
      atx::vol::iv_on_inserted_slice(sf, *h_blend, k_call_blend);

  const double k_put_lo = solve_k_for_delta_on_handle(sf, *h_lo, F, -0.25, Side::Put);
  const double k_call_lo = solve_k_for_delta_on_handle(sf, *h_lo, F, 0.25, Side::Call);
  const double spread_lo = atx::vol::iv_on_inserted_slice(sf, *h_lo, k_put_lo) -
                           atx::vol::iv_on_inserted_slice(sf, *h_lo, k_call_lo);

  const double k_put_hi = solve_k_for_delta_on_handle(sf, *h_hi, F, -0.25, Side::Put);
  const double k_call_hi = solve_k_for_delta_on_handle(sf, *h_hi, F, 0.25, Side::Call);
  const double spread_hi = atx::vol::iv_on_inserted_slice(sf, *h_hi, k_put_hi) -
                          atx::vol::iv_on_inserted_slice(sf, *h_hi, k_call_hi);

  // Sanity: the two parents really do differ in skew (lo >> hi).
  ASSERT_GT(spread_lo, spread_hi);

  // linear-w does not guarantee this in z-space; ShapeBlend does by
  // construction (each slice's own multiplier, at its own standardized
  // moneyness, is what gets blended).
  EXPECT_GT(spread_blend, spread_hi);
  EXPECT_LT(spread_blend, spread_lo);
}

TEST(VolProjection, ShapeBlendExactAtSliceT) {
  SviParams sl_lo{};
  sl_lo.a = 0.008;
  sl_lo.b = 0.04;
  sl_lo.rho = -0.7;
  sl_lo.m = 0.0;
  sl_lo.sigma = 0.05;
  sl_lo.T = 0.25;

  SviParams sl_hi{};
  sl_hi.a = 0.035;
  sl_hi.b = 0.05;
  sl_hi.rho = 0.0;
  sl_hi.m = 0.0;
  sl_hi.sigma = 0.1;
  sl_hi.T = 1.00;

  VolSurface sf = make_svi_surface(sl_lo, sl_hi);
  const auto tm = atx::vol::time_model_clock();

  auto h = atx::vol::surface_insert_vol_slice(
      sf, nullptr, tm, sl_lo.T, InterpMode::ShapeBlend, ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->exact_slice_idx, 0);
  // Exact-pillar hit: single slice, no blend, no calendar-safety caveat.
  EXPECT_EQ(h->flags & atx::vol::kFlagShapeBlendCalendarUnsafe, 0u);

  const std::array<double, 5> ks{-0.20, -0.05, 0.0, 0.05, 0.20};
  for (double k : ks) {
    const double iv_blend = atx::vol::iv_on_inserted_slice(sf, *h, k);
    const double iv_direct = std::sqrt(atx::vol::svi_total_w(sl_lo, k) / sl_lo.T);
    EXPECT_NEAR(iv_blend, iv_direct, 1e-13);
  }
}

TEST(VolProjection, ShapeBlendAtmIsLinearInTotalVariance) {
  SviParams sl_lo{};
  sl_lo.a = 0.008;
  sl_lo.b = 0.04;
  sl_lo.rho = -0.7;
  sl_lo.m = 0.0;
  sl_lo.sigma = 0.05;
  sl_lo.T = 0.25;

  SviParams sl_hi{};
  sl_hi.a = 0.035;
  sl_hi.b = 0.05;
  sl_hi.rho = 0.0;
  sl_hi.m = 0.0;
  sl_hi.sigma = 0.1;
  sl_hi.T = 1.00;

  VolSurface sf = make_svi_surface(sl_lo, sl_hi);
  const auto tm = atx::vol::time_model_clock();

  const double w_lo0 = atx::vol::svi_total_w(sl_lo, 0.0);
  const double w_hi0 = atx::vol::svi_total_w(sl_hi, 0.0);

  for (double T_q : {0.30, 0.50, 0.75, 0.90}) {
    auto h = atx::vol::surface_insert_vol_slice(
        sf, nullptr, tm, T_q, InterpMode::ShapeBlend, ProjExtrapPolicy::Forbid);
    ASSERT_TRUE(h.has_value());
    const double ww_hi = h->alpha_T;
    const double ww_lo = 1.0 - ww_hi;
    // SpiderRock formula: atm(Tq)^2 * Tq == wwLo*Tlo*atm_lo^2 + wwHi*Thi*atm_hi^2
    //                                     == wwLo*w_lo(0) + wwHi*w_hi(0).
    const double expected_w_atm = ww_lo * w_lo0 + ww_hi * w_hi0;

    const double w_atm = atx::vol::w_on_inserted_slice(sf, *h, 0.0);
    const double iv_atm = atx::vol::iv_on_inserted_slice(sf, *h, 0.0);
    EXPECT_NEAR(w_atm, expected_w_atm, 1e-12);
    EXPECT_NEAR(iv_atm * iv_atm * T_q, expected_w_atm, 1e-10);
  }
}

TEST(VolProjection, EvalEx_ShapeBlend_MatchesInsertedSliceHotPath) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();

  EvalRequest req = atx::vol::eval_request_default();
  req.T_clock = 0.375;  // between fitted slices 0.25 and 0.50
  req.coord_kind = CoordKind::LogMoneyness;
  req.x = -0.05;
  req.side = Side::Put;
  req.interp_mode = InterpMode::ShapeBlend;
  auto res = atx::vol::surface_eval_ex(sf, cs, nullptr, tm, req);
  ASSERT_TRUE(res.has_value());

  auto h = atx::vol::surface_insert_vol_slice(
      sf, &cs, tm, 0.375, InterpMode::ShapeBlend, ProjExtrapPolicy::Forbid);
  ASSERT_TRUE(h.has_value());
  const double expected_iv = atx::vol::iv_on_inserted_slice(sf, *h, res->k_log);
  EXPECT_NEAR(res->iv, expected_iv, 1e-13);
  EXPECT_NE(res->flags & atx::vol::kFlagShapeBlendCalendarUnsafe, 0u);
}

TEST(VolProjection, EvalEx_ReservedInterpMode_ReturnsNotImplemented) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();
  const auto tm = atx::vol::time_model_clock();

  EvalRequest req = atx::vol::eval_request_default();
  req.T_clock = 0.50;
  req.x = -0.10;
  req.interp_mode = static_cast<InterpMode>(2);
  auto res = atx::vol::surface_eval_ex(sf, cs, nullptr, tm, req);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::NotImplemented);
}

// ── project_compare ──────────────────────────────────────────────────────

TEST(VolProjection, ProjectCompare_SameSurface_ZeroDiff) {
  CurveSet cs = make_cs();
  VolSurface sf = make_surface();

  atx::vol::ProjectCompareInputs in;
  in.source_surface = &sf;
  in.source_curves = &cs;
  in.target_surface = &sf;
  in.target_curves = &cs;
  in.basis = atx::vol::ForwardBasis::Self;
  in.route_policy = RoutePolicy::B76Only;

  constexpr int kN = 4;
  std::array<atx::vol::ProjectGridRow, kN> rows{};
  const std::array<double, kN> ts{0.25, 0.30, 0.50, 0.75};
  const std::array<double, kN> ks{-0.10, 0.0, 0.05, 0.15};
  for (int i = 0; i < kN; ++i) {
    rows[static_cast<std::size_t>(i)].T_clock = ts[static_cast<std::size_t>(i)];
    rows[static_cast<std::size_t>(i)].x = ks[static_cast<std::size_t>(i)];
    rows[static_cast<std::size_t>(i)].coord_kind = CoordKind::LogMoneyness;
    rows[static_cast<std::size_t>(i)].side = Side::Call;
  }
  auto st = atx::vol::surface_project_compare(in, rows);
  ASSERT_TRUE(st.has_value());
  for (const auto& row : rows) {
    EXPECT_NEAR(row.price_diff, 0.0, 1e-12);
    EXPECT_NEAR(row.iv_diff, 0.0, 1e-15);
  }
}
