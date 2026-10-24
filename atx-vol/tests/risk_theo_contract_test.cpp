// risk_theo_contract_test — relative option templates resolved on fitted surfaces
// (risk-engine sprint 2026-10-24, lane B): fixed-(T, k) and fixed-(T, delta) series
// across a history, absolute expiries ageing, calendar maturities landing on the
// 16:00 ET settlement, the always-present forward delta, cold Greeks bit-equal to
// PricedSurface::greeks_analytic, the batch form, and typed market failures.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/fitting/surface_parity.hpp" // SliceContext
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp" // EssviParams
#include "atx/vol/api/marketdata/data.hpp"     // expiry_instant_ns
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/risk/strike_coord.hpp"
#include "atx/vol/api/risk/theo_contract.hpp"

namespace {

using namespace atx::vol; // NOLINT(google-build-using-namespace): test-local brevity

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] std::int64_t utc_ns(int y, unsigned mo, unsigned d, unsigned h, unsigned mi) {
  return atx::core::time::timestamp_from_utc(y, mo, d, h, mi, 0, 0).unix_nanos();
}

// A skewed eSSVI term structure on a consistent flat carry (F = S e^{(r - q) T}).
[[nodiscard]] PricedSurface make_surface(double spot, double r, double q, double skew = -0.55,
                                         double level = 1.0) {
  constexpr std::array<double, 4> kT{0.08, 0.25, 0.5, 1.0};
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (std::size_t i = 0; i < kT.size(); ++i) {
    const double T = kT[i];
    const double F = spot * std::exp((r - q) * T);
    EssviParams e{};
    e.theta = level * (0.045 * T + 0.002 * static_cast<double>(i));
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
  pc.S = spot;
  pc.r = r;
  pc.now_ts_ns = 1'790'000'000'000'000'000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_default_opts();
  pc.uid = 21;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

// NaN-aware, bit-exact equality of every field.
[[nodiscard]] bool same_bits(const ResolvedTheo &a, const ResolvedTheo &b) {
  const auto greeks_equal = [](const AmericanGreeks &x, const AmericanGreeks &y) {
    return bits_equal(x.delta, y.delta) && bits_equal(x.gamma, y.gamma) &&
           bits_equal(x.vega, y.vega) && bits_equal(x.theta, y.theta) &&
           bits_equal(x.rho, y.rho) && bits_equal(x.vanna, y.vanna) &&
           bits_equal(x.volga, y.volga) && bits_equal(x.charm, y.charm) &&
           bits_equal(x.price, y.price);
  };
  return a.leg.id == b.leg.id && a.leg.uid == b.leg.uid && bits_equal(a.leg.K, b.leg.K) &&
         a.leg.expiry_ns == b.leg.expiry_ns && a.leg.side == b.leg.side &&
         bits_equal(a.leg.qty, b.leg.qty) && bits_equal(a.leg.multiplier, b.leg.multiplier) &&
         a.valuation_ns == b.valuation_ns && bits_equal(a.T, b.T) &&
         bits_equal(a.forward, b.forward) && bits_equal(a.k_log, b.k_log) &&
         bits_equal(a.standard_moneyness, b.standard_moneyness) && bits_equal(a.delta, b.delta) &&
         bits_equal(a.forward_delta, b.forward_delta) && bits_equal(a.iv, b.iv) &&
         bits_equal(a.mark, b.mark) && greeks_equal(a.greeks, b.greeks) &&
         a.delta_evaluations == b.delta_evaluations && a.flags == b.flags &&
         a.status == b.status && a.fingerprint == b.fingerprint;
}

} // namespace

TEST(TheoContract, FixedMoneynessSeriesHoldsTAndK) {
  // Three historical states with different spots (so different forwards), one
  // relative template: 0.3 years, log-moneyness -0.05, held on every state.
  const std::array<PricedSurface, 3> hist{make_surface(95.0, 0.04, 0.01),
                                          make_surface(100.0, 0.045, 0.02, -0.4),
                                          make_surface(107.0, 0.05, 0.005, -0.7, 1.3)};
  const std::array<SurfaceRef, 3> refs{SurfaceRef{hist[0]}, SurfaceRef{hist[1]},
                                       SurfaceRef{hist[2]}};
  const std::array<std::int64_t, 3> ts{utc_ns(2026, 10, 20, 15, 0), utc_ns(2026, 10, 21, 15, 0),
                                       utc_ns(2026, 10, 22, 15, 0)};
  constexpr double kT = 0.3;
  constexpr double kK = -0.05;
  const TheoContract spec{9, MaturitySpec::years_of(kT), StrikeCoord::log_moneyness(kK), Side::Put};
  std::array<ResolvedTheo, 3> out{};
  ASSERT_TRUE(project_series(spec, refs, ts, ProjectionPolicy{}, out).has_value());
  for (std::size_t i = 0; i < out.size(); ++i) {
    ASSERT_EQ(out[i].status, TheoStatus::Ok) << i << ' ' << to_string(out[i].status);
    EXPECT_LT(std::fabs(out[i].T - kT), 1.0e-12) << i;
    EXPECT_TRUE(bits_equal(out[i].k_log, kK)) << i;
    EXPECT_TRUE(bits_equal(out[i].forward, hist[i].forward_at(out[i].T))) << i;
    EXPECT_TRUE(bits_equal(out[i].leg.K, out[i].forward * std::exp(kK))) << i;
    EXPECT_EQ(out[i].valuation_ns, ts[i]);
    EXPECT_EQ(out[i].leg.expiry_ns, ns_from_year_fraction(ts[i], kT));
    EXPECT_EQ(out[i].leg.uid, 9u);
    EXPECT_NE(out[i].fingerprint, 0u);
  }
  EXPECT_GT(std::fabs(out[0].leg.K - out[2].leg.K), 5.0) << "forwards differ, so strikes must";
}

TEST(TheoContract, FixedDeltaSeriesReSolves) {
  const std::array<PricedSurface, 3> hist{make_surface(95.0, 0.04, 0.01),
                                          make_surface(100.0, 0.045, 0.02, -0.3),
                                          make_surface(107.0, 0.05, 0.005, -0.7, 1.3)};
  const std::array<SurfaceRef, 3> refs{SurfaceRef{hist[0]}, SurfaceRef{hist[1]},
                                       SurfaceRef{hist[2]}};
  const std::array<std::int64_t, 3> ts{utc_ns(2026, 10, 20, 15, 0), utc_ns(2026, 10, 21, 15, 0),
                                       utc_ns(2026, 10, 22, 15, 0)};
  for (const DeltaConvention conv :
       {DeltaConvention::Forward, DeltaConvention::Spot, DeltaConvention::American}) {
    ProjectionPolicy policy;
    policy.delta_conv = conv;
    const TheoContract spec{3, MaturitySpec::years_of(0.3), StrikeCoord::delta(-0.25, conv),
                            Side::Put};
    std::array<ResolvedTheo, 3> out{};
    ASSERT_TRUE(project_series(spec, refs, ts, policy, out).has_value());
    for (std::size_t i = 0; i < out.size(); ++i) {
      ASSERT_EQ(out[i].status, TheoStatus::Ok) << i << ' ' << to_string(out[i].status);
      // Each state re-solves its own strike to the same delta.
      const double tol = conv == DeltaConvention::American ? policy.delta_tolerance : 1.0e-9;
      EXPECT_NEAR(out[i].delta, -0.25, tol) << "conv " << static_cast<int>(conv) << " i " << i;
      EXPECT_LT(std::fabs(out[i].T - 0.3), 1.0e-12);
    }
    EXPECT_GT(std::fabs(out[0].leg.K - out[1].leg.K), 1.0e-3);
    EXPECT_GT(std::fabs(out[1].leg.K - out[2].leg.K), 1.0e-3);
  }
}

TEST(TheoContract, AbsoluteExpiryAgesAcrossValuations) {
  const PricedSurface ps = make_surface(100.0, 0.04, 0.015);
  const SurfaceRef ref{ps};
  const std::int64_t expiry = expiry_instant_ns("2026-12-18", SettlementSession::Pm);
  ASSERT_NE(expiry, 0);
  const std::array<SurfaceRef, 4> refs{ref, ref, ref, ref};
  const std::array<std::int64_t, 4> ts{utc_ns(2026, 10, 20, 15, 0), utc_ns(2026, 10, 27, 15, 0),
                                       utc_ns(2026, 11, 3, 15, 0), utc_ns(2026, 12, 21, 15, 0)};
  const TheoContract spec{4, MaturitySpec::absolute(expiry), StrikeCoord::delta(-0.2), Side::Put};
  std::array<ResolvedTheo, 4> out{};
  ASSERT_TRUE(project_series(spec, refs, ts, ProjectionPolicy{}, out).has_value());
  for (std::size_t i = 0; i < 3; ++i) {
    ASSERT_EQ(out[i].status, TheoStatus::Ok) << i;
    EXPECT_EQ(out[i].leg.expiry_ns, expiry);
    EXPECT_TRUE(bits_equal(out[i].T, time_to_expiry_years(ts[i], expiry, TimeSpec{}).value()));
    if (i > 0) {
      EXPECT_LT(out[i].T, out[i - 1].T) << "the same expiry ages";
    }
  }
  // Rolled past expiry: a typed market status, not a structural error.
  EXPECT_EQ(out[3].status, TheoStatus::MaturityUnavailable);
  EXPECT_LT(out[3].T, 0.0);
  EXPECT_TRUE(std::isnan(out[3].leg.K));
}

TEST(TheoContract, CalendarDaysLandOn1600ET) {
  const TimeSpec clock{};
  // 14:30 EDT Saturday 2026-10-24 (18:30Z). +7 days stays in EDT (20:00Z close);
  // +14 crosses the 2026-11-01 DST end (21:00Z close).
  const std::int64_t afternoon = utc_ns(2026, 10, 24, 18, 30);
  EXPECT_EQ(resolve_maturity(afternoon, MaturitySpec::days(7), clock).value(),
            expiry_instant_ns("2026-10-31"));
  EXPECT_EQ(resolve_maturity(afternoon, MaturitySpec::days(7), clock).value(),
            utc_ns(2026, 10, 31, 20, 0));
  EXPECT_EQ(resolve_maturity(afternoon, MaturitySpec::days(14), clock).value(),
            utc_ns(2026, 11, 7, 21, 0));
  // 23:30 EDT on 10-24 is already 10-25 in UTC: the ET date anchors the count.
  const std::int64_t evening = utc_ns(2026, 10, 25, 3, 30);
  EXPECT_EQ(resolve_maturity(evening, MaturitySpec::days(7), clock).value(),
            expiry_instant_ns("2026-10-31"));
  // Calendar months clamp to the month's end, at the 16:00 ET settlement.
  const std::int64_t new_years_eve = utc_ns(2026, 12, 31, 15, 0);
  EXPECT_EQ(resolve_maturity(new_years_eve, MaturitySpec::months(2), clock).value(),
            expiry_instant_ns("2027-02-28"));
  EXPECT_EQ(resolve_maturity(new_years_eve, MaturitySpec::months(14), clock).value(),
            expiry_instant_ns("2028-02-29"));
  // The resolved theo carries that instant and its year-fraction.
  const PricedSurface ps = make_surface(100.0, 0.04, 0.015);
  const auto theo = resolve_theo(TheoContract{5, MaturitySpec::days(30), StrikeCoord::delta(0.3),
                                              Side::Call},
                                 SurfaceRef{ps}, afternoon);
  ASSERT_TRUE(theo.has_value()) << theo.error().to_string();
  ASSERT_EQ(theo->status, TheoStatus::Ok);
  EXPECT_EQ(theo->leg.expiry_ns, expiry_instant_ns("2026-11-23"));
  EXPECT_TRUE(
      bits_equal(theo->T, time_to_expiry_years(afternoon, theo->leg.expiry_ns, clock).value()));
  // Years under the vol-time clock has no inverse: NotImplemented, never a guess.
  TimeSpec vol_clock;
  vol_clock.convention = TimeConvention::VolTime;
  const auto years_vol = resolve_maturity(afternoon, MaturitySpec::years_of(0.25), vol_clock);
  ASSERT_FALSE(years_vol.has_value());
  EXPECT_EQ(years_vol.error().code(), ErrorCode::NotImplemented);
}

TEST(TheoContract, ForwardDeltaAlwaysPresent) {
  const PricedSurface ps = make_surface(100.0, 0.10, 0.08);
  const SurfaceRef ref{ps};
  const std::int64_t ts = utc_ns(2026, 10, 20, 15, 0);
  for (const DeltaConvention conv :
       {DeltaConvention::Forward, DeltaConvention::Spot, DeltaConvention::American}) {
    ProjectionPolicy policy;
    policy.delta_conv = conv;
    const auto theo = resolve_theo(TheoContract{6, MaturitySpec::years_of(0.5),
                                                StrikeCoord::log_moneyness(-0.08), Side::Call},
                                   ref, ts, policy);
    ASSERT_TRUE(theo.has_value()) << theo.error().to_string();
    ASSERT_EQ(theo->status, TheoStatus::Ok);
    const double fd = forward_delta(theo->forward, theo->leg.K, theo->T, theo->iv, Side::Call);
    ASSERT_TRUE(std::isfinite(theo->forward_delta));
    EXPECT_TRUE(bits_equal(theo->forward_delta, fd)) << static_cast<int>(conv);
    if (conv == DeltaConvention::Forward) {
      EXPECT_TRUE(bits_equal(theo->delta, theo->forward_delta));
    } else {
      // High carry: the spot and American deltas are not the forward delta.
      EXPECT_GT(std::fabs(theo->delta - theo->forward_delta), 1.0e-3) << static_cast<int>(conv);
    }
  }
}

TEST(TheoContract, GreeksMatchPricedSurfaceAnalytic) {
  const PricedSurface ps = make_surface(100.0, 0.04, 0.015);
  const std::int64_t ts = utc_ns(2026, 10, 20, 15, 0);
  for (const Side side : {Side::Put, Side::Call}) {
    const double target = side == Side::Call ? 0.35 : -0.3;
    const auto theo = resolve_theo(
        TheoContract{7, MaturitySpec::days(45), StrikeCoord::delta(target), side}, SurfaceRef{ps},
        ts);
    ASSERT_TRUE(theo.has_value()) << theo.error().to_string();
    ASSERT_EQ(theo->status, TheoStatus::Ok);
    const double K = theo->leg.K;
    const double T = theo->T;
    const AmericanGreeks g = ps.greeks_analytic(K, T, side).value();
    EXPECT_TRUE(bits_equal(theo->greeks.delta, g.delta));
    EXPECT_TRUE(bits_equal(theo->greeks.gamma, g.gamma));
    EXPECT_TRUE(bits_equal(theo->greeks.vega, g.vega));
    EXPECT_TRUE(bits_equal(theo->greeks.theta, g.theta));
    EXPECT_TRUE(bits_equal(theo->greeks.rho, g.rho));
    EXPECT_TRUE(bits_equal(theo->greeks.vanna, g.vanna));
    EXPECT_TRUE(bits_equal(theo->greeks.volga, g.volga));
    EXPECT_TRUE(bits_equal(theo->greeks.charm, g.charm));
    EXPECT_TRUE(bits_equal(theo->greeks.price, g.price));
    EXPECT_TRUE(bits_equal(theo->mark, ps.fair_value(K, T, side).value()));
    EXPECT_TRUE(bits_equal(theo->iv, ps.iv(K, T)));
  }
  // ShapeBlend prices on its own smile: iv is the ShapeBlend vol at (K, T).
  ProjectionPolicy blend;
  blend.interp = TenorInterp::ShapeBlend;
  const auto theo = resolve_theo(TheoContract{8, MaturitySpec::years_of(0.37),
                                              StrikeCoord::strike(85.0), Side::Put},
                                 SurfaceRef{ps}, ts, blend);
  ASSERT_TRUE(theo.has_value());
  ASSERT_EQ(theo->status, TheoStatus::Ok);
  EXPECT_TRUE(bits_equal(theo->iv, ps.iv(85.0, theo->T, TenorInterp::ShapeBlend)));
  EXPECT_FALSE(bits_equal(theo->greeks.vega, ps.greeks_analytic(85.0, theo->T, Side::Put)->vega));
}

TEST(TheoContract, ManyMatchesScalar) {
  const PricedSurface ps = make_surface(100.0, 0.10, 0.08);
  const SurfaceRef ref{ps};
  const std::int64_t ts = utc_ns(2026, 10, 24, 18, 30);
  const std::int64_t past = utc_ns(2026, 10, 1, 20, 0);
  const std::vector<TheoContract> specs{
      {1, MaturitySpec::years_of(0.3), StrikeCoord::log_moneyness(-0.05), Side::Put},
      {2, MaturitySpec::days(30), StrikeCoord::delta(0.25, DeltaConvention::American),
       Side::Call},
      {3, MaturitySpec::months(3), StrikeCoord::standard_moneyness(1.05), Side::Call},
      {4, MaturitySpec::absolute(expiry_instant_ns("2027-01-15")), StrikeCoord::strike(97.0),
       Side::Put},
      {5, MaturitySpec::years_of(0.3), StrikeCoord::standardized_moneyness(-1.0), Side::Put},
      {6, MaturitySpec::years_of(0.3), StrikeCoord::delta(1.5), Side::Call}, // malformed
      {7, MaturitySpec::years_of(0.3), StrikeCoord::delta(0.99, DeltaConvention::Spot),
       Side::Call}, // unreachable under q = 0.08
      {8, MaturitySpec::days(1), StrikeCoord::delta(-0.4), Side::Put}, // next 16:00 ET:
                                                                       // short-end, flagged
      {9, MaturitySpec::absolute(past), StrikeCoord::strike(100.0), Side::Call}, // expired
  };
  std::vector<ResolvedTheo> out(specs.size());
  ASSERT_TRUE(resolve_theo_many(specs, ref, ts, ProjectionPolicy{}, out).has_value());
  for (std::size_t i = 0; i < specs.size(); ++i) {
    const auto scalar = resolve_theo(specs[i], ref, ts);
    if (!scalar) {
      EXPECT_EQ(specs[i].uid, 6u) << scalar.error().to_string();
      EXPECT_EQ(out[i].status, TheoStatus::InvalidSpec);
      continue;
    }
    EXPECT_TRUE(same_bits(*scalar, out[i])) << "row " << i;
  }
  EXPECT_EQ(out[0].status, TheoStatus::Ok);
  EXPECT_EQ(out[6].status, TheoStatus::DeltaUnreachable);
  EXPECT_EQ(out[7].status, TheoStatus::Ok);
  EXPECT_TRUE(has_flag(out[7].flags, RiskFlag::TenorExtrapolated));
  EXPECT_EQ(out[8].status, TheoStatus::MaturityUnavailable);
  std::vector<ResolvedTheo> short_out(2);
  EXPECT_FALSE(resolve_theo_many(specs, ref, ts, ProjectionPolicy{}, short_out).has_value());
}

TEST(TheoContract, UnreachableDeltaIsTyped) {
  const PricedSurface ps = make_surface(100.0, 0.10, 0.08);
  const std::int64_t ts = utc_ns(2026, 10, 20, 15, 0);
  // e^{-qT} = 0.96 caps a call's spot delta below 0.97: unreachable, typed.
  const auto theo = resolve_theo(TheoContract{10, MaturitySpec::years_of(0.5),
                                              StrikeCoord::delta(0.97, DeltaConvention::Spot),
                                              Side::Call},
                                 SurfaceRef{ps}, ts);
  ASSERT_TRUE(theo.has_value()) << theo.error().to_string();
  EXPECT_EQ(theo->status, TheoStatus::DeltaUnreachable);
  EXPECT_TRUE(std::isfinite(theo->T));
  EXPECT_TRUE(std::isfinite(theo->forward));
  EXPECT_TRUE(std::isnan(theo->leg.K));
  EXPECT_TRUE(std::isnan(theo->mark));
  EXPECT_EQ(theo->fingerprint, 0u);
  // A malformed template is structural; a null surface too.
  const auto bad = resolve_theo(TheoContract{10, MaturitySpec::years_of(0.5),
                                             StrikeCoord::delta(-0.2), Side::Call},
                                SurfaceRef{ps}, ts);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
  const auto none = resolve_theo(TheoContract{}, SurfaceRef{}, ts);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().code(), ErrorCode::InvalidArgument);
}

