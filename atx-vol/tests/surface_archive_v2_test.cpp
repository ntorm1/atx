#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/calib.hpp" // FitObs
#include "atx/vol/dense_slice.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/priced_surface_view.hpp"
#include "atx/vol/spline_curve.hpp" // SplineVolParams, fit_spline_vol_slice
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

// ATXVSA2 (v2) zero-copy suite. The economic-correctness gate: a PricedSurfaceView
// over the mapped v2 record serves BIT-IDENTICAL theo (iv / total variance /
// fair_value / greeks / delta / vega / evaluate_batch) to the ORIGINAL
// PricedSurface (which the v1 reconstruct path also reproduces bit-for-bit). Plus
// subset-map isolation, lazy-CRC validate-on-demand, big-fixture alignment, and
// clean-break cross-format rejection.

namespace {

using atx::vol::AlOpts;
using atx::vol::AmericanGreeks;
using atx::vol::AmericanMethod;
using atx::vol::ArchiveV2WriteOpts;
using atx::vol::C8Curve;
using atx::vol::C8Params;
using atx::vol::ConvexDenseCurve;
using atx::vol::ConvexSliceFit;
using atx::vol::CurveSurface;
using atx::vol::ErrorCode;
using atx::vol::EssviCurve;
using atx::vol::EssviParams;
using atx::vol::fit_spline_vol_slice;
using atx::vol::FitObs;
using atx::vol::FitQualityMode;
using atx::vol::LinearVarianceCurve;
using atx::vol::SplineVolCurve;
using atx::vol::SplineVolParams;
using atx::vol::svi_total_w;
using atx::vol::PricedSurface;
using atx::vol::PricedSurfaceView;
using atx::vol::PricingContext;
using atx::vol::Side;
using atx::vol::SliceContext;
using atx::vol::SurfaceArchive;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveV2;
using atx::vol::SurfaceProvenance;
using atx::vol::SurfacePurpose;
using atx::vol::SurfaceState;
using atx::vol::SviCurve;
using atx::vol::SviParams;
using atx::vol::ValidationFailure;
using atx::vol::VolCurveKind;
using atx::vol::write_surface_archive;
using atx::vol::write_surface_archive_v2;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] bool greeks_bits_equal(const AmericanGreeks &a, const AmericanGreeks &b) noexcept {
  return bits_equal(a.price, b.price) && bits_equal(a.delta, b.delta) &&
         bits_equal(a.gamma, b.gamma) && bits_equal(a.vega, b.vega) && bits_equal(a.theta, b.theta) &&
         bits_equal(a.rho, b.rho) && bits_equal(a.vanna, b.vanna) && bits_equal(a.volga, b.volga) &&
         bits_equal(a.charm, b.charm);
}

constexpr double kS = 100.0;
constexpr double kR = 0.043;

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid) {
  PricingContext pc;
  pc.S = kS;
  pc.r = kR;
  pc.now_ts_ns = 1700000000000000000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = AlOpts{};
  pc.uid = uid;
  return pc;
}

[[nodiscard]] SurfaceProvenance make_provenance() {
  SurfaceProvenance p;
  p.purpose = SurfacePurpose::Risk;
  p.quality_mode = FitQualityMode::Balanced;
  p.state = SurfaceState::Healthy;
  p.validation.failures = ValidationFailure::None;
  p.validation.validation_id = 0xABCDEF12u;
  p.source_generation = 7;
  p.served_generation = 9;
  return p;
}

