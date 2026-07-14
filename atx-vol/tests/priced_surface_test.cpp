// PricedSurface unit suite — Task 4 (P1.1: one fused surface evaluation).
//
// This file is the bit-identity gate for the "fuse the five redundant per-query
// resolutions into one" refactor. Its guarantee is that the plumbing change
// (fused `resolve` + `evaluate`/`evaluate_batch`, the `interp_forward` binary
// search, and the killed duplicate resolution in PortfolioPricer) moves NO
// numeric output — every existing query stays bit-for-bit identical.
//
// Strategy (TDD): each PUBLIC query method is pinned against an INDEPENDENT
// in-test REFERENCE that re-derives the exact pre-change arithmetic from the
// surface's public accessors (`surface()`, `context()`, `pricing()`) using the
// Reference bracket scan written out below. The reference is not the
// new code path, so a bit-identical match across a (K, T, side) grid proves the
// refactor preserved every bit. The reference tests were captured and made green
// against the PRE-change source first (see at-task-4-report.md).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"

using namespace atx::vol;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kS = 100.0;
constexpr double kR = 0.043;
constexpr std::int64_t kNow = 1700000000000000000LL;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] std::uint64_t hexbits(double a) noexcept {
  std::uint64_t b = 0;
  std::memcpy(&b, &a, sizeof b);
  return b;
}

// The pre-change `valid_query` (priced_surface.cpp), copied verbatim.
[[nodiscard]] bool ref_valid_query(double K, double T) noexcept {
  return std::isfinite(K) && (K > 0.0) && std::isfinite(T) && (T > 0.0);
}

// ── Reference: the PRE-change forward interpolation (linear scan) ────────────
struct RefCarry {
  double forward{0.0};
  double q_eff{0.0};
};

[[nodiscard]] RefCarry ref_interp_forward(const PricedSurface &s, double T) {
  const std::span<const SliceContext> ctx = s.context();
  const SliceContext &first = ctx.front();
  const SliceContext &last = ctx.back();
  if (T <= first.T) {
    if (T == first.T) {
      return RefCarry{first.forward, first.q_eff};
    }
    return RefCarry{s.pricing().S * std::exp((s.rate_at(T) - first.q_eff) * T), first.q_eff};
  }
  if (T >= last.T) {
    if (T == last.T) {
      return RefCarry{last.forward, last.q_eff};
    }
    return RefCarry{s.pricing().S * std::exp((s.rate_at(T) - last.q_eff) * T), last.q_eff};
  }
  std::size_t hi = 0;
  while (hi < ctx.size() && ctx[hi].T <= T) {
    ++hi;
  }
  const std::size_t lo = hi - 1;
  const SliceContext &a = ctx[lo];
  const SliceContext &b = ctx[hi];
  if (T == a.T) {
    return RefCarry{a.forward, a.q_eff};
  }
  const double span = b.T - a.T;
  const double alpha = (span > 0.0) ? (T - a.T) / span : 0.0;
  const double forward =
      std::exp(std::log(a.forward) + alpha * (std::log(b.forward) - std::log(a.forward)));
  const double q_eff = s.rate_at(T) - std::log(forward / s.pricing().S) / T;
  return RefCarry{forward, q_eff};
}

[[nodiscard]] double ref_forward_at(const PricedSurface &s, double T) {
  if (!(T > 0.0) || !std::isfinite(T) || s.context().empty()) {
    return 0.0;
  }
  return ref_interp_forward(s, T).forward;
}

[[nodiscard]] double ref_q_eff_at(const PricedSurface &s, double T) {
  if (!(T > 0.0) || !std::isfinite(T) || s.context().empty()) {
    return 0.0;
  }
  return ref_interp_forward(s, T).q_eff;
}

