// PortfolioPricer synthetic suite — prices a multi-underlying, multi-kind book
// against PricedSurfaces and validates the Taylor PnL-explain decomposition.
//
// Coverage:
//   * price() rows are bit-identical to direct PricedSurface::greeks (qty*mult
//     scaled), iv matches, totals equal the column sums;
//   * contract dedup (many positions, fewer unique contracts) prices once;
//   * a missing-uid position is ModelUnavailable; a degenerate contract Invalid;
//   * price()/pnl_explain() output is bit-identical across thread counts;
//   * pnl_explain isolates each state axis: a spot-only / rate-only / vol-only /
//     time-only shift lights up exactly the matching Taylor term (coefficient ==
//     base Greek * state move) and leaves the inactive axes exactly zero; the
//     eight components + unexplained sum to the full reprice; a small move is
//     reconstructed to a tight residual.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

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

// Relative-tolerance closeness for coefficient-wiring checks (avoids fragile
// floating-point associativity assumptions between the pricer and the test).
[[nodiscard]] bool close(double a, double b, double rel = 1e-9) noexcept {
  return std::fabs(a - b) <= rel * (std::fabs(a) + std::fabs(b) + 1e-300);
}

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

// eSSVI priced surface with knobs: `theta_bump` shifts the whole smile (vol
// axis); the forward F stays at the fixed reference kS across all variants so
// bumping only S / r / now leaves the model IV untouched (clean axis isolation).
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n, double theta_bump = 0.0,
                                       double S = kS, double r = kR, std::int64_t now = kNow,
                                       double q_eff = 0.02) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + theta_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
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

[[nodiscard]] PricedSurface make_svi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    SviParams v{};
    v.a = 0.02 + 0.001 * static_cast<double>(i);
    v.b = 0.10;
    v.rho = -0.3;
    v.m = 0.0;
    v.sigma = 0.15;
    v.T = T;
    v.F = F;
    v.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<SviCurve>(v, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 180, 4});
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

[[nodiscard]] SurfaceSet set_of(const std::vector<const PricedSurface *> &v) {
  auto ss = SurfaceSet::create(v);
  EXPECT_TRUE(ss.has_value());
  return std::move(*ss);
}

// A small single-underlying (uid 1) essvi book used across the isolation tests.
[[nodiscard]] std::vector<Position> pnl_book() {
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (double K : {92.0, 98.0, 100.0, 104.0, 110.0}) {
    book.push_back({id++, {1, K, 0.18, Side::Call}, +4.0, 100.0});
    book.push_back({id++, {1, K, 0.30, Side::Put}, -3.0, 100.0});
  }
  return book;
}

// A multi-underlying, multi-kind book with a dedup pair (id12/id14) and a
// no-surface uid (99 -> ModelUnavailable). Priced against {convex 1, essvi 2,
// svi 3}. Used by the in-place / totals-only bit-identity tests.
[[nodiscard]] std::vector<Position> multi_uid_book() {
  return {
      {10, {1, 100.0, 0.18, Side::Call}, +10.0, 100.0},
      {11, {1, 95.0, 0.18, Side::Put}, -5.0, 100.0},
      {12, {2, 105.0, 0.25, Side::Call}, +3.0, 100.0},
      {13, {3, 98.0, 0.15, Side::Put}, +7.0, 100.0},
      {14, {2, 105.0, 0.25, Side::Call}, +2.0, 100.0}, // dup of id12's contract
      {15, {99, 100.0, 0.10, Side::Call}, +1.0, 100.0}, // no surface -> unavailable
  };
}

// Caller-owned backing storage for a PriceFrameView, sized to `n`. The eight
// Greek columns are allocated only under `want_greeks` — mirroring how a
// marks-only caller sizes its buffers and leaves the greek spans empty.
struct FrameStore {
  std::vector<std::uint64_t> id;
  std::vector<std::uint32_t> uid;
  std::vector<double> pv, price, iv;
  std::vector<double> delta, gamma, vega, theta, rho, vanna, volga, charm;
  std::vector<PriceStatus> status;
  PriceTotals total{};

  FrameStore(std::size_t n, bool want_greeks) {
    id.resize(n);
    uid.resize(n);
    pv.resize(n);
    price.resize(n);
    iv.resize(n);
    status.resize(n);
    if (want_greeks) {
      delta.resize(n);
      gamma.resize(n);
      vega.resize(n);
      theta.resize(n);
      rho.resize(n);
      vanna.resize(n);
      volga.resize(n);
      charm.resize(n);
    }
  }

  [[nodiscard]] PriceFrameView view() {
    return PriceFrameView{id,    uid,   pv,    price, iv,     delta,  gamma, vega,
                          theta, rho,   vanna, volga, charm,  status, &total};
  }
};

} // namespace

// ── Pricing: multi-kind, multi-underlying, dedup, missing uid ────────────────

