// risk_strike_coord_test — strike-coordinate conversions under the three delta
// conventions (risk-engine sprint 2026-10-24, lane B): closed forms, every
// (from, to) round trip on a skewed eSSVI surface, the conventions genuinely
// disagreeing on a high-carry name, parity of the American route with the strategy
// resolver, tenor-extrapolation policy, and the batch form.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/backtest/strategy.hpp" // resolve_strike_by_delta
#include "atx/vol/api/fitting/surface_parity.hpp" // SliceContext
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp" // EssviParams
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/risk/strike_coord.hpp"

namespace {

using namespace atx::vol; // NOLINT(google-build-using-namespace): test-local brevity

constexpr double kSpot = 100.0;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// A skewed eSSVI term structure on a consistent flat carry (F = S e^{(r - q) T}).
[[nodiscard]] PricedSurface make_surface(double r, double q, double skew = -0.55) {
  constexpr std::array<double, 4> kT{0.08, 0.25, 0.5, 1.0};
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (std::size_t i = 0; i < kT.size(); ++i) {
    const double T = kT[i];
    const double F = kSpot * std::exp((r - q) * T);
    EssviParams e{};
    e.theta = 0.045 * T + 0.002 * static_cast<double>(i);
    e.phi = 1.2 - 0.15 * static_cast<double>(i);
    e.rho = skew + 0.08 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = F;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-r * T)));
    ctx.push_back(SliceContext{T, F, 0.0, q, 50, 0});
  }
  PricingContext pc;
  pc.S = kSpot;
  pc.r = r;
  pc.now_ts_ns = 1'790'000'000'000'000'000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_default_opts();
  pc.uid = 11;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

[[nodiscard]] double rel(double a, double b) noexcept { return std::fabs(a - b) / std::fabs(b); }

// The coordinate value of `r` in `kind` (delta: `r` must have been resolved under
// the wanted convention).
[[nodiscard]] double coord_value(const CoordResult &r, StrikeCoordKind kind) noexcept {
  switch (kind) {
  case StrikeCoordKind::Strike:
    return r.K;
  case StrikeCoordKind::LogMoneyness:
    return r.k_log;
  case StrikeCoordKind::StandardMoneyness:
    return r.standard_moneyness;
  case StrikeCoordKind::StandardizedMoneyness:
    return r.standardized_moneyness;
  case StrikeCoordKind::Delta:
    return r.delta;
  }
  return std::nan("");
}

struct CoordSpec {
  StrikeCoordKind kind;
  DeltaConvention conv;
};
constexpr std::array<CoordSpec, 7> kAllCoords{{
    {StrikeCoordKind::Strike, DeltaConvention::Forward},
    {StrikeCoordKind::LogMoneyness, DeltaConvention::Forward},
    {StrikeCoordKind::StandardMoneyness, DeltaConvention::Forward},
    {StrikeCoordKind::StandardizedMoneyness, DeltaConvention::Forward},
    {StrikeCoordKind::Delta, DeltaConvention::Forward},
    {StrikeCoordKind::Delta, DeltaConvention::Spot},
    {StrikeCoordKind::Delta, DeltaConvention::American},
}};

} // namespace

TEST(StrikeCoord, ClosedFormsRoundTrip) {
  constexpr double F = 103.0;
  std::size_t checked = 0;
  for (const double T : {0.05, 0.5, 2.0}) {
    for (const double sigma : {0.1, 0.3, 0.8}) {
      for (const double q : {0.0, 0.03, 0.08}) {
        for (const Side side : {Side::Call, Side::Put}) {
          // delta -> strike -> delta
          for (const double a : {0.03, 0.1, 0.25, 0.5, 0.75, 0.9, 0.97}) {
            const double target = side == Side::Call ? a : -a;
            const double K = strike_from_forward_delta(F, T, sigma, target, side);
            ASSERT_TRUE(std::isfinite(K)) << T << ' ' << sigma << ' ' << target;
            EXPECT_NEAR(forward_delta(F, K, T, sigma, side), target, 1.0e-12);
            const double Ks = strike_from_spot_delta(F, T, sigma, q, target, side);
            if (a / std::exp(-q * T) >= 1.0) {
              EXPECT_TRUE(std::isnan(Ks)) << "spot target beyond e^{-qT} must be unreachable";
            } else {
              ASSERT_TRUE(std::isfinite(Ks));
              EXPECT_NEAR(spot_delta(F, Ks, T, sigma, q, side), target, 1.0e-12);
            }
            ++checked;
          }
          // strike -> delta -> strike over |d1| <= 2
          for (const double d1 : {-2.0, -1.0, -0.3, 0.0, 0.4, 1.1, 2.0}) {
            const double K = F * std::exp(0.5 * sigma * sigma * T - sigma * std::sqrt(T) * d1);
            const double fd = forward_delta(F, K, T, sigma, side);
            EXPECT_LT(rel(strike_from_forward_delta(F, T, sigma, fd, side), K), 1.0e-12);
            const double sd = spot_delta(F, K, T, sigma, q, side);
            EXPECT_LT(rel(strike_from_spot_delta(F, T, sigma, q, sd, side), K), 1.0e-12);
            ++checked;
          }
        }
      }
    }
  }
  EXPECT_EQ(checked, 3u * 3u * 3u * 2u * 14u);
  // Degenerate inputs are NaN, never a number.
  EXPECT_TRUE(std::isnan(forward_delta(F, -1.0, 0.5, 0.2, Side::Call)));
  EXPECT_TRUE(std::isnan(strike_from_forward_delta(F, 0.5, 0.2, -0.3, Side::Call)));
  EXPECT_TRUE(std::isnan(strike_from_forward_delta(F, 0.5, 0.2, 1.0, Side::Call)));
}