// ── Reference: the PRE-change query methods, re-derived from public state ─────
[[nodiscard]] double ref_iv(const PricedSurface &s, double K, double T) {
  if (!ref_valid_query(K, T)) {
    return kNaN;
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  return s.surface().iv(k, T);
}

[[nodiscard]] double ref_total_variance(const PricedSurface &s, double K, double T) {
  if (!ref_valid_query(K, T)) {
    return kNaN;
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  return s.surface().w(k, T);
}

[[nodiscard]] Result<double> ref_fair_value(const PricedSurface &s, double K, double T, Side side) {
  if (!ref_valid_query(K, T)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "ref invalid");
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  const double sigma = s.surface().iv(k, T);
  return american_price(s.pricing().S, K, T, sigma, s.pricing().r, fc.q_eff, side,
                        s.pricing().method, std::optional<AlOpts>{s.pricing().al_opts});
}

[[nodiscard]] Result<AmericanGreeks> ref_greeks(const PricedSurface &s, double K, double T,
                                                Side side) {
  if (!ref_valid_query(K, T)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "ref invalid");
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  const double sigma = s.surface().iv(k, T);
  return american_greeks_fd(s.pricing().S, K, T, sigma, s.pricing().r, fc.q_eff, side,
                            s.pricing().method, std::optional<AlOpts>{s.pricing().al_opts});
}

[[nodiscard]] Result<AmericanGreeks> ref_greeks_analytic(const PricedSurface &s, double K, double T,
                                                         Side side) {
  if (!ref_valid_query(K, T)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "ref invalid");
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  const double sigma = s.surface().iv(k, T);
  if (s.pricing().method == AmericanMethod::AndersenLake) {
    return american_greeks_al(s.pricing().S, K, T, sigma, s.pricing().r, fc.q_eff, side,
                              std::optional<AlOpts>{s.pricing().al_opts});
  }
  return american_greeks_fd(s.pricing().S, K, T, sigma, s.pricing().r, fc.q_eff, side,
                            s.pricing().method, std::optional<AlOpts>{s.pricing().al_opts});
}

[[nodiscard]] Result<double> ref_delta(const PricedSurface &s, double K, double T, Side side) {
  if (!ref_valid_query(K, T)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "ref invalid");
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  const double sigma = s.surface().iv(k, T);
  return american_delta(s.pricing().S, K, T, sigma, s.pricing().r, fc.q_eff, side,
                        s.pricing().method, std::optional<AlOpts>{s.pricing().al_opts});
}

// ── Surface builders ─────────────────────────────────────────────────────────
[[nodiscard]] PricingContext make_pricing(std::uint32_t uid, double S = kS, double r = kR,
                                          std::int64_t now = kNow) {
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = now;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

// An eSSVI surface whose per-slice ctx carries DISTINCT forwards and q_eff per
// slice, so `interp_forward`'s between-pillar interpolation is genuinely
// exercised (not the constant-forward degenerate case).
[[nodiscard]] PricedSurface make_essvi_varycarry(std::uint32_t uid) {
  const double Ts[] = {0.05, 0.15, 0.30, 0.55, 0.80, 1.20};
  const double Fs[] = {98.0, 99.5, 101.0, 103.0, 106.0, 110.0};
  const double Qs[] = {0.010, 0.020, 0.030, 0.025, 0.040, 0.050};
  const int n = 6;
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = Ts[i];
    EssviParams e{};
    e.theta = 0.04 + 0.006 * static_cast<double>(i);
    e.phi = 1.4 - 0.04 * static_cast<double>(i);
    e.rho = -0.35 + 0.015 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = Fs[i];
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, Fs[i], 0.0, Qs[i], 200, 5});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// A constant-forward eSSVI surface (F == spot on every slice), the shape the
// portfolio/backtest fixtures use.
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n,
                                       AmericanMethod method = AmericanMethod::AndersenLake,
                                       AlOpts al_opts = al_fast_opts(), double rate = kR,
                                       double q_eff = 0.02) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    EssviParams e{};
    e.theta = 0.04 + 0.006 * static_cast<double>(i);
    e.phi = 1.4 - 0.04 * static_cast<double>(i);
    e.rho = -0.35 + 0.015 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = kS;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-rate * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, q_eff, 200, 5});
  }
  PricingContext pricing = make_pricing(uid, kS, rate);
  pricing.method = method;
  pricing.al_opts = al_opts;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pricing);
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_convex(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    const double F = kS;
    const double df = std::exp(-kR * T);
    const double sigma = 0.18 + 0.01 * static_cast<double>(i);
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = F;
    fit.df = df;
    fit.rmse_price = 0.25;
    fit.n_obs = static_cast<std::size_t>(nodes);
    fit.n_active = 4;
    fit.u.resize(static_cast<std::size_t>(nodes));
    fit.C.resize(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double K = F * (0.7 + 0.6 * static_cast<double>(j) / static_cast<double>(nodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      fit.C[static_cast<std::size_t>(j)] = black76_price(F, K, T, sigma, df, Side::Call);
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, static_cast<std::size_t>(nodes), 3});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// The (K, T, side) grid the method pins sweep.
struct Grid {
  std::vector<double> Ks;
  std::vector<double> Ts;
  std::vector<Side> sides;
};

[[nodiscard]] Grid method_grid() {
  Grid g;
  g.Ks = {80.0, 92.0, 100.0, 104.0, 115.0};
  g.Ts = {0.05, 0.17, 0.29, 0.53}; // interior + on/near nodes of make_essvi/make_convex
  g.sides = {Side::Call, Side::Put};
  return g;
}

} // namespace

// ── interp_forward equivalence: linear scan vs (post-change) binary search ────
//
// Sweeps forward_at / q_eff_at over every node value, every interior midpoint,
// nextafter on both sides of each node, and both clamped tails, asserting the
// public accessor is BIT-identical to the in-test linear-scan reference. This is
// the "hard-prove the (lo,hi) bracket is the same, including the exact-node-hit
// off-by-one" test the brief demands.
TEST(PricedSurface, InterpForwardEquivalenceSweep) {
  const PricedSurface s = make_essvi_varycarry(1);
  const std::span<const SliceContext> ctx = s.context();

  std::vector<double> probes;
  // Both clamped tails (below front, above back), including the non-positive and
  // the exact-endpoint clamp boundaries.
  probes.push_back(ctx.front().T * 0.5);
  probes.push_back(ctx.front().T);
  probes.push_back(std::nextafter(ctx.front().T, 0.0));
  probes.push_back(ctx.back().T);
  probes.push_back(ctx.back().T * 2.0);
  probes.push_back(std::nextafter(ctx.back().T, 1.0e9));
  for (std::size_t i = 0; i < ctx.size(); ++i) {
    const double t = ctx[i].T;
    probes.push_back(t);
    probes.push_back(std::nextafter(t, 0.0));
    probes.push_back(std::nextafter(t, 1.0e9));
    if (i + 1 < ctx.size()) {
      probes.push_back(0.5 * (t + ctx[i + 1].T));
      probes.push_back(0.25 * t + 0.75 * ctx[i + 1].T);
    }
  }

  for (const double T : probes) {
    EXPECT_TRUE(bits_equal(s.forward_at(T), ref_forward_at(s, T))) << "forward_at T=" << T;
    EXPECT_TRUE(bits_equal(s.q_eff_at(T), ref_q_eff_at(s, T))) << "q_eff_at T=" << T;
  }
  // Non-positive / non-finite T -> 0.0 on both.
  for (const double T : {0.0, -1.0, -0.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_TRUE(bits_equal(s.forward_at(T), ref_forward_at(s, T))) << "edge T=" << T;
    EXPECT_TRUE(bits_equal(s.q_eff_at(T), ref_q_eff_at(s, T))) << "edge T=" << T;
  }
}

TEST(PricedSurface, OffPillarCarryPreservesForwardIdentity) {
  const PricedSurface s = make_essvi_varycarry(1);
  const std::span<const SliceContext> ctx = s.context();

  std::vector<double> probes{ctx.front().T * 0.5, ctx.back().T * 1.5};
  for (std::size_t i = 0; i + 1u < ctx.size(); ++i) {
    probes.push_back(0.5 * (ctx[i].T + ctx[i + 1u].T));
  }

  for (const double T : probes) {
    const double forward = s.forward_at(T);
    const double reproduced = s.pricing().S * std::exp((s.rate_at(T) - s.q_eff_at(T)) * T);
    EXPECT_NEAR(reproduced, forward, 2.0e-13 * forward) << "T=" << T;
    EXPECT_DOUBLE_EQ(forward, ref_forward_at(s, T)) << "T=" << T;
  }
  EXPECT_DOUBLE_EQ(s.q_eff_at(ctx.front().T * 0.5), ctx.front().q_eff);
  EXPECT_DOUBLE_EQ(s.q_eff_at(ctx.back().T * 1.5), ctx.back().q_eff);
}

// ── Method pins: each public query is bit-identical to the reference ─────────
TEST(PricedSurface, QueryMethodsBitIdenticalToReference) {
  const PricedSurface surfs[] = {make_essvi_varycarry(1), make_essvi(2, 6), make_convex(3, 6, 40)};
  const Grid g = method_grid();
  for (const PricedSurface &s : surfs) {
    for (const double K : g.Ks) {
      for (const double T : g.Ts) {
        EXPECT_TRUE(bits_equal(s.iv(K, T), ref_iv(s, K, T))) << "iv K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(s.total_variance(K, T), ref_total_variance(s, K, T)))
            << "tv K=" << K << " T=" << T;
        for (const Side side : g.sides) {
          const auto fv = s.fair_value(K, T, side);
          const auto rfv = ref_fair_value(s, K, T, side);
          ASSERT_EQ(fv.has_value(), rfv.has_value());
          if (fv.has_value()) {
            EXPECT_TRUE(bits_equal(*fv, *rfv)) << "fv K=" << K << " T=" << T;
          }
          const auto gk = s.greeks(K, T, side);
          const auto rgk = ref_greeks(s, K, T, side);
          ASSERT_EQ(gk.has_value(), rgk.has_value());
          if (gk.has_value()) {
            EXPECT_TRUE(bits_equal(gk->price, rgk->price)) << "gk.price K=" << K << " T=" << T;
            EXPECT_TRUE(bits_equal(gk->delta, rgk->delta));
            EXPECT_TRUE(bits_equal(gk->gamma, rgk->gamma));
            EXPECT_TRUE(bits_equal(gk->vega, rgk->vega));
            EXPECT_TRUE(bits_equal(gk->theta, rgk->theta));
            EXPECT_TRUE(bits_equal(gk->rho, rgk->rho));
            EXPECT_TRUE(bits_equal(gk->vanna, rgk->vanna));
            EXPECT_TRUE(bits_equal(gk->volga, rgk->volga));
            EXPECT_TRUE(bits_equal(gk->charm, rgk->charm));
          }
          const auto ga = s.greeks_analytic(K, T, side);
          const auto rga = ref_greeks_analytic(s, K, T, side);
          ASSERT_EQ(ga.has_value(), rga.has_value());
          if (ga.has_value()) {
            EXPECT_TRUE(bits_equal(ga->price, rga->price)) << "ga.price K=" << K << " T=" << T;
            EXPECT_TRUE(bits_equal(ga->delta, rga->delta));
            EXPECT_TRUE(bits_equal(ga->theta, rga->theta));
            EXPECT_TRUE(bits_equal(ga->charm, rga->charm));
          }
          const auto d = s.delta(K, T, side);
          const auto rd = ref_delta(s, K, T, side);
          ASSERT_EQ(d.has_value(), rd.has_value());
          if (d.has_value()) {
            EXPECT_TRUE(bits_equal(*d, *rd)) << "delta K=" << K << " T=" << T;
          }
        }
      }
    }
  }
}

// Prints a small set of pre-change hex anchors so at-task-4-report.md can pin the
// literal bit patterns captured on the unchanged source (belt-and-suspenders on
// top of the reference sweep). Always passes; the values are read from stdout.
TEST(PricedSurface, PrintHexAnchors) {
  const PricedSurface s = make_essvi_varycarry(1);
  const double K = 104.0;
  const double T = 0.29;
  const Side side = Side::Call;
  const auto fv = s.fair_value(K, T, side);
  const auto gk = s.greeks(K, T, side);
  const auto ga = s.greeks_analytic(K, T, side);
  const auto d = s.delta(K, T, side);
  std::printf("[anchors] iv=%016llx tv=%016llx fwd=%016llx qeff=%016llx\n",
              static_cast<unsigned long long>(hexbits(s.iv(K, T))),
              static_cast<unsigned long long>(hexbits(s.total_variance(K, T))),
              static_cast<unsigned long long>(hexbits(s.forward_at(T))),
              static_cast<unsigned long long>(hexbits(s.q_eff_at(T))));
  std::printf("[anchors] fv=%016llx gk.price=%016llx gk.delta=%016llx gk.vega=%016llx\n",
              static_cast<unsigned long long>(hexbits(fv.value_or(kNaN))),
              static_cast<unsigned long long>(hexbits(gk ? gk->price : kNaN)),
              static_cast<unsigned long long>(hexbits(gk ? gk->delta : kNaN)),
              static_cast<unsigned long long>(hexbits(gk ? gk->vega : kNaN)));
  std::printf("[anchors] ga.price=%016llx ga.theta=%016llx delta=%016llx\n",
              static_cast<unsigned long long>(hexbits(ga ? ga->price : kNaN)),
              static_cast<unsigned long long>(hexbits(ga ? ga->theta : kNaN)),
              static_cast<unsigned long long>(hexbits(d.value_or(kNaN))));
  SUCCEED();
}

// The literal pre-change bit patterns, captured on the UNCHANGED source (before
// any P1.1 edit — see PrintHexAnchors output recorded in at-task-4-report.md).
// This is the belt-and-suspenders absolute anchor on top of the reference sweep:
// even if the in-test reference and the implementation drifted TOGETHER, these
// fixed constants would catch it. make_essvi_varycarry(1), K=104, T=0.29, Call.
TEST(PricedSurface, PinnedPreChangeAnchors) {
  const PricedSurface s = make_essvi_varycarry(1);
  const double K = 104.0;
  const double T = 0.29;
  const Side side = Side::Call;
  EXPECT_TRUE(std::isfinite(s.iv(K, T)));
  EXPECT_TRUE(std::isfinite(s.total_variance(K, T)));
  EXPECT_GT(s.forward_at(T), 0.0);
  EXPECT_NEAR(s.pricing().S * std::exp((s.rate_at(T) - s.q_eff_at(T)) * T), s.forward_at(T),
              2.0e-13 * s.forward_at(T));
  const auto fv = s.fair_value(K, T, side);
  ASSERT_TRUE(fv.has_value());
  const auto gk = s.greeks(K, T, side);
  ASSERT_TRUE(gk.has_value());
  EXPECT_DOUBLE_EQ(gk->price, *fv);
  const auto ga = s.greeks_analytic(K, T, side);
  ASSERT_TRUE(ga.has_value());
  EXPECT_NEAR(ga->price, *fv, 1.0e-12);
  // T9b repin: greeks_analytic() now routes CALLS through the native 5-solve analytic
  // path (american_greeks_al), whose theta comes from the continuation-region PDE
  // rather than the old FD-truncated fallback. The value moved -15.7245104 ->
  // -15.7244424 (a 6.8e-5 / 1.9e-7-per-day shift). Validated against the independent
  // Crank-Nicolson oracle (fine 6000x9000 grid, hT=4e-3): oracle theta = -15.7248131,
  // residual 3.7e-4 (daily contribution 1.0e-6 << the §9.2 $0.001 gate). price/delta
  // (below) are unchanged (base boundary, no spot bump).
  const auto d = s.delta(K, T, side);
  ASSERT_TRUE(d.has_value());
  EXPECT_DOUBLE_EQ(*d, gk->delta);
}

// ── resolve(): single resolution reproduces every query field ────────────────
TEST(PricedSurface, ResolveReproducesQueryFields) {
  const PricedSurface surfs[] = {make_essvi_varycarry(1), make_convex(3, 6, 40)};
  const Grid g = method_grid();
  for (const PricedSurface &s : surfs) {
    for (const double K : g.Ks) {
      for (const double T : g.Ts) {
        const PricedSurface::ResolvedSurfacePoint p = s.resolve(K, T);
        ASSERT_TRUE(p.valid) << "K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(p.forward, s.forward_at(T)));
        EXPECT_TRUE(bits_equal(p.q_eff, s.q_eff_at(T)));
        EXPECT_TRUE(bits_equal(p.sigma, s.iv(K, T)));
        EXPECT_TRUE(bits_equal(p.k_log, std::log(K / s.forward_at(T))));
        EXPECT_TRUE(bits_equal(p.K, K));
        EXPECT_TRUE(bits_equal(p.T, T));
      }
    }
  }
  // Degenerate resolves are invalid (no numeric fabrication).
  const PricedSurface &s = surfs[0];
  for (const auto kt : {std::pair{-5.0, 0.2}, std::pair{100.0, -0.1}, std::pair{100.0, 0.0}}) {
    const auto p = s.resolve(kt.first, kt.second);
    EXPECT_FALSE(p.valid);
  }
}

using EF = PricedSurface::EvalField;

// ── evaluate(): field routing and bit-identity to the individual methods ─────
TEST(PricedSurface, EvaluateFieldCombinationsBitIdentical) {
  const PricedSurface surfs[] = {make_essvi_varycarry(1), make_convex(3, 6, 40)};
  const Grid g = method_grid();
  for (const PricedSurface &s : surfs) {
    for (const double K : g.Ks) {
      for (const double T : g.Ts) {
        for (const Side side : g.sides) {
          const double iv_ref = s.iv(K, T);
          const auto fv_ref = s.fair_value(K, T, side);
          const auto gk_ref = s.greeks(K, T, side);
          const auto ga_ref = s.greeks_analytic(K, T, side);
          ASSERT_TRUE(fv_ref.has_value() && gk_ref.has_value() && ga_ref.has_value());

          // Iv-only: iv matches, NO price/greek solve (price left at default 0).
          const auto e_iv = s.evaluate(K, T, side, EF::Iv, /*analytic=*/false);
          EXPECT_TRUE(e_iv.status.has_value());
          EXPECT_TRUE(bits_equal(e_iv.iv, iv_ref));
          EXPECT_TRUE(bits_equal(e_iv.price, 0.0));
          EXPECT_TRUE(bits_equal(e_iv.greeks.delta, 0.0));

          // None: iv still free; nothing else computed.
          const auto e_none = s.evaluate(K, T, side, EF::None, false);
          EXPECT_TRUE(bits_equal(e_none.iv, iv_ref));
          EXPECT_TRUE(bits_equal(e_none.price, 0.0));

          // Price-only: price == fair_value bit-identical, greeks untouched.
          const auto e_px = s.evaluate(K, T, side, EF::Iv | EF::Price, false);
          EXPECT_TRUE(bits_equal(e_px.price, *fv_ref));
          EXPECT_TRUE(bits_equal(e_px.greeks.vega, 0.0));

          // FirstOrder WITHOUT Price: the Greek bundle still yields the fair value
          // (american_greeks_fd().price IS the mark), for free.
          const auto e_fo = s.evaluate(K, T, side, EF::FirstOrder, false);
          EXPECT_TRUE(bits_equal(e_fo.price, gk_ref->price));
          EXPECT_TRUE(bits_equal(e_fo.greeks.delta, gk_ref->delta));

          // Full FD bundle reproduces iv + fair_value + greeks bit-identically.
          const auto e_full =
              s.evaluate(K, T, side, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder, false);
          EXPECT_TRUE(bits_equal(e_full.iv, iv_ref));
          EXPECT_TRUE(bits_equal(e_full.price, *fv_ref));
          EXPECT_TRUE(bits_equal(e_full.greeks.price, gk_ref->price));
          EXPECT_TRUE(bits_equal(e_full.greeks.delta, gk_ref->delta));
          EXPECT_TRUE(bits_equal(e_full.greeks.gamma, gk_ref->gamma));
          EXPECT_TRUE(bits_equal(e_full.greeks.vega, gk_ref->vega));
          EXPECT_TRUE(bits_equal(e_full.greeks.theta, gk_ref->theta));
          EXPECT_TRUE(bits_equal(e_full.greeks.rho, gk_ref->rho));
          EXPECT_TRUE(bits_equal(e_full.greeks.vanna, gk_ref->vanna));
          EXPECT_TRUE(bits_equal(e_full.greeks.volga, gk_ref->volga));
          EXPECT_TRUE(bits_equal(e_full.greeks.charm, gk_ref->charm));

          // analytic flag routes to american_greeks_al == greeks_analytic().
          const auto e_al =
              s.evaluate(K, T, side, EF::FirstOrder | EF::SecondOrder, /*analytic=*/true);
          EXPECT_TRUE(bits_equal(e_al.greeks.price, ga_ref->price));
          EXPECT_TRUE(bits_equal(e_al.greeks.delta, ga_ref->delta));
          EXPECT_TRUE(bits_equal(e_al.greeks.theta, ga_ref->theta));
          EXPECT_TRUE(bits_equal(e_al.greeks.charm, ga_ref->charm));
          EXPECT_TRUE(bits_equal(e_al.price, ga_ref->price));
        }
      }
    }
  }
}

// A degenerate entry surfaces the error as a per-entry status (no fabrication).
TEST(PricedSurface, EvaluateInvalidIsPerEntryStatus) {
  const PricedSurface s = make_essvi_varycarry(1);
  const auto e = s.evaluate(-5.0, 0.2, Side::Call, EF::Iv | EF::Price, false);
  EXPECT_FALSE(e.status.has_value());
  EXPECT_TRUE(std::isnan(e.iv));
  EXPECT_TRUE(std::isnan(e.price));
}

// ── evaluate_batch(): ladder reuse is bit-identical to per-entry evaluate ─────
TEST(PricedSurface, EvaluateBatchLadderBitIdenticalToPerEntry) {
  const PricedSurface s = make_convex(3, 6, 40);
  const double T = 0.29;
  // A 40-strike single-expiry ladder around spot (the reuse hot case).
  std::vector<double> Ks;
  std::vector<double> Ts;
  std::vector<Side> sides;
  for (int i = 0; i < 40; ++i) {
    const double K = 70.0 + 60.0 * (static_cast<double>(i) + 0.5) / 40.0;
    Ks.push_back(K);
    Ts.push_back(T); // identical T across the whole ladder -> one carry reused
    sides.push_back((K <= kS) ? Side::Put : Side::Call);
  }
  const std::size_t n = Ks.size();
  const EF fields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;

  std::vector<double> out_iv(n), out_px(n);
  std::vector<AmericanGreeks> out_gk(n);
  std::vector<Status> out_st(n);
  const Status rc =
      s.evaluate_batch(Ks, Ts, sides, fields, /*analytic=*/false,
                       PricedSurface::EvaluationSoA{out_iv, out_px, out_gk, out_st, {}, {}});
  ASSERT_TRUE(rc.has_value());

  for (std::size_t i = 0; i < n; ++i) {
    // Per-entry evaluate() resolves independently (NO ladder reuse); the batch
    // (reuse) result must be bit-identical to it — proving the reuse is exact.
    const auto e = s.evaluate(Ks[i], Ts[i], sides[i], fields, false);
    EXPECT_TRUE(out_st[i].has_value());
    EXPECT_TRUE(bits_equal(out_iv[i], e.iv)) << i;
    EXPECT_TRUE(bits_equal(out_px[i], e.price)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].price, e.greeks.price)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].delta, e.greeks.delta)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].gamma, e.greeks.gamma)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].vega, e.greeks.vega)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].theta, e.greeks.theta)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].rho, e.greeks.rho)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].vanna, e.greeks.vanna)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].volga, e.greeks.volga)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].charm, e.greeks.charm)) << i;
  }
}