// Design gate (spec section 8): a theo resolved outside the fitted tenor range is
// served and flagged under Flag, refused (typed, not structural) under Forbid.
TEST(TheoContract, TenorExtrapolationIsFlagged) {
  const PricedSurface ps = make_surface(100.0, 0.04, 0.015);
  const SurfaceRef ref{ps};
  const std::int64_t ts = utc_ns(2026, 10, 20, 15, 0);
  const TheoContract long_dated{11, MaturitySpec::years_of(1.6), StrikeCoord::delta(-0.25),
                                Side::Put};
  const auto flagged = resolve_theo(long_dated, ref, ts);
  ASSERT_TRUE(flagged.has_value()) << flagged.error().to_string();
  EXPECT_EQ(flagged->status, TheoStatus::Ok);
  EXPECT_TRUE(has_flag(flagged->flags, RiskFlag::TenorExtrapolated));
  EXPECT_TRUE(std::isfinite(flagged->mark));
  ProjectionPolicy forbid;
  forbid.extrap = TenorExtrapPolicy::Forbid;
  const auto refused = resolve_theo(long_dated, ref, ts, forbid);
  ASSERT_TRUE(refused.has_value()) << refused.error().to_string();
  EXPECT_EQ(refused->status, TheoStatus::MaturityUnavailable);
  EXPECT_TRUE(has_flag(refused->flags, RiskFlag::TenorExtrapolated));
  EXPECT_TRUE(std::isnan(refused->leg.K));
  const auto inside = resolve_theo(
      TheoContract{11, MaturitySpec::years_of(0.6), StrikeCoord::delta(-0.25), Side::Put}, ref, ts,
      forbid);
  ASSERT_TRUE(inside.has_value());
  EXPECT_EQ(inside->status, TheoStatus::Ok);
  EXPECT_FALSE(has_flag(inside->flags, RiskFlag::TenorExtrapolated));
}
