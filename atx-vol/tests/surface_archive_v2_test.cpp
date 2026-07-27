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
#include "atx/vol/detail/archive_util.hpp" // crc32c (C2 prior-salt fixture)
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

// ── Padding-controlled copies of the blitted POD slice params ───────────────
//
// `EssviParams`/`SviParams`/`C8Params` are serialized as their OBJECT
// representation, which is WIDER than their members (e.g. the 6 bytes after
// `SviParams::expiry_id`). Nothing in the fitters ever writes those pad bytes,
// so they hold whatever the producing thread's stack last left there. These
// helpers rebuild a params object from a fully dirtied object representation and
// then assign every VALUE member, so what survives is exactly the padding —
// which lets a test pin "the record is a function of the values, not of the
// pad". `pad == 0x00` is the canonical (already-normalized) representation.
[[nodiscard]] SviParams repad(const SviParams &src, unsigned char pad) noexcept {
  SviParams out;
  std::memset(&out, pad, sizeof out);
  out.a = src.a;
  out.b = src.b;
  out.rho = src.rho;
  out.m = src.m;
  out.sigma = src.sigma;
  out.T = src.T;
  out.F = src.F;
  out.expiry_ns = src.expiry_ns;
  out.expiry_id = src.expiry_id;
  return out;
}

[[nodiscard]] EssviParams repad(const EssviParams &src, unsigned char pad) noexcept {
  EssviParams out;
  std::memset(&out, pad, sizeof out);
  out.theta = src.theta;
  out.phi = src.phi;
  out.rho = src.rho;
  out.rho_R = src.rho_R;
  out.rho_scale = src.rho_scale;
  out.psi = src.psi;
  out.p = src.p;
  out.lambda = src.lambda;
  out.lambda_R = src.lambda_R;
  out.T = src.T;
  out.F = src.F;
  out.expiry_ns = src.expiry_ns;
  out.expiry_id = src.expiry_id;
  out.resid_coef = src.resid_coef;
  out.resid_scale = src.resid_scale;
  out.resid_basis_kind = src.resid_basis_kind;
  out.resid_n_basis = src.resid_n_basis;
  return out;
}

[[nodiscard]] C8Params repad(const C8Params &src, unsigned char pad) noexcept {
  C8Params out;
  std::memset(&out, pad, sizeof out);
  out.T = src.T;
  out.F = src.F;
  out.expiry_ns = src.expiry_ns;
  out.expiry_id = src.expiry_id;
  out.v = src.v;
  out.psi = src.psi;
  out.p = src.p;
  out.c = src.c;
  out.v_min = src.v_min;
  out.kappa = src.kappa;
  out.q_L = src.q_L;
  out.q_R = src.q_R;
  out.h_atm = src.h_atm;
  out.k_L = src.k_L;
  out.h_L = src.h_L;
  out.k_R = src.k_R;
  out.h_R = src.h_R;
  out.arb_damping_factor = src.arb_damping_factor;
  out.rmse_price = src.rmse_price;
  out.rmse_vol = src.rmse_vol;
  out.n_lm_iters = src.n_lm_iters;
  out.n_irls_iters = src.n_irls_iters;
  out.bumps_active = src.bumps_active;
  return out;
}

