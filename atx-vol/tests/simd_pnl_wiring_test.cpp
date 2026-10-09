// End-to-end parity gate for wiring the vectorized Taylor P&L-explain kernel into
// PortfolioPricer::pnl_explain (wiring finding 1).
//
// PortfolioPricer::scatter_pnl_rows now routes the eight per-position Taylor
// components through simd::pnl_taylor_explain_batch instead of the old fused scalar
// loop. These tests drive the FULL pnl_explain path on real American surfaces and
// check the frame against an INDEPENDENT scalar reference built from the base
// surface's own Greeks (bit-identical to the deduped base bundle the pricer solves)
// and the frame's own state-move columns:
//
//   * MixedMove_...: every one of the eight component columns matches the scalar
//     reference to the simd_pnl_test bound (1e-7 abs + 1e-12 rel) — the AVX2
//     main-lane parity class — and the eight components + unexplained reconstruct
//     pnl_total per row.
//   * ScalarTail_...: a <4-position book runs entirely on the kernel's scalar tail,
//     where the decomposition is bit-identical to the reference (the previous scalar
//     path), demonstrating the bit-identical scalar-tail class directly.
//   * InertAxes_...: a shock that is exactly zero leaves its component exactly 0.0
//     (the kernel's multiply-by-zero is exact on every lane).
//   * BitIdenticalAcrossWorkerCounts: the whole frame is bit-identical at n_threads
//     1 vs 8 — the kernel runs the batch in one serial call, so its lane grouping,
//     and every bit, is independent of worker count.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"          // al_fast_opts, AmericanGreeks, AmericanMethod
#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"         // CurveSurface, EssviCurve, EssviParams

using namespace atx::vol;

namespace {

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

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid, double S, double r,
                                          std::int64_t now) {
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = now;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

// Coherent eSSVI priced surface with genuine American early-exercise premium. A
// skewed smile (flat_smile=false) makes a coherent spot move also shift IV at a
// fixed strike, so a mixed shock lights up every Taylor axis; a flat smile
// (flat_smile=true) isolates the spot axis (a pure spot move leaves d_vol == 0).
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n, double theta_bump, double S,
                                       double r, std::int64_t now, bool flat_smile = false) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  constexpr double q_eff = 0.02;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = S * std::exp((r - q_eff) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + theta_bump;
    e.phi = flat_smile ? 0.0 : (1.5 - 0.05 * static_cast<double>(i));
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = F;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-r * T)));
    ctx.push_back(SliceContext{T, F, 0.0, q_eff, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid, S, r, now));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] SurfaceSet set_of(const PricedSurface &s) {
  const PricedSurface *p = &s;
  auto ss = SurfaceSet::create(std::span<const PricedSurface *const>(&p, 1));
  EXPECT_TRUE(ss.has_value());
  return std::move(*ss);
}

// eff_multiplier mirror (portfolio_pricer.cpp): non-positive / non-finite -> 100.
[[nodiscard]] double eff_mult(double m) noexcept {
  return (std::isfinite(m) && m > 0.0) ? m : 100.0;
}

// A book straddling the forward across the term structure, both sides, long/short
// quantities, so the weight (qty*multiplier) exercises positive and negative w.
[[nodiscard]] std::vector<Position> make_book(std::size_t target) {
  std::vector<Position> book;
  std::uint64_t id = 0;
  const double ts[] = {0.05, 0.15, 0.25, 0.35};
  const double ms[] = {0.9, 0.97, 1.0, 1.03, 1.1};
  for (double T : ts) {
    for (double m : ms) {
      const double K = kS * m;
      const Side side = (m <= 1.0) ? Side::Put : Side::Call;
      const double qty = (id % 2 == 0) ? (2.0 + static_cast<double>(id)) : -(1.0 + static_cast<double>(id));
      book.push_back({id, {1, K, T, side}, qty, 100.0});
      ++id;
      if (book.size() >= target) {
        return book;
      }
    }
  }
  return book;
}

// The old fused-scalar decomposition, computed independently from the base
// surface's Greeks and the frame's per-share state moves.
struct RefRow {
  double pnl_delta, pnl_gamma, pnl_vega, pnl_volga, pnl_vanna, pnl_theta, pnl_rho, pnl_charm;
};

