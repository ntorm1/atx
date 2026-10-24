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
#include <span>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/fitting/spline_curve.hpp"   // SplineVolParams
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

// A two-pillar SplineVol surface (T 0.25 / 0.5, ATM 24% / 22%) whose
// data-supported standardized range is [-1.5, 1.2]: k_hi = 0.144 / 0.187.
[[nodiscard]] PricedSurface make_spline_surface() {
  constexpr std::array<double, 2> kT{0.25, 0.5};
  constexpr std::array<double, 2> kAtm{0.24, 0.22};
  constexpr double r = 0.04;
  constexpr double q = 0.015;
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (std::size_t i = 0; i < kT.size(); ++i) {
    const double F = 100.0 * std::exp((r - q) * kT[i]);
    SplineVolParams p;
    p.atm_vol = kAtm[i];
    p.z = {-3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0};
    p.mult = {1.34, 1.21, 1.09, 1.0, 0.95, 0.93, 0.94};
    p.z_lo_valid = -1.5;
    p.z_hi_valid = 1.2;
    cs.push(std::make_unique<SplineVolCurve>(std::move(p), kT[i], F, std::exp(-r * kT[i])));
    ctx.push_back(SliceContext{kT[i], F, 0.0, q, 20, 0});
  }
  PricingContext pc;
  pc.S = 100.0;
  pc.r = r;
  pc.now_ts_ns = 1'790'000'000'000'000'000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_default_opts();
  pc.uid = 22;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

// A test-only flat-vol slice with a vol HOLE: w(k) is NaN for lo < k < hi.
class HoleCurve final : public IVolCurve {
public:
  HoleCurve(double T, double F, double lo, double hi) noexcept
      : IVolCurve(T, F, 1.0), lo_(lo), hi_(hi) {}
  [[nodiscard]] double w(double k_log) const noexcept override {
    return (k_log > lo_ && k_log < hi_) ? std::nan("") : 0.04 * T_;
  }
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::Svi; }
  [[nodiscard]] std::size_t dof() const noexcept override { return 1u; }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<HoleCurve>(T_, F_, lo_, hi_);
  }

private:
  double lo_;
  double hi_;
};

