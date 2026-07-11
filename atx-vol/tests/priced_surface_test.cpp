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
// OLD linear forward scan written out verbatim below. The reference is not the
// new code path, so a bit-identical match across a (K, T, side) grid proves the
// refactor preserved every bit. The reference tests were captured and made green
// against the PRE-change source first (see at-task-4-report.md).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
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

[[nodiscard]] RefCarry ref_interp_forward(const PricedSurface& s, double T) {
  const std::span<const SliceContext> ctx = s.context();
  const SliceContext& first = ctx.front();
  const SliceContext& last = ctx.back();
  if (T <= first.T) {
    return RefCarry{first.forward, first.q_eff};
  }
  if (T >= last.T) {
    return RefCarry{last.forward, last.q_eff};
  }
  std::size_t hi = 0;
  while (hi < ctx.size() && ctx[hi].T <= T) {
    ++hi;
  }
  const std::size_t lo = hi - 1;
  const SliceContext& a = ctx[lo];
  const SliceContext& b = ctx[hi];
  const double span = b.T - a.T;
  const double alpha = (span > 0.0) ? (T - a.T) / span : 0.0;
  return RefCarry{a.forward + alpha * (b.forward - a.forward),
                  a.q_eff + alpha * (b.q_eff - a.q_eff)};
}

[[nodiscard]] double ref_forward_at(const PricedSurface& s, double T) {
  if (!(T > 0.0) || s.context().empty()) {
    return 0.0;
  }
  return ref_interp_forward(s, T).forward;
}

[[nodiscard]] double ref_q_eff_at(const PricedSurface& s, double T) {
  if (!(T > 0.0) || s.context().empty()) {
    return 0.0;
  }
  return ref_interp_forward(s, T).q_eff;
}

// ── Reference: the PRE-change query methods, re-derived from public state ─────
[[nodiscard]] double ref_iv(const PricedSurface& s, double K, double T) {
  if (!ref_valid_query(K, T)) {
    return kNaN;
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  return s.surface().iv(k, T);
}

[[nodiscard]] double ref_total_variance(const PricedSurface& s, double K, double T) {
  if (!ref_valid_query(K, T)) {
    return kNaN;
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  return s.surface().w(k, T);
}

[[nodiscard]] Result<double> ref_fair_value(const PricedSurface& s, double K, double T, Side side) {
  if (!ref_valid_query(K, T)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "ref invalid");
  }
  const RefCarry fc = ref_interp_forward(s, T);
  const double k = std::log(K / fc.forward);
  const double sigma = s.surface().iv(k, T);
  return american_price(s.pricing().S, K, T, sigma, s.pricing().r, fc.q_eff, side,
                        s.pricing().method, std::optional<AlOpts>{s.pricing().al_opts});
}

[[nodiscard]] Result<AmericanGreeks> ref_greeks(const PricedSurface& s, double K, double T,
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

[[nodiscard]] Result<AmericanGreeks> ref_greeks_analytic(const PricedSurface& s, double K, double T,
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

[[nodiscard]] Result<double> ref_delta(const PricedSurface& s, double K, double T, Side side) {
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
// slice, so `interp_forward`'s linear-between interpolation is genuinely
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
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n) {
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
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 200, 5});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
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
  g.Ts = {0.05, 0.17, 0.29, 0.53};  // interior + on/near nodes of make_essvi/make_convex
  g.sides = {Side::Call, Side::Put};
  return g;
}

}  // namespace

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
    EXPECT_TRUE(bits_equal(s.forward_at(T), ref_forward_at(s, T)))
        << "forward_at T=" << T;
    EXPECT_TRUE(bits_equal(s.q_eff_at(T), ref_q_eff_at(s, T))) << "q_eff_at T=" << T;
  }
  // Non-positive / non-finite T -> 0.0 on both.
  for (const double T : {0.0, -1.0, -0.0}) {
    EXPECT_TRUE(bits_equal(s.forward_at(T), ref_forward_at(s, T))) << "edge T=" << T;
    EXPECT_TRUE(bits_equal(s.q_eff_at(T), ref_q_eff_at(s, T))) << "edge T=" << T;
  }
}