TEST(StrikeCoord, RoundTripsAllPairsOnSurface) {
  const PricedSurface ps = make_surface(0.04, 0.015);
  const SurfaceRef ref{ps};
  ProjectionPolicy policy;
  policy.delta_tolerance = 2.0e-10; // K to 1e-9 relative needs a tight American residual
  std::size_t pairs = 0;
  for (const double T : {0.25, 0.4}) {
    for (const Side side : {Side::Call, Side::Put}) {
      for (const double K0 : {82.0, 96.0, 104.0, 118.0}) {
        for (const CoordSpec from : kAllCoords) {
          // The starting coordinate's value, read off a Strike resolution of K0.
          ProjectionPolicy from_policy = policy;
          from_policy.delta_conv = from.conv;
          const auto seed =
              convert_strike_coord(ref, T, side, StrikeCoord::strike(K0), from_policy);
          ASSERT_TRUE(seed.has_value()) << seed.error().to_string();
          const StrikeCoord from_coord{from.kind, coord_value(*seed, from.kind), from.conv};
          const auto r1 = convert_strike_coord(ref, T, side, from_coord, policy);
          ASSERT_TRUE(r1.has_value()) << to_string(from.kind) << ": " << r1.error().to_string();
          EXPECT_LT(rel(r1->K, K0), 1.0e-9) << to_string(from.kind) << ' ' << K0;
          for (const CoordSpec to : kAllCoords) {
            ProjectionPolicy to_policy = policy;
            to_policy.delta_conv = to.conv;
            const auto value = convert_strike_coord(ref, T, side, from_coord, to_policy);
            ASSERT_TRUE(value.has_value()) << value.error().to_string();
            const StrikeCoord to_coord{to.kind, coord_value(*value, to.kind), to.conv};
            const auto r2 = convert_strike_coord(ref, T, side, to_coord, policy);
            ASSERT_TRUE(r2.has_value()) << r2.error().to_string();
            EXPECT_LT(rel(r2->K, r1->K), 1.0e-9)
                << to_string(from.kind) << "->" << to_string(to.kind) << " conv "
                << static_cast<int>(to.conv) << " T=" << T << " K0=" << K0;
            ++pairs;
          }
        }
      }
    }
  }
  EXPECT_EQ(pairs, 2u * 2u * 4u * 7u * 7u);
}

TEST(StrikeCoord, ThreeDeltaConventionsDifferOnHighCarry) {
  // q = 0.08 against r = 0.10: the forward/spot gap is e^{-qT}, and the American put
  // exercise boundary sits near K, so all three strikes separate on a deep-ITM put.
  const PricedSurface carry = make_surface(0.10, 0.08);
  const SurfaceRef ref{carry};
  constexpr double T = 1.0;
  const auto strike_for = [&](DeltaConvention conv, double target, Side side,
                              const SurfaceRef &s) {
    const auto r = convert_strike_coord(s, T, side, StrikeCoord::delta(target, conv));
    EXPECT_TRUE(r.has_value()) << r.error().to_string();
    return r.has_value() ? r->K : std::nan("");
  };
  const double k_fwd = strike_for(DeltaConvention::Forward, -0.85, Side::Put, ref);
  const double k_spot = strike_for(DeltaConvention::Spot, -0.85, Side::Put, ref);
  const double k_amer = strike_for(DeltaConvention::American, -0.85, Side::Put, ref);
  EXPECT_GT(rel(k_fwd, k_spot), 1.0e-3) << k_fwd << ' ' << k_spot;
  EXPECT_GT(rel(k_fwd, k_amer), 1.0e-3) << k_fwd << ' ' << k_amer;
  EXPECT_GT(rel(k_spot, k_amer), 1.0e-3) << k_spot << ' ' << k_amer;

  // Zero carry: the spot discount e^{-qT} is 1, Forward and Spot coincide.
  const PricedSurface flat = make_surface(0.03, 0.0);
  const SurfaceRef flat_ref{flat};
  const double c_fwd = strike_for(DeltaConvention::Forward, 0.3, Side::Call, flat_ref);
  const double c_spot = strike_for(DeltaConvention::Spot, 0.3, Side::Call, flat_ref);
  EXPECT_LT(rel(c_spot, c_fwd), 1.0e-12) << c_fwd << ' ' << c_spot;
}

