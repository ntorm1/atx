#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

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
using atx::vol::InterpMode;
using atx::vol::Parametrization;
using atx::vol::ProjExtrapPolicy;
using atx::vol::RoutePolicy;
using atx::vol::Side;
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
