// priced_surface_shape_blend_test — the additive TenorInterp::ShapeBlend maturity
// interpolation on CurveSurface / PricedSurface / PricedSurfaceView (risk-engine
// sprint 2026-10-24, lane B): the two-argument default path stays bit-identical,
// ShapeBlend equals TotalVariance at the money and on single-slice brackets,
// preserves a standardized skew shape across pillars where TotalVariance does not,
// and a zero-copy view over an archived copy answers bit-for-bit like the owner.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/backtest/priced_surface_view.hpp"
#include "atx/vol/api/fitting/spline_curve.hpp"   // SplineVolParams
#include "atx/vol/api/fitting/surface_parity.hpp" // SliceContext
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp" // EssviParams
#include "atx/vol/api/pricing/american.hpp"    // al_fast_opts
#include "atx/vol/api/storage/surface_archive.hpp"

namespace {

using atx::vol::AmericanMethod;
using atx::vol::CurveSurface;
using atx::vol::EssviCurve;
using atx::vol::EssviParams;
using atx::vol::IVolCurve;
using atx::vol::PricedSurface;
using atx::vol::PricedSurfaceView;
using atx::vol::PricingContext;
using atx::vol::SliceContext;
using atx::vol::SplineVolCurve;
using atx::vol::SplineVolParams;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveV2;
using atx::vol::SurfaceRef;
using atx::vol::TenorInterp;
using atx::vol::VolCurveKind;

constexpr double kSpot = 100.0;
constexpr double kRate = 0.04;
constexpr double kCarry = 0.015;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// A skewed eSSVI term structure on a consistent carry (F = S e^{(r - q) T}).
[[nodiscard]] PricedSurface make_essvi_surface() {
  constexpr std::array<double, 4> kT{0.08, 0.25, 0.5, 1.0};
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (std::size_t i = 0; i < kT.size(); ++i) {
    const double T = kT[i];
    const double F = kSpot * std::exp((kRate - kCarry) * T);
    EssviParams e{};
    e.theta = 0.045 * T + 0.002 * static_cast<double>(i);
    e.phi = 1.2 - 0.15 * static_cast<double>(i);
    e.rho = -0.55 + 0.08 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = F;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kRate * T)));
    ctx.push_back(SliceContext{T, F, 0.0, kCarry, 50, 0});
  }
  PricingContext pc;
  pc.S = kSpot;
  pc.r = kRate;
  pc.now_ts_ns = 1'790'000'000'000'000'000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = atx::vol::al_fast_opts();
  pc.uid = 7;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

// A test-only slice whose smile is a FIXED vol-multiple shape in standardized
// moneyness: sigma(k) = atm * m(z), z = k / (atm sqrt T), m(z) = 1 + a z + b z^2.
// Two of these with different (atm, T) share one standardized skew exactly.
class StdSkewCurve final : public IVolCurve {
public:
  StdSkewCurve(double T, double F, double atm) noexcept : IVolCurve(T, F, 1.0), atm_(atm) {}
  [[nodiscard]] static double multiple(double z) noexcept { return 1.0 - 0.12 * z + 0.03 * z * z; }
  [[nodiscard]] double w(double k_log) const noexcept override {
    const double z = k_log / (atm_ * std::sqrt(T_));
    const double sigma = atm_ * multiple(z);
    return sigma * sigma * T_;
  }
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::Svi; }
  [[nodiscard]] std::size_t dof() const noexcept override { return 1u; }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<StdSkewCurve>(T_, F_, atm_);
  }

private:
  double atm_;
};

[[nodiscard]] std::vector<std::byte> archive_of(const PricedSurface &ps, std::string_view symbol) {
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{symbol, &ps, std::nullopt}};
  auto built = atx::vol::write_surface_archive_v2(items);
  EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
  return built.has_value() ? std::move(*built) : std::vector<std::byte>{};
}

// A two-pillar SplineVol surface (the one curve kind that reports
// `is_extrapolated`): hand-built knots, ATM 24% / 22%, data-supported
// standardized range [-1.5, 1.2] on pillar 0 and [-0.9, 1.2] on pillar 1. The
// asymmetry shows a sign slip; pillar 1's NARROWER low side (k_lo = -0.140 vs
// pillar 0's -0.180, while its high side is wider: 0.187 vs 0.144) means each
// pillar has strikes only IT calls extrapolated, so both arms of the OR count.
constexpr std::array<double, 2> kSplineT{0.25, 0.5};
constexpr std::array<double, 2> kSplineAtm{0.24, 0.22};
constexpr std::array<double, 2> kSplineZLo{-1.5, -0.9};
constexpr double kSplineZHi = 1.2;