TEST(StrikeCoord, AmericanMatchesStrategyResolver) {
  const PricedSurface ps = make_surface(0.04, 0.015);
  const SurfaceRef ref{ps};
  ProjectionPolicy policy;
  policy.delta_conv = DeltaConvention::American;
  constexpr double T = 0.4;
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double a : {0.15, 0.25, 0.5, 0.8}) {
      const double target = side == Side::Call ? a : -a;
      const auto ours =
          convert_strike_coord(ref, T, side, StrikeCoord::delta(target, DeltaConvention::American),
                               policy);
      ASSERT_TRUE(ours.has_value()) << ours.error().to_string();
      EXPECT_LE(std::fabs(ours->delta - target), policy.delta_tolerance);
      EXPECT_GT(ours->delta_evaluations, 0u);
      ResolutionOptions options;
      options.fast_screen_cold_confirm = true;
      options.cold_delta_tolerance = policy.delta_tolerance;
      const auto theirs = resolve_strike_by_delta(ref, T, side, a, options);
      ASSERT_TRUE(theirs.has_value()) << theirs.error().to_string();
      // Both are roots of |delta(K) - target| <= tol: the strikes agree to within
      // the strike width that tolerance spans on the local delta slope.
      const double K = ours->K;
      const double h = 1.0e-4 * K;
      const double slope = (ps.delta(K + h, T, side).value() - ps.delta(K - h, T, side).value()) /
                           (2.0 * h);
      ASSERT_GT(std::fabs(slope), 0.0);
      const double width = 2.0 * policy.delta_tolerance / std::fabs(slope) + 1.0e-12 * K;
      EXPECT_LE(std::fabs(K - *theirs), width)
          << "side " << static_cast<int>(side) << " target " << target;
    }
  }
}

TEST(StrikeCoord, ForbidRefusesTenorExtrapolation) {
  const PricedSurface ps = make_surface(0.04, 0.015);
  const SurfaceRef ref{ps};
  ProjectionPolicy forbid;
  forbid.extrap = TenorExtrapPolicy::Forbid;
  for (const double T : {0.02, 1.5}) {
    const auto r = convert_strike_coord(ref, T, Side::Call, StrikeCoord::strike(100.0), forbid);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
  }
  const auto inside =
      convert_strike_coord(ref, 0.4, Side::Call, StrikeCoord::strike(100.0), forbid);
  ASSERT_TRUE(inside.has_value()) << inside.error().to_string();
  EXPECT_FALSE(has_flag(inside->flags, RiskFlag::TenorExtrapolated));
}

TEST(StrikeCoord, FlagsTenorExtrapolation) {
  const PricedSurface ps = make_surface(0.04, 0.015);
  const SurfaceRef ref{ps};
  for (const double T : {0.02, 1.5}) {
    const auto r = convert_strike_coord(ref, T, Side::Put, StrikeCoord::delta(-0.25));
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    EXPECT_TRUE(has_flag(r->flags, RiskFlag::TenorExtrapolated)) << T;
    EXPECT_FALSE(has_flag(r->flags, RiskFlag::MoneynessExtrapolated)); // parametric slices
    EXPECT_NEAR(r->delta, -0.25, 1.0e-9);
  }
  for (const double T : {0.08, 0.4, 1.0}) {
    const auto r = convert_strike_coord(ref, T, Side::Put, StrikeCoord::delta(-0.25));
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(has_flag(r->flags, RiskFlag::TenorExtrapolated)) << T;
  }
}