[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = kS;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_svi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    SviParams v{};
    v.a = 0.02 + 0.001 * static_cast<double>(i);
    v.b = 0.10;
    v.rho = -0.3;
    v.m = 0.0;
    v.sigma = 0.15;
    v.T = T;
    v.F = kS;
    v.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<SviCurve>(v, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 180, 4});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_convex(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    const double df = std::exp(-kR * T);
    const double sigma = 0.20 + 0.01 * static_cast<double>(i);
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = F;
    fit.df = df;
    fit.rmse_price = 0.25;
    fit.n_obs = static_cast<std::size_t>(nodes);
    fit.n_active = 3;
    fit.u.resize(static_cast<std::size_t>(nodes));
    fit.C.resize(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double K = F * (0.7 + 0.6 * static_cast<double>(j) / static_cast<double>(nodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      fit.C[static_cast<std::size_t>(j)] = atx::vol::black76_price(F, K, T, sigma, df, Side::Call);
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, static_cast<std::size_t>(nodes), 2});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_linear(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    std::vector<double> k(static_cast<std::size_t>(nodes));
    std::vector<double> w(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double x = -0.4 + 0.8 * static_cast<double>(j) / static_cast<double>(nodes - 1);
      k[static_cast<std::size_t>(j)] = x;
      w[static_cast<std::size_t>(j)] = (0.20 * 0.20 + 0.01 * x + 0.02 * x * x) * T;
    }
    cs.push(std::make_unique<LinearVarianceCurve>(T, kS, std::exp(-kR * T), std::move(k),
                                                  std::move(w)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, static_cast<std::size_t>(nodes), 2});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_c8(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    C8Params c8{};
    c8.T = T;
    c8.F = kS;
    c8.v = (0.22 * 0.22) * T;
    c8.v_min = 0.92 * c8.v;
    c8.psi = -0.01 * T;
    c8.kappa = -0.001 * T;
    c8.q_L = 0.0002 * T;
    c8.q_R = -0.0001 * T;
    c8.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<C8Curve>(c8, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 120, 5});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// SVI-generated observations for the SplineVol fitter (mirrors the existing v1
// surface_archive_test.cpp fixture).
[[nodiscard]] std::vector<FitObs> svi_smile_obs(const SviParams &p, double T, int n,
                                                double k_half_width) {
  std::vector<FitObs> obs(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = (n > 1) ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.5;
    const double k = -k_half_width + t * (2.0 * k_half_width);
    const double w = svi_total_w(p, k);
    FitObs o;
    o.k = k;
    o.sigma_mkt = std::sqrt(w / T);
    o.weight_w = 1.0;
    obs[static_cast<std::size_t>(i)] = o;
  }
  return obs;
}

// SplineVol priced surface. C1 regression fixture: each fitted slice's params are
// overridden with a LOW mult_cap (clamps the fitted wing multiples at the queried
// wings) and a NONZERO w_offset (the calendar-cone additive lift) — both are live
// eval-time terms of SplineVolCurve::w(). A view that drops either field
// misprices, so this is the gate that C1's fix must turn green.
[[nodiscard]] PricedSurface make_spline(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    const double df = std::exp(-kR * T);
    SviParams svi{};
    svi.a = 0.02 + 0.001 * static_cast<double>(i);
    svi.b = 0.4;
    svi.rho = -0.3;
    svi.m = 0.0;
    svi.sigma = 0.4;
    const std::vector<FitObs> obs = svi_smile_obs(svi, T, 25, 0.6);
    auto fitted = fit_spline_vol_slice(obs, F, T, df);
    EXPECT_TRUE(fitted.has_value()) << (fitted.has_value() ? "" : fitted.error().to_string());
    auto *svc = static_cast<SplineVolCurve *>(fitted->get());
    SplineVolParams p = svc->params(); // deep copy (owns z/mult vectors)
    p.mult_cap = 1.1;                  // low: clamps the fitted put/call wing multiples
    p.w_offset = 0.015 + 0.002 * static_cast<double>(i); // nonzero calendar lift
    cs.push(std::make_unique<SplineVolCurve>(std::move(p), T, F, df));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 25, 0});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// The core economic gate: a view over the v2 record prices bit-identically to the
// source PricedSurface across a (K,T,side) grid — iv, total variance, term carry,
// fair_value, greeks, greeks_analytic, delta, vega, AND evaluate_batch.
void expect_view_bit_identical(const PricedSurface &a, const PricedSurfaceView &v) {
  ASSERT_EQ(a.n_slices(), v.n_slices());
  ASSERT_EQ(a.uid(), v.uid());
  const std::array<double, 5> Ks{82.0, 100.0, 105.0, 113.0, 128.0};
  const std::array<double, 4> Ts{0.03, 0.06, 0.18, 0.40};
  for (const double T : Ts) {
    EXPECT_TRUE(bits_equal(a.forward_at(T), v.forward_at(T))) << "fwd T=" << T;
    EXPECT_TRUE(bits_equal(a.q_eff_at(T), v.q_eff_at(T))) << "qeff T=" << T;
    EXPECT_TRUE(bits_equal(a.rate_at(T), v.rate_at(T))) << "rate T=" << T;
    for (const double K : Ks) {
      EXPECT_TRUE(bits_equal(a.iv(K, T), v.iv(K, T))) << "iv K=" << K << " T=" << T;
      EXPECT_TRUE(bits_equal(a.total_variance(K, T), v.total_variance(K, T)))
          << "w K=" << K << " T=" << T;
      for (const Side side : {Side::Call, Side::Put}) {
        const auto fa = a.fair_value(K, T, side);
        const auto fv = v.fair_value(K, T, side);
        ASSERT_EQ(fa.has_value(), fv.has_value()) << "fv K=" << K << " T=" << T;
        if (fa.has_value()) {
          EXPECT_TRUE(bits_equal(*fa, *fv)) << "fv K=" << K << " T=" << T;
        }
        const auto ga = a.greeks(K, T, side);
        const auto gv = v.greeks(K, T, side);
        ASSERT_EQ(ga.has_value(), gv.has_value()) << "gr K=" << K << " T=" << T;
        if (ga.has_value()) {
          EXPECT_TRUE(greeks_bits_equal(*ga, *gv)) << "gr K=" << K << " T=" << T;
        }
        const auto da = a.delta(K, T, side);
        const auto dv = v.delta(K, T, side);
        ASSERT_EQ(da.has_value(), dv.has_value());
        if (da.has_value()) {
          EXPECT_TRUE(bits_equal(*da, *dv)) << "delta K=" << K << " T=" << T;
        }
        const auto va = a.vega(K, T, side);
        const auto vv = v.vega(K, T, side);
        ASSERT_EQ(va.has_value(), vv.has_value());
        if (va.has_value()) {
          EXPECT_TRUE(bits_equal(*va, *vv)) << "vega K=" << K << " T=" << T;
        }
      }
    }
  }
}

// evaluate_batch parity (the primary hot kernel) for a given field mask.
void expect_batch_bit_identical(const PricedSurface &a, const PricedSurfaceView &v,
                                PricedSurface::EvalField fields, bool analytic) {
  // A ladder that mixes shared-T runs (bracket reuse) and distinct T's.
  const std::vector<double> K{90, 100, 110, 90, 100, 110, 100};
  const std::vector<double> T{0.06, 0.06, 0.06, 0.20, 0.20, 0.20, 0.35};
  const std::vector<Side> side{Side::Call, Side::Put,  Side::Call, Side::Put,
                               Side::Call, Side::Call, Side::Put};
  const std::size_t n = K.size();
  std::vector<double> iv_a(n), iv_v(n), px_a(n), px_v(n);
  std::vector<AmericanGreeks> gr_a(n), gr_v(n);
  std::vector<atx::vol::Status> st_a(n), st_v(n);
  PricedSurface::EvaluationSoA out_a{iv_a, px_a, gr_a, st_a, {}, {}};
  PricedSurface::EvaluationSoA out_v{iv_v, px_v, gr_v, st_v, {}, {}};
  const auto sa = a.evaluate_batch(K, T, side, fields, analytic, out_a);
  const auto sv = v.evaluate_batch(K, T, side, fields, analytic, out_v);
  ASSERT_EQ(sa.has_value(), sv.has_value());
  // WS-P1a GOLDEN REFRESH (ANALYTIC route only): PricedSurface::evaluate_batch now
  // dispatches LANED (AVX2) analytic Greeks under Auto (P1a) for both sides (P1b), while
  // PricedSurfaceView::evaluate_batch still runs the scalar per-contract fan — the view has
  // no laned path at all. The two implementations therefore differ on the ANALYTIC route by
  // the AVX2 laned-vs-scalar delta (~1e-13/greek, sub-economic), so price/greeks are
  // re-gated there from bit-identity to the same economic tolerance the laned kernels are
  // validated against. IV (from the resolution, no pricer solve) and status stay
  // bit-identical, and the NON-analytic (FD) route — where neither side lanes — keeps the
  // full bit-identity contract that proves the archive round-trip is exact.
  // FOLLOW-UP for the PM: wiring the same laned path into PricedSurfaceView would restore
  // bit-identity here AND extend the P1 speedup to reloaded/archived surfaces.
  const auto rel_close = [](double x, double y, double rtol, double atol) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
      return std::isnan(x) == std::isnan(y);
    }
    return std::fabs(x - y) <= atol + rtol * std::fabs(y);
  };
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(st_a[i].has_value(), st_v[i].has_value()) << "batch i=" << i;
    EXPECT_TRUE(bits_equal(iv_a[i], iv_v[i])) << "batch iv i=" << i;
    if (!analytic) {
      EXPECT_TRUE(bits_equal(px_a[i], px_v[i])) << "batch px i=" << i;
      EXPECT_TRUE(greeks_bits_equal(gr_a[i], gr_v[i])) << "batch gr i=" << i;
      continue;
    }
    EXPECT_TRUE(rel_close(px_a[i], px_v[i], 1e-6, 1e-8)) << "batch px i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].price, gr_v[i].price, 1e-6, 1e-8)) << "batch gr.price i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].delta, gr_v[i].delta, 1e-5, 1e-7)) << "batch gr.delta i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].gamma, gr_v[i].gamma, 1e-3, 1e-7)) << "batch gr.gamma i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].vega, gr_v[i].vega, 1e-5, 1e-7)) << "batch gr.vega i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].theta, gr_v[i].theta, 1e-4, 1e-6)) << "batch gr.theta i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].rho, gr_v[i].rho, 1e-4, 1e-6)) << "batch gr.rho i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].vanna, gr_v[i].vanna, 1e-3, 1e-6)) << "batch gr.vanna i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].volga, gr_v[i].volga, 1e-2, 1e-5)) << "batch gr.volga i=" << i;
    EXPECT_TRUE(rel_close(gr_a[i].charm, gr_v[i].charm, 1e-3, 1e-6)) << "batch gr.charm i=" << i;
  }
}