TEST(PortfolioPricer, Price_MultiKind_MultiUnderlying_BitIdenticalToGreeks) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});

  const std::vector<Position> book{
      {/*id*/ 10, {1, 100.0, 0.18, Side::Call}, +10.0, 100.0},
      {/*id*/ 11, {1, 95.0, 0.18, Side::Put}, -5.0, 100.0},
      {/*id*/ 12, {2, 105.0, 0.25, Side::Call}, +3.0, 100.0},
      {/*id*/ 13, {3, 98.0, 0.15, Side::Put}, +7.0, 100.0},
      {/*id*/ 14, {2, 105.0, 0.25, Side::Call}, +2.0, 100.0},  // dup of id 12's contract
      {/*id*/ 15, {99, 100.0, 0.10, Side::Call}, +1.0, 100.0}, // no surface -> unavailable
  };
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  EXPECT_EQ(pf->n_positions(), 6u);
  EXPECT_EQ(pf->n_contracts(), 5u);   // id12 & id14 share a contract
  EXPECT_EQ(pf->n_underlyings(), 4u); // uids {1,2,3,99}

  const PortfolioPricer pricer(std::move(*pf));
  auto fr = pricer.price(surfaces);
  ASSERT_TRUE(fr.has_value());
  const PriceFrame &f = *fr;
  ASSERT_EQ(f.size(), 6u);

  const std::array<const PricedSurface *, 4> by_uid{nullptr, &s1, &s2, &s3};
  PriceTotals expect_total{};
  for (std::size_t i = 0; i < book.size(); ++i) {
    const Position &p = book[i];
    EXPECT_EQ(f.id[i], p.id);
    EXPECT_EQ(f.uid[i], p.contract.uid);
    if (p.contract.uid == 99) {
      EXPECT_EQ(f.status[i], PriceStatus::ModelUnavailable);
      EXPECT_TRUE(std::isnan(f.pv[i]));
      continue;
    }
    ASSERT_EQ(f.status[i], PriceStatus::Ok);
    const PricedSurface &s = *by_uid[p.contract.uid];
    const auto g = s.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    const auto fv = s.fair_value(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(fv.has_value());
    EXPECT_TRUE(bits_equal(f.price[i], *fv)); // per-share American mark
    EXPECT_TRUE(bits_equal(f.iv[i], s.iv(p.contract.K, p.contract.T)));
    EXPECT_TRUE(bits_equal(f.pv[i], w * *fv));
    EXPECT_TRUE(bits_equal(f.delta[i], w * g->delta));
    EXPECT_TRUE(bits_equal(f.gamma[i], w * g->gamma));
    EXPECT_TRUE(bits_equal(f.vega[i], w * g->vega));
    EXPECT_TRUE(bits_equal(f.theta[i], w * g->theta));
    EXPECT_TRUE(bits_equal(f.rho[i], w * g->rho));
    EXPECT_TRUE(bits_equal(f.vanna[i], w * g->vanna));
    EXPECT_TRUE(bits_equal(f.volga[i], w * g->volga));
    EXPECT_TRUE(bits_equal(f.charm[i], w * g->charm));
    expect_total.pv += f.pv[i];
    expect_total.delta += f.delta[i];
    expect_total.vega += f.vega[i];
    ++expect_total.n_ok;
  }
  EXPECT_EQ(f.total.n_ok, 5u);
  EXPECT_TRUE(bits_equal(f.total.pv, expect_total.pv));
  EXPECT_TRUE(bits_equal(f.total.delta, expect_total.delta));
  EXPECT_TRUE(bits_equal(f.total.vega, expect_total.vega));

  // id12 and id14 reference one contract: same per-share price/iv, scaled PV.
  EXPECT_TRUE(bits_equal(f.price[2], f.price[4]));
  EXPECT_TRUE(bits_equal(f.iv[2], f.iv[4]));
  EXPECT_TRUE(bits_equal(f.pv[2], 1.5 * f.pv[4])); // qty 3 vs 2
}

TEST(PortfolioPricer, Price_DegenerateContract_Invalid) {
  const PricedSurface s1 = make_essvi(1, 4);
  const SurfaceSet surfaces = set_of({&s1});
  const std::vector<Position> book{
      {1, {1, -5.0, 0.2, Side::Call}, 1.0, 100.0},  // K <= 0
      {2, {1, 100.0, 0.0, Side::Call}, 1.0, 100.0}, // T <= 0
  };
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto fr = pricer.price(surfaces);
  ASSERT_TRUE(fr.has_value());
  EXPECT_EQ(fr->status[0], PriceStatus::InvalidContract);
  EXPECT_EQ(fr->status[1], PriceStatus::InvalidContract);
  EXPECT_EQ(fr->total.n_ok, 0u);
}