[[nodiscard]] RefRow reference_row(const AmericanGreeks &g, double dS, double dvol, double dt,
                                   double dr, double w) {
  const double pd = g.delta * dS;
  const double pg = 0.5 * g.gamma * dS * dS;
  const double pv = g.vega * dvol;
  const double pvol = 0.5 * g.volga * dvol * dvol;
  const double pvanna = g.vanna * dS * dvol;
  const double pth = g.theta * dt;
  const double prho = g.rho * dr;
  const double pcharm = g.charm * dS * dt;
  return {w * pd,  w * pg,   w * pv,   w * pvol,
          w * pvanna, w * pth, w * prho, w * pcharm};
}

// Combined absolute + relative closeness, matching simd_pnl_test's bound.
void expect_close(double got, double want, const char *col, std::size_t i) {
  constexpr double kAbs = 1e-7;
  constexpr double kRel = 1e-12;
  EXPECT_LE(std::abs(got - want), kAbs + kRel * std::abs(want))
      << "col=" << col << " i=" << i << " got=" << got << " want=" << want;
}

} // namespace

// ── The eight component columns match the independent scalar reference; the eight
//    plus unexplained reconstruct pnl_total (mixed spot+vol+rate+time move). ──────
TEST(SimdPnlWiring, MixedMove_ComponentsMatchScalarReference) {
  const double dS = 0.75;
  const double dr = 1.5e-4;
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  const PricedSurface base = make_essvi(1, 5, 0.0, kS, kR, kNow);
  const PricedSurface shifted = make_essvi(1, 5, 0.0015, kS + dS, kR + dr, kNow + one_hour);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);

  const std::vector<Position> book = make_book(19); // >4 groups + a scalar-tail lane
  ASSERT_GT(book.size(), 12u);
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value()) << er.error().to_string();
  const PnlFrame &f = *er;
  ASSERT_EQ(f.size(), book.size());

  int active_vega = 0;
  int active_charm = 0;
  int n_ok = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (f.status[i] != PriceStatus::Ok) {
      continue;
    }
    ++n_ok;
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * eff_mult(p.multiplier);
    const RefRow r = reference_row(*g, f.d_spot[i], f.d_vol[i], f.d_time[i], f.d_rate[i], w);

    expect_close(f.pnl_delta[i], r.pnl_delta, "delta", i);
    expect_close(f.pnl_gamma[i], r.pnl_gamma, "gamma", i);
    expect_close(f.pnl_vega[i], r.pnl_vega, "vega", i);
    expect_close(f.pnl_volga[i], r.pnl_volga, "volga", i);
    expect_close(f.pnl_vanna[i], r.pnl_vanna, "vanna", i);
    expect_close(f.pnl_theta[i], r.pnl_theta, "theta", i);
    expect_close(f.pnl_rho[i], r.pnl_rho, "rho", i);
    expect_close(f.pnl_charm[i], r.pnl_charm, "charm", i);

    // Reconstruction: the eight components + unexplained sum to pnl_total.
    const double scale = std::abs(f.pnl_delta[i]) + std::abs(f.pnl_gamma[i]) +
                         std::abs(f.pnl_vega[i]) + std::abs(f.pnl_volga[i]) +
                         std::abs(f.pnl_vanna[i]) + std::abs(f.pnl_theta[i]) +
                         std::abs(f.pnl_rho[i]) + std::abs(f.pnl_charm[i]) +
                         std::abs(f.pnl_unexplained[i]);
    const double recon = f.pnl_delta[i] + f.pnl_gamma[i] + f.pnl_vega[i] + f.pnl_volga[i] +
                         f.pnl_vanna[i] + f.pnl_theta[i] + f.pnl_rho[i] + f.pnl_charm[i] +
                         f.pnl_unexplained[i];
    EXPECT_LE(std::abs(recon - f.pnl_total[i]), 1e-9 * scale + 1e-9) << i;

    if (f.pnl_vega[i] != 0.0) {
      ++active_vega;
    }
    if (f.pnl_charm[i] != 0.0) {
      ++active_charm;
    }
  }
  ASSERT_GT(n_ok, 10);
  // A mixed move must genuinely exercise the vol and cross (charm) axes, else the
  // component parity above is vacuous on those terms.
  EXPECT_GT(active_vega, 0) << "no vega P&L — mixed move did not shift vol";
  EXPECT_GT(active_charm, 0) << "no charm P&L — mixed move did not cross spot*time";
}