[[nodiscard]] PricedSurface make_spline_surface() {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (std::size_t i = 0; i < kSplineT.size(); ++i) {
    const double T = kSplineT[i];
    const double F = kSpot * std::exp((kRate - kCarry) * T);
    SplineVolParams p;
    p.atm_vol = kSplineAtm[i];
    p.z = {-3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0};
    p.mult = {1.34, 1.21, 1.09, 1.0, 0.95, 0.93, 0.94};
    p.z_lo_valid = kSplineZLo[i];
    p.z_hi_valid = kSplineZHi;
    cs.push(std::make_unique<SplineVolCurve>(std::move(p), T, F, std::exp(-kRate * T)));
    ctx.push_back(SliceContext{T, F, 0.0, kCarry, 20, 0});
  }
  PricingContext pc;
  pc.S = kSpot;
  pc.r = kRate;
  pc.now_ts_ns = 1'790'000'000'000'000'000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = atx::vol::al_fast_opts();
  pc.uid = 8;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

// The data-supported log-moneyness edges of spline pillar i.
[[nodiscard]] double spline_k_hi(std::size_t i) noexcept {
  return kSplineZHi * kSplineAtm[i] * std::sqrt(kSplineT[i]);
}
[[nodiscard]] double spline_k_lo(std::size_t i) noexcept {
  return kSplineZLo[i] * kSplineAtm[i] * std::sqrt(kSplineT[i]);
}

// 20 strikes x 10 tenors = 200 grid points, spanning short-end extrapolation,
// every pillar, interior brackets and long-end extrapolation.
constexpr std::array<double, 10> kGridT{0.02, 0.08, 0.12, 0.25, 0.31, 0.5, 0.73, 1.0, 1.4, 2.0};
[[nodiscard]] double grid_K(std::size_t j) noexcept {
  return 55.0 + 5.0 * static_cast<double>(j); // 55 .. 150
}

} // namespace

// The two-argument overloads are untouched; the TotalVariance overload forwards to
// them. Pinned bit-for-bit over 200 points on CurveSurface (w and iv),
// PricedSurface, a SurfaceRef and the archived view, plus the documented formula.
TEST(PricedSurfaceShapeBlend, DefaultOverloadBitIdentical) {
  const PricedSurface ps = make_essvi_surface();
  auto arch = SurfaceArchiveV2::open(archive_of(ps, "shp"));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto view = arch->map_symbol("shp");
  ASSERT_TRUE(view.has_value()) << view.error().to_string();
  const SurfaceRef ref{ps};
  const CurveSurface &cs = ps.surface();
  std::size_t checked = 0;
  for (const double T : kGridT) {
    for (std::size_t j = 0; j < 20; ++j) {
      const double K = grid_K(j);
      const double k = std::log(K / ps.forward_at(T));
      EXPECT_TRUE(bits_equal(ps.iv(K, T), ps.iv(K, T, TenorInterp::TotalVariance)))
          << K << ' ' << T;
      EXPECT_TRUE(bits_equal(ref.iv(K, T), ref.iv(K, T, TenorInterp::TotalVariance)));
      EXPECT_TRUE(bits_equal(view->iv(K, T), view->iv(K, T, TenorInterp::TotalVariance)));
      EXPECT_TRUE(bits_equal(view->iv(K, T), ps.iv(K, T)));
      EXPECT_TRUE(bits_equal(cs.w(k, T), cs.w(k, T, TenorInterp::TotalVariance)));
      EXPECT_TRUE(bits_equal(cs.iv(k, T), cs.iv(k, T, TenorInterp::TotalVariance)));
      // The documented TotalVariance formula on interior brackets (to rounding only:
      // FMA contraction may move the reference by an ulp).
      const auto slices = cs.slices();
      for (std::size_t s = 0; s + 1 < slices.size(); ++s) {
        const double T_lo = slices[s]->T();
        const double T_hi = slices[s + 1]->T();
        if (T > T_lo && T < T_hi) {
          const double a = (T - T_lo) / (T_hi - T_lo);
          const double ref_w = (1.0 - a) * slices[s]->w(k) + a * slices[s + 1]->w(k);
          EXPECT_NEAR(cs.w(k, T), ref_w, 4.0e-16 * std::fabs(ref_w));
        }
      }
      ++checked;
    }
  }
  EXPECT_EQ(checked, 200u);
}