TEST(PortfolioPricer, Price_ThreadCounts_BitIdentical) {
  std::vector<PricedSurface> surfs;
  std::vector<const PricedSurface *> ptrs;
  std::vector<Position> book;
  for (std::uint32_t u = 1; u <= 6; ++u) {
    surfs.push_back((u & 1u) ? make_convex(u, 4, 28) : make_essvi(u, 5));
  }
  for (const PricedSurface &s : surfs) {
    ptrs.push_back(&s);
  }
  const SurfaceSet surfaces = set_of(ptrs);
  std::uint64_t id = 0;
  for (std::uint32_t u = 1; u <= 6; ++u) {
    for (double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
      book.push_back({id++, {u, K, 0.18, Side::Call}, 1.0 + 0.1 * static_cast<double>(u), 100.0});
      book.push_back({id++, {u, K, 0.28, Side::Put}, -2.0, 100.0});
    }
  }
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  auto a = pricer.price(surfaces, PriceOptions{1});
  auto b = pricer.price(surfaces, PriceOptions{8});
  auto c = pricer.price(surfaces, PriceOptions{0}); // hardware concurrency
  ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
  ASSERT_EQ(a->size(), b->size());
  for (std::size_t i = 0; i < a->size(); ++i) {
    EXPECT_TRUE(bits_equal(a->pv[i], b->pv[i])) << i;
    EXPECT_TRUE(bits_equal(a->delta[i], b->delta[i])) << i;
    EXPECT_TRUE(bits_equal(a->gamma[i], b->gamma[i])) << i;
    EXPECT_TRUE(bits_equal(a->vega[i], b->vega[i])) << i;
    EXPECT_TRUE(bits_equal(a->pv[i], c->pv[i])) << i;
  }
  EXPECT_TRUE(bits_equal(a->total.pv, b->total.pv));
  EXPECT_TRUE(bits_equal(a->total.delta, b->total.delta));
  EXPECT_TRUE(bits_equal(a->total.vega, b->total.vega));
}

// ── PnL explain: per-axis isolation, coefficient wiring, reconstruction ──────

TEST(PortfolioPricer, PnlExplain_SpotBump_DeltaGammaOnly) {
  const double dS = 0.05; // small spot move -> 2nd-order Taylor is tight
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface shifted = make_essvi(1, 5, /*theta_bump*/ 0.0, /*S*/ kS + dS);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  const std::vector<Position> book = pnl_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_NEAR(f.d_spot[i], dS, 1e-12);
    EXPECT_EQ(f.d_vol[i], 0.0) << i; // identical curves -> exact zero
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    // Only spot moved: the non-spot Taylor terms are exactly zero.
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_volga[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    // Coefficient wiring: delta/gamma terms are the base Greeks * move.
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(close(f.pnl_delta[i], w * g->delta * dS)) << i;
    EXPECT_TRUE(close(f.pnl_gamma[i], w * 0.5 * g->gamma * dS * dS)) << i;
    // Components + residual == the full reprice (bookkeeping).
    EXPECT_NEAR(f.pnl_delta[i] + f.pnl_gamma[i] + f.pnl_unexplained[i], f.pnl_total[i],
                1e-6 * (std::fabs(f.pnl_total[i]) + 1.0))
        << i;
    // Small move: delta + gamma explain the bulk.
    // (residual is now the pure higher-order Taylor tail: the early-exercise
    // premium lives inside the American delta/gamma. See TaylorReconstruction_Tight.)
  }
}

TEST(PortfolioPricer, PnlExplain_RateBump_RhoOnly) {
  const double dr = 1e-4; // 1 bp
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface shifted = make_essvi(1, 5, 0.0, kS, kR + dr);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  const std::vector<Position> book = pnl_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_NEAR(f.d_rate[i], dr, 1e-15);
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(close(f.pnl_rho[i], w * g->rho * dr)) << i;
    EXPECT_NEAR(f.pnl_rho[i] + f.pnl_unexplained[i], f.pnl_total[i],
                1e-6 * (std::fabs(f.pnl_total[i]) + 1.0))
        << i;
    // (residual is now the pure higher-order Taylor tail: the early-exercise
    // premium lives inside the American delta/gamma. See TaylorReconstruction_Tight.)
  }
}

TEST(PortfolioPricer, PnlExplain_VolBump_VegaVolgaOnly) {
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface shifted = make_essvi(1, 5, /*theta_bump*/ 0.001);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  const std::vector<Position> book = pnl_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    EXPECT_NE(f.d_vol[i], 0.0) << i;
    // dS = 0 -> spot/rate/time/cross terms are exactly zero.
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    // Coefficient wiring: vega/volga terms are base Greeks * dvol.
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(close(f.pnl_vega[i], w * g->vega * f.d_vol[i])) << i;
    EXPECT_TRUE(close(f.pnl_volga[i], w * 0.5 * g->volga * f.d_vol[i] * f.d_vol[i])) << i;
    EXPECT_NEAR(f.pnl_vega[i] + f.pnl_volga[i] + f.pnl_unexplained[i], f.pnl_total[i],
                1e-6 * (std::fabs(f.pnl_total[i]) + 1.0))
        << i;
    // (residual is now the pure higher-order Taylor tail: the early-exercise
    // premium lives inside the American delta/gamma. See TaylorReconstruction_Tight.)
  }
}