[[nodiscard]] PricedSurface make_hole_surface(double lo, double hi) {
  constexpr std::array<double, 2> kT{0.25, 0.5};
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (const double T : kT) {
    const double F = 100.0 * std::exp(0.03 * T);
    cs.push(std::make_unique<HoleCurve>(T, F, lo, hi));
    ctx.push_back(SliceContext{T, F, 0.0, 0.01, 20, 0});
  }
  PricingContext pc;
  pc.S = 100.0;
  pc.r = 0.04;
  pc.now_ts_ns = 1'790'000'000'000'000'000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_default_opts();
  pc.uid = 23;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

// The uniform non-Ok rule: no strike-level number is served and the fingerprint is 0.
void expect_unserved(const ResolvedTheo &r) {
  EXPECT_NE(r.status, TheoStatus::Ok);
  EXPECT_EQ(r.fingerprint, 0u);
  EXPECT_TRUE(std::isnan(r.leg.K));
  EXPECT_TRUE(std::isnan(r.k_log));
  EXPECT_TRUE(std::isnan(r.standard_moneyness));
  EXPECT_TRUE(std::isnan(r.delta));
  EXPECT_TRUE(std::isnan(r.forward_delta));
  EXPECT_TRUE(std::isnan(r.iv));
  EXPECT_TRUE(std::isnan(r.mark));
  EXPECT_TRUE(std::isnan(r.greeks.delta));
  EXPECT_TRUE(std::isnan(r.greeks.price));
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

// The best-effort moneyness provenance reaches a resolved theo: a strike in a
// SplineVol data-free wing carries MoneynessExtrapolated, one inside does not.
TEST(TheoContract, MoneynessExtrapolationIsFlagged) {
  const PricedSurface ps = make_spline_surface();
  const SurfaceRef ref{ps};
  const std::int64_t ts = utc_ns(2026, 10, 20, 15, 0);
  const auto resolve = [&](StrikeCoord strike, Side side) {
    const auto r = resolve_theo(TheoContract{30, MaturitySpec::years_of(0.37), strike, side}, ref,
                                ts);
    EXPECT_TRUE(r.has_value()) << r.error().to_string();
    return r.value_or(ResolvedTheo{});
  };
  const ResolvedTheo wing = resolve(StrikeCoord::log_moneyness(0.4), Side::Call);
  EXPECT_EQ(wing.status, TheoStatus::Ok);
  EXPECT_TRUE(has_flag(wing.flags, RiskFlag::MoneynessExtrapolated));
  EXPECT_FALSE(has_flag(wing.flags, RiskFlag::TenorExtrapolated));
  EXPECT_TRUE(std::isfinite(wing.mark));
  const ResolvedTheo deep_put = resolve(StrikeCoord::delta(-0.03), Side::Put);
  EXPECT_EQ(deep_put.status, TheoStatus::Ok);
  EXPECT_TRUE(has_flag(deep_put.flags, RiskFlag::MoneynessExtrapolated));
  const ResolvedTheo atm = resolve(StrikeCoord::log_moneyness(0.0), Side::Call);
  EXPECT_EQ(atm.status, TheoStatus::Ok);
  EXPECT_FALSE(has_flag(atm.flags, RiskFlag::MoneynessExtrapolated));
}

// project_series: a missing surface at one valuation is that row's
// SurfaceUnavailable (uniform non-Ok shape); its neighbours resolve normally.
TEST(TheoContract, NullSurfaceRowIsSurfaceUnavailable) {
  const PricedSurface ps = make_surface(100.0, 0.04, 0.015);
  const std::array<SurfaceRef, 3> refs{SurfaceRef{ps}, SurfaceRef{}, SurfaceRef{ps}};
  const std::array<std::int64_t, 3> ts{utc_ns(2026, 10, 20, 15, 0), utc_ns(2026, 10, 21, 15, 0),
                                       utc_ns(2026, 10, 22, 15, 0)};
  const TheoContract spec{31, MaturitySpec::years_of(0.3), StrikeCoord::log_moneyness(-0.05),
                          Side::Put};
  std::array<ResolvedTheo, 3> out{};
  ASSERT_TRUE(project_series(spec, refs, ts, ProjectionPolicy{}, out).has_value());
  EXPECT_EQ(out[0].status, TheoStatus::Ok);
  EXPECT_EQ(out[2].status, TheoStatus::Ok);
  EXPECT_EQ(out[1].status, TheoStatus::SurfaceUnavailable);
  expect_unserved(out[1]);
  EXPECT_EQ(out[1].valuation_ns, ts[1]);
  EXPECT_EQ(out[1].leg.uid, 31u);
}

// No vol where the template lands is the surface's failure, for every strike kind:
// a moneyness strike in a vol hole, and a Delta template on a surface without an
// ATM vol (which must not read as an unreachable delta).
TEST(TheoContract, NoVolIsSurfaceUnavailable) {
  const std::int64_t ts = utc_ns(2026, 10, 20, 15, 0);
  const PricedSurface wing_hole = make_hole_surface(0.15, 0.30);
  const auto in_hole = resolve_theo(
      TheoContract{32, MaturitySpec::years_of(0.3), StrikeCoord::log_moneyness(0.2), Side::Call},
      SurfaceRef{wing_hole}, ts);
  ASSERT_TRUE(in_hole.has_value()) << in_hole.error().to_string();
  EXPECT_EQ(in_hole->status, TheoStatus::SurfaceUnavailable);
  expect_unserved(*in_hole);
  EXPECT_TRUE(std::isfinite(in_hole->T));
  const auto outside = resolve_theo(
      TheoContract{32, MaturitySpec::years_of(0.3), StrikeCoord::log_moneyness(0.0), Side::Call},
      SurfaceRef{wing_hole}, ts);
  ASSERT_TRUE(outside.has_value());
  EXPECT_EQ(outside->status, TheoStatus::Ok);

  const PricedSurface atm_hole = make_hole_surface(-0.02, 0.02);
  for (const DeltaConvention conv : {DeltaConvention::Forward, DeltaConvention::American}) {
    ProjectionPolicy policy;
    policy.delta_conv = conv;
    const auto delta_theo = resolve_theo(
        TheoContract{33, MaturitySpec::years_of(0.3), StrikeCoord::delta(-0.25, conv), Side::Put},
        SurfaceRef{atm_hole}, ts, policy);
    ASSERT_TRUE(delta_theo.has_value()) << delta_theo.error().to_string();
    EXPECT_EQ(delta_theo->status, TheoStatus::SurfaceUnavailable) << static_cast<int>(conv);
    expect_unserved(*delta_theo);
  }
}

// A deterministic pricer refusal: a put in the double-continuation regime
// (q < r <= 0) has no single-boundary Andersen-Lake price. The strike resolves,
// but the row follows the uniform non-Ok rule.
TEST(TheoContract, PricingErrorRowFollowsUniformRule) {
  const PricedSurface ps = make_surface(100.0, -0.01, -0.02);
  const std::int64_t ts = utc_ns(2026, 10, 20, 15, 0);
  const TheoContract spec{34, MaturitySpec::years_of(0.3), StrikeCoord::log_moneyness(-0.05),
                          Side::Put};
  const auto theo = resolve_theo(spec, SurfaceRef{ps}, ts);
  ASSERT_TRUE(theo.has_value()) << theo.error().to_string();
  EXPECT_EQ(theo->status, TheoStatus::PricingError);
  expect_unserved(*theo);
  EXPECT_TRUE(std::isfinite(theo->T));
  EXPECT_TRUE(std::isfinite(theo->forward));
  ASSERT_FALSE(ps.fair_value(100.0, theo->T, Side::Put).has_value()) << "the regime is refused";
  // The batch form reports the same row bit for bit.
  std::array<ResolvedTheo, 1> many{};
  ASSERT_TRUE(resolve_theo_many(std::span<const TheoContract>{&spec, 1}, SurfaceRef{ps}, ts,
                                ProjectionPolicy{}, many)
                  .has_value());
  EXPECT_TRUE(same_bits(*theo, many[0]));

  // The delta convention must not reclassify the same refusal: an American
  // reported delta (policy) and an American delta STRIKE both reach the refusing
  // pricer, and both are the same PricingError row, scalar == batch == series.
  ProjectionPolicy american;
  american.delta_conv = DeltaConvention::American;
  const TheoContract american_strike{34, MaturitySpec::years_of(0.3),
                                     StrikeCoord::delta(-0.3, DeltaConvention::American),
                                     Side::Put};
  const std::array<std::pair<TheoContract, ProjectionPolicy>, 2> cases{
      std::pair{spec, american}, std::pair{american_strike, ProjectionPolicy{}}};
  for (const auto &[c, policy] : cases) {
    const auto scalar = resolve_theo(c, SurfaceRef{ps}, ts, policy);
    ASSERT_TRUE(scalar.has_value()) << scalar.error().to_string();
    EXPECT_EQ(scalar->status, TheoStatus::PricingError) << to_string(scalar->status);
    expect_unserved(*scalar);
    EXPECT_TRUE(std::isfinite(scalar->T));
    EXPECT_TRUE(std::isfinite(scalar->forward));
    EXPECT_NE(scalar->leg.expiry_ns, 0);
    std::array<ResolvedTheo, 1> batch{};
    ASSERT_TRUE(resolve_theo_many(std::span<const TheoContract>{&c, 1}, SurfaceRef{ps}, ts,
                                  policy, batch)
                    .has_value());
    EXPECT_TRUE(same_bits(*scalar, batch[0]));
    const std::array<SurfaceRef, 1> refs{SurfaceRef{ps}};
    const std::array<std::int64_t, 1> stamps{ts};
    std::array<ResolvedTheo, 1> series{};
    ASSERT_TRUE(project_series(c, refs, stamps, policy, series).has_value());
    EXPECT_TRUE(same_bits(*scalar, series[0]));
  }
}

// A moneyness coordinate that names no finite positive strike on this state is a
// template problem, reported as InvalidSpec (never as an unreachable delta).
TEST(TheoContract, NonFiniteStrikeIsInvalidSpec) {
  const PricedSurface ps = make_surface(100.0, 0.04, 0.015);
  const std::int64_t ts = utc_ns(2026, 10, 20, 15, 0);
  const std::vector<TheoContract> specs{
      {35, MaturitySpec::years_of(0.3), StrikeCoord::log_moneyness(800.0), Side::Call},
      {36, MaturitySpec::years_of(0.3), StrikeCoord::log_moneyness(-800.0), Side::Put},
      {37, MaturitySpec::years_of(0.3), StrikeCoord::standard_moneyness(1.0e307), Side::Call},
  };
  std::vector<ResolvedTheo> many(specs.size());
  ASSERT_TRUE(resolve_theo_many(specs, SurfaceRef{ps}, ts, ProjectionPolicy{}, many).has_value());
  for (std::size_t i = 0; i < specs.size(); ++i) {
    const auto theo = resolve_theo(specs[i], SurfaceRef{ps}, ts);
    ASSERT_TRUE(theo.has_value()) << theo.error().to_string();
    EXPECT_EQ(theo->status, TheoStatus::InvalidSpec) << i;
    expect_unserved(*theo);
    EXPECT_TRUE(same_bits(*theo, many[i])) << i;
  }
}

// The instant bound is EXCLUSIVE: +/- 2^62 itself is refused, so no accepted pair
// can make time_to_expiry_years subtract its way to 2^63.
TEST(TheoContract, InstantBoundIsExclusive) {
  constexpr std::int64_t kBound = std::int64_t{1} << 62;
  const TimeSpec clock{};
  const auto low = resolve_maturity(-kBound, MaturitySpec::absolute(0), clock);
  ASSERT_FALSE(low.has_value());
  EXPECT_EQ(low.error().code(), ErrorCode::InvalidArgument);
  const auto high = resolve_maturity(0, MaturitySpec::absolute(kBound), clock);
  ASSERT_FALSE(high.has_value());
  EXPECT_EQ(high.error().code(), ErrorCode::InvalidArgument);
  // The widest accepted pair: its difference 2^63 - 2 still fits int64.
  const auto widest = resolve_maturity(-(kBound - 1), MaturitySpec::absolute(kBound - 1), clock);
  ASSERT_TRUE(widest.has_value()) << widest.error().to_string();
  EXPECT_EQ(*widest, kBound - 1);
  const double T = time_to_expiry_years(-(kBound - 1), *widest, clock).value();
  EXPECT_GT(T, 290.0);
  // The pair the old closed bound admitted (-2^62 -> +2^62) is now structural.
  const PricedSurface ps = make_surface(100.0, 0.04, 0.015);
  const auto refused =
      resolve_theo(TheoContract{38, MaturitySpec::absolute(kBound), StrikeCoord::strike(100.0),
                                Side::Call},
                   SurfaceRef{ps}, -kBound);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
}