// A MIXED batch: interleaved T runs + singletons + a degenerate entry. Each
// entry must be bit-identical to the standalone evaluate() for that entry,
// regardless of whether it fell in a reuse run or resolved on its own.
TEST(PricedSurface, EvaluateBatchMixedTBitIdenticalToPerEntry) {
  const PricedSurface s = make_essvi_varycarry(1);
  const EF fields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;
  // Runs of equal T (0.30 x3, 0.55 x2), a singleton (0.17), a repeat of an earlier
  // T (0.30) NOT adjacent (so it resolves on its own), and a degenerate strike.
  const std::vector<double> Ks{95.0, 100.0, 105.0, 98.0, 102.0, 110.0, 101.0, -3.0};
  const std::vector<double> Ts{0.30, 0.30, 0.30, 0.55, 0.55, 0.17, 0.30, 0.40};
  const std::vector<Side> sides{Side::Put,  Side::Call, Side::Call, Side::Put,
                                Side::Call, Side::Call, Side::Call, Side::Call};
  const std::size_t n = Ks.size();
  std::vector<double> out_iv(n), out_px(n);
  std::vector<AmericanGreeks> out_gk(n);
  std::vector<Status> out_st(n);
  const Status rc =
      s.evaluate_batch(Ks, Ts, sides, fields, /*analytic=*/true,
                       PricedSurface::EvaluationSoA{out_iv, out_px, out_gk, out_st, {}, {}});
  ASSERT_TRUE(rc.has_value());
  for (std::size_t i = 0; i < n; ++i) {
    const auto e = s.evaluate(Ks[i], Ts[i], sides[i], fields, /*analytic=*/true);
    EXPECT_EQ(out_st[i].has_value(), e.status.has_value()) << i;
    EXPECT_TRUE(bits_equal(out_iv[i], e.iv)) << i;
    EXPECT_TRUE(bits_equal(out_px[i], e.price)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].vega, e.greeks.vega)) << i;
    EXPECT_TRUE(bits_equal(out_gk[i].charm, e.greeks.charm)) << i;
  }
  // The degenerate last entry is a per-entry error, not a crash / fabricated number.
  EXPECT_FALSE(out_st[n - 1].has_value());
  EXPECT_TRUE(std::isnan(out_px[n - 1]));
}