[[nodiscard]] std::vector<std::byte> build_v2(const PricedSurface &ps, std::string_view symbol,
                                              std::optional<SurfaceProvenance> prov = std::nullopt) {
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{symbol, &ps, prov}};
  auto built = write_surface_archive_v2(items);
  EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
  return built.has_value() ? std::move(*built) : std::vector<std::byte>{};
}

} // namespace

// ── Round-trip: view theo bit-identical to source, every dispatch path ────────

TEST(SurfaceArchiveV2, ViewBitIdentical_Essvi) {
  const PricedSurface orig = make_essvi(42, 5);
  auto arch = SurfaceArchiveV2::open(build_v2(orig, "spy"));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto v = arch->map_symbol("SPY"); // case-insensitive
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_EQ(v->kind_at(0), VolCurveKind::Essvi);
  expect_view_bit_identical(orig, *v);
}

TEST(SurfaceArchiveV2, ViewBitIdentical_Svi) {
  const PricedSurface orig = make_svi(7, 4);
  auto arch = SurfaceArchiveV2::open(build_v2(orig, "aaa"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("aaa");
  ASSERT_TRUE(v.has_value());
  expect_view_bit_identical(orig, *v);
}

TEST(SurfaceArchiveV2, ViewBitIdentical_ConvexDense) {
  const PricedSurface orig = make_convex(11, 5, 21);
  auto arch = SurfaceArchiveV2::open(build_v2(orig, "idx"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("idx");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->kind_at(0), VolCurveKind::ConvexDense);
  expect_view_bit_identical(orig, *v);
}

TEST(SurfaceArchiveV2, ViewBitIdentical_Linear) {
  const PricedSurface orig = make_linear(3, 4, 11);
  auto arch = SurfaceArchiveV2::open(build_v2(orig, "lin"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("lin");
  ASSERT_TRUE(v.has_value());
  expect_view_bit_identical(orig, *v);
}

TEST(SurfaceArchiveV2, ViewBitIdentical_C8) {
  const PricedSurface orig = make_c8(5, 4);
  auto arch = SurfaceArchiveV2::open(build_v2(orig, "c8x"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("c8x");
  ASSERT_TRUE(v.has_value());
  expect_view_bit_identical(orig, *v);
}

// C1 gate: SplineVol (the 6th kind) with a clamping mult_cap and a nonzero
// w_offset must round-trip bit-exact. This is RED unless both fields are
// serialized in the v2 payload and rebuilt by the view.
TEST(SurfaceArchiveV2, ViewBitIdentical_SplineVol) {
  const PricedSurface orig = make_spline(23, 5);
  auto arch = SurfaceArchiveV2::open(build_v2(orig, "spl"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("spl");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->kind_at(0), VolCurveKind::SplineVol);
  expect_view_bit_identical(orig, *v);
}

// ── evaluate_batch parity (the primary hot kernel) ────────────────────────────

TEST(SurfaceArchiveV2, EvaluateBatchBitIdentical) {
  using EF = PricedSurface::EvalField;
  const PricedSurface convex = make_convex(11, 5, 21);
  auto arch = SurfaceArchiveV2::open(build_v2(convex, "idx"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("idx");
  ASSERT_TRUE(v.has_value());
  // Price-only ladder (exercises the resolved-batch fast path) + full greeks.
  expect_batch_bit_identical(convex, *v, EF::Iv | EF::Price, false);
  expect_batch_bit_identical(convex, *v, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder,
                             false);
  expect_batch_bit_identical(convex, *v, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder,
                             true);

  const PricedSurface essvi = make_essvi(1, 5);
  auto arch2 = SurfaceArchiveV2::open(build_v2(essvi, "e"));
  ASSERT_TRUE(arch2.has_value());
  auto v2 = arch2->map_symbol("e");
  ASSERT_TRUE(v2.has_value());
  expect_batch_bit_identical(essvi, *v2, EF::Iv | EF::Price, false);
  expect_batch_bit_identical(essvi, *v2, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder,
                             false);

  // SplineVol batch (heavy materialize path with mult_cap clamp + w_offset).
  const PricedSurface spline = make_spline(24, 5);
  auto arch3 = SurfaceArchiveV2::open(build_v2(spline, "s"));
  ASSERT_TRUE(arch3.has_value());
  auto v3 = arch3->map_symbol("s");
  ASSERT_TRUE(v3.has_value());
  expect_batch_bit_identical(spline, *v3, EF::Iv | EF::Price, false);
  expect_batch_bit_identical(spline, *v3, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder,
                             false);
}

// A big SplineVol fixture (its own record with many slices) — mirrors the
// ConvexDense big-fixture alignment exercise for the other heavy kind.
TEST(SurfaceArchiveV2, BigSplineFixtureParity) {
  const PricedSurface big = make_spline(25, 16);
  auto arch = SurfaceArchiveV2::open(build_v2(big, "bigspl"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("bigspl");
  ASSERT_TRUE(v.has_value());
  ASSERT_EQ(v->n_slices(), 16u);
  expect_view_bit_identical(big, *v);
}

// ── Big-fixture alignment (S1 trap §11.3: reads must respect alignment) ───────

TEST(SurfaceArchiveV2, BigFixtureAlignmentAndParity) {
  // A wide ConvexDense surface: 24 slices × 61 nodes -> a large record whose f64
  // columns and node arrays must stay naturally 8-B aligned in the mapping.
  const PricedSurface big = make_convex(9, 24, 61);
  auto arch = SurfaceArchiveV2::open(build_v2(big, "big"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("big");
  ASSERT_TRUE(v.has_value());
  ASSERT_EQ(v->n_slices(), 24u);
  expect_view_bit_identical(big, *v);
}

// ── Multi-symbol map_all + provenance ─────────────────────────────────────────

namespace {
// A mixed 6-kind board so map_all / subset-isolation cover EVERY dispatch path,
// incl. both heavy (eager-materialize) kinds ConvexDense + SplineVol.
[[nodiscard]] std::vector<std::byte> build_multi() {
  static const PricedSurface s_spy = make_convex(100, 6, 25);
  static const PricedSurface s_aaa = make_essvi(101, 5);
  static const PricedSurface s_bbb = make_c8(102, 4);
  static const PricedSurface s_ccc = make_linear(103, 4, 13);
  static const PricedSurface s_ddd = make_svi(104, 5);
  static const PricedSurface s_eee = make_spline(105, 5);
  const SurfaceProvenance prov = make_provenance();
  const std::array<SurfaceArchiveItem, 6> items{
      SurfaceArchiveItem{"SPY", &s_spy, prov}, SurfaceArchiveItem{"AAA", &s_aaa, prov},
      SurfaceArchiveItem{"BBB", &s_bbb, prov}, SurfaceArchiveItem{"CCC", &s_ccc, prov},
      SurfaceArchiveItem{"DDD", &s_ddd, prov}, SurfaceArchiveItem{"EEE", &s_eee, prov}};
  auto built = write_surface_archive_v2(items);
  EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
  return built.has_value() ? std::move(*built) : std::vector<std::byte>{};
}
} // namespace

TEST(SurfaceArchiveV2, MapAllAndProvenance) {
  auto arch = SurfaceArchiveV2::open(build_multi());
  ASSERT_TRUE(arch.has_value());
  EXPECT_EQ(arch->count(), 6u);
  auto all = arch->map_all_with_provenance();
  ASSERT_TRUE(all.has_value()) << all.error().to_string();
  ASSERT_EQ(all->size(), 6u);
  const SurfaceProvenance expected = make_provenance();
  for (const auto &av : *all) {
    EXPECT_EQ(av.provenance.purpose, expected.purpose);
    EXPECT_EQ(av.provenance.state, expected.state);
    EXPECT_EQ(av.provenance.validation.validation_id, expected.validation.validation_id);
    EXPECT_EQ(av.provenance.served_generation, expected.served_generation);
    EXPECT_FALSE(av.provenance.legacy_format);
  }
  // A symbol probe resolves to a specific uid.
  auto bbb = arch->map_symbol("BBB");
  ASSERT_TRUE(bbb.has_value());
  EXPECT_EQ(bbb->uid(), 102u);
  EXPECT_EQ(arch->find("ZZZ").has_value(), false);
}

// ── Subset-map isolation: mapping one symbol must not read another's bytes ─────

TEST(SurfaceArchiveV2, SubsetMapIsolation) {
  std::vector<std::byte> bytes = build_multi();
  ASSERT_FALSE(bytes.empty());

  // Target the SplineVol record (highest-risk eager-materialize kind); learn its
  // extent from an intact copy.
  auto probe = SurfaceArchiveV2::open(std::vector<std::byte>(bytes));
  ASSERT_TRUE(probe.has_value());
  auto tgt = probe->find("EEE");
  ASSERT_TRUE(tgt.has_value());
  const std::size_t tgt_off = static_cast<std::size_t>(tgt->surface_offset);
  const std::size_t tgt_end = tgt_off + static_cast<std::size_t>(tgt->surface_size);

  // The source, priced through an INTACT archive (the parity oracle).
  const PricedSurface eee_src = make_spline(105, 5);

  // POISON every byte that is NOT part of EEE's record extent — every sibling
  // record's payload (the other five kinds), plus the inter-record gaps — with
  // 0xFF. If map_symbol(EEE) touched any of them, its prices would change or it
  // would fault.
  const std::size_t data_off = static_cast<std::size_t>(probe->header().data_offset);
  for (std::size_t i = data_off; i < bytes.size(); ++i) {
    if (i >= tgt_off && i < tgt_end) {
      continue;
    }
    bytes[i] = std::byte{0xFF};
  }

  auto arch = SurfaceArchiveV2::open(std::move(bytes)); // lazy CRC -> still opens
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto v = arch->map_symbol("EEE");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  expect_view_bit_identical(eee_src, *v); // bit-exact despite poisoned siblings

  // Lazy integrity: EEE validates; the corrupted board as a whole does not.
  EXPECT_TRUE(arch->validate_symbol("EEE").has_value());
  EXPECT_FALSE(arch->validate_all().has_value());
}

// ── Lazy CRC: validate-on-demand catches record corruption ────────────────────

TEST(SurfaceArchiveV2, ValidateDetectsRecordCorruption) {
  std::vector<std::byte> bytes = build_v2(make_essvi(1, 3), "sym");
  auto probe = SurfaceArchiveV2::open(std::vector<std::byte>(bytes));
  ASSERT_TRUE(probe.has_value());
  auto de = probe->find("sym");
  ASSERT_TRUE(de.has_value());
  // Flip a payload byte deep inside the record (past the header).
  const std::size_t off =
      static_cast<std::size_t>(de->surface_offset) + sizeof(atx::vol::ArchiveV2SurfaceHeader) + 8;
  ASSERT_LT(off, bytes.size());
  bytes[off] ^= std::byte{0xFF};

  auto arch = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_TRUE(arch.has_value()); // open does NOT verify record payloads (lazy)
  EXPECT_FALSE(arch->validate_symbol("sym").has_value()); // but validate does
}

// ── Framing CRC + clean-break cross-format rejection ──────────────────────────

TEST(SurfaceArchiveV2, RejectsHeaderCorruption) {
  std::vector<std::byte> bytes = build_v2(make_essvi(1, 3), "sym");
  bytes[16] ^= std::byte{0xFF}; // inside the header (created_ts region)
  EXPECT_FALSE(SurfaceArchiveV2::open(std::move(bytes)).has_value());
}

TEST(SurfaceArchiveV2, CleanBreakCrossFormatRejection) {
  // v1 bytes are not a v2 archive, and vice versa — no dual read (§0).
  const PricedSurface ps = make_essvi(1, 3);
  const std::array<SurfaceArchiveItem, 1> v1_items{SurfaceArchiveItem{"sym", &ps}};
  auto v1 = write_surface_archive(v1_items);
  ASSERT_TRUE(v1.has_value());
  EXPECT_FALSE(SurfaceArchiveV2::open(std::vector<std::byte>(*v1)).has_value());

  std::vector<std::byte> v2 = build_v2(ps, "sym");
  EXPECT_FALSE(SurfaceArchive::open(std::move(v2)).has_value());
}

// ── instance_id: never-reused, move transfers, distinct per view ──────────────

TEST(SurfaceArchiveV2, InstanceIdSemantics) {
  auto arch = SurfaceArchiveV2::open(build_v2(make_essvi(1, 3), "sym"));
  ASSERT_TRUE(arch.has_value());
  auto a = arch->map_symbol("sym");
  auto b = arch->map_symbol("sym");
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_NE(a->instance_id(), b->instance_id());
  const std::uint64_t id = a->instance_id();
  PricedSurfaceView moved = std::move(*a);
  EXPECT_EQ(moved.instance_id(), id); // move transfers identity
}