TEST(StrikeCoord, BatchMatchesScalar) {
  const PricedSurface ps = make_surface(0.04, 0.015);
  const SurfaceRef ref{ps};
  ProjectionPolicy policy;
  policy.delta_conv = DeltaConvention::Spot;
  const std::vector<double> T{0.25, 0.4, 0.4, 1.5, 0.7, 0.1, 0.4, -1.0};
  const std::vector<Side> side{Side::Call, Side::Put, Side::Call, Side::Put,
                               Side::Call, Side::Put, Side::Call, Side::Call};
  const std::vector<StrikeCoord> from{
      StrikeCoord::strike(97.0),
      StrikeCoord::delta(-0.3, DeltaConvention::Forward),
      StrikeCoord::delta(0.4, DeltaConvention::American),
      StrikeCoord::log_moneyness(-0.1),
      StrikeCoord::standardized_moneyness(1.2),
      StrikeCoord::standard_moneyness(0.9),
      StrikeCoord::delta(-0.3, DeltaConvention::Forward), // wrong sign: row error
      StrikeCoord::strike(100.0),                         // T <= 0: row error
  };
  std::vector<CoordResult> out(T.size());
  std::vector<Status> status(T.size());
  ASSERT_TRUE(convert_strike_coord_batch(ref, T, side, from, policy, out, status).has_value());
  std::size_t ok_rows = 0;
  for (std::size_t i = 0; i < T.size(); ++i) {
    const auto scalar = convert_strike_coord(ref, T[i], side[i], from[i], policy);
    ASSERT_EQ(scalar.has_value(), status[i].has_value()) << i;
    if (!scalar) {
      EXPECT_EQ(scalar.error().code(), status[i].error().code()) << i;
      EXPECT_TRUE(std::isnan(out[i].K)) << i;
      continue;
    }
    ++ok_rows;
    const CoordResult &a = *scalar;
    const CoordResult &b = out[i];
    for (const auto field : {&CoordResult::T, &CoordResult::forward, &CoordResult::spot,
                             &CoordResult::K, &CoordResult::k_log, &CoordResult::standard_moneyness,
                             &CoordResult::standardized_moneyness, &CoordResult::sigma_atm,
                             &CoordResult::delta, &CoordResult::sigma}) {
      EXPECT_TRUE(bits_equal(a.*field, b.*field)) << "row " << i;
    }
    EXPECT_EQ(a.delta_conv, b.delta_conv);
    EXPECT_EQ(a.flags, b.flags);
    EXPECT_EQ(a.delta_evaluations, b.delta_evaluations);
  }
  EXPECT_EQ(ok_rows, 6u);
  // Structural errors fail the whole call before any row.
  std::vector<CoordResult> short_out(2);
  EXPECT_FALSE(
      convert_strike_coord_batch(ref, T, side, from, policy, short_out, status).has_value());
  EXPECT_FALSE(
      convert_strike_coord_batch(SurfaceRef{}, T, side, from, policy, out, status).has_value());
}

TEST(StrikeCoord, InvalidDeltaIsInvalidArgument) {
  const PricedSurface ps = make_surface(0.04, 0.015);
  const SurfaceRef ref{ps};
  const auto code_of = [&](double T, Side side, StrikeCoord from) {
    const auto r = convert_strike_coord(ref, T, side, from);
    return r.has_value() ? ErrorCode::Unknown : r.error().code();
  };
  for (const DeltaConvention conv :
       {DeltaConvention::Forward, DeltaConvention::Spot, DeltaConvention::American}) {
    EXPECT_EQ(code_of(0.4, Side::Call, StrikeCoord::delta(-0.2, conv)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(0.4, Side::Put, StrikeCoord::delta(0.3, conv)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(0.4, Side::Call, StrikeCoord::delta(1.0, conv)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(0.4, Side::Put, StrikeCoord::delta(-1.0, conv)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(0.4, Side::Call, StrikeCoord::delta(0.0, conv)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(0.4, Side::Call, StrikeCoord::delta(std::nan(""), conv)),
              ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(code_of(0.0, Side::Call, StrikeCoord::delta(0.3)), ErrorCode::InvalidArgument);
  EXPECT_EQ(code_of(0.4, Side::Call, StrikeCoord::strike(-5.0)), ErrorCode::InvalidArgument);
  const auto null_surface =
      convert_strike_coord(SurfaceRef{}, 0.4, Side::Call, StrikeCoord::delta(0.3));
  ASSERT_FALSE(null_surface.has_value());
  EXPECT_EQ(null_surface.error().code(), ErrorCode::InvalidArgument);
  // A spot delta the carry puts out of reach is a typed NotFound, not a wrong answer.
  const PricedSurface carry = make_surface(0.10, 0.08);
  const auto unreachable = convert_strike_coord(SurfaceRef{carry}, 1.0, Side::Call,
                                                StrikeCoord::delta(0.97, DeltaConvention::Spot));
  ASSERT_FALSE(unreachable.has_value());
  EXPECT_EQ(unreachable.error().code(), ErrorCode::NotFound);
}