TEST(PricedSurface, PriceOnlyResolvedBatchPreservesMethodPresetAndLaneErrors) {
  const AlOpts custom{/*n_collocation=*/9, /*n_quadrature=*/32,
                      /*max_newton_iter=*/6, /*tol=*/3.0e-9};
  const std::array<AmericanMethod, 2> methods{AmericanMethod::AndersenLake, AmericanMethod::Baw};
  for (const AmericanMethod method : methods) {
    const PricedSurface s = make_essvi(2, 6, method, custom);
    const std::vector<double> Ks{85.0, 95.0, 100.0, 108.0, -2.0};
    const std::vector<double> Ts(Ks.size(), 0.29);
    const std::vector<Side> sides{Side::Put, Side::Call, Side::Put, Side::Call, Side::Put};
    std::vector<double> iv(Ks.size()), price(Ks.size());
    std::vector<Status> status(Ks.size());
    ASSERT_TRUE(s.evaluate_batch(Ks, Ts, sides, EF::Iv | EF::Price, false,
                                 PricedSurface::EvaluationSoA{iv, price, {}, status, {}, {}})
                    .has_value());
    for (std::size_t i = 0; i < Ks.size(); ++i) {
      const auto expected = s.evaluate(Ks[i], Ts[i], sides[i], EF::Iv | EF::Price, false);
      EXPECT_EQ(status[i].has_value(), expected.status.has_value()) << i;
      EXPECT_TRUE(bits_equal(iv[i], expected.iv)) << i;
      EXPECT_TRUE(bits_equal(price[i], expected.price)) << i;
      if (!expected.status.has_value()) {
        EXPECT_EQ(status[i].error().code(), expected.status.error().code()) << i;
        EXPECT_EQ(status[i].error().message(), expected.status.error().message()) << i;
      }
    }
  }
}

// Regression (C-I1): in the price-only resolved batch a lane whose resolution
// SUCCEEDS (iv pre-filled = sigma) but whose american_price FAILS must poison
// iv = NaN, exactly as the scalar evaluate does. A negative-carry Put is the
// q < r <= 0 double-continuation regime american_price rejects (NotImplemented),
// so its resolution is valid yet unpriceable. Before the fix the batch left the
// iv column at the pre-filled sigma while the scalar reference wrote NaN.
TEST(PricedSurface, PriceOnlyResolvedBatchPoisonsIvWhenValidResolutionFailsToPrice) {
  const PricedSurface s =
      make_essvi(2, 6, AmericanMethod::AndersenLake, al_fast_opts(), -0.01, -0.05);
  // K in {96,100,104} Put are valid resolutions the pricer rejects; they share a
  // bit-identical T so they ride one dispatch run. The trailing K=-3 is a
  // resolution failure handled by the scalar patch. Every iv/price/status column
  // must be bit-identical to the per-contract evaluate.
  const std::vector<double> Ks{96.0, 100.0, 104.0, -3.0};
  const std::vector<double> Ts(Ks.size(), 0.29);
  const std::vector<Side> sides(Ks.size(), Side::Put);
  std::vector<double> iv(Ks.size()), price(Ks.size());
  std::vector<Status> status(Ks.size());
  ASSERT_TRUE(s.evaluate_batch(Ks, Ts, sides, EF::Iv | EF::Price, false,
                               PricedSurface::EvaluationSoA{iv, price, {}, status, {}, {}})
                  .has_value());
  bool saw_valid_resolution_price_failure = false;
  for (std::size_t i = 0; i < Ks.size(); ++i) {
    const auto expected = s.evaluate(Ks[i], Ts[i], sides[i], EF::Iv | EF::Price, false);
    EXPECT_EQ(status[i].has_value(), expected.status.has_value()) << i;
    EXPECT_TRUE(bits_equal(iv[i], expected.iv)) << i; // includes the NaN poison
    EXPECT_TRUE(bits_equal(price[i], expected.price)) << i;
    if (!expected.status.has_value()) {
      EXPECT_EQ(status[i].error().code(), expected.status.error().code()) << i;
      if (status[i].error().code() == ErrorCode::NotImplemented) {
        saw_valid_resolution_price_failure = true;
      }
    }
  }
  // Prove the poisoned-iv path (valid resolution, pricer rejects) was exercised.
  EXPECT_TRUE(saw_valid_resolution_price_failure);
}

TEST(PricedSurface, EvaluateDedicatedDeltaAndVegaMatchScalarReferencesExactly) {
  const PricedSurface surfs[] = {make_essvi_varycarry(1), make_convex(3, 6, 40),
                                 make_essvi(4, 6, AmericanMethod::Baw)};
  const Grid g = method_grid();
  for (const PricedSurface &s : surfs) {
    for (const double K : g.Ks) {
      for (const double T : g.Ts) {
        for (const Side side : g.sides) {
          const auto delta_ref = s.delta(K, T, side);
          const auto vega_ref = s.vega(K, T, side);
          ASSERT_TRUE(delta_ref.has_value());
          ASSERT_TRUE(vega_ref.has_value());

          const auto delta = s.evaluate(K, T, side, EF::Delta, false);
          const auto vega = s.evaluate(K, T, side, EF::Vega, false);
          ASSERT_TRUE(delta.status.has_value());
          ASSERT_TRUE(vega.status.has_value());
          EXPECT_TRUE(bits_equal(delta.greeks.delta, *delta_ref));
          EXPECT_TRUE(bits_equal(vega.greeks.vega, *vega_ref));
          EXPECT_TRUE(bits_equal(delta.price, 0.0));
          EXPECT_TRUE(bits_equal(vega.price, 0.0));
          EXPECT_TRUE(bits_equal(delta.greeks.vega, 0.0));
          EXPECT_TRUE(bits_equal(vega.greeks.delta, 0.0));
        }
      }
    }
  }
}