TEST(PortfolioPricer, PnlExplain_TimeBump_ThetaAndVolRoll) {
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface shifted = make_essvi(1, 5, 0.0, kS, kR, kNow + one_hour);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  const std::vector<Position> book = pnl_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  const double dt_expect = static_cast<double>(one_hour) / kNsPerYear;
  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_NEAR(f.d_time[i], dt_expect, 1e-15);
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    // dS = 0 -> delta/gamma/vanna/charm/rho vanish. Vol is measured at the COMMON
    // base maturity, so an identical-curve time roll leaves dvol EXACTLY zero (the
    // term roll now stays inside theta), and vega/volga are inert.
    EXPECT_EQ(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_volga[i], 0.0) << i;
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(close(f.pnl_theta[i], w * g->theta * dt_expect)) << i;
    const double sum = f.pnl_theta[i] + f.pnl_unexplained[i];
    EXPECT_NEAR(sum, f.pnl_total[i], 1e-6 * (std::fabs(f.pnl_total[i]) + 1.0)) << i;
    // (residual is now the pure higher-order Taylor tail: the early-exercise
    // premium lives inside the American delta/gamma. See TaylorReconstruction_Tight.)
  }
}

TEST(PortfolioPricer, PnlExplain_CombinedShift_SumsToTotal_And_ThreadDeterministic) {
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  const PricedSurface base = make_essvi(1, 5);
  // Combined small shift: spot +0.1, rate +5bp, vol +0.0005 theta, +1 hour.
  const PricedSurface shifted = make_essvi(1, 5, 0.0005, kS + 0.1, kR + 0.0005, kNow + one_hour);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto a = pricer.pnl_explain(bset, sset, PriceOptions{1});
  auto b = pricer.pnl_explain(bset, sset, PriceOptions{8});
  ASSERT_TRUE(a.has_value() && b.has_value());
  const PnlFrame &f = *a;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    const double sum = f.pnl_delta[i] + f.pnl_gamma[i] + f.pnl_vega[i] + f.pnl_volga[i] +
                       f.pnl_vanna[i] + f.pnl_theta[i] + f.pnl_rho[i] + f.pnl_charm[i] +
                       f.pnl_unexplained[i];
    EXPECT_NEAR(sum, f.pnl_total[i], 1e-6 * (std::fabs(f.pnl_total[i]) + 1.0)) << i;
    // Thread determinism.
    EXPECT_TRUE(bits_equal(a->pnl_total[i], b->pnl_total[i])) << i;
    EXPECT_TRUE(bits_equal(a->pnl_delta[i], b->pnl_delta[i])) << i;
    EXPECT_TRUE(bits_equal(a->pnl_vega[i], b->pnl_vega[i])) << i;
    EXPECT_TRUE(bits_equal(a->pnl_unexplained[i], b->pnl_unexplained[i])) << i;
  }
  EXPECT_TRUE(bits_equal(a->total.pnl_total, b->total.pnl_total));
}

// American-consistent Taylor reconstruction. The Greeks are now AMERICAN (cold
// finite differences on american_price), so a GENUINE early-exercise surface
// (q_eff > 0, both sides) reconstructs its American reprice from
// delta/gamma/vega/volga/vanna to a tight residual — the early-exercise premium
// lives inside the coefficients, so the old q_eff = 0 (European == American)
// trick is no longer needed to isolate the pure higher-order Taylor tail.
TEST(PortfolioPricer, PnlExplain_TaylorReconstruction_Tight) {
  const double dS = 0.05;
  const double dvol_bump = 0.0004;
  const PricedSurface base = make_essvi(1, 5);                        // q_eff = 0.02
  const PricedSurface shifted = make_essvi(1, 5, dvol_bump, kS + dS); // spot + vol move
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  // A genuine American book: both sides carry an early-exercise premium.
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (double K : {96.0, 100.0, 104.0, 110.0}) {
    book.push_back({id++, {1, K, 0.18, Side::Call}, +4.0, 100.0});
    book.push_back({id++, {1, K, 0.30, Side::Put}, -3.0, 100.0});
  }
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    // delta+gamma+vega+volga+vanna explain the American reprice to a tight residual.
    EXPECT_LT(std::fabs(f.pnl_unexplained[i]), 5e-3 * (std::fabs(f.pnl_total[i]) + 1.0)) << i;
  }
  EXPECT_LT(std::fabs(f.total.pnl_unexplained), 5e-3 * (std::fabs(f.total.pnl_total) + 1.0));
}

TEST(PortfolioPricer, PnlExplain_MissingShiftedSurface_ModelUnavailable) {
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface other = make_essvi(2, 5);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&other}); // uid 1 absent on the shifted side

  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  for (const PriceStatus st : er->status) {
    EXPECT_EQ(st, PriceStatus::ModelUnavailable);
  }
  EXPECT_EQ(er->total.n_ok, 0u);
}

TEST(Portfolio, HundredThousandPositionsUseBoundedDedupHint) {
  constexpr std::size_t kUnique = 128;
  constexpr std::size_t kPositions = 100'000;
  std::vector<Position> positions;
  positions.reserve(kPositions);
  for (std::size_t i = 0; i < kPositions; ++i) {
    const std::size_t j = i % kUnique;
    positions.push_back(Position{
        static_cast<std::uint64_t>(i),
        OptionContract{1u, 80.0 + static_cast<double>(j), 0.25, (j & 1u) ? Side::Call : Side::Put},
        1.0, 100.0});
  }
  auto portfolio =
      Portfolio::create(positions, PortfolioBuildOptions{.expected_unique_contracts = kUnique});
  ASSERT_TRUE(portfolio.has_value());
  EXPECT_EQ(portfolio->n_positions(), kPositions);
  EXPECT_EQ(portfolio->n_contracts(), kUnique);
}