TEST(PricedSurfaceShapeBlend, ShapeBlendEqualsTotalVarianceAtAtm) {
  const PricedSurface ps = make_essvi_surface();
  for (const double T : kGridT) {
    const double F = ps.forward_at(T);
    const double tv = ps.iv(F, T, TenorInterp::TotalVariance);
    const double sb = ps.iv(F, T, TenorInterp::ShapeBlend);
    ASSERT_TRUE(std::isfinite(tv)) << T;
    EXPECT_LT(std::fabs(sb - tv), 1.0e-12) << T;
    EXPECT_LT(std::fabs(ps.surface().iv(0.0, T, TenorInterp::ShapeBlend) -
                        ps.surface().iv(0.0, T, TenorInterp::TotalVariance)),
              1.0e-12)
        << T;
  }
}

TEST(PricedSurfaceShapeBlend, ShapeBlendPreservesSkew) {
  constexpr double T_lo = 0.1;
  constexpr double T_hi = 1.0;
  CurveSurface cs;
  cs.push(std::make_unique<StdSkewCurve>(T_lo, kSpot, 0.32));
  cs.push(std::make_unique<StdSkewCurve>(T_hi, kSpot, 0.19));
  for (const double T_q : {0.2, 0.45, 0.8}) {
    const double atm_q = cs.iv(0.0, T_q, TenorInterp::ShapeBlend);
    ASSERT_TRUE(std::isfinite(atm_q) && atm_q > 0.0);
    double tv_departure = 0.0;
    for (const double z : {-1.0, 1.0}) {
      const double k = z * atm_q * std::sqrt(T_q);
      const double blended = cs.iv(k, T_q, TenorInterp::ShapeBlend) / atm_q;
      EXPECT_NEAR(blended, StdSkewCurve::multiple(z), 1.0e-9) << "T=" << T_q << " z=" << z;
      const double tv = cs.iv(k, T_q, TenorInterp::TotalVariance) / atm_q;
      tv_departure = std::fmax(tv_departure, std::fabs(tv - StdSkewCurve::multiple(z)));
    }
    EXPECT_GT(tv_departure, 1.0e-4) << "TotalVariance must NOT preserve the standardized skew";
  }
}

TEST(PricedSurfaceShapeBlend, ExactPillarIdenticalInBothModes) {
  const PricedSurface ps = make_essvi_surface();
  const CurveSurface &cs = ps.surface();
  // Exact pillars plus both extrapolation branches: single-slice brackets.
  std::vector<double> single;
  for (const auto &s : cs.slices()) {
    single.push_back(s->T());
  }
  single.push_back(0.01);
  single.push_back(3.0);
  for (const double T : single) {
    for (std::size_t j = 0; j < 20; ++j) {
      const double K = grid_K(j);
      const double k = std::log(K / ps.forward_at(T));
      EXPECT_TRUE(bits_equal(ps.iv(K, T, TenorInterp::ShapeBlend),
                             ps.iv(K, T, TenorInterp::TotalVariance)))
          << K << ' ' << T;
      EXPECT_TRUE(bits_equal(cs.w(k, T, TenorInterp::ShapeBlend), cs.w(k, T))) << k << ' ' << T;
    }
  }
  // Off-pillar the two modes genuinely differ away from the money.
  EXPECT_FALSE(bits_equal(ps.iv(70.0, 0.31, TenorInterp::ShapeBlend), ps.iv(70.0, 0.31)));
}

TEST(PricedSurfaceShapeBlend, ViewMatchesOwned) {
  const PricedSurface ps = make_essvi_surface();
  auto arch = SurfaceArchiveV2::open(archive_of(ps, "shp"));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto view = arch->map_symbol("shp");
  ASSERT_TRUE(view.has_value()) << view.error().to_string();
  const SurfaceRef owned_ref{ps};
  const SurfaceRef view_ref{*view};
  std::size_t interior = 0;
  for (const double T : kGridT) {
    for (std::size_t j = 0; j < 20; ++j) {
      const double K = grid_K(j);
      const double owned = ps.iv(K, T, TenorInterp::ShapeBlend);
      EXPECT_TRUE(bits_equal(owned, view->iv(K, T, TenorInterp::ShapeBlend))) << K << ' ' << T;
      EXPECT_TRUE(bits_equal(owned_ref.iv(K, T, TenorInterp::ShapeBlend),
                             view_ref.iv(K, T, TenorInterp::ShapeBlend)));
      EXPECT_EQ(ps.moneyness_extrapolated(K, T), view->moneyness_extrapolated(K, T));
      if (!bits_equal(owned, ps.iv(K, T))) {
        ++interior;
      }
    }
  }
  EXPECT_GT(interior, 20u) << "the grid must exercise genuine interior blends";
  // Parametric slices never claim a data-free wing.
  EXPECT_FALSE(ps.moneyness_extrapolated(20.0, 0.31));
  EXPECT_FALSE(view->moneyness_extrapolated(20.0, 0.31));
}