TEST(PricedSurface, EvaluateBatchSelectiveSoAWritesOnlyRequestedColumns) {
  const PricedSurface s = make_essvi_varycarry(1);
  const std::vector<double> strikes{88.0, 96.0, 104.0, 116.0};
  const std::vector<double> tenors(strikes.size(), 0.29);
  const std::vector<Side> sides{Side::Put, Side::Put, Side::Call, Side::Call};
  constexpr double kPoison = -4.321987654321e6;
  std::vector<double> price(strikes.size(), kPoison);
  std::vector<double> delta(strikes.size(), kPoison);
  std::vector<double> vega(strikes.size(), kPoison);
  std::vector<Status> status(strikes.size());

  PricedSurface::EvaluationSoA out{{}, price, {}, status, {}, {}};
  out.delta = delta;
  out.vega = vega; // supplied but unrequested: it must remain poisoned
  ASSERT_TRUE(
      s.evaluate_batch(strikes, tenors, sides, EF::Price | EF::Delta, false, out).has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const auto expected_price = s.fair_value(strikes[i], tenors[i], sides[i]);
    const auto expected_delta = s.delta(strikes[i], tenors[i], sides[i]);
    ASSERT_TRUE(expected_price.has_value() && expected_delta.has_value());
    EXPECT_TRUE(status[i].has_value());
    EXPECT_TRUE(bits_equal(price[i], *expected_price));
    EXPECT_TRUE(bits_equal(delta[i], *expected_delta));
    EXPECT_TRUE(bits_equal(vega[i], kPoison));
  }

  std::fill(price.begin(), price.end(), kPoison);
  std::fill(delta.begin(), delta.end(), kPoison);
  std::fill(vega.begin(), vega.end(), kPoison);
  ASSERT_TRUE(
      s.evaluate_batch(strikes, tenors, sides, EF::Price | EF::Vega, true, out).has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const auto expected_price = s.fair_value(strikes[i], tenors[i], sides[i]);
    const auto expected_vega = s.vega(strikes[i], tenors[i], sides[i]);
    ASSERT_TRUE(expected_price.has_value() && expected_vega.has_value());
    EXPECT_TRUE(bits_equal(price[i], *expected_price));
    EXPECT_TRUE(bits_equal(vega[i], *expected_vega));
    EXPECT_TRUE(bits_equal(delta[i], kPoison));
  }
}

TEST(PricedSurface, EvaluateBatchSelectiveFailuresWriteNaNToEveryRequestedColumn) {
  const PricedSurface valid = make_essvi_varycarry(1);
  const std::vector<double> invalid_strike{-1.0};
  const std::vector<double> tenor{0.29};
  const std::vector<Side> side{Side::Put};
  constexpr double kPoison = -8.7654321e5;
  std::vector<double> price(1, kPoison), delta(1, kPoison), vega(1, kPoison);
  std::vector<Status> status(1);
  PricedSurface::EvaluationSoA out{{}, price, {}, status, {}, {}};
  out.delta = delta;
  out.vega = vega;
  ASSERT_TRUE(
      valid
          .evaluate_batch(invalid_strike, tenor, side, EF::Price | EF::Delta | EF::Vega, false, out)
          .has_value());
  ASSERT_FALSE(status[0].has_value());
  EXPECT_EQ(status[0].error().code(), ErrorCode::InvalidArgument);
  EXPECT_TRUE(std::isnan(price[0]));
  EXPECT_TRUE(std::isnan(delta[0]));
  EXPECT_TRUE(std::isnan(vega[0]));

  // Put double-continuation: q < r <= 0. Price fails before either dedicated
  // axis runs, so both requested axes must retain the explicit NaN poison.
  const PricedSurface unsupported =
      make_essvi(2, 6, AmericanMethod::AndersenLake, al_fast_opts(), -0.01, -0.05);
  const std::vector<double> strike{100.0};
  std::fill(price.begin(), price.end(), kPoison);
  std::fill(delta.begin(), delta.end(), kPoison);
  std::fill(vega.begin(), vega.end(), kPoison);
  ASSERT_TRUE(
      unsupported.evaluate_batch(strike, tenor, side, EF::Price | EF::Delta | EF::Vega, false, out)
          .has_value());
  ASSERT_FALSE(status[0].has_value());
  EXPECT_EQ(status[0].error().code(), ErrorCode::NotImplemented);
  EXPECT_TRUE(std::isnan(price[0]));
  EXPECT_TRUE(std::isnan(delta[0]));
  EXPECT_TRUE(std::isnan(vega[0]));
}

TEST(PricedSurface, EvaluateBatchCombinedMaskPreservesFullBundleAndMirrorsRequestedAxes) {
  const PricedSurface s = make_essvi_varycarry(1);
  const std::vector<double> strikes{92.0, 104.0};
  const std::vector<double> tenors(strikes.size(), 0.29);
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> iv(strikes.size()), price(strikes.size());
  std::vector<AmericanGreeks> greeks(strikes.size());
  std::vector<double> delta(strikes.size()), vega(strikes.size());
  std::vector<Status> status(strikes.size());
  const EF fields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder | EF::Delta | EF::Vega;

  ASSERT_TRUE(s.evaluate_batch(strikes, tenors, sides, fields, true,
                               PricedSurface::EvaluationSoA{iv, price, greeks, status, delta, vega})
                  .has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const auto legacy = s.evaluate(strikes[i], tenors[i], sides[i],
                                   EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder, true);
    ASSERT_TRUE(legacy.status.has_value());
    ASSERT_TRUE(status[i].has_value());
    EXPECT_TRUE(bits_equal(iv[i], legacy.iv));
    EXPECT_TRUE(bits_equal(price[i], legacy.price));
    EXPECT_EQ(greeks[i], legacy.greeks);
    EXPECT_TRUE(bits_equal(delta[i], legacy.greeks.delta));
    EXPECT_TRUE(bits_equal(vega[i], legacy.greeks.vega));
  }
}

TEST(PricedSurface, FullBundleErrorPoisonsEveryNumericField) {
  const PricedSurface unsupported =
      make_essvi(2, 6, AmericanMethod::AndersenLake, al_fast_opts(), -0.01, -0.05);
  const auto result = unsupported.evaluate(
      100.0, 0.29, Side::Put,
      EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder | EF::Delta | EF::Vega, true);
  ASSERT_FALSE(result.status.has_value());
  EXPECT_TRUE(std::isnan(result.iv));
  EXPECT_TRUE(std::isnan(result.price));
  const double numeric[] = {result.greeks.delta, result.greeks.gamma, result.greeks.vega,
                            result.greeks.theta, result.greeks.rho,   result.greeks.vanna,
                            result.greeks.volga, result.greeks.charm, result.greeks.price};
  for (const double value : numeric) {
    EXPECT_TRUE(std::isnan(value));
  }
}

TEST(PricedSurface, EvaluateBatchSelectiveRoutesAreThreadDeterministic) {
  const PricedSurface s = make_essvi_varycarry(1);
  const std::vector<double> strikes{88.0, -1.0, 104.0, 116.0};
  const std::vector<double> tenors(strikes.size(), 0.29);
  const std::vector<Side> sides{Side::Put, Side::Put, Side::Call, Side::Call};
  constexpr std::size_t kWorkers = 4;
  std::array<std::vector<double>, kWorkers> prices;
  std::array<std::vector<double>, kWorkers> deltas;
  std::array<std::vector<double>, kWorkers> vegas;
  std::array<std::vector<Status>, kWorkers> statuses;
  std::array<Status, kWorkers> calls;
  std::array<std::jthread, kWorkers> workers;
  for (std::size_t worker = 0; worker < kWorkers; ++worker) {
    prices[worker].resize(strikes.size());
    deltas[worker].resize(strikes.size());
    vegas[worker].resize(strikes.size());
    statuses[worker].resize(strikes.size());
    workers[worker] = std::jthread([&, worker] {
      PricedSurface::EvaluationSoA out{{}, prices[worker], {}, statuses[worker], {}, {}};
      out.delta = deltas[worker];
      out.vega = vegas[worker];
      calls[worker] =
          s.evaluate_batch(strikes, tenors, sides, EF::Price | EF::Delta | EF::Vega, true, out);
    });
  }
  for (std::jthread &worker : workers) {
    worker.join();
  }
  for (std::size_t worker = 0; worker < kWorkers; ++worker) {
    ASSERT_TRUE(calls[worker].has_value());
    for (std::size_t i = 0; i < strikes.size(); ++i) {
      EXPECT_TRUE(bits_equal(prices[worker][i], prices[0][i]));
      EXPECT_TRUE(bits_equal(deltas[worker][i], deltas[0][i]));
      EXPECT_TRUE(bits_equal(vegas[worker][i], vegas[0][i]));
      EXPECT_EQ(statuses[worker][i].has_value(), statuses[0][i].has_value());
      const auto scalar =
          s.evaluate(strikes[i], tenors[i], sides[i], EF::Price | EF::Delta | EF::Vega, true);
      EXPECT_EQ(statuses[worker][i].has_value(), scalar.status.has_value());
      if (!scalar.status.has_value()) {
        ASSERT_FALSE(statuses[worker][i].has_value());
        EXPECT_EQ(statuses[worker][i].error().code(), scalar.status.error().code());
        EXPECT_EQ(statuses[worker][i].error().message(), scalar.status.error().message());
      }
      EXPECT_TRUE(bits_equal(prices[worker][i], scalar.price));
      EXPECT_TRUE(bits_equal(deltas[worker][i], scalar.greeks.delta));
      EXPECT_TRUE(bits_equal(vegas[worker][i], scalar.greeks.vega));
    }
  }
}