TEST(PortfolioPricer, PricesOnlyPopulatesMarksAndLeavesRiskNaN) {
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto frame = pricer.price(surfaces, PriceOptions{.n_threads = 4, .prices_only = true});
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->total.n_ok, frame->size());

  // Under the Marks mask the eight Greek column vectors are left EMPTY (the
  // 64 B/pos saving), not resized-and-NaN-filled. A marks-only caller reads the
  // marks columns and gates any Greek access on greeks_materialized().
  EXPECT_FALSE(frame->greeks_materialized());
  EXPECT_EQ(frame->delta.size(), 0u);
  EXPECT_EQ(frame->gamma.size(), 0u);
  EXPECT_EQ(frame->vega.size(), 0u);
  EXPECT_EQ(frame->theta.size(), 0u);
  EXPECT_EQ(frame->rho.size(), 0u);
  EXPECT_EQ(frame->vanna.size(), 0u);
  EXPECT_EQ(frame->volga.size(), 0u);
  EXPECT_EQ(frame->charm.size(), 0u);
  for (std::size_t i = 0; i < frame->size(); ++i) {
    EXPECT_EQ(frame->status[i], PriceStatus::Ok);
    EXPECT_TRUE(std::isfinite(frame->price[i]));
    EXPECT_TRUE(std::isfinite(frame->pv[i]));
    EXPECT_TRUE(std::isfinite(frame->iv[i]));
  }

  // The aggregate must agree with the columns it aggregates. A finite 0.0 total
  // vega alongside n_ok > 0 is indistinguishable from a genuinely vega-flat book
  // -- exactly the false-flat reading the unpriced-greek accounting exists to
  // prevent. PV stays finite: prices_only computes marks, it only skips risk.
  EXPECT_GT(frame->total.n_ok, 0u);
  EXPECT_TRUE(std::isfinite(frame->total.pv));
  EXPECT_TRUE(std::isnan(frame->total.delta));
  EXPECT_TRUE(std::isnan(frame->total.gamma));
  EXPECT_TRUE(std::isnan(frame->total.vega));
  EXPECT_TRUE(std::isnan(frame->total.theta));
  EXPECT_TRUE(std::isnan(frame->total.rho));
  EXPECT_TRUE(std::isnan(frame->total.vanna));
  EXPECT_TRUE(std::isnan(frame->total.volga));
  EXPECT_TRUE(std::isnan(frame->total.charm));
}

// ── In-place API: price_into / price_totals bit-identity + zero-alloc ────────