// The TRUE branch of the best-effort moneyness provenance: on a SplineVol surface
// `moneyness_extrapolated` is the OR of the bracketing slices' data-free wings,
// identically on the owned surface and on a zero-copy view of its archived copy.
TEST(PricedSurfaceShapeBlend, MoneynessExtrapolatedOnSplineWing) {
  const PricedSurface ps = make_spline_surface();
  ASSERT_EQ(ps.kind_at(0), VolCurveKind::SplineVol);
  auto arch = SurfaceArchiveV2::open(archive_of(ps, "spl"));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto view = arch->map_symbol("spl");
  ASSERT_TRUE(view.has_value()) << view.error().to_string();
  ASSERT_EQ(view->kind_at(1), VolCurveKind::SplineVol);
  const auto strike = [&](double k, double T) { return ps.forward_at(T) * std::exp(k); };
  const auto both = [&](double K, double T) {
    const bool owned = ps.moneyness_extrapolated(K, T);
    EXPECT_EQ(owned, view->moneyness_extrapolated(K, T)) << "parity K=" << K << " T=" << T;
    EXPECT_EQ(owned, SurfaceRef{ps}.moneyness_extrapolated(K, T));
    return owned;
  };

  // Exact pillar 0: its own range decides, both wings.
  const double T0 = kSplineT[0];
  EXPECT_FALSE(both(strike(0.0, T0), T0));
  EXPECT_FALSE(both(strike(0.9 * spline_k_hi(0), T0), T0));
  EXPECT_FALSE(both(strike(0.9 * spline_k_lo(0), T0), T0));
  EXPECT_TRUE(both(strike(1.1 * spline_k_hi(0), T0), T0));
  EXPECT_TRUE(both(strike(1.1 * spline_k_lo(0), T0), T0));

  // Interior T: the OR of both pillars. Pillar 1's edge sits further out than
  // pillar 0's, so a k between them is extrapolated on pillar 0 only -> true.
  constexpr double Tq = 0.37;
  ASSERT_LT(spline_k_hi(0), spline_k_hi(1));
  EXPECT_FALSE(both(strike(0.5 * spline_k_hi(0), Tq), Tq));
  EXPECT_TRUE(both(strike(0.5 * (spline_k_hi(0) + spline_k_hi(1)), Tq), Tq));
  EXPECT_TRUE(both(strike(1.2 * spline_k_hi(1), Tq), Tq));
  EXPECT_TRUE(both(strike(1.2 * spline_k_lo(1), Tq), Tq));

  // The upper slice's arm: inside pillar 0's range but below pillar 1's narrower
  // low edge -> extrapolated on pillar 1 only -> true. At pillar 0 alone the same
  // k is inside (false); past the last pillar, pillar 1 alone says true.
  ASSERT_GT(spline_k_lo(1), spline_k_lo(0));
  const double k_upper_only = 0.5 * (spline_k_lo(0) + spline_k_lo(1)); // ~ -0.16
  EXPECT_TRUE(both(strike(k_upper_only, Tq), Tq));
  EXPECT_FALSE(both(strike(k_upper_only, T0), T0));
  EXPECT_TRUE(both(strike(k_upper_only, 0.8), 0.8));

  // Past the last pillar: pillar 1 alone serves.
  constexpr double T_long = 0.8;
  EXPECT_FALSE(both(strike(0.5 * (spline_k_hi(0) + spline_k_hi(1)), T_long), T_long));
  EXPECT_TRUE(both(strike(1.2 * spline_k_hi(1), T_long), T_long));

  // A grid: owned == view everywhere, and both answers occur.
  std::size_t n_true = 0;
  std::size_t n_false = 0;
  for (const double T : kGridT) {
    for (std::size_t j = 0; j < 20; ++j) {
      (both(grid_K(j), T) ? n_true : n_false) += 1;
    }
  }
  EXPECT_GT(n_true, 20u);
  EXPECT_GT(n_false, 20u);
  EXPECT_FALSE(both(-1.0, 0.3)); // invalid K answers false, never a guess
}