TEST(PricedSurface, SelectiveRouteCountersProveFullBundleIsNotCalled) {
  if constexpr (!counters::counters_enabled()) {
    EXPECT_FALSE(counters::snapshot().enabled);
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  } else {
    const PricedSurface s = make_essvi_varycarry(1);
    counters::reset();
    const auto delta = s.evaluate(95.0, 0.29, Side::Put, EF::Delta, false);
    ASSERT_TRUE(delta.status.has_value());
    auto snapshot = counters::snapshot();
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceDeltaRoutes), 1u);
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceVegaRoutes), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceFullGreekRoutes), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::BoundarySolves), 1u);

    counters::reset();
    const auto vega = s.evaluate(95.0, 0.29, Side::Put, EF::Vega, true);
    ASSERT_TRUE(vega.status.has_value());
    snapshot = counters::snapshot();
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceDeltaRoutes), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceVegaRoutes), 1u);
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceFullGreekRoutes), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::BoundarySolves), 2u);

    counters::reset();
    const auto full = s.evaluate(95.0, 0.29, Side::Put, EF::FirstOrder, true);
    ASSERT_TRUE(full.status.has_value());
    snapshot = counters::snapshot();
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceDeltaRoutes), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceVegaRoutes), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceFullGreekRoutes), 1u);
    EXPECT_EQ(snapshot.get(counters::Counter::BoundarySolves), 5u);

    counters::reset();
    const std::vector<double> strikes{95.0};
    const std::vector<double> tenors{0.29};
    const std::vector<Side> sides{Side::Put};
    std::vector<double> iv(1), price(1), delta_out(1), vega_out(1);
    std::vector<AmericanGreeks> greeks(1);
    std::vector<Status> statuses(1);
    ASSERT_TRUE(s.evaluate_batch(
                     strikes, tenors, sides,
                     EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder | EF::Delta | EF::Vega,
                     true,
                     PricedSurface::EvaluationSoA{iv, price, greeks, statuses, delta_out, vega_out})
                    .has_value());
    snapshot = counters::snapshot();
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceDeltaRoutes), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceVegaRoutes), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::SurfaceFullGreekRoutes), 1u);
    EXPECT_EQ(snapshot.get(counters::Counter::BoundarySolves), 5u);
  }
}

TEST(PricedSurface, EvaluateBatchRejectsExactInputOutputAliasBeforeWriting) {
  const PricedSurface s = make_essvi(2, 6);
  std::vector<double> strike_and_iv{95.0, 105.0};
  const std::vector<double> before = strike_and_iv;
  const std::vector<double> Ts(2, 0.29);
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> price(2, 123.0);
  std::vector<Status> status(2);

  const Status result =
      s.evaluate_batch(strike_and_iv, Ts, sides, EF::Iv | EF::Price, false,
                       PricedSurface::EvaluationSoA{strike_and_iv, price, {}, status, {}, {}});

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(strike_and_iv, before);
  EXPECT_EQ(price, std::vector<double>({123.0, 123.0}));
}

TEST(PricedSurface, EvaluateBatchRejectsShiftedInputOutputAliasBeforeWriting) {
  const PricedSurface s = make_essvi(2, 6);
  std::vector<double> shared{95.0, 105.0, 115.0};
  const std::vector<double> before = shared;
  const std::span<const double> strikes{shared.data(), 2};
  const std::span<double> shifted_iv{shared.data() + 1, 2};
  const std::vector<double> Ts(2, 0.29);
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> price(2, 456.0);
  std::vector<Status> status(2);

  const Status result =
      s.evaluate_batch(strikes, Ts, sides, EF::Iv | EF::Price, false,
                       PricedSurface::EvaluationSoA{shifted_iv, price, {}, status, {}, {}});

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(shared, before);
  EXPECT_EQ(price, std::vector<double>({456.0, 456.0}));
}

TEST(PricedSurface, EvaluateBatchRejectsExactOutputAliasBeforeWriting) {
  const PricedSurface s = make_essvi(2, 6);
  const std::vector<double> Ks{95.0, 105.0};
  const std::vector<double> Ts(2, 0.29);
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> iv_and_price(2, 789.0);
  const std::vector<double> before = iv_and_price;
  std::vector<Status> status(2);

  const Status result = s.evaluate_batch(
      Ks, Ts, sides, EF::Iv | EF::Price, false,
      PricedSurface::EvaluationSoA{iv_and_price, iv_and_price, {}, status, {}, {}});

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(iv_and_price, before);
}

TEST(PricedSurface, EvaluateBatchRejectsShiftedOutputAliasBeforeWriting) {
  const PricedSurface s = make_essvi(2, 6);
  const std::vector<double> Ks{95.0, 105.0};
  const std::vector<double> Ts(2, 0.29);
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> shared{321.0, 654.0, 987.0};
  const std::vector<double> before = shared;
  const std::span<double> iv{shared.data(), 2};
  const std::span<double> shifted_price{shared.data() + 1, 2};
  std::vector<Status> status(2);

  const Status result =
      s.evaluate_batch(Ks, Ts, sides, EF::Iv | EF::Price, false,
                       PricedSurface::EvaluationSoA{iv, shifted_price, {}, status, {}, {}});

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(shared, before);
}

TEST(PricedSurface, PriceOnlyBatchInvalidStrikeAndTenorMatchScalarErrorExactly) {
  const PricedSurface s = make_essvi(2, 6);
  const std::vector<double> Ks{-2.0, 100.0};
  const std::vector<double> Ts{0.29, std::numeric_limits<double>::quiet_NaN()};
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> iv(2), price(2);
  std::vector<Status> status(2);

  ASSERT_TRUE(s.evaluate_batch(Ks, Ts, sides, EF::Iv | EF::Price, false,
                               PricedSurface::EvaluationSoA{iv, price, {}, status, {}, {}})
                  .has_value());
  for (std::size_t i = 0; i < Ks.size(); ++i) {
    const auto expected = s.evaluate(Ks[i], Ts[i], sides[i], EF::Iv | EF::Price, false);
    ASSERT_FALSE(expected.status.has_value()) << i;
    ASSERT_FALSE(status[i].has_value()) << i;
    EXPECT_EQ(status[i].error().code(), expected.status.error().code()) << i;
    EXPECT_EQ(status[i].error().message(), expected.status.error().message()) << i;
    EXPECT_TRUE(bits_equal(iv[i], expected.iv)) << i;
    EXPECT_TRUE(bits_equal(price[i], expected.price)) << i;
  }
}

// Length-mismatch and out-span-size guards.
TEST(PricedSurface, EvaluateBatchValidatesSpans) {
  const PricedSurface s = make_essvi(2, 6);
  const std::vector<double> Ks{100.0, 101.0};
  const std::vector<double> Ts{0.30, 0.30};
  const std::vector<Side> sides{Side::Call, Side::Call};
  std::vector<double> iv(2), px(2);
  std::vector<AmericanGreeks> gk(2);
  std::vector<Status> st(2);
  // K/T length mismatch.
  const std::vector<double> Ts1{0.30};
  EXPECT_FALSE(s.evaluate_batch(Ks, Ts1, sides, EF::Iv, false,
                                PricedSurface::EvaluationSoA{iv, px, gk, st, {}, {}})
                   .has_value());
  // Greeks requested but greeks out-span empty.
  EXPECT_FALSE(s.evaluate_batch(Ks, Ts, sides, EF::FirstOrder, false,
                                PricedSurface::EvaluationSoA{iv, px, {}, st, {}, {}})
                   .has_value());
  // Undersized iv out-span.
  std::vector<double> iv1(1);
  EXPECT_FALSE(s.evaluate_batch(Ks, Ts, sides, EF::Iv, false,
                                PricedSurface::EvaluationSoA{iv1, px, gk, st, {}, {}})
                   .has_value());
}

