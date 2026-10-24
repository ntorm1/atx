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
      EXPECT_TRUE(bits_equal(ps.iv(K, T), ps.iv(K, T, TenorInterp::TotalVariance))) << K << ' ' << T;
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