TEST(PortfolioPricer, PriceInto_FullGreeks_BitIdenticalToPrice) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});
  auto pf = Portfolio::create(multi_uid_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  const PriceOptions opts{.n_threads = 4};
  auto ref = pricer.price(surfaces, opts);
  ASSERT_TRUE(ref.has_value());
  ASSERT_TRUE(ref->greeks_materialized());

  FrameStore fs(ref->size(), /*want_greeks=*/true);
  PortfolioWorkspace ws;
  ws.reserve(pricer.portfolio().n_contracts(), pricer.portfolio().n_positions());
  PriceFrameView v = fs.view();
  const Status s = pricer.price_into(surfaces, PriceFieldMask::FullGreeks, v, ws, opts);
  ASSERT_TRUE(s.has_value());

  for (std::size_t i = 0; i < ref->size(); ++i) {
    EXPECT_EQ(fs.id[i], ref->id[i]) << i;
    EXPECT_EQ(fs.uid[i], ref->uid[i]) << i;
    EXPECT_EQ(fs.status[i], ref->status[i]) << i;
    EXPECT_TRUE(bits_equal(fs.iv[i], ref->iv[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pv[i], ref->pv[i])) << i;
    EXPECT_TRUE(bits_equal(fs.price[i], ref->price[i])) << i;
    EXPECT_TRUE(bits_equal(fs.delta[i], ref->delta[i])) << i;
    EXPECT_TRUE(bits_equal(fs.gamma[i], ref->gamma[i])) << i;
    EXPECT_TRUE(bits_equal(fs.vega[i], ref->vega[i])) << i;
    EXPECT_TRUE(bits_equal(fs.theta[i], ref->theta[i])) << i;
    EXPECT_TRUE(bits_equal(fs.rho[i], ref->rho[i])) << i;
    EXPECT_TRUE(bits_equal(fs.vanna[i], ref->vanna[i])) << i;
    EXPECT_TRUE(bits_equal(fs.volga[i], ref->volga[i])) << i;
    EXPECT_TRUE(bits_equal(fs.charm[i], ref->charm[i])) << i;
  }
  EXPECT_EQ(fs.total.n_ok, ref->total.n_ok);
  EXPECT_TRUE(bits_equal(fs.total.pv, ref->total.pv));
  EXPECT_TRUE(bits_equal(fs.total.delta, ref->total.delta));
  EXPECT_TRUE(bits_equal(fs.total.gamma, ref->total.gamma));
  EXPECT_TRUE(bits_equal(fs.total.vega, ref->total.vega));
  EXPECT_TRUE(bits_equal(fs.total.theta, ref->total.theta));
  EXPECT_TRUE(bits_equal(fs.total.rho, ref->total.rho));
  EXPECT_TRUE(bits_equal(fs.total.vanna, ref->total.vanna));
  EXPECT_TRUE(bits_equal(fs.total.volga, ref->total.volga));
  EXPECT_TRUE(bits_equal(fs.total.charm, ref->total.charm));
}

TEST(PortfolioPricer, PriceInto_Marks_MarksBitIdentical_GreeksUntouched) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});
  auto pf = Portfolio::create(multi_uid_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  auto ref = pricer.price(surfaces, PriceOptions{.n_threads = 4, .prices_only = true});
  ASSERT_TRUE(ref.has_value());
  ASSERT_FALSE(ref->greeks_materialized());

  // A marks-only caller allocates NO greek storage; the greek spans stay empty.
  FrameStore fs(ref->size(), /*want_greeks=*/false);
  PortfolioWorkspace ws;
  PriceFrameView v = fs.view();
  const Status s = pricer.price_into(surfaces, PriceFieldMask::Marks, v, ws,
                                     PriceOptions{.n_threads = 4});
  ASSERT_TRUE(s.has_value());

  for (std::size_t i = 0; i < ref->size(); ++i) {
    EXPECT_EQ(fs.id[i], ref->id[i]) << i;
    EXPECT_EQ(fs.uid[i], ref->uid[i]) << i;
    EXPECT_EQ(fs.status[i], ref->status[i]) << i;
    EXPECT_TRUE(bits_equal(fs.iv[i], ref->iv[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pv[i], ref->pv[i])) << i;
    EXPECT_TRUE(bits_equal(fs.price[i], ref->price[i])) << i;
  }
  // Greek spans were never touched (empty), matching the returning API's Marks.
  EXPECT_EQ(fs.delta.size(), 0u);
  EXPECT_EQ(fs.charm.size(), 0u);

  // pv total + n_ok bit-identical; greek totals stay NaN (not a clean zero).
  EXPECT_EQ(fs.total.n_ok, ref->total.n_ok);
  EXPECT_TRUE(bits_equal(fs.total.pv, ref->total.pv));
  EXPECT_TRUE(std::isnan(fs.total.delta));
  EXPECT_TRUE(std::isnan(fs.total.vega));
  EXPECT_TRUE(std::isnan(fs.total.charm));
}

TEST(PortfolioPricer, PriceTotals_BitIdenticalToPriceTotal_BothMasks) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});
  auto pf = Portfolio::create(multi_uid_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  const PriceOptions opts{.n_threads = 4};
  auto ref_full = pricer.price(surfaces, opts);
  auto ref_marks = pricer.price(surfaces, PriceOptions{.n_threads = 4, .prices_only = true});
  ASSERT_TRUE(ref_full.has_value() && ref_marks.has_value());

  PortfolioWorkspace ws;
  auto tf = pricer.price_totals(surfaces, PriceFieldMask::FullGreeks, ws, opts);
  auto tm = pricer.price_totals(surfaces, PriceFieldMask::Marks, ws, opts);
  ASSERT_TRUE(tf.has_value() && tm.has_value());

  // FullGreeks: every total field bit-identical to price().total.
  EXPECT_EQ(tf->n_ok, ref_full->total.n_ok);
  EXPECT_TRUE(bits_equal(tf->pv, ref_full->total.pv));
  EXPECT_TRUE(bits_equal(tf->delta, ref_full->total.delta));
  EXPECT_TRUE(bits_equal(tf->gamma, ref_full->total.gamma));
  EXPECT_TRUE(bits_equal(tf->vega, ref_full->total.vega));
  EXPECT_TRUE(bits_equal(tf->theta, ref_full->total.theta));
  EXPECT_TRUE(bits_equal(tf->rho, ref_full->total.rho));
  EXPECT_TRUE(bits_equal(tf->vanna, ref_full->total.vanna));
  EXPECT_TRUE(bits_equal(tf->volga, ref_full->total.volga));
  EXPECT_TRUE(bits_equal(tf->charm, ref_full->total.charm));

  // Marks: pv + n_ok bit-identical, greek sums NaN.
  EXPECT_EQ(tm->n_ok, ref_marks->total.n_ok);
  EXPECT_TRUE(bits_equal(tm->pv, ref_marks->total.pv));
  EXPECT_TRUE(std::isnan(tm->delta));
  EXPECT_TRUE(std::isnan(tm->vega));
}