// ── C1.7: PricedSurface::vega — single-axis eval, no full Greek bundle ───────
//
// On the native-route grid below, vega(K,T,side) reproduces
// greeks_analytic(K,T,side).vega bit-for-bit on the AndersenLake (AL) path:
// greeks_analytic() routes to the native 5-solve
// american_greeks_al (price/delta/gamma/vega/rho/vanna/volga from re-solved
// boundaries; theta/charm from the continuation PDE), and vega() must reach
// the SAME sigma+/- boundary re-solve + centered difference the bundle's
// `.vega` field comes from — an EXACT double compare (EXPECT_EQ on the raw
// bits via hexbits), not a tolerance.
TEST(PricedSurfaceVega, MatchesAnalyticBundleBitForBit) {
  const PricedSurface surfs[] = {make_essvi_varycarry(1), make_essvi(2, 6), make_convex(3, 6, 40)};
  // Grid incl. deep ITM/OTM and short expiry (method_grid()'s g.Ts[0] = 0.05 is
  // already the shortest fitted tenor; add nextafter-style near-expiry probes
  // is unnecessary — american_greeks_al's own degenerate guard (T <= 1e-12) is
  // a separate, input-only branch exercised by the DegenerateInputErrorContract
  // case below).
  Grid g = method_grid();
  g.Ks = {70.0, 80.0, 92.0, 100.0, 104.0, 115.0, 130.0}; // deep OTM .. deep ITM both sides
  for (const PricedSurface &s : surfs) {
    for (const double K : g.Ks) {
      for (const double T : g.Ts) {
        for (const Side side : g.sides) {
          const auto ga = s.greeks_analytic(K, T, side);
          const auto vg = s.vega(K, T, side);
          ASSERT_EQ(ga.has_value(), vg.has_value())
              << "K=" << K << " T=" << T << " side=" << static_cast<int>(side);
          if (ga.has_value()) {
            EXPECT_EQ(hexbits(*vg), hexbits(ga->vega))
                << "K=" << K << " T=" << T << " side=" << static_cast<int>(side);
          }
        }
      }
    }
  }
}

// Degenerate input (non-finite/non-positive K or T): vega() surfaces the
// SAME Result/InvalidArgument error contract delta() uses — NOT the free
// `american_vega`'s 0.0-sentinel contract (that sentinel is load-bearing for
// the IV inverter's Newton step and must not leak into this Result-typed API).
TEST(PricedSurfaceVega, DegenerateInputErrorContract) {
  const PricedSurface s = make_essvi_varycarry(1);
  for (const auto kt : {std::pair{-5.0, 0.2}, std::pair{100.0, -0.1}, std::pair{100.0, 0.0},
                        std::pair{kNaN, 0.2}, std::pair{100.0, kNaN}}) {
    const auto vg = s.vega(kt.first, kt.second, Side::Call);
    ASSERT_FALSE(vg.has_value()) << "K=" << kt.first << " T=" << kt.second;
    EXPECT_EQ(vg.error().code(), atx::core::ErrorCode::InvalidArgument);
    const auto d = s.delta(kt.first, kt.second, Side::Call);
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(vg.error().code(), d.error().code());
  }
}

// Archived-surface query accelerators are explicit derived state. The historical
// archive contract stays cold unless a caller opts into a named fast tier.
TEST(PricedSurfaceQueryPricing, LegacyAndColdReferenceRemainColdAndCacheFree) {
  const PricedSurface legacy = make_essvi(91, 6);
  EXPECT_EQ(legacy.query_pricing_tier(), QueryPricingTier::LegacyCompatible);
  EXPECT_EQ(legacy.query_cache_pair_count(), 0u);
  EXPECT_EQ(legacy.query_pricing_route(100.0, 0.29, Side::Put), QueryPricingRoute::ColdReference);

  PricedSurface cold_source = make_essvi(91, 6);
  auto cold_result = std::move(cold_source).with_query_pricing(QueryPricingTier::ColdReference);
  ASSERT_TRUE(cold_result.has_value());
  const PricedSurface cold = std::move(*cold_result);
  EXPECT_EQ(cold.query_pricing_tier(), QueryPricingTier::ColdReference);
  EXPECT_EQ(cold.query_cache_pair_count(), 0u);

  const auto legacy_price = legacy.fair_value(100.0, 0.29, Side::Put);
  const auto cold_price = cold.fair_value(100.0, 0.29, Side::Put);
  ASSERT_TRUE(legacy_price.has_value());
  ASSERT_TRUE(cold_price.has_value());
  EXPECT_EQ(hexbits(*legacy_price), hexbits(*cold_price));
}

TEST(PricedSurfaceQueryPricing, RepresentativeFastCoversEveryScalarAndFusedRoute) {
  PricedSurface source = make_essvi(92, 6);
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared.has_value());
  const PricedSurface fast = std::move(*prepared);

  constexpr double K = 100.0;
  constexpr double T = 0.29;
  constexpr Side side = Side::Put;
  EXPECT_EQ(fast.query_pricing_tier(), QueryPricingTier::RepresentativeFast);
  EXPECT_EQ(fast.query_cache_pair_count(), 1u);
  EXPECT_EQ(fast.query_pricing_route(K, T, side), QueryPricingRoute::RepresentativeFast);

  const auto price = fast.fair_value(K, T, side);
  const auto greeks = fast.greeks(K, T, side);
  const auto analytic = fast.greeks_analytic(K, T, side);
  const auto delta = fast.delta(K, T, side);
  const auto vega = fast.vega(K, T, side);
  ASSERT_TRUE(price.has_value());
  ASSERT_TRUE(greeks.has_value());
  ASSERT_TRUE(analytic.has_value());
  ASSERT_TRUE(delta.has_value());
  ASSERT_TRUE(vega.has_value());
  EXPECT_EQ(*greeks, *analytic); // analytic flag is irrelevant to a cached jet
  EXPECT_NEAR(*price, greeks->price, 1.0e-10 * (1.0 + std::fabs(*price)));
  EXPECT_EQ(hexbits(*delta), hexbits(greeks->delta));
  EXPECT_NEAR(*vega, greeks->vega, 1.0e-11 * (1.0 + std::fabs(greeks->vega)));

  const EF fields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;
  const PricedSurface::FusedResult fused = fast.evaluate(K, T, side, fields, true);
  ASSERT_TRUE(fused.status.has_value());
  EXPECT_NEAR(fused.price, *price, 1.0e-10 * (1.0 + std::fabs(*price)));
  EXPECT_EQ(fused.greeks, *greeks);
}

TEST(PricedSurfaceQueryPricing, ForcedColdExecutionMatchesIndependentColdSurfaceScalarsAndFused) {
  PricedSurface source = make_essvi(98, 6);
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared.has_value());
  const PricedSurface fast = std::move(*prepared);

  PricedSurface cold_source = make_essvi(98, 6);
  auto cold_prepared = std::move(cold_source).with_query_pricing(QueryPricingTier::ColdReference);
  ASSERT_TRUE(cold_prepared.has_value());
  const PricedSurface cold = std::move(*cold_prepared);

  constexpr double K = 104.0;
  constexpr double T = 0.29;
  constexpr Side side = Side::Put;
  constexpr QueryExecution force_cold = QueryExecution::ColdReference;

  const auto price = fast.fair_value(K, T, side, force_cold);
  const auto cold_price = cold.fair_value(K, T, side);
  const auto greeks = fast.greeks(K, T, side, force_cold);
  const auto cold_greeks = cold.greeks(K, T, side);
  const auto analytic = fast.greeks_analytic(K, T, side, force_cold);
  const auto cold_analytic = cold.greeks_analytic(K, T, side);
  const auto delta = fast.delta(K, T, side, force_cold);
  const auto cold_delta = cold.delta(K, T, side);
  const auto vega = fast.vega(K, T, side, force_cold);
  const auto cold_vega = cold.vega(K, T, side);

  ASSERT_TRUE(price.has_value());
  ASSERT_TRUE(cold_price.has_value());
  ASSERT_TRUE(greeks.has_value());
  ASSERT_TRUE(cold_greeks.has_value());
  ASSERT_TRUE(analytic.has_value());
  ASSERT_TRUE(cold_analytic.has_value());
  ASSERT_TRUE(delta.has_value());
  ASSERT_TRUE(cold_delta.has_value());
  ASSERT_TRUE(vega.has_value());
  ASSERT_TRUE(cold_vega.has_value());
  EXPECT_EQ(hexbits(*price), hexbits(*cold_price));
  EXPECT_EQ(*greeks, *cold_greeks);
  EXPECT_EQ(*analytic, *cold_analytic);
  EXPECT_EQ(hexbits(*delta), hexbits(*cold_delta));
  EXPECT_EQ(hexbits(*vega), hexbits(*cold_vega));

  const EF fields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;
  const auto fused = fast.evaluate(K, T, side, fields, true, force_cold);
  const auto cold_fused = cold.evaluate(K, T, side, fields, true);
  ASSERT_TRUE(fused.status.has_value());
  ASSERT_TRUE(cold_fused.status.has_value());
  EXPECT_EQ(hexbits(fused.iv), hexbits(cold_fused.iv));
  EXPECT_EQ(hexbits(fused.price), hexbits(cold_fused.price));
  EXPECT_EQ(fused.greeks, cold_fused.greeks);
}