// ── Method pins: each public query is bit-identical to the reference ─────────
TEST(PricedSurface, QueryMethodsBitIdenticalToReference) {
  const PricedSurface surfs[] = {make_essvi_varycarry(1), make_essvi(2, 6), make_convex(3, 6, 40)};
  const Grid g = method_grid();
  for (const PricedSurface& s : surfs) {
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
  EXPECT_EQ(hexbits(s.iv(K, T)), 0x3fdad3d2c6635f6cULL);
  EXPECT_EQ(hexbits(s.total_variance(K, T)), 0x3faa16eba81a34c7ULL);
  EXPECT_EQ(hexbits(s.forward_at(T)), 0x405939999999999aULL);
  EXPECT_EQ(hexbits(s.q_eff_at(T)), 0x3f9e098ead65b7a2ULL);
  const auto fv = s.fair_value(K, T, side);
  ASSERT_TRUE(fv.has_value());
  EXPECT_EQ(hexbits(*fv), 0x401d9b28c191f3f5ULL);
  const auto gk = s.greeks(K, T, side);
  ASSERT_TRUE(gk.has_value());
  EXPECT_EQ(hexbits(gk->price), 0x401d9b28c191f3f5ULL);
  EXPECT_EQ(hexbits(gk->delta), 0x3fdea32238037610ULL);
  EXPECT_EQ(hexbits(gk->vega), 0x40354a37fee9bd09ULL);
  const auto ga = s.greeks_analytic(K, T, side);
  ASSERT_TRUE(ga.has_value());
  EXPECT_EQ(hexbits(ga->price), 0x401d9b28c191f3f5ULL);
  // T9b repin: greeks_analytic() now routes CALLS through the native 5-solve analytic
  // path (american_greeks_al), whose theta comes from the continuation-region PDE
  // rather than the old FD-truncated fallback. The value moved -15.7245104 ->
  // -15.7244424 (a 6.8e-5 / 1.9e-7-per-day shift). Validated against the independent
  // Crank-Nicolson oracle (fine 6000x9000 grid, hT=4e-3): oracle theta = -15.7248131,
  // residual 3.7e-4 (daily contribution 1.0e-6 << the §9.2 $0.001 gate). price/delta
  // (below) are unchanged (base boundary, no spot bump).
  EXPECT_EQ(hexbits(ga->theta), 0xc02f72ea1df85bd8ULL);
  const auto d = s.delta(K, T, side);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(hexbits(*d), 0x3fdea32238037610ULL);
}

// ── resolve(): single resolution reproduces every query field ────────────────
TEST(PricedSurface, ResolveReproducesQueryFields) {
  const PricedSurface surfs[] = {make_essvi_varycarry(1), make_convex(3, 6, 40)};
  const Grid g = method_grid();
  for (const PricedSurface& s : surfs) {
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
  const PricedSurface& s = surfs[0];
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
  for (const PricedSurface& s : surfs) {
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
                       PricedSurface::EvaluationSoA{out_iv, out_px, out_gk, out_st});
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
                       PricedSurface::EvaluationSoA{out_iv, out_px, out_gk, out_st});
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
  EXPECT_FALSE(
      s.evaluate_batch(Ks, Ts1, sides, EF::Iv, false, PricedSurface::EvaluationSoA{iv, px, gk, st})
          .has_value());
  // Greeks requested but greeks out-span empty.
  EXPECT_FALSE(s.evaluate_batch(Ks, Ts, sides, EF::FirstOrder, false,
                                PricedSurface::EvaluationSoA{iv, px, {}, st})
                   .has_value());
  // Undersized iv out-span.
  std::vector<double> iv1(1);
  EXPECT_FALSE(s.evaluate_batch(Ks, Ts, sides, EF::Iv, false,
                                PricedSurface::EvaluationSoA{iv1, px, gk, st})
                   .has_value());
}

// ── C1.7: PricedSurface::vega — single-axis eval, no full Greek bundle ───────
//
// vega(K,T,side) must reproduce greeks_analytic(K,T,side).vega bit-for-bit on
// the AndersenLake (AL) path: greeks_analytic() routes to the native 5-solve
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
  for (const PricedSurface& s : surfs) {
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