// ── A <4-position book runs entirely on the kernel's scalar tail: the components
//    are BIT-IDENTICAL to the scalar reference (== the previous fused loop). ──────
TEST(SimdPnlWiring, ScalarTail_ComponentsBitIdenticalToReference) {
  const double dS = 0.6;
  const double dr = 1e-4;
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  const PricedSurface base = make_essvi(1, 5, 0.0, kS, kR, kNow);
  const PricedSurface shifted = make_essvi(1, 5, 0.001, kS + dS, kR + dr, kNow + one_hour);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);

  const std::vector<Position> book = make_book(3); // 3 < 4 => all scalar-tail lanes
  ASSERT_EQ(book.size(), 3u);
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value()) << er.error().to_string();
  const PnlFrame &f = *er;

  int n_ok = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (f.status[i] != PriceStatus::Ok) {
      continue;
    }
    ++n_ok;
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * eff_mult(p.multiplier);
    const RefRow r = reference_row(*g, f.d_spot[i], f.d_vol[i], f.d_time[i], f.d_rate[i], w);
    EXPECT_TRUE(bits_equal(f.pnl_delta[i], r.pnl_delta)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_gamma[i], r.pnl_gamma)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_vega[i], r.pnl_vega)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_volga[i], r.pnl_volga)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_vanna[i], r.pnl_vanna)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_theta[i], r.pnl_theta)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_rho[i], r.pnl_rho)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_charm[i], r.pnl_charm)) << i;
  }
  ASSERT_EQ(n_ok, 3);
}

// ── A pure spot move on a FLAT smile leaves every non-spot component exactly 0.0
//    (the kernel's multiply-by-zero shock is exact on every lane, AVX2 or scalar). ─
TEST(SimdPnlWiring, InertAxesExactlyZero) {
  const double dS = 0.5;
  // Flat smile => a coherent spot move does not shift fixed-strike IV, so d_vol,
  // and no rate/time move either, so every non-spot shock is EXACTLY zero.
  const PricedSurface base = make_essvi(1, 5, 0.0, kS, kR, kNow, /*flat_smile=*/true);
  const PricedSurface shifted = make_essvi(1, 5, 0.0, kS + dS, kR, kNow, /*flat_smile=*/true);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);

  const std::vector<Position> book = make_book(17);
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value()) << er.error().to_string();
  const PnlFrame &f = *er;

  int active_spot = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (f.status[i] != PriceStatus::Ok) {
      continue;
    }
    EXPECT_EQ(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_volga[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    if (f.pnl_delta[i] != 0.0) {
      ++active_spot;
    }
  }
  EXPECT_GT(active_spot, 0) << "pure-spot move produced no delta P&L";
}

// ── The frame is bit-identical across worker counts (serial kernel + disjoint
//    scatter writes). ──────────────────────────────────────────────────────────────
TEST(SimdPnlWiring, BitIdenticalAcrossWorkerCounts) {
  const double dS = 0.75;
  const double dr = 1.5e-4;
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  const PricedSurface base = make_essvi(1, 5, 0.0, kS, kR, kNow);
  const PricedSurface shifted = make_essvi(1, 5, 0.0015, kS + dS, kR + dr, kNow + one_hour);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);

  const std::vector<Position> book = make_book(19);
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  PriceOptions o1;
  o1.n_threads = 1;
  PriceOptions o8;
  o8.n_threads = 8;
  auto a = pricer.pnl_explain(bset, sset, o1);
  auto b = pricer.pnl_explain(bset, sset, o8);
  ASSERT_TRUE(a.has_value() && b.has_value());
  const PnlFrame &fa = *a;
  const PnlFrame &fb = *b;
  ASSERT_EQ(fa.size(), fb.size());

  for (std::size_t i = 0; i < fa.size(); ++i) {
    EXPECT_EQ(fa.status[i], fb.status[i]) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_total[i], fb.pnl_total[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_delta[i], fb.pnl_delta[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_gamma[i], fb.pnl_gamma[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_vega[i], fb.pnl_vega[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_volga[i], fb.pnl_volga[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_vanna[i], fb.pnl_vanna[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_theta[i], fb.pnl_theta[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_rho[i], fb.pnl_rho[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_charm[i], fb.pnl_charm[i])) << i;
    EXPECT_TRUE(bits_equal(fa.pnl_unexplained[i], fb.pnl_unexplained[i])) << i;
  }
}