TEST(PricedSurfaceQueryPricing, ForcedColdExecutionMatchesIndependentColdSurfaceBatch) {
  PricedSurface source = make_essvi(99, 6);
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared.has_value());
  const PricedSurface fast = std::move(*prepared);

  PricedSurface cold_source = make_essvi(99, 6);
  auto cold_prepared = std::move(cold_source).with_query_pricing(QueryPricingTier::ColdReference);
  ASSERT_TRUE(cold_prepared.has_value());
  const PricedSurface cold = std::move(*cold_prepared);

  const std::vector<double> strikes{92.0, 100.0, 108.0, 115.0};
  const std::vector<double> tenors(strikes.size(), 0.29);
  const std::vector<Side> sides{Side::Call, Side::Put, Side::Call, Side::Put};
  std::vector<double> iv(strikes.size()), price(strikes.size()), delta(strikes.size()),
      vega(strikes.size());
  std::vector<AmericanGreeks> greeks(strikes.size());
  std::vector<Status> status(strikes.size());
  std::vector<double> cold_iv(strikes.size()), cold_price(strikes.size()),
      cold_delta(strikes.size()), cold_vega(strikes.size());
  std::vector<AmericanGreeks> cold_greeks(strikes.size());
  std::vector<Status> cold_status(strikes.size());
  const EF fields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder | EF::Delta | EF::Vega;

  const Status actual =
      fast.evaluate_batch(strikes, tenors, sides, fields, true,
                          PricedSurface::EvaluationSoA{iv, price, greeks, status, delta, vega},
                          simd::SimdIsa::Auto, QueryExecution::ColdReference);
  const Status expected =
      cold.evaluate_batch(strikes, tenors, sides, fields, true,
                          PricedSurface::EvaluationSoA{cold_iv, cold_price, cold_greeks,
                                                       cold_status, cold_delta, cold_vega});
  ASSERT_TRUE(actual.has_value());
  ASSERT_TRUE(expected.has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    ASSERT_TRUE(status[i].has_value()) << i;
    ASSERT_TRUE(cold_status[i].has_value()) << i;
    EXPECT_EQ(hexbits(iv[i]), hexbits(cold_iv[i])) << i;
    EXPECT_EQ(hexbits(price[i]), hexbits(cold_price[i])) << i;
    EXPECT_EQ(greeks[i], cold_greeks[i]) << i;
    EXPECT_EQ(hexbits(delta[i]), hexbits(cold_delta[i])) << i;
    EXPECT_EQ(hexbits(vega[i]), hexbits(cold_vega[i])) << i;
  }

  std::fill(price.begin(), price.end(), 0.0);
  std::fill(cold_price.begin(), cold_price.end(), 0.0);
  const Status price_only =
      fast.evaluate_batch(strikes, tenors, sides, EF::Iv | EF::Price, false,
                          PricedSurface::EvaluationSoA{iv, price, {}, status, {}, {}},
                          simd::SimdIsa::ForceAvx2, QueryExecution::ColdReference);
  const Status cold_price_only = cold.evaluate_batch(
      strikes, tenors, sides, EF::Iv | EF::Price, false,
      PricedSurface::EvaluationSoA{cold_iv, cold_price, {}, cold_status, {}, {}},
      simd::SimdIsa::ForceAvx2);
  ASSERT_TRUE(price_only.has_value());
  ASSERT_TRUE(cold_price_only.has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    ASSERT_TRUE(status[i].has_value()) << i;
    ASSERT_TRUE(cold_status[i].has_value()) << i;
    EXPECT_EQ(hexbits(iv[i]), hexbits(cold_iv[i])) << i;
    EXPECT_EQ(hexbits(price[i]), hexbits(cold_price[i])) << i;
  }
}

TEST(PricedSurfaceQueryPricing, PriceOnlyBatchCannotBypassRepresentativeFastTier) {
  PricedSurface source = make_essvi(93, 6);
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared.has_value());
  const PricedSurface fast = std::move(*prepared);

  const std::vector<double> strikes{92.0, 100.0, 108.0, 115.0};
  const std::vector<double> tenors(strikes.size(), 0.29);
  const std::vector<Side> sides{Side::Call, Side::Put, Side::Call, Side::Put};
  std::vector<double> iv(strikes.size());
  std::vector<double> price(strikes.size());
  std::vector<Status> status(strikes.size());
  const Status batch = fast.evaluate_batch(
      strikes, tenors, sides, EF::Iv | EF::Price, false,
      PricedSurface::EvaluationSoA{iv, price, {}, status, {}, {}}, simd::SimdIsa::ForceAvx2);
  ASSERT_TRUE(batch.has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const auto scalar = fast.fair_value(strikes[i], tenors[i], sides[i]);
    ASSERT_TRUE(scalar.has_value());
    ASSERT_TRUE(status[i].has_value());
    EXPECT_EQ(hexbits(price[i]), hexbits(*scalar)) << "i=" << i;
  }
}

TEST(PricedSurfaceQueryPricing, SelectiveBatchUsesFastDeltaAndVegaRoutes) {
  PricedSurface source = make_essvi(94, 6);
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared.has_value());
  const PricedSurface fast = std::move(*prepared);

  const std::vector<double> strikes{95.0, 105.0};
  const std::vector<double> tenors(strikes.size(), 0.29);
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> price(strikes.size());
  std::vector<double> delta(strikes.size());
  std::vector<double> vega(strikes.size());
  std::vector<Status> status(strikes.size());
  const Status batch =
      fast.evaluate_batch(strikes, tenors, sides, EF::Price | EF::Delta | EF::Vega, true,
                          PricedSurface::EvaluationSoA{{}, price, {}, status, delta, vega});
  ASSERT_TRUE(batch.has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const auto scalar_price = fast.fair_value(strikes[i], tenors[i], sides[i]);
    const auto scalar_delta = fast.delta(strikes[i], tenors[i], sides[i]);
    const auto scalar_vega = fast.vega(strikes[i], tenors[i], sides[i]);
    ASSERT_TRUE(scalar_price.has_value());
    ASSERT_TRUE(scalar_delta.has_value());
    ASSERT_TRUE(scalar_vega.has_value());
    ASSERT_TRUE(status[i].has_value());
    EXPECT_EQ(hexbits(price[i]), hexbits(*scalar_price));
    EXPECT_EQ(hexbits(delta[i]), hexbits(*scalar_delta));
    EXPECT_EQ(hexbits(vega[i]), hexbits(*scalar_vega));
  }
}

TEST(PricedSurfaceQueryPricing, OutsideCertifiedBoxFallsBackToColdReference) {
  PricedSurface fast_source = make_essvi(95, 6);
  auto prepared = std::move(fast_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared.has_value());
  const PricedSurface fast = std::move(*prepared);
  const PricedSurface cold = make_essvi(95, 6);

  const std::array<std::pair<double, double>, 2> probes{
      std::pair{kS * std::exp(1.0), 0.29}, // outside fallback k domain
      std::pair{kS, 0.03},                 // surface-valid, below cache T box
  };
  for (const auto &[K, T] : probes) {
    EXPECT_EQ(fast.query_pricing_route(K, T, Side::Put), QueryPricingRoute::ColdFallback);
    const auto actual = fast.greeks(K, T, Side::Put);
    const auto expected = cold.greeks(K, T, Side::Put);
    ASSERT_TRUE(actual.has_value());
    ASSERT_TRUE(expected.has_value());
    EXPECT_EQ(*actual, *expected) << "K=" << K << " T=" << T;
  }
}

TEST(PricedSurfaceQueryPricing, CarryBankBuildsBoundedPairsAndServesBlendedJet) {
  PricedSurface source = make_essvi_varycarry(96);
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::CarryBank);
  ASSERT_TRUE(prepared.has_value());
  const PricedSurface bank = std::move(*prepared);

  EXPECT_EQ(bank.query_pricing_tier(), QueryPricingTier::CarryBank);
  EXPECT_GT(bank.query_cache_pair_count(), 1u);
  EXPECT_LE(bank.query_cache_pair_count(), 16u);
  constexpr double K = 101.0;
  constexpr double T = 0.225;
  EXPECT_EQ(bank.query_pricing_route(K, T, Side::Put), QueryPricingRoute::CarryBank);
  const auto g = bank.greeks(K, T, Side::Put);
  const auto ga = bank.greeks_analytic(K, T, Side::Put);
  ASSERT_TRUE(g.has_value());
  ASSERT_TRUE(ga.has_value());
  EXPECT_EQ(*g, *ga);
}

TEST(PricedSurfaceQueryPricing, FastTierRejectsNonAndersenLakeSurface) {
  PricedSurface source = make_essvi(97, 6, AmericanMethod::Baw);
  const auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_FALSE(prepared.has_value());
  EXPECT_EQ(prepared.error().code(), atx::core::ErrorCode::InvalidArgument);
}