TEST(PortfolioPricer, PriceInto_ThreadCounts_TotalsBitIdentical) {
  std::vector<PricedSurface> surfs;
  std::vector<const PricedSurface *> ptrs;
  std::vector<Position> book;
  for (std::uint32_t u = 1; u <= 6; ++u) {
    surfs.push_back((u & 1u) ? make_convex(u, 4, 28) : make_essvi(u, 5));
  }
  for (const PricedSurface &s : surfs) {
    ptrs.push_back(&s);
  }
  const SurfaceSet surfaces = set_of(ptrs);
  std::uint64_t id = 0;
  for (std::uint32_t u = 1; u <= 6; ++u) {
    for (double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
      book.push_back({id++, {u, K, 0.18, Side::Call}, 1.0 + 0.1 * static_cast<double>(u), 100.0});
      book.push_back({id++, {u, K, 0.28, Side::Put}, -2.0, 100.0});
    }
  }
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();

  PortfolioWorkspace ws;
  PriceTotals totals[4];
  const unsigned thread_counts[4] = {1, 2, 4, 8};
  for (int k = 0; k < 4; ++k) {
    FrameStore fs(n, /*want_greeks=*/true);
    PriceFrameView v = fs.view();
    ASSERT_TRUE(pricer
                    .price_into(surfaces, PriceFieldMask::FullGreeks, v, ws,
                                PriceOptions{.n_threads = thread_counts[k]})
                    .has_value());
    totals[k] = fs.total;
  }
  for (int k = 1; k < 4; ++k) {
    EXPECT_TRUE(bits_equal(totals[k].pv, totals[0].pv)) << k;
    EXPECT_TRUE(bits_equal(totals[k].delta, totals[0].delta)) << k;
    EXPECT_TRUE(bits_equal(totals[k].vega, totals[0].vega)) << k;
    EXPECT_EQ(totals[k].n_ok, totals[0].n_ok) << k;
  }
}

// Zero-allocation proof + retained-substrate reuse. The assertions are only
// meaningful under -DATX_VOL_COUNTERS=ON; the OFF build compiles it as a
// disabled-sentinel check so the default suite still exercises the in-place path.
TEST(PortfolioPricer, PriceInto_ZeroAllocation_And_PreparedReuse) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();
  const std::size_t nu = pricer.portfolio().n_contracts();

  PortfolioWorkspace ws;
  ws.reserve(nu, n);
  FrameStore fg(n, /*want_greeks=*/true);
  PriceFrameView vg = fg.view();
  // Warm up: first call builds the retained PreparedPortfolio + sizes the scratch.
  ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws).has_value());

  if constexpr (counters_enabled()) {
    // Second FullGreeks call: reuses the substrate, allocates no frame.
    atx::vol::counters::reset();
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws).has_value());
    auto sg = atx::vol::counters::snapshot();
    EXPECT_EQ(sg.get(Counter::FrameAllocations), 0u);
    EXPECT_EQ(sg.get(Counter::FrameBytes), std::uint64_t{101} * n);
    EXPECT_EQ(sg.get(Counter::PreparedBuilds), 0u); // reused, not rebuilt

    // Marks touches 37 B/pos and still allocates nothing. The retained substrate
    // is shared across masks (it depends only on (uid,side,T), not the Greek
    // route), so this Marks call reuses the SAME PreparedPortfolio built above
    // -- no rebuild -- and the reuse is re-measured below.
    FrameStore fm(n, /*want_greeks=*/false);
    PriceFrameView vm = fm.view();
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, vm, ws).has_value());
    atx::vol::counters::reset();
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, vm, ws).has_value());
    auto sm = atx::vol::counters::snapshot();
    EXPECT_EQ(sm.get(Counter::FrameAllocations), 0u);
    EXPECT_EQ(sm.get(Counter::FrameBytes), std::uint64_t{37} * n);
    EXPECT_EQ(sm.get(Counter::PreparedBuilds), 0u);
  } else {
    EXPECT_FALSE(atx::vol::counters::snapshot().enabled);
    SUCCEED();
  }
}