[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n, unsigned char pad = 0x00) {
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
    cs.push(std::make_unique<EssviCurve>(repad(e, pad), std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_svi(std::uint32_t uid, int n, unsigned char pad = 0x00) {
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
    cs.push(std::make_unique<SviCurve>(repad(v, pad), std::exp(-kR * T)));
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

[[nodiscard]] PricedSurface make_c8(std::uint32_t uid, int n, unsigned char pad = 0x00) {
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
    cs.push(std::make_unique<C8Curve>(repad(c8, pad), std::exp(-kR * T)));
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
  // WS-P1v GOLDEN RESTORED (relaxed by WS-P1a, re-tightened here): PricedSurface and
  // PricedSurfaceView now drive the SAME laned analytic-Greek kernel through the one
  // shared driver (src/laned_greek_run.hpp). WS-P1a had to re-gate the ANALYTIC route
  // from bit-identity to an economic tolerance for exactly one reason — the surface had
  // a laned path and the view did not, so the two ran different kernels and drifted by
  // the ~1e-13/greek AVX2-vs-scalar delta. With the seam unified there is no second
  // implementation left to drift against, so the FULL bit-identity contract is reinstated
  // on BOTH routes (analytic and FD). That is what actually proves the archive round-trip
  // is exact: a mapped view and its source surface must be the same pricer, not merely
  // two pricers that agree to a tolerance.
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(st_a[i].has_value(), st_v[i].has_value()) << "batch i=" << i;
    EXPECT_TRUE(bits_equal(iv_a[i], iv_v[i])) << "batch iv i=" << i;
    EXPECT_TRUE(bits_equal(px_a[i], px_v[i])) << "batch px i=" << i;
    EXPECT_TRUE(greeks_bits_equal(gr_a[i], gr_v[i])) << "batch gr i=" << i;
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

// ── find() must hand back the archive's REAL directory entry ──────────────────
//
// `find` is documented as "resolve `symbol` to its directory entry", and callers
// may legitimately feed the result straight into `map_entry`/`reconstruct_entry`
// or compare it against `directory()`. Anything less than the stored entry is a
// silent lie: the lookup slot carries no n_slices/kind_bits/payload_crc32c, so a
// hand-assembled entry reads back zeros for exactly the fields a framing-only
// consumer (and the F6 content-identity check) depends on.
TEST(SurfaceArchiveV2, FindReturnsTheStoredDirectoryEntry) {
  auto arch = SurfaceArchiveV2::open(build_multi()); // 6 mixed-kind surfaces
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  const std::span<const atx::vol::ArchiveV2DirEntry> dir = arch->directory();
  ASSERT_EQ(dir.size(), 6u);

  for (const atx::vol::ArchiveV2DirEntry &want : dir) {
    const std::string sym(want.symbol, want.symbol_len);
    auto got = arch->find(sym);
    ASSERT_TRUE(got.has_value()) << sym << ": " << got.error().to_string();
    // Field-wise first so a failure names what was dropped, then the whole
    // 80-byte record (no padding: 8+8+8+4+2+2+2+2+4+32+8) for completeness.
    EXPECT_EQ(got->surface_offset, want.surface_offset) << sym;
    EXPECT_EQ(got->surface_size, want.surface_size) << sym;
    EXPECT_EQ(got->symbol_hash, want.symbol_hash) << sym;
    EXPECT_EQ(got->uid, want.uid) << sym;
    EXPECT_EQ(got->n_slices, want.n_slices) << sym;
    EXPECT_EQ(got->kind_bits, want.kind_bits) << sym;
    EXPECT_EQ(got->payload_crc32c, want.payload_crc32c) << sym;
    EXPECT_EQ(got->symbol_len, want.symbol_len) << sym;
    EXPECT_EQ(0, std::memcmp(&*got, &want, sizeof(atx::vol::ArchiveV2DirEntry))) << sym;
    // The entry is usable exactly like a `directory()` one.
    EXPECT_TRUE(arch->map_entry(*got).has_value()) << sym;
  }

  // Case-insensitive probe resolves to the same stored entry, and an absent
  // symbol is still NotFound.
  auto lower = arch->find("bbb");
  ASSERT_TRUE(lower.has_value()) << lower.error().to_string();
  EXPECT_GT(lower->n_slices, 0u);
  EXPECT_EQ(lower->uid, 102u);
  auto absent = arch->find("ZZZ");
  ASSERT_FALSE(absent.has_value());
  EXPECT_EQ(absent.error().code(), ErrorCode::NotFound);
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

// ── Corpus reproducibility: content-derived created_ts_ns ─────────────────────
// The production corpus path leaves ArchiveV2WriteOpts::created_ts_ns == 0. That 0
// sentinel used to be filled from the WALL CLOCK, so two identical builds produced
// containers differing only in the header timestamp -> corpus builds were not
// byte-reproducible run-to-run. The writer now fills the sentinel from a
// deterministic CRC-32C of the archive CONTENT. This gate observes all three
// required properties at once.
TEST(SurfaceArchiveV2, ContentDerivedCreatedTsIsReproducible) {
  const PricedSurface a = make_essvi(42, 5);
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &a, std::nullopt}};

  // Default opts => created_ts_ns left at the 0 sentinel (the production path).
  auto first = write_surface_archive_v2(items);
  auto second = write_surface_archive_v2(items);
  ASSERT_TRUE(first.has_value() && second.has_value());

  // (1) Determinism: two identical builds are byte-identical containers, header
  //     timestamp included. Under the old wall-clock fill this diverged.
  EXPECT_EQ(*first, *second);

  auto arch1 = SurfaceArchiveV2::open(std::vector<std::byte>(*first));
  ASSERT_TRUE(arch1.has_value()) << arch1.error().to_string();
  const std::uint64_t ts1 = arch1->header().created_ts_ns;

  // (2) Non-vacuous: the sentinel was actually filled (not left at 0), so (1) is
  //     not passing trivially over two zero-stamped headers.
  EXPECT_NE(ts1, 0u);

  // (3) Content-sensitivity (staleness property): a DIFFERENT surface must get a
  //     DIFFERENT stamp, else two distinct builds at one path would share an
  //     ArchiveContentIdentity and SnapshotCache would serve a stale surface --
  //     exactly why a constant stamp is wrong.
  const PricedSurface b = make_essvi(42, 6); // one more slice => different content
  const std::array<SurfaceArchiveItem, 1> items_b{SurfaceArchiveItem{"SPY", &b, std::nullopt}};
  auto third = write_surface_archive_v2(items_b);
  ASSERT_TRUE(third.has_value());
  EXPECT_NE(*first, *third);
  auto arch3 = SurfaceArchiveV2::open(std::vector<std::byte>(*third));
  ASSERT_TRUE(arch3.has_value()) << arch3.error().to_string();
  EXPECT_NE(ts1, arch3->header().created_ts_ns);

  // (4) The explicit-nonzero path is UNCHANGED: a caller-supplied stamp is honored
  //     verbatim (unit tests pin created_ts_ns and must keep working).
  ArchiveV2WriteOpts pinned;
  pinned.created_ts_ns = 12345;
  auto fixed = write_surface_archive_v2(items, pinned);
  ASSERT_TRUE(fixed.has_value());
  auto archf = SurfaceArchiveV2::open(std::vector<std::byte>(*fixed));
  ASSERT_TRUE(archf.has_value()) << archf.error().to_string();
  EXPECT_EQ(archf->header().created_ts_ns, 12345u);
}

// ── C2 (SE-P1-2): AlOpts::n_quad_price persists across the v2 round-trip ───────

namespace {
[[nodiscard]] PricedSurface make_essvi_alopts(std::uint32_t uid, int n, const AlOpts &al) {
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
  PricingContext pc = make_pricing(uid);
  pc.al_opts = al;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}
} // namespace

// A surface priced under the "ql_fast" rung DECOUPLES the premium quadrature
// (n_quad_price=32) from the fixed-point order (n_quadrature=8). Before this
// change the archive dropped n_quad_price, so the round-tripped view/reconstruct
// read it back as 0 (tied) and repriced with premium order 8 — a different theo,
// violating the format's "round-trips with IDENTICAL theo values" contract
// (SE-P1-2). scheme_from_opts honors n_quad_price>=8 on the live pricing path, so
// the bit-identity check below is non-vacuous.
TEST(SurfaceArchiveV2, RoundTripsNQuadPrice) {
  AlOpts al;
  al.n_collocation = 12;
  al.n_quadrature = 8;
  al.n_quad_price = 32;
  const PricedSurface orig = make_essvi_alopts(77, 5, al);
  auto arch = SurfaceArchiveV2::open(build_v2(orig, "nqp"));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();

  auto v = arch->map_symbol("nqp");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_EQ(v->pricing().al_opts.n_quad_price, 32u); // RED: reads back 0 today
  expect_view_bit_identical(orig, *v);               // RED: view reprices tied (8)

  auto rec = arch->reconstruct_symbol("nqp");
  ASSERT_TRUE(rec.has_value()) << rec.error().to_string();
  EXPECT_EQ(rec->pricing().al_opts.n_quad_price, 32u); // owned path persists too
}

// C2 back-compat: an archive written under the PRIOR schema salt (before
// n_quad_price occupied the reserved u16) must still open under the new reader
// and reprice with the tied premium order. The reserved_u16 reuse is
// layout-invariant, so a default-AlOpts (n_quad_price==0) record is byte-identical
// to what an old writer produced; we re-stamp its header to the prior salt fold
// (recomputing only the header CRC — the two bytes a salt bump touches) to obtain
// a faithful pre-bump fixture. This guards the reader's prior-salt accept-list.
TEST(SurfaceArchiveV2, OpensPriorSaltArchiveAsTiedNQuadPrice) {
  using atx::vol::ArchiveV2DirEntry;
  using atx::vol::ArchiveV2Header;
  using atx::vol::ArchiveV2LookupSlot;
  using atx::vol::ArchiveV2SurfaceHeader;
  const PricedSurface orig = make_essvi(88, 4); // default AlOpts -> n_quad_price==0
  std::vector<std::byte> bytes = build_v2(orig, "old");

  // Prior schema salt fold (…0101) — the identical sizeof-fold, one older salt.
  constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
  constexpr std::uint64_t kV2SaltPrev = 0xA7C3'5F04'2E1F'0101ull;
  std::uint64_t prev = 0x9e3779b97f4a7c15ull ^ kV2SaltPrev;
  prev ^= static_cast<std::uint64_t>(sizeof(ArchiveV2Header)) * kFnvPrime;
  prev ^= static_cast<std::uint64_t>(sizeof(ArchiveV2LookupSlot)) * kFnvPrime;
  prev ^= static_cast<std::uint64_t>(sizeof(ArchiveV2DirEntry)) * kFnvPrime;
  prev ^= static_cast<std::uint64_t>(sizeof(ArchiveV2SurfaceHeader)) * kFnvPrime;
  prev ^= static_cast<std::uint64_t>(sizeof(EssviParams)) * kFnvPrime;
  prev ^= static_cast<std::uint64_t>(sizeof(SviParams)) * kFnvPrime;

  ArchiveV2Header h;
  std::memcpy(&h, bytes.data(), sizeof h);
  h.schema_hash = prev;
  h.header_crc32c = 0;
  std::array<std::byte, sizeof(ArchiveV2Header)> hb{};
  std::memcpy(hb.data(), &h, sizeof h);
  h.header_crc32c = atx::vol::detail::crc32c(hb.data(), hb.size());
  std::memcpy(bytes.data(), &h, sizeof h);

  auto arch = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto v = arch->map_symbol("old");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_EQ(v->pricing().al_opts.n_quad_price, 0u); // prior-salt file reads tied
  expect_view_bit_identical(orig, *v);
}

// ── C6 (SE-P2-6): entries()/entry_count() are a FRAMING-ONLY enumeration ───────
// The checkpoint/resume counter must not pay map_all()'s eager ConvexDense/
// SplineVol materialization just to count surfaces. entries()/entry_count() read
// ONLY the directory (parsed at open from the metadata section) and touch no
// surface record body. Proof of non-materialization: poison every record body,
// then entries()/entry_count() still enumerate correctly while map_all() (which
// builds a PricedSurfaceView per record) fails. WS-T (corpus.cpp) consumes these.
TEST(SurfaceArchiveV2, EntriesEnumerateWithoutMaterializingViews) {
  auto arch = SurfaceArchiveV2::open(build_multi()); // 6 mixed-kind surfaces
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();

  auto all = arch->map_all();
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(arch->entry_count(), all->size());
  EXPECT_EQ(arch->entries().size(), all->size());
  EXPECT_EQ(arch->entry_count(), static_cast<std::size_t>(arch->count()));
  for (const auto &e : arch->entries()) {
    EXPECT_GT(e.n_slices, 0u); // framing (uid/n_slices/kind_bits) with no view built
  }

  // Poison the whole data section (every record body). open stays lazy, so the
  // directory (metadata section) is intact: framing enumeration keeps working,
  // but map_all() — which materializes each record — now fails.
  std::vector<std::byte> bytes = build_multi();
  const std::size_t data_off = static_cast<std::size_t>(arch->header().data_offset);
  ASSERT_LT(data_off, bytes.size());
  for (std::size_t i = data_off; i < bytes.size(); ++i) {
    bytes[i] = std::byte{0xFF};
  }
  auto poisoned = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_TRUE(poisoned.has_value()) << poisoned.error().to_string(); // lazy CRC -> opens
  EXPECT_EQ(poisoned->entry_count(), arch->entry_count());
  EXPECT_EQ(poisoned->entries().size(), arch->entry_count());
  EXPECT_FALSE(poisoned->map_all().has_value()); // materialization hits the poison
}

// ── FIX-D gate: reconstruct -> re-emit is BYTE-lossless ──────────────────────
//
// The surface-db carry-over (populate_universe_streaming) re-emits an
// already-stored surface into a rewritten partition INSTEAD of re-fitting it. That
// is only sound if the round trip
//
//     stored record bytes -> reconstruct_entry -> write_surface_archive_v2
//
// reproduces the record byte-for-byte. `surface_archive.hpp` CLAIMS this ("the
// inverse of write_surface_archive_v2, bit-identical to the source surface") but
// nothing tested the claim in the direction the carry-over depends on, and a
// single dropped field would silently perturb stored surfaces while every summary,
// checksum and view-parity test stayed green (the record CRC is recomputed from
// whatever the re-emit produced, so it cannot catch a lossy field either).
//
// These tests compare ACTUAL BYTES: the whole file, plus each record's extent
// individually so a failure names the losing symbol. `created_ts_ns` is pinned so
// even the header is comparable (0 would mean "system clock").
namespace {

constexpr std::int64_t kPinnedCreatedTs = 1'700'000'000'123'456'789LL;

[[nodiscard]] ArchiveV2WriteOpts pinned_opts() {
  ArchiveV2WriteOpts opts;
  opts.created_ts_ns = kPinnedCreatedTs;
  return opts;
}

// Re-emit an archive using ONLY what the reader hands back — no source surface is
// in scope, exactly as in the carry-over path.
[[nodiscard]] std::vector<std::byte> reemit_via_reconstruct(const std::vector<std::byte> &bytes) {
  auto arch = SurfaceArchiveV2::open(std::vector<std::byte>(bytes));
  EXPECT_TRUE(arch.has_value());
  if (!arch.has_value()) {
    return {};
  }
  const std::span<const atx::vol::ArchiveV2DirEntry> dir = arch->directory();
  std::vector<atx::vol::ArchivedSurface> recon;
  std::vector<std::string> names;
  recon.reserve(dir.size());
  names.reserve(dir.size());
  for (const atx::vol::ArchiveV2DirEntry &e : dir) {
    auto got = arch->reconstruct_entry(e);
    EXPECT_TRUE(got.has_value()) << (got.has_value() ? "" : got.error().to_string());
    if (!got.has_value()) {
      return {};
    }
    recon.push_back(std::move(*got));
    names.emplace_back(e.symbol, e.symbol_len);
  }
  std::vector<SurfaceArchiveItem> items;
  items.reserve(recon.size());
  for (std::size_t i = 0; i < recon.size(); ++i) {
    // A record written with no provenance reads back as `legacy_format`, which the
    // writer refuses to re-emit explicitly; nullopt reproduces the same zero bytes.
    std::optional<SurfaceProvenance> prov;
    if (!recon[i].provenance.legacy_format) {
      prov = recon[i].provenance;
    }
    items.push_back(SurfaceArchiveItem{names[i], &recon[i].surface, prov});
  }
  auto rebuilt = write_surface_archive_v2(items, pinned_opts());
  EXPECT_TRUE(rebuilt.has_value()) << (rebuilt.has_value() ? "" : rebuilt.error().to_string());
  return rebuilt.has_value() ? std::move(*rebuilt) : std::vector<std::byte>{};
}

void expect_reemit_byte_identical(std::span<const SurfaceArchiveItem> items, const char *label) {
  auto first = write_surface_archive_v2(items, pinned_opts());
  ASSERT_TRUE(first.has_value()) << label << ": " << first.error().to_string();
  const std::vector<std::byte> a = std::move(*first);
  const std::vector<std::byte> b = reemit_via_reconstruct(a);
  ASSERT_FALSE(b.empty()) << label << ": re-emit produced nothing";

  // Per-record extents first: a mismatch then names the symbol that lost a field.
  auto arch_a = SurfaceArchiveV2::open(std::vector<std::byte>(a));
  ASSERT_TRUE(arch_a.has_value()) << label;
  for (const atx::vol::ArchiveV2DirEntry &e : arch_a->directory()) {
    const std::string sym(e.symbol, e.symbol_len);
    ASSERT_LE(e.surface_offset + e.surface_size, b.size()) << label << " " << sym;
    EXPECT_EQ(0, std::memcmp(a.data() + e.surface_offset, b.data() + e.surface_offset,
                             static_cast<std::size_t>(e.surface_size)))
        << label << ": record bytes differ for " << sym;
  }
  // Then the whole file (header, lookup table, directory, padding and all).
  ASSERT_EQ(a.size(), b.size()) << label << ": file size differs";
  EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size())) << label << ": file bytes differ";
}

void expect_kind_reemit_byte_identical(const PricedSurface &ps, const char *label) {
  const SurfaceProvenance prov = make_provenance();
  const std::array<SurfaceArchiveItem, 1> with_prov{SurfaceArchiveItem{"SYM", &ps, prov}};
  expect_reemit_byte_identical(with_prov, label);
  // And the legacy (marker == 0) shape, which takes the nullopt branch above.
  const std::array<SurfaceArchiveItem, 1> no_prov{SurfaceArchiveItem{"SYM", &ps, std::nullopt}};
  expect_reemit_byte_identical(no_prov, label);
}

} // namespace

TEST(SurfaceArchiveV2, ReemitByteIdentical_Essvi) {
  expect_kind_reemit_byte_identical(make_essvi(42, 5), "essvi");
}

TEST(SurfaceArchiveV2, ReemitByteIdentical_Svi) {
  expect_kind_reemit_byte_identical(make_svi(7, 4), "svi");
}

TEST(SurfaceArchiveV2, ReemitByteIdentical_C8) {
  expect_kind_reemit_byte_identical(make_c8(8, 4), "c8");
}

TEST(SurfaceArchiveV2, ReemitByteIdentical_ConvexDense) {
  expect_kind_reemit_byte_identical(make_convex(9, 24, 61), "convex_dense");
}

TEST(SurfaceArchiveV2, ReemitByteIdentical_Linear) {
  expect_kind_reemit_byte_identical(make_linear(10, 4, 13), "linear_variance");
}

TEST(SurfaceArchiveV2, ReemitByteIdentical_SplineVol) {
  expect_kind_reemit_byte_identical(make_spline(11, 16), "spline_vol");
}

// The shape the carry-over actually writes: one partition, every curve kind, mixed
// provenance — re-emitted from the reader alone.
TEST(SurfaceArchiveV2, ReemitByteIdentical_MixedBoard) {
  const PricedSurface s_spy = make_convex(100, 6, 25);
  const PricedSurface s_aaa = make_essvi(101, 5);
  const PricedSurface s_bbb = make_c8(102, 4);
  const PricedSurface s_ccc = make_linear(103, 4, 13);
  const PricedSurface s_ddd = make_svi(104, 5);
  const PricedSurface s_eee = make_spline(105, 5);
  SurfaceProvenance degraded = make_provenance();
  degraded.state = SurfaceState::Degraded;
  degraded.validation.failures = ValidationFailure::CarryGap;
  degraded.source_generation = 11;
  const SurfaceProvenance healthy = make_provenance();
  const std::array<SurfaceArchiveItem, 6> items{
      SurfaceArchiveItem{"SPY", &s_spy, healthy},   SurfaceArchiveItem{"AAA", &s_aaa, degraded},
      SurfaceArchiveItem{"BBB", &s_bbb, std::nullopt}, SurfaceArchiveItem{"CCC", &s_ccc, healthy},
      SurfaceArchiveItem{"DDD", &s_ddd, degraded},  SurfaceArchiveItem{"EEE", &s_eee, healthy}};
  expect_reemit_byte_identical(items, "mixed_board");
}

// Two round trips: re-emitting the re-emit must still be the same bytes, so a
// repeatedly-resumed partition cannot drift one field per rewrite.
TEST(SurfaceArchiveV2, ReemitByteIdenticalIsIdempotent) {
  const PricedSurface ps = make_convex(9, 8, 33);
  const SurfaceProvenance prov = make_provenance();
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SYM", &ps, prov}};
  auto first = write_surface_archive_v2(items, pinned_opts());
  ASSERT_TRUE(first.has_value());
  const std::vector<std::byte> a = std::move(*first);
  const std::vector<std::byte> b = reemit_via_reconstruct(a);
  ASSERT_FALSE(b.empty());
  const std::vector<std::byte> c = reemit_via_reconstruct(b);
  ASSERT_EQ(a.size(), c.size());
  EXPECT_EQ(0, std::memcmp(a.data(), c.data(), a.size()));
}

// ── The record is a function of the VALUES, never of the struct padding ──────
//
// `EssviParams`/`SviParams`/`C8Params` are blitted into the record whole, so the
// record used to inherit their PADDING as well as their members. No fitter ever
// writes those pad bytes — a fit builds its result as `SviParams out{}` and
// assigns members, and clang-cl does not materialize the zero-initialization of
// padding bits — so the pad carries whatever the producing thread's stack last
// left at that address. Blitted verbatim it reached the record, `payload_crc32c`
// and (via the directory mirror) the archive's content identity, which made the
// stored bytes of an IDENTICAL fitted slice depend on which thread fitted it —
// i.e. on the fit worker count. That is what
// `SurfaceDbPopulate.CarryOverIsByteIdenticalAcrossWorkerCounts` observes end to
// end; this is the same defect pinned at the writer, one kind at a time.
//
// Two surfaces that differ ONLY in the padding of their slice params must
// serialize to identical bytes.
void expect_padding_blind(const PricedSurface &clean, const PricedSurface &dirty,
                          const char *what) {
  const SurfaceProvenance prov = make_provenance();
  const std::array<SurfaceArchiveItem, 1> clean_items{SurfaceArchiveItem{"SYM", &clean, prov}};
  const std::array<SurfaceArchiveItem, 1> dirty_items{SurfaceArchiveItem{"SYM", &dirty, prov}};
  auto a = write_surface_archive_v2(clean_items, pinned_opts());
  auto b = write_surface_archive_v2(dirty_items, pinned_opts());
  ASSERT_TRUE(a.has_value()) << what;
  ASSERT_TRUE(b.has_value()) << what;
  ASSERT_EQ(a->size(), b->size()) << what;
  EXPECT_EQ(0, std::memcmp(a->data(), b->data(), a->size()))
      << what << ": archive bytes depend on the slice params' padding, so the same "
                 "fitted surface stores differently depending on the producing thread";
}

TEST(SurfaceArchiveV2, SliceParamPaddingDoesNotReachTheRecord) {
  expect_padding_blind(make_svi(3, 4, 0x00), make_svi(3, 4, 0xAB), "svi");
  expect_padding_blind(make_essvi(3, 4, 0x00), make_essvi(3, 4, 0xAB), "essvi");
  expect_padding_blind(make_c8(3, 4, 0x00), make_c8(3, 4, 0xAB), "c8");
}