// Regression for the workspace-cache ABA hazard: a single PortfolioWorkspace
// reused across two DIFFERENT books (different unique-contract counts) held,
// one after another, by the SAME PortfolioPricer variable (its address is
// fixed for its whole lifetime; reassigning it mirrors the reviewer's
// `PortfolioPricer pr(build(book));` reconstructed-at-the-same-address loop).
// Before the ensure_prepared fix, `prepared_book == &pf` alone would consider
// the first (larger) book's substrate still valid for the second (smaller)
// book, so solve_uniques() would index oci[p] up to the STALE larger
// n_unique while px had only been resized to the new, smaller book's contract
// count -- a heap out-of-bounds write (or, had the stale count been <= the new
// count, a silent mis-price). The fix additionally requires
// prepared->n_unique() == pf.n_contracts() and a content fingerprint match, so
// the substrate is correctly rebuilt for the new book and the result below
// must be bit-identical to a fresh-workspace price of the same book.
TEST(PortfolioPricer, PriceInto_WorkspaceReuseAcrossDifferentBooksAtSamePricerAddress) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});

  // Book A: 6 unique contracts -- LARGER unique count.
  std::vector<Position> book_a;
  std::uint64_t id_a = 0;
  for (double K : {90.0, 95.0, 100.0, 105.0, 110.0, 115.0}) {
    book_a.push_back({id_a++, {1, K, 0.18, Side::Call}, 1.0, 100.0});
  }
  auto pf_a = Portfolio::create(book_a);
  ASSERT_TRUE(pf_a.has_value());
  ASSERT_EQ(pf_a->n_contracts(), 6u);

  // Book B: 2 unique contracts -- SMALLER unique count, different uids.
  const std::vector<Position> book_b{
      {100, {2, 105.0, 0.25, Side::Call}, 3.0, 100.0},
      {101, {3, 98.0, 0.15, Side::Put}, 7.0, 100.0},
  };
  auto pf_b = Portfolio::create(book_b);
  ASSERT_TRUE(pf_b.has_value());
  ASSERT_EQ(pf_b->n_contracts(), 2u);

  PortfolioWorkspace ws;
  PortfolioPricer pr(std::move(*pf_a));
  const void *addr_before = static_cast<const void *>(&pr);

  {
    const std::size_t na = pr.portfolio().n_positions();
    FrameStore fa(na, /*want_greeks=*/true);
    ASSERT_TRUE(pr.price_into(surfaces, PriceFieldMask::FullGreeks, fa.view(), ws).has_value());
  }

  // Reassign the SAME variable to a DIFFERENT, SMALLER book. `pr` occupies one
  // stack slot for its whole lifetime, so its (and its `pf_` member's) address
  // is unchanged by the reassignment.
  pr = PortfolioPricer(std::move(*pf_b));
  ASSERT_EQ(static_cast<const void *>(&pr), addr_before);
  const std::size_t nb = pr.portfolio().n_positions();

  FrameStore fb_reused(nb, /*want_greeks=*/true);
  ASSERT_TRUE(
      pr.price_into(surfaces, PriceFieldMask::FullGreeks, fb_reused.view(), ws).has_value());

  // Reference: a FRESH workspace pricing the SAME book B.
  PortfolioWorkspace ws_fresh;
  FrameStore fb_fresh(nb, /*want_greeks=*/true);
  ASSERT_TRUE(
      pr.price_into(surfaces, PriceFieldMask::FullGreeks, fb_fresh.view(), ws_fresh).has_value());

  ASSERT_EQ(fb_reused.status.size(), fb_fresh.status.size());
  for (std::size_t i = 0; i < nb; ++i) {
    EXPECT_EQ(fb_reused.status[i], fb_fresh.status[i]) << i;
    EXPECT_TRUE(bits_equal(fb_reused.pv[i], fb_fresh.pv[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.delta[i], fb_fresh.delta[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.gamma[i], fb_fresh.gamma[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.vega[i], fb_fresh.vega[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.theta[i], fb_fresh.theta[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.rho[i], fb_fresh.rho[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.vanna[i], fb_fresh.vanna[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.volga[i], fb_fresh.volga[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.charm[i], fb_fresh.charm[i])) << i;
  }
  EXPECT_TRUE(bits_equal(fb_reused.total.pv, fb_fresh.total.pv));
}

// Minor #2 regression: PreparedPortfolio's permutation/groups/oci/aligned
// columns depend only on (uid,side,T), not the Greek route, so alternating
// Marks <-> FullGreeks on one warm workspace must NOT rebuild the substrate --
// exactly one PreparedPortfolio build for the whole sequence (the initial
// warm-up before the counter reset below). Meaningful only under
// -DATX_VOL_COUNTERS=ON; the OFF build checks the disabled-sentinel so the
// default suite still exercises the mask-alternation path.
TEST(PortfolioPricer, PriceInto_AlternatingMasks_NoRebuild) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();

  PortfolioWorkspace ws;
  FrameStore fg(n, /*want_greeks=*/true);
  FrameStore fm(n, /*want_greeks=*/false);
  PriceFrameView vg = fg.view();
  PriceFrameView vm = fm.view();

  // Warm-up build (the mask does not affect the substrate it builds).
  ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws).has_value());

  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, vm, ws).has_value());
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws).has_value());
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, vm, ws).has_value());
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws).has_value());
    const auto snap = atx::vol::counters::snapshot();
    EXPECT_EQ(snap.get(Counter::PreparedBuilds), 0u); // reused across every mask flip
  } else {
    EXPECT_FALSE(atx::vol::counters::snapshot().enabled);
    SUCCEED();
  }
}

// An over-sized hint is advisory, not load-bearing: it must not change the dedup
// result, and must not reach reserve() unclamped.
TEST(PortfolioPricer, OversizedUniqueContractHintIsClampedAndHarmless) {
  const auto positions = pnl_book();
  auto exact = Portfolio::create(positions);
  ASSERT_TRUE(exact.has_value());

  auto hinted = Portfolio::create(
      positions, PortfolioBuildOptions{.expected_unique_contracts = 1'000'000'000u});
  ASSERT_TRUE(hinted.has_value());
  EXPECT_EQ(hinted->n_positions(), exact->n_positions());
  EXPECT_EQ(hinted->n_contracts(), exact->n_contracts());
}
